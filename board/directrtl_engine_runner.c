// F-ASTRA board runner of the unified ModernTCN engine (experimental track, AGENTS.md "Unified ModernTCN engine"; never
// a FINN DirectRTL result) in a multi-model harness (directrtl_harness.route_models, model kind "engine").
// The engine window (program at 0, activation arena and weights at DDR_OFFSET; moderntcn_engine_harness.layout) sits
// at WINDOW_OFFSET of the models pool (default 0; a concurrent session places it above the FINN models' buffers): the runner holds the engine (shell CTRL), copies the window image, writes the pool's
// bus address into the shell's BASE (f_astra_axi_rebase adds it to every engine address; the shell refuses START
// without it), programs the shell's FM registers (memory WINDOW, run DEADLINE, heartbeat WDT) and the engine's
// PROG_BASE/DDR_BASE, and only then releases the engine; per frame it writes the
// input slots, sets START and polls the shell until the run ends. Outputs are compared byte for byte with the expected
// file (the FINN integer reference); the first measured run is saved. The timer covers START, completion polling and
// heartbeats of every frame, not the input copies or the comparisons. Inputs and outputs are little-endian elements of
// IN_BYTES / OUT_BYTES (default 4: int32 slots; 1 or 2 for packed tensors), sign extended, [time][channel]; an input
// code beyond the declared width is refused before the run (L1-S2-02).
// Progress and failures go to stderr (unbuffered); exit 0 pass, 2 setup failure, 3 timeout or abort, 4 mismatch,
// 5 an FM fault. --quiesce holds the engine and waits for an idle bus (before any PL reprogramming).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct pool_info { uint64_t dma_address; uint32_t bytes; uint32_t output_offset; };
#define POOL_INFO _IOR('F', 1, struct pool_info)

#define APERTURE 0x10000u
#include "f_astra_pl_io.h"          // rd, wr, map_registers: /dev/mem or the secure monitor
enum { E_CTRL = 0x00, E_STATUS = 0x04, E_PROG = 0x08, E_DDR = 0x0C, E_CYC_LO = 0x10, E_CYC_HI = 0x14, E_INSTR = 0x18,
       E_ID = 0x1C, E_CFG0 = 0x20, E_CFG1 = 0x24, E_CFG2 = 0x28, E_PC = 0x2C };
enum { S_ID = 0x80, S_CTRL = 0x84, S_STATUS = 0x88, S_FAULTS = 0x8C, S_DEADLINE = 0x90, S_WDT = 0x94, S_HEARTBEAT = 0x98,
       S_WINDOW = 0x9C, S_RUNS = 0xA0, S_DONES = 0xA4, S_LAST = 0xA8, S_MAX = 0xAC, S_FAULT_RUN = 0xB0,
       S_FAULT_COUNT = 0xB4, S_ENGINE_STATUS = 0xBC, S_BUS = 0xC0, S_BASE = 0xC8 };
enum { ST_RUNNING = 1, ST_DONE = 2, ST_ISOLATED = 4, ST_RESET = 8, ST_DRAINED = 16 };
enum { C_HOLD = 1, C_WDT = 4, C_SOFT_RESET = 0x100, C_CLEAR = 0x200 };
#define SHELL_ID 0x4D545348u
#define ENGINE_ID 0x4D54434Eu
#define WDT_CYCLES (1u << 27)
#define MAX_RUNS 2048            // the measurement protocol: 100 warm-up and 1000 measured runs
#define MAX_TIMES 65536

static volatile uint32_t *shell;

static void fail(const char *what)
{
    fprintf(stderr, "F_ASTRA_ENGINE_RUNNER_FAILED %s errno=%d\n", what, errno);
    exit(2);
}

static void phase(const char *what)
{
    // the phase's start on the session's timeline clock (/proc/uptime = CLOCK_BOOTTIME): a runner's setup and
    // teardown can be told from its run in the session's timeline.txt
    struct timespec up;
#ifdef CLOCK_BOOTTIME
    clock_gettime(CLOCK_BOOTTIME, &up);
#else
    clock_gettime(CLOCK_MONOTONIC, &up);
#endif
    fprintf(stderr, "F_ASTRA_ENGINE_RUNNER_PHASE %s %ld.%02ld\n", what, (long)up.tv_sec, (long)(up.tv_nsec / 10000000));
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// Thermal stop (environment THERMAL_PACE_HIGH_C and THERMAL_PACE_LOW_C, both set): before every frame a die at or above
// the high mark waits until it is below the low mark; every frame is timed alone, so a pause changes no measurement.
static double pace_high, pace_low, pause_s;
static unsigned long pauses;

// XADC die temperature in degrees C (sysfs iio), or a negative value when it cannot be read
static double die_temp_c(void)
{
    static const char *part[3] = {"raw", "offset", "scale"};
    double v[3];
    int k;
    for (k = 0; k < 3; k++) {
        char path[96];
        FILE *f;
        snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device0/in_temp0_%s", part[k]);
        f = fopen(path, "r");
        if (!f) return -1.0;
        if (fscanf(f, "%lf", &v[k]) != 1) { fclose(f); return -1.0; }
        fclose(f);
    }
    return (v[0] + v[1]) * v[2] / 1000.0;
}

static void thermal_setup(void)
{
    if (!getenv("THERMAL_PACE_HIGH_C") || !getenv("THERMAL_PACE_LOW_C")) return;
    pace_high = strtod(getenv("THERMAL_PACE_HIGH_C"), NULL);
    pace_low = strtod(getenv("THERMAL_PACE_LOW_C"), NULL);
    if (!(pace_low > 0.0 && pace_low < pace_high && pace_high <= 100.0) || die_temp_c() < 0.0) {
        errno = EINVAL;
        fail("thermal marks or die temperature");
    }
}

static void thermal_pause(unsigned long run)
{
    uint64_t start, beat;
    double t;
    if (pace_high <= 0.0 || die_temp_c() < pace_high) return;
    start = beat = now_ns();
    pauses++;
    fprintf(stderr, "F_ASTRA_ENGINE_THERMAL_STOP run=%lu\n", run);
    while ((t = die_temp_c()) >= pace_low) {
        struct timespec rest = {0, 200000000};
        if (now_ns() - beat > 100000000ull) { wr(shell, S_HEARTBEAT, 1); beat = now_ns(); }
        nanosleep(&rest, NULL);
    }
    pause_s += (double)(now_ns() - start) / 1e9;
    fprintf(stderr, "F_ASTRA_ENGINE_THERMAL_RESUME run=%lu temp_c=%.2f\n", run, t);
    (void)beat;
}

static void *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    struct stat st;
    void *data;
    if (!f || fstat(fileno(f), &st)) fail(path);
    data = malloc((size_t)st.st_size ? (size_t)st.st_size : 1);
    if (!data || fread(data, 1, (size_t)st.st_size, f) != (size_t)st.st_size) fail(path);
    fclose(f);
    *size = (size_t)st.st_size;
    return data;
}

// wait until (STATUS & mask) == want, at most limit_ns; returns the last STATUS and whether it matched
static int wait_status(uint32_t mask, uint32_t want, uint64_t limit_ns, uint32_t *status)
{
    uint64_t start = now_ns();
    for (;;) {
        *status = rd(shell, S_STATUS);
        if ((*status & mask) == want) return 1;
        if (now_ns() - start > limit_ns) return 0;
    }
}

// hold the engine: abort a run in flight, isolate it, reset it once the bus has drained
static int hold(uint32_t extra, uint32_t *status)
{
    wr(shell, S_CTRL, C_HOLD | extra);
    return wait_status(ST_ISOLATED | ST_RESET | ST_DRAINED, ST_ISOLATED | ST_RESET | ST_DRAINED, 1000000000ull, status);
}

static void dump_shell(const char *tag)
{
    fprintf(stderr, "F_ASTRA_ENGINE_SHELL %s status=0x%08x faults=0x%08x runs=%u dones=%u last_cycles=%u fault_run=%u "
                    "engine_status=0x%08x bus=0x%08x\n", tag, rd(shell, S_STATUS), rd(shell, S_FAULTS), rd(shell, S_RUNS),
            rd(shell, S_DONES), rd(shell, S_LAST), rd(shell, S_FAULT_RUN), rd(shell, S_ENGINE_STATUS), rd(shell, S_BUS));
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    const char *pool_path, *image_path, *input_path, *expected_path, *output_path;
    unsigned long frames, warmup, repeat, timeout_ms, r, f, n = 0, mismatched_runs = 0, mismatched_frames = 0, faulted = 0;
    uint32_t shell_base, window, ddr_offset, in_offset, in_slots, in_bits, out_offset, out_slots, weight_offset;
    uint32_t deadline, status, faults_seen = 0, pool_address, window_offset = 0, in_eb = 4, out_eb = 4;
    double ops_per_frame;
    size_t image_bytes, in_bytes, out_bytes, in_frame, out_frame, i;
    uint8_t *image, *input, *expected, *pool, *saved, *win;
    int32_t low, high;
    struct pool_info info;
    int pool_fd, mem_fd, image_intact;
    static uint64_t times[MAX_TIMES], sorted[MAX_TIMES];
    uint64_t total = 0, cycles, cycles_min = ~0ull, cycles_max = 0, cycles_sum = 0;

    if (argc == 3 && !strcmp(argv[1], "--quiesce")) {
        int halted = 0;
        uint32_t id;
        mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mem_fd < 0) fail("/dev/mem");
        shell = map_registers(mem_fd, (uint32_t)strtoul(argv[2], NULL, 0));
        id = rd(shell, S_ID);
        status = 0;
        if (id == SHELL_ID) halted = hold(0, &status);
        printf("{\"schema\":\"f-astra/moderntcn-engine-quiesce/v1\",\"shell_base\":%lu,\"shell_id\":%u,\"halted\":%s,"
               "\"status\":%u,\"faults\":%u,\"runs\":%u,\"dones\":%u}\n", strtoul(argv[2], NULL, 0), id,
               halted ? "true" : "false", status, id == SHELL_ID ? rd(shell, S_FAULTS) : 0,
               id == SHELL_ID ? rd(shell, S_RUNS) : 0, id == SHELL_ID ? rd(shell, S_DONES) : 0);
        return 0;
    }
    if (argc != 21 && argc != 22 && argc != 24) {
        fprintf(stderr, "usage: %s POOL IMAGE INPUT EXPECTED OUTPUT FRAMES OPS_PER_FRAME WARMUP REPEAT TIMEOUT_MS SHELL_BASE "
                        "WINDOW_BYTES DDR_OFFSET IN_OFFSET IN_SLOTS IN_BITS OUT_OFFSET OUT_SLOTS WEIGHT_OFFSET DEADLINE_CYCLES "
                        "[WINDOW_OFFSET [IN_BYTES OUT_BYTES]]\n"
                        "       %s --quiesce SHELL_BASE\n", argv[0], argv[0]);
        return 64;
    }
    pool_path = argv[1]; image_path = argv[2]; input_path = argv[3]; expected_path = argv[4]; output_path = argv[5];
    frames = strtoul(argv[6], NULL, 10); ops_per_frame = strtod(argv[7], NULL);
    warmup = strtoul(argv[8], NULL, 10); repeat = strtoul(argv[9], NULL, 10); timeout_ms = strtoul(argv[10], NULL, 10);
    shell_base = (uint32_t)strtoul(argv[11], NULL, 0);
    window = (uint32_t)strtoul(argv[12], NULL, 0); ddr_offset = (uint32_t)strtoul(argv[13], NULL, 0);
    in_offset = (uint32_t)strtoul(argv[14], NULL, 0); in_slots = (uint32_t)strtoul(argv[15], NULL, 0);
    in_bits = (uint32_t)strtoul(argv[16], NULL, 0); out_offset = (uint32_t)strtoul(argv[17], NULL, 0);
    out_slots = (uint32_t)strtoul(argv[18], NULL, 0); weight_offset = (uint32_t)strtoul(argv[19], NULL, 0);
    deadline = (uint32_t)strtoul(argv[20], NULL, 0);
    if (argc >= 22) window_offset = (uint32_t)strtoul(argv[21], NULL, 0);
    if (argc == 24) { in_eb = (uint32_t)strtoul(argv[22], NULL, 0); out_eb = (uint32_t)strtoul(argv[23], NULL, 0); }
    if (!frames || ops_per_frame <= 0 || !repeat || warmup + repeat > MAX_RUNS || frames * repeat > MAX_TIMES || !timeout_ms
        || !window || (window & 4095) || (ddr_offset & 4095) || !in_slots || !out_slots || in_bits < 2 || in_bits > 32
        || (in_eb != 1 && in_eb != 2 && in_eb != 4) || (out_eb != 1 && out_eb != 2 && out_eb != 4) || in_bits > 8 * in_eb
        || (in_offset & 3) || (out_offset & 3) || in_offset + (uint64_t)in_eb * in_slots > window
        || out_offset + (uint64_t)out_eb * out_slots > window
        || ddr_offset > in_offset || weight_offset < ddr_offset || weight_offset > window || (window_offset & 4095)) {
        errno = EINVAL;
        fail("arguments");
    }
    phase("load");
    image = load(image_path, &image_bytes);
    input = load(input_path, &in_bytes);
    expected = load(expected_path, &out_bytes);
    in_frame = (size_t)in_eb * in_slots; out_frame = (size_t)out_eb * out_slots;
    if (image_bytes > window || image_bytes < weight_offset || in_bytes != frames * in_frame || out_bytes != frames * out_frame) {
        errno = EINVAL;
        fail("sizes");
    }
    // input codes at their declared width (L1-S2-02): refused before the engine sees them
    low = in_bits == 32 ? INT32_MIN : -(int32_t)(1u << (in_bits - 1));
    high = in_bits == 32 ? INT32_MAX : (int32_t)((1u << (in_bits - 1)) - 1);
    for (i = 0; i < in_bytes / in_eb; i++) {
        int32_t v;
        if (in_eb == 1) v = (int8_t)input[i];
        else if (in_eb == 2) { int16_t h; memcpy(&h, input + 2 * i, 2); v = h; }
        else memcpy(&v, input + 4 * i, 4);
        if (v < low || v > high) { errno = ERANGE; fail("input code beyond the declared width"); }
    }

    phase("pool");
    // "fd:N": the pool descriptor the session opened once (a concurrent session shares the single-open pool)
    pool_fd = strncmp(pool_path, "fd:", 3) ? open(pool_path, O_RDWR) : atoi(pool_path + 3);
    if (pool_fd < 0 || ioctl(pool_fd, POOL_INFO, &info)) fail(pool_path);
    if ((uint64_t)window_offset + window > info.output_offset || (info.dma_address & 4095)
        || info.dma_address + info.bytes > 0x40000000ull) {
        errno = ENOSPC;
        fail("pool size");
    }
    pool = mmap(NULL, info.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, 0);
    if (pool == MAP_FAILED) fail("mmap pool");
    win = pool + window_offset;
    pool_address = (uint32_t)(info.dma_address + window_offset);
    saved = malloc(out_bytes);
    if (!saved) fail("output buffer");

    phase("registers");
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) fail("/dev/mem");
    shell = map_registers(mem_fd, shell_base);
    if (rd(shell, S_ID) != SHELL_ID) { errno = ENODEV; fail("shell id"); }
    // abort anything left over, clear the faults and keep the engine held while its window is written
    if (!hold(C_SOFT_RESET | C_CLEAR, &status)) { dump_shell("hold"); errno = EIO; fail("engine did not quiesce"); }

    phase("image");
    memset(win, 0, window);
    memcpy(win, image, image_bytes);
    __sync_synchronize();
    if (memcmp(win, image, image_bytes)) { errno = EIO; fail("window image readback"); }
    wr(shell, S_BASE, pool_address);
    if (rd(shell, S_BASE) != pool_address) { errno = EIO; fail("base register readback"); }
    wr(shell, S_WINDOW, window);
    wr(shell, S_DEADLINE, deadline);
    wr(shell, S_WDT, WDT_CYCLES);
    if (rd(shell, S_WINDOW) != window || rd(shell, S_DEADLINE) != deadline) { errno = EIO; fail("shell register readback"); }
    // release with the heartbeat watchdog on (L0-H3-02): a runner that stops beating leaves the engine held
    wr(shell, S_CTRL, C_WDT);
    if (!wait_status(ST_ISOLATED | ST_RESET, 0, 100000000ull, &status)) { dump_shell("release"); errno = EIO; fail("release"); }
    if (rd(shell, E_ID) != ENGINE_ID) { errno = ENODEV; fail("engine id"); }
    wr(shell, E_PROG, 0);
    wr(shell, E_DDR, ddr_offset);
    if (rd(shell, E_PROG) != 0 || rd(shell, E_DDR) != ddr_offset) { errno = EIO; fail("engine register readback"); }

    thermal_setup();
    phase("run");
    for (r = 0; r < warmup + repeat; r++) {
        unsigned long bad = 0;
        uint64_t elapsed_run = 0;
        for (f = 0; f < frames; f++) {
            uint64_t start, elapsed, polls = 0;
            uint32_t dones = rd(shell, S_DONES), faults;
            thermal_pause(r);
            memcpy(win + in_offset, input + f * in_frame, in_frame);
            __sync_synchronize();
            start = now_ns();
            wr(shell, S_HEARTBEAT, 1);
            wr(shell, E_CTRL, 1);
            for (;;) {
                status = rd(shell, S_STATUS);
                if (!(status & ST_RUNNING) && (status & ST_DONE) && rd(shell, S_DONES) == dones + 1) break;
                if (status & ST_ISOLATED) {
                    dump_shell("aborted");
                    fprintf(stderr, "F_ASTRA_ENGINE_ABORTED run=%lu frame=%lu\n", r, f);
                    hold(0, &status);
                    return 3;
                }
                if ((++polls & 1023) == 0) {
                    wr(shell, S_HEARTBEAT, 1);
                    if (now_ns() - start > (uint64_t)timeout_ms * 1000000ull) {
                        dump_shell("timeout");
                        fprintf(stderr, "F_ASTRA_ENGINE_TIMEOUT run=%lu frame=%lu pc=%u\n", r, f, rd(shell, E_PC));
                        hold(0, &status);
                        return 3;
                    }
                }
            }
            elapsed = now_ns() - start;
            __sync_synchronize();
            faults = rd(shell, S_FAULTS);
            if (faults) {
                faults_seen |= faults;
                faulted++;
                dump_shell("fault");
            }
            cycles = rd(shell, E_CYC_LO) | ((uint64_t)rd(shell, E_CYC_HI) << 32);
            if (r >= warmup) {
                times[n++] = elapsed;
                cycles_sum += cycles;
                if (cycles < cycles_min) cycles_min = cycles;
                if (cycles > cycles_max) cycles_max = cycles;
            }
            elapsed_run += elapsed;
            if (memcmp(win + out_offset, expected + f * out_frame, out_frame)) bad++;
            if (r == warmup) memcpy(saved + f * out_frame, win + out_offset, out_frame);
            fprintf(stderr, "F_ASTRA_ENGINE_RUNNER_FRAME run=%lu frame=%lu ns=%llu cycles=%llu faults=0x%x bad=%lu\n", r, f,
                    (unsigned long long)elapsed, (unsigned long long)cycles, faults, bad);
        }
        if (r == warmup) {
            FILE *o = fopen(output_path, "wb");
            if (!o || fwrite(saved, 1, out_bytes, o) != out_bytes || fclose(o)) fail(output_path);
        }
        if (r >= warmup) total += elapsed_run;
        mismatched_frames += bad;
        mismatched_runs += bad != 0;
    }
    // the program and the weights are never written by a run (the arena between them is)
    image_intact = !memcmp(win, image, ddr_offset) && !memcmp(win + weight_offset, image + weight_offset, image_bytes - weight_offset);
    {
        uint32_t final_faults = rd(shell, S_FAULTS), runs = rd(shell, S_RUNS), dones = rd(shell, S_DONES);
        uint32_t max_cycles = rd(shell, S_MAX), cfg0 = rd(shell, E_CFG0), cfg1 = rd(shell, E_CFG1), cfg2 = rd(shell, E_CFG2);
        double mean_frame_s = (double)total / n / 1e9;
        int held = hold(0, &status);
        memcpy(sorted, times, n * sizeof(times[0]));
        qsort(sorted, n, sizeof(sorted[0]), cmp_u64);
        printf("{\"schema\":\"f-astra/moderntcn-engine-run/v1\",\"track\":\"experimental: unified ModernTCN engine, not a FINN "
               "DirectRTL result\",\"frames\":%lu,\"warmup\":%lu,\"repeat\":%lu,\"input_bytes\":%zu,\"output_bytes\":%zu,"
               "\"image_bytes\":%zu,\"window_bytes\":%u,\"pool_dma_address\":%llu,\"window_offset\":%u,\"shell_base\":%u,"
               "\"engine_config\":[%u,%u,%u],"
               "\"ops_per_frame\":%.0f,\"deadline_cycles\":%u,\"mean_frame_ns\":%.1f,\"min_frame_ns\":%llu,"
               "\"median_frame_ns\":%llu,\"max_frame_ns\":%llu,\"mean_run_ns\":%.1f,\"gops_mean\":%.6f,"
               "\"frames_per_second_mean\":%.4f,\"engine_cycles_min\":%llu,\"engine_cycles_max\":%llu,"
               "\"engine_cycles_mean\":%.1f,\"shell_max_cycles\":%u,\"mismatched_runs\":%lu,\"mismatched_frames\":%lu,"
               "\"faulted_frames\":%lu,\"faults_seen\":%u,\"final_faults\":%u,\"shell_runs\":%u,\"shell_dones\":%u,"
               "\"image_intact\":%s,\"held_after\":%s,\"thermal_pauses\":%lu,\"thermal_pause_s\":%.1f,"
               "\"timer\":\"CLOCK_MONOTONIC_RAW around START, completion polling and heartbeats of every frame\","
               "\"times_ns\":[",
               frames, warmup, repeat, in_bytes, out_bytes, image_bytes, window, (unsigned long long)info.dma_address,
               window_offset, shell_base, cfg0, cfg1, cfg2, ops_per_frame, deadline, (double)total / n, (unsigned long long)sorted[0],
               (unsigned long long)sorted[n / 2], (unsigned long long)sorted[n - 1], (double)total / repeat,
               ops_per_frame / mean_frame_s / 1e9, 1.0 / mean_frame_s, (unsigned long long)cycles_min,
               (unsigned long long)cycles_max, (double)cycles_sum / n, max_cycles, mismatched_runs, mismatched_frames,
               faulted, faults_seen, final_faults, runs, dones, image_intact ? "true" : "false", held ? "true" : "false",
               pauses, pause_s);
        for (i = 0; i < n; i++) printf("%s%llu", i ? "," : "", (unsigned long long)times[i]);
        printf("]}\n");
        if (faulted || final_faults) return 5;
    }
    return (mismatched_runs || !image_intact) ? 4 : 0;
}
