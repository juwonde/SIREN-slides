// F-ASTRA board runner for one model of the multi-model harness (directrtl_harness.route_models): the model's
// AXI DMA (simple mode) and framing/FM-guard GPIO at the given bases. A model with DDR masters (FINN loop weights,
// intermediate frames, external weights) gets its DDR region just below the pool's output area: the runner copies
// the weight image there, clears the rest and writes the region's bus address into the model's base GPIO, which
// f_astra_axi_rebase adds to every DDR address. The corpus runs in chunks (an AXI DMA transfer stays below 8 MB);
// the timer covers register programming, the DMA transfers and completion polling of every chunk, not the copies
// of input chunks into the pool or the comparisons. Every transfer is timed on the global timer as well (f_astra_gt.h,
// 11/04: image r23's clock_gettime moves in 18.4 us steps); RUN_TIMER=clock or the secure monitor: clock_gettime only.
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
#include "f_astra_gt.h"             // the fine timer
enum { MM2S_CR = 0x00, MM2S_SR = 0x04, MM2S_SA = 0x18, MM2S_LEN = 0x28,
       S2MM_CR = 0x30, S2MM_SR = 0x34, S2MM_DA = 0x48, S2MM_LEN = 0x58 };
enum { SR_HALTED = 0x1, SR_IDLE = 0x2, SR_ERRORS = 0x70, SR_IOC = 0x1000, SR_ERR_IRQ = 0x4000 };
#define MAX_REPEAT 1000
#define DMA_LIMIT (1u << 23)

static volatile uint32_t *dma, *gpio, *base_gpio, *guard;

static void fail(const char *what)
{
    fprintf(stderr, "F_ASTRA_MODELS_RUNNER_FAILED %s errno=%d\n", what, errno);
    exit(2);
}

// Progress on stderr (unbuffered): a runner that dies on a signal still shows how far it got.
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
    fprintf(stderr, "F_ASTRA_MODELS_RUNNER_PHASE %s %ld.%02ld\n", what, (long)up.tv_sec, (long)(up.tv_nsec / 10000000));
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// Thermal stop (environment THERMAL_PACE_HIGH_C and THERMAL_PACE_LOW_C, both set): before every chunk a die at or above
// the high mark waits until it is below the low mark; every chunk is timed alone, so a pause changes no measurement.
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
    fprintf(stderr, "F_ASTRA_MODELS_THERMAL_STOP run=%lu\n", run);
    while ((t = die_temp_c()) >= pace_low) {
        struct timespec rest = {0, 200000000};
        nanosleep(&rest, NULL);
    }
    pause_s += (double)(now_ns() - start) / 1e9;
    fprintf(stderr, "F_ASTRA_MODELS_THERMAL_RESUME run=%lu temp_c=%.2f\n", run, t);
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

static int reset_dma(void)
{
    uint64_t start = now_ns();
    wr(dma, MM2S_CR, 4);
    while (rd(dma, MM2S_CR) & 4)
        if (now_ns() - start > 100000000ull) return 0;
    return (rd(dma, MM2S_SR) & SR_HALTED) && (rd(dma, S2MM_SR) & SR_HALTED);
}

// Polling sleep (environment MODELS_POLL_SLEEP_US, 0..100000): the PS sleeps that long between DMA status reads
// instead of reading back to back (a 35 ms BUS transfer kept a PS core and the AXI GP port busy; 12 V samples near the
// 5 W limit during BUS, 10/28); the completion is seen at most that much later.
static struct timespec poll_sleep;
static unsigned long fine_transfers, coarse_transfers;   // transfers timed on the global timer / on clock_gettime

// the global timer unless RUN_TIMER=clock or the registers go through the secure monitor
static void fine_timer(int mem_fd)
{
    const char *rt = getenv("RUN_TIMER");
    if (rt && *rt && strcmp(rt, "clock") && strcmp(rt, "global")) { errno = EINVAL; fail("RUN_TIMER global or clock"); }
    if (!(rt && !strcmp(rt, "clock")) && !pl_io_secure_monitor() && gt_cpu_khz())
        gt_start(map_registers(mem_fd, GT_PAGE));
}

static uint64_t transfer(uint32_t in_addr, uint32_t in_bytes, uint32_t out_addr, uint32_t out_bytes,
                         uint32_t beats, uint64_t timeout_ns, uint32_t *mm2s, uint32_t *s2mm)
{
    unsigned long khz = gt ? gt_cpu_khz() : 0;
    uint64_t c0 = gt_read(), fine;
    uint64_t start = now_ns(), end;
    wr(gpio, 0x0, beats);
    wr(dma, S2MM_CR, 1);
    wr(dma, MM2S_CR, 1);
    wr(dma, S2MM_DA, out_addr);
    wr(dma, S2MM_LEN, out_bytes);
    wr(dma, MM2S_SA, in_addr);
    wr(dma, MM2S_LEN, in_bytes);
    for (;;) {
        *mm2s = rd(dma, MM2S_SR);
        *s2mm = rd(dma, S2MM_SR);
        if ((*mm2s | *s2mm) & SR_ERRORS) return 0;
        if ((*mm2s & SR_IOC) && (*s2mm & SR_IOC)) break;
        if (now_ns() - start > timeout_ns) return 0;
        if (poll_sleep.tv_nsec) nanosleep(&poll_sleep, NULL);
    }
    end = now_ns();
    fine = gt_ns(c0, gt_read(), khz);
    wr(dma, MM2S_SR, SR_IOC | SR_ERR_IRQ);
    wr(dma, S2MM_SR, SR_IOC | SR_ERR_IRQ);
    if (fine) fine_transfers++;
    else coarse_transfers++;
    return fine ? fine : end - start;
}

// Overlapped repeats: waits for the IOC of one channel (MM2S_SR or S2MM_SR), the polling sleep between status reads;
// 0 on an error bit of either channel or the timeout.
static int wait_ioc(uint32_t reg, uint64_t start, uint64_t timeout_ns, uint32_t *mm2s, uint32_t *s2mm)
{
    for (;;) {
        *mm2s = rd(dma, MM2S_SR);
        *s2mm = rd(dma, S2MM_SR);
        if ((*mm2s | *s2mm) & SR_ERRORS) return 0;
        if ((reg == MM2S_SR ? *mm2s : *s2mm) & SR_IOC) return 1;
        if (now_ns() - start > timeout_ns) return 0;
        if (poll_sleep.tv_nsec) nanosleep(&poll_sleep, NULL);
    }
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// Signed element k of a frame of little-endian 64-bit words, ``per_word`` elements of ``bits`` bits a word.
static int64_t get_element(const uint8_t *frame, unsigned bits, unsigned per_word, unsigned k)
{
    uint64_t w, v;
    memcpy(&w, frame + (size_t)(k / per_word) * 8, 8);
    v = (w >> ((k % per_word) * bits)) & ((1ull << bits) - 1);
    return (v >> (bits - 1)) ? (int64_t)v - (int64_t)(1ull << bits) : (int64_t)v;
}

static void set_element(uint8_t *frame, unsigned bits, unsigned per_word, unsigned k, int64_t value)
{
    uint64_t w, mask = ((1ull << bits) - 1) << ((k % per_word) * bits);
    memcpy(&w, frame + (size_t)(k / per_word) * 8, 8);
    w = (w & ~mask) | (((uint64_t)value << ((k % per_word) * bits)) & mask);
    memcpy(frame + (size_t)(k / per_word) * 8, &w, 8);
}

// --sequence: the real-time path of a recurrent model (the GRU predictor): one frame a DMA transfer, and the PS
// writes elements of each output back into the next input frame. FEEDBACK is
// IN_BITS:IN_PER_WORD:WORD_BYTES:IN_FIRST:COUNT:OUT_BITS:OUT_FIRST (input codes IN_FIRST.. of frame t + 1 <- output
// elements OUT_FIRST.. of frame t, signed; 64-bit words). Every output is compared with the expected trajectory; the
// timer of a step covers its register programming, the transfer and completion polling (the loop time also the
// copies and the feedback).
static int run_sequence(char **argv)
{
    const char *pool_path = argv[2], *input_path = argv[3], *expected_path = argv[4], *output_path = argv[5];
    unsigned long steps = strtoul(argv[6], NULL, 10), beat_bytes = strtoul(argv[7], NULL, 10);
    unsigned long timeout_ms = strtoul(argv[8], NULL, 10), t, bad = 0, k;
    uint32_t dma_base = (uint32_t)strtoul(argv[9], NULL, 0), gpio_base = (uint32_t)strtoul(argv[10], NULL, 0);
    uint32_t mm2s = 0, s2mm = 0, in_addr, out_addr;
    unsigned in_bits, in_per, word_bytes, in_first, count, out_bits, out_first;
    long first_bad = -1;
    size_t in_bytes, out_bytes, in_frame, out_frame;
    uint8_t *input, *expected, *pool, *saved, *in, *out;
    uint64_t *lat, total = 0, loop_start, loop_ns;
    struct pool_info info;
    int pool_fd, mem_fd;

    if (sscanf(argv[11], "%u:%u:%u:%u:%u:%u:%u", &in_bits, &in_per, &word_bytes, &in_first, &count, &out_bits,
               &out_first) != 7 || word_bytes != 8 || !in_bits || !in_per || in_bits * in_per > 64 || !out_bits
        || out_bits > 32 || 64 % out_bits || !count || !steps || !beat_bytes || !timeout_ms) {
        errno = EINVAL;
        fail("sequence arguments");
    }
    phase("load");
    input = load(input_path, &in_bytes);
    expected = load(expected_path, &out_bytes);
    if (in_bytes % steps || out_bytes % steps) { errno = EINVAL; fail("sizes"); }
    in_frame = in_bytes / steps; out_frame = out_bytes / steps;
    if (in_frame % 8 || out_frame % beat_bytes || (in_first + count) > in_frame / 8 * in_per
        || (out_first + count) > out_frame / 8 * (64 / out_bits)) { errno = EINVAL; fail("frame layout"); }
    phase("pool");
    pool_fd = strncmp(pool_path, "fd:", 3) ? open(pool_path, O_RDWR) : atoi(pool_path + 3);
    if (pool_fd < 0 || ioctl(pool_fd, POOL_INFO, &info)) fail(pool_path);
    if (in_frame > info.output_offset || out_frame > info.bytes - info.output_offset
        || info.dma_address + info.bytes > 0x40000000ull) { errno = ENOSPC; fail("pool size"); }
    pool = mmap(NULL, info.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, 0);
    if (pool == MAP_FAILED) fail("mmap pool");
    in = pool; out = pool + info.output_offset;
    in_addr = (uint32_t)info.dma_address;
    out_addr = (uint32_t)(info.dma_address + info.output_offset);
    saved = malloc(out_bytes);
    lat = malloc(steps * sizeof(*lat));
    if (!saved || !lat) fail("buffers");
    phase("registers");
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) fail("/dev/mem");
    dma = map_registers(mem_fd, dma_base);
    gpio = map_registers(mem_fd, gpio_base);
    fine_timer(mem_fd);
    if (!reset_dma()) { errno = EIO; fail("dma reset"); }
    if (rd(gpio, 0x8) != 0) { errno = EIO; fail("framing counter not idle"); }
    thermal_setup();
    phase("sequence");
    loop_start = now_ns();
    for (t = 0; t < steps; t++) {
        uint64_t elapsed;
        if (t % 1024 == 0) thermal_pause(t);
        memcpy(in, input + t * in_frame, in_frame);
        if (t > 0)
            for (k = 0; k < count; k++)
                set_element(in, in_bits, in_per, in_first + (unsigned)k,
                            get_element(saved + (t - 1) * out_frame, out_bits, 64 / out_bits, out_first + (unsigned)k));
        memset(out, 0xA5, out_frame);
        elapsed = transfer(in_addr, (uint32_t)in_frame, out_addr, (uint32_t)out_frame, (uint32_t)(out_frame / beat_bytes),
                           (uint64_t)timeout_ms * 1000000ull, &mm2s, &s2mm);
        if (!elapsed) {
            int halted = reset_dma();
            fprintf(stderr, "F_ASTRA_DMA_TRANSFER_FAILED step=%lu mm2s_sr=%u s2mm_sr=%u halted_after_reset=%d\n", t, mm2s,
                    s2mm, halted);
            return 3;
        }
        if (rd(dma, S2MM_LEN) != out_frame) { errno = EIO; fail("S2MM received length"); }
        memcpy(saved + t * out_frame, out, out_frame);
        lat[t] = elapsed;
        total += elapsed;
        if (memcmp(saved + t * out_frame, expected + t * out_frame, out_frame)) {
            bad++;
            if (first_bad < 0) first_bad = (long)t;
        }
    }
    loop_ns = now_ns() - loop_start;
    if (!reset_dma()) { errno = EIO; fail("final dma reset"); }
    {
        FILE *o = fopen(output_path, "wb");
        if (!o || fwrite(saved, 1, out_bytes, o) != out_bytes || fclose(o)) fail(output_path);
    }
    qsort(lat, steps, sizeof(*lat), cmp_u64);
    printf("{\"schema\":\"f-astra/directrtl-sequence-run/v1\",\"steps\":%lu,\"in_frame_bytes\":%zu,\"out_frame_bytes\":%zu,"
           "\"dma_base\":%u,\"gpio_base\":%u,\"feedback\":\"%s\",\"mismatched_steps\":%lu,\"first_mismatch\":%ld,"
           "\"transfer_mean_ns\":%.1f,\"transfer_min_ns\":%llu,\"transfer_median_ns\":%llu,\"transfer_p99_ns\":%llu,"
           "\"transfer_max_ns\":%llu,\"loop_ns_per_step\":%.1f,\"thermal_pauses\":%lu,\"thermal_pause_s\":%.1f,"
           "\"final_mm2s_sr\":%u,\"final_s2mm_sr\":%u,\"fine_transfers\":%lu,\"coarse_transfers\":%lu,"
           "\"timer\":\"per step around register programming, the DMA transfer and completion polling (the global timer, "
           "else CLOCK_MONOTONIC_RAW: fine_transfers / coarse_transfers); loop_ns_per_step (CLOCK_MONOTONIC_RAW) also "
           "covers the frame copy and the feedback\"}\n",
           steps, in_frame, out_frame, dma_base, gpio_base, argv[11], bad, first_bad, (double)total / steps,
           (unsigned long long)lat[0], (unsigned long long)lat[steps / 2], (unsigned long long)lat[steps - 1 - steps / 100],
           (unsigned long long)lat[steps - 1], (double)loop_ns / steps, pauses, pause_s, rd(dma, MM2S_SR), rd(dma, S2MM_SR),
           fine_transfers, coarse_transfers);
    return bad ? 4 : 0;
}

int main(int argc, char **argv)
{
    const char *pool_path, *input_path, *expected_path, *output_path, *image_path;
    unsigned long frames, warmup, repeat, beat_bytes, timeout_ms, chunk_frames, r;
    unsigned long mismatched_runs = 0, mismatched_frames = 0;
    uint32_t dma_base, gpio_base, ddr_base_gpio, ddr_bytes, guard_base, mm2s = 0, s2mm = 0;
    double ops_per_frame;
    size_t in_bytes, out_bytes, image_bytes = 0, in_frame, out_frame;
    uint8_t *input, *expected, *image = NULL, *pool, *saved;
    struct pool_info info;
    int pool_fd, mem_fd;
    uint64_t times[MAX_REPEAT], sorted[MAX_REPEAT], starts[MAX_REPEAT], total = 0;
    uint32_t in_addr, out_addr, ddr_addr;

    if (argc == 3 && !strcmp(argv[1], "--quiesce")) {
        mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mem_fd < 0) fail("/dev/mem");
        dma = map_registers(mem_fd, (uint32_t)strtoul(argv[2], NULL, 0));
        printf("{\"schema\":\"f-astra/directrtl-dma-quiesce/v1\",\"dma_base\":%lu,\"halted\":%s,\"mm2s_sr\":%u,\"s2mm_sr\":%u}\n",
               strtoul(argv[2], NULL, 0), reset_dma() ? "true" : "false", rd(dma, MM2S_SR), rd(dma, S2MM_SR));
        return 0;
    }
    if (argc == 12 && !strcmp(argv[1], "--sequence"))
        return run_sequence(argv);
    if (argc != 18) {
        fprintf(stderr, "usage: %s POOL INPUT EXPECTED OUTPUT FRAMES BEAT_BYTES OPS_PER_FRAME WARMUP REPEAT TIMEOUT_MS "
                        "DMA_BASE GPIO_BASE DDR_IMAGE|- DDR_BASE_GPIO DDR_BYTES CHUNK_FRAMES GUARD_BASE\n"
                        "       %s --sequence POOL INPUT EXPECTED OUTPUT STEPS BEAT_BYTES TIMEOUT_MS DMA_BASE GPIO_BASE "
                        "IN_BITS:IN_PER_WORD:8:IN_FIRST:COUNT:OUT_BITS:OUT_FIRST\n"
                        "       %s --quiesce DMA_BASE\n", argv[0], argv[0], argv[0]);
        return 64;
    }
    pool_path = argv[1]; input_path = argv[2]; expected_path = argv[3]; output_path = argv[4];
    frames = strtoul(argv[5], NULL, 10); beat_bytes = strtoul(argv[6], NULL, 10);
    ops_per_frame = strtod(argv[7], NULL); warmup = strtoul(argv[8], NULL, 10);
    repeat = strtoul(argv[9], NULL, 10); timeout_ms = strtoul(argv[10], NULL, 10);
    dma_base = (uint32_t)strtoul(argv[11], NULL, 0); gpio_base = (uint32_t)strtoul(argv[12], NULL, 0);
    image_path = argv[13]; ddr_base_gpio = (uint32_t)strtoul(argv[14], NULL, 0);
    ddr_bytes = (uint32_t)strtoul(argv[15], NULL, 0); chunk_frames = strtoul(argv[16], NULL, 10);
    guard_base = (uint32_t)strtoul(argv[17], NULL, 0);
    if (!frames || !beat_bytes || ops_per_frame <= 0 || !repeat || repeat > MAX_REPEAT || !timeout_ms || !chunk_frames
        || (ddr_bytes & 4095) || ((strcmp(image_path, "-") != 0) != (ddr_base_gpio != 0))) {
        errno = EINVAL;
        fail("arguments");
    }
    phase("load");
    input = load(input_path, &in_bytes);
    expected = load(expected_path, &out_bytes);
    if (in_bytes % frames || out_bytes % frames || (out_bytes / frames) % beat_bytes) { errno = EINVAL; fail("sizes"); }
    in_frame = in_bytes / frames; out_frame = out_bytes / frames;
    if (chunk_frames > frames) chunk_frames = frames;
    if (strcmp(image_path, "-")) {
        image = load(image_path, &image_bytes);
        if (image_bytes > ddr_bytes) { errno = EINVAL; fail("DDR image larger than the DDR region"); }
    }

    phase("pool");
    // "fd:N": the pool descriptor the session opened once (a concurrent session shares the single-open pool)
    pool_fd = strncmp(pool_path, "fd:", 3) ? open(pool_path, O_RDWR) : atoi(pool_path + 3);
    if (pool_fd < 0 || ioctl(pool_fd, POOL_INFO, &info)) fail(pool_path);
    if (ddr_bytes + chunk_frames * in_frame > info.output_offset || chunk_frames * out_frame > info.bytes - info.output_offset
        || chunk_frames * in_frame >= DMA_LIMIT || chunk_frames * out_frame >= DMA_LIMIT
        || info.dma_address + info.bytes > 0x40000000ull) { errno = ENOSPC; fail("pool size"); }
    pool = mmap(NULL, info.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, 0);
    if (pool == MAP_FAILED) fail("mmap pool");
    // inputs at the pool start (where the FM service also puts its batches), the DDR region just below the outputs
    ddr_addr = (uint32_t)(info.dma_address + info.output_offset - ddr_bytes);
    in_addr = (uint32_t)info.dma_address;
    out_addr = (uint32_t)(info.dma_address + info.output_offset);
    saved = malloc(out_bytes);
    if (!saved) fail("output buffer");

    phase("registers");
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) fail("/dev/mem");
    dma = map_registers(mem_fd, dma_base);
    gpio = map_registers(mem_fd, gpio_base);
    fine_timer(mem_fd);
    if (!reset_dma()) { errno = EIO; fail("dma reset"); }
    if (rd(gpio, 0x8) != 0) { errno = EIO; fail("framing counter not idle"); }
    if (image) {
        uint8_t *ddr = pool + info.output_offset - ddr_bytes;
        phase("ddr_image");
        memset(ddr, 0, ddr_bytes);
        memcpy(ddr, image, image_bytes);
        if (memcmp(ddr, image, image_bytes)) { errno = EIO; fail("DDR image readback"); }
        base_gpio = map_registers(mem_fd, ddr_base_gpio);
        wr(base_gpio, 0x0, ddr_addr);
        if (rd(base_gpio, 0x0) != ddr_addr) { errno = EIO; fail("DDR base register readback"); }
        // FINN's weight fetchers prefetch as soon as the accelerator leaves reset, i.e. at bitstream load, when the
        // base was still 0: a soft reset through the model's FM guard (16 cycles) restarts them with the base in place
        phase("accelerator_reset");
        guard = map_registers(mem_fd, guard_base);
        wr(guard, 0x04, 0x100);
        usleep(1000);
        if (rd(guard, 0x08) & (1u << 9)) { errno = EIO; fail("accelerator still held in reset"); }
    }

    thermal_setup();
    if (getenv("MODELS_POLL_SLEEP_US") && *getenv("MODELS_POLL_SLEEP_US")) {
        long us = strtol(getenv("MODELS_POLL_SLEEP_US"), NULL, 10);
        if (us < 0 || us > 100000) { errno = EINVAL; fail("MODELS_POLL_SLEEP_US 0..100000"); }
        poll_sleep.tv_sec = us / 1000000; poll_sleep.tv_nsec = (us % 1000000) * 1000;
    }
    // static input (environment MODELS_STATIC_INPUT, a corpus of one chunk): the input goes into the pool once; every
    // repeat streams the same bytes (the accelerator only reads them), so the PS copy (1.9 MB of uncached writes for
    // the BUS corpus, milliseconds) no longer leaves the PL idle between repeats, each restart a load step that rang
    // the 12 V input
    int static_input = getenv("MODELS_STATIC_INPUT") && *getenv("MODELS_STATIC_INPUT") && chunk_frames >= frames;
    if (getenv("MODELS_STATIC_INPUT") && *getenv("MODELS_STATIC_INPUT") && !static_input) {
        errno = EINVAL;
        fail("MODELS_STATIC_INPUT needs the corpus in one chunk");
    }
    if (static_input) {
        memcpy(pool, input, (size_t)frames * in_frame);
        __sync_synchronize();
        if (memcmp(pool, input, (size_t)frames * in_frame)) { errno = EIO; fail("static input readback"); }
    }
    // overlapped repeats (environment MODELS_OVERLAP, static input only): the next repeat's input DMA starts as soon as
    // the current one's input has gone out, so the accelerator never drains between repeats (the restart load step at
    // every repeat boundary was the BUS 12 V peak, a 0.22 s beat of the 36 ms repeats in the 100 Hz samples, 10/28).
    // The output is compared and its DMA armed again after each repeat; the stream waits on the DMA meanwhile and the
    // framing counts on (TLAST every transfer's beats, no rewrite). A repeat's time runs from the previous completion.
    int overlap = getenv("MODELS_OVERLAP") && *getenv("MODELS_OVERLAP");
    if (overlap && !static_input) { errno = EINVAL; fail("MODELS_OVERLAP needs MODELS_STATIC_INPUT"); }
    phase("run");
    if (overlap) {
        uint8_t *out = pool + info.output_offset;
        uint32_t cin = (uint32_t)(frames * in_frame), cout = (uint32_t)(frames * out_frame);
        uint64_t timeout_ns = (uint64_t)timeout_ms * 1000000ull, t_prev;
        memset(out, 0xA5, cout);
        __sync_synchronize();
        wr(gpio, 0x0, (uint32_t)(cout / beat_bytes));
        wr(dma, S2MM_CR, 1);
        wr(dma, MM2S_CR, 1);
        wr(dma, S2MM_DA, out_addr);
        wr(dma, S2MM_LEN, cout);
        t_prev = now_ns();
        wr(dma, MM2S_SA, in_addr);
        wr(dma, MM2S_LEN, cin);
        for (r = 0; r < warmup + repeat; r++) {
            uint64_t run_start = t_prev, t;
            unsigned long f, bad = 0;
            int last = r + 1 == warmup + repeat;
            fprintf(stderr, "F_ASTRA_MODELS_RUNNER_CHUNK run=%lu start=0 frames=%lu overlap=1\n", r, frames);
            if (!wait_ioc(MM2S_SR, t_prev, timeout_ns, &mm2s, &s2mm)) break;
            wr(dma, MM2S_SR, SR_IOC | SR_ERR_IRQ);
            if (!last) {
                thermal_pause(r);
                wr(dma, MM2S_SA, in_addr);
                wr(dma, MM2S_LEN, cin);
            }
            if (!wait_ioc(S2MM_SR, t_prev, timeout_ns, &mm2s, &s2mm)) break;
            t = now_ns();
            wr(dma, S2MM_SR, SR_IOC | SR_ERR_IRQ);
            if (rd(dma, S2MM_LEN) != cout) { errno = EIO; fail("S2MM received length"); }
            for (f = 0; f < frames; f++) bad += memcmp(out + f * out_frame, expected + f * out_frame, out_frame) != 0;
            if (r == warmup) {
                FILE *o = fopen(output_path, "wb");
                if (!o || fwrite(out, 1, out_bytes, o) != out_bytes || fclose(o)) fail(output_path);
            }
            if (!last) {
                memset(out, 0xA5, cout);
                __sync_synchronize();
                wr(dma, S2MM_DA, out_addr);
                wr(dma, S2MM_LEN, cout);
            }
            if (r >= warmup) {
                starts[r - warmup] = run_start;
                times[r - warmup] = t - t_prev;
                total += t - t_prev;
            }
            t_prev = t;
            mismatched_frames += bad;
            mismatched_runs += bad != 0;
        }
        if (r < warmup + repeat) {
            int halted = reset_dma();
            fprintf(stderr, "F_ASTRA_DMA_TRANSFER_FAILED run=%lu frame=0 mm2s_sr=%u s2mm_sr=%u halted_after_reset=%d\n",
                    r, mm2s, s2mm, halted);
            return 3;
        }
    }
    for (r = 0; !overlap && r < warmup + repeat; r++) {
        uint64_t elapsed_run = 0, run_start = now_ns();
        unsigned long start, bad = 0;
        for (start = 0; start < frames; start += chunk_frames) {
            unsigned long n = frames - start < chunk_frames ? frames - start : chunk_frames, f;
            uint8_t *out = pool + info.output_offset;
            uint32_t cin = (uint32_t)(n * in_frame), cout = (uint32_t)(n * out_frame);
            uint64_t elapsed;
            fprintf(stderr, "F_ASTRA_MODELS_RUNNER_CHUNK run=%lu start=%lu frames=%lu\n", r, start, n);
            thermal_pause(r);
            if (!static_input) memcpy(pool, input + start * in_frame, cin);
            memset(out, 0xA5, cout);
            elapsed = transfer(in_addr, cin, out_addr, cout, (uint32_t)(cout / beat_bytes), (uint64_t)timeout_ms * 1000000ull,
                               &mm2s, &s2mm);
            if (!elapsed) {
                int halted = reset_dma();
                fprintf(stderr, "F_ASTRA_DMA_TRANSFER_FAILED run=%lu frame=%lu mm2s_sr=%u s2mm_sr=%u halted_after_reset=%d\n",
                        r, start, mm2s, s2mm, halted);
                return 3;
            }
            if (rd(dma, S2MM_LEN) != cout) { errno = EIO; fail("S2MM received length"); }
            elapsed_run += elapsed;
            for (f = 0; f < n; f++)
                bad += memcmp(out + f * out_frame, expected + (start + f) * out_frame, out_frame) != 0;
            if (r == warmup) memcpy(saved + start * out_frame, out, cout);
        }
        if (r == warmup) {
            FILE *o = fopen(output_path, "wb");
            if (!o || fwrite(saved, 1, out_bytes, o) != out_bytes || fclose(o)) fail(output_path);
        }
        if (r >= warmup) {
            starts[r - warmup] = run_start;      // CLOCK_MONOTONIC_RAW at the repeat's start (power-trace alignment)
            times[r - warmup] = elapsed_run;
            total += elapsed_run;
        }
        mismatched_frames += bad;
        mismatched_runs += bad != 0;
    }
    if (!reset_dma()) { errno = EIO; fail("final dma reset"); }
    // diagnosis: a small DDR region (weights, parameters, intermediate frames) as the accelerator left it
    if (image && ddr_bytes <= (1u << 20)) {
        FILE *dump = fopen("ddr-after.bin", "wb");
        if (!dump || fwrite(pool + info.output_offset - ddr_bytes, 1, ddr_bytes, dump) != ddr_bytes) fail("ddr-after.bin");
        fclose(dump);
    }
    memcpy(sorted, times, repeat * sizeof(times[0]));
    qsort(sorted, repeat, sizeof(sorted[0]), cmp_u64);
    {
        double mean_s = (double)total / repeat / 1e9;
        printf("{\"schema\":\"f-astra/directrtl-models-run/v1\",\"frames\":%lu,\"chunk_frames\":%lu,\"input_bytes\":%zu,"
               "\"output_bytes\":%zu,\"dma_base\":%u,\"gpio_base\":%u,\"ddr_image_bytes\":%zu,\"ddr_region_bytes\":%u,"
               "\"pool_dma_address\":%llu,\"warmup\":%lu,\"repeat\":%lu,\"ops_per_frame\":%.0f,\"mean_ns\":%.1f,"
               "\"min_ns\":%llu,\"median_ns\":%llu,\"max_ns\":%llu,\"gops_mean\":%.6f,\"frames_per_second_mean\":%.3f,"
               "\"mismatched_runs\":%lu,\"mismatched_frames\":%lu,\"final_mm2s_sr\":%u,\"final_s2mm_sr\":%u,"
               "\"thermal_pauses\":%lu,\"thermal_pause_s\":%.1f,\"static_input\":%s,\"fine_transfers\":%lu,"
               "\"coarse_transfers\":%lu,\"timer\":\"around register programming, every chunk DMA and completion polling: "
               "the global timer, else CLOCK_MONOTONIC_RAW (fine_transfers / coarse_transfers)\",\"times_ns\":[",
               frames, chunk_frames, in_bytes, out_bytes, dma_base, gpio_base, image_bytes, ddr_bytes,
               (unsigned long long)info.dma_address, warmup, repeat, ops_per_frame, (double)total / repeat,
               (unsigned long long)sorted[0], (unsigned long long)sorted[repeat / 2], (unsigned long long)sorted[repeat - 1],
               frames * ops_per_frame / mean_s / 1e9, frames / mean_s, mismatched_runs, mismatched_frames,
               rd(dma, MM2S_SR), rd(dma, S2MM_SR), pauses, pause_s, static_input ? "true" : "false", fine_transfers,
               coarse_transfers);
        for (r = 0; r < repeat; r++) printf("%s%llu", r ? "," : "", (unsigned long long)times[r]);
        printf("],\"starts_ns\":[");
        for (r = 0; r < repeat; r++) printf("%s%llu", r ? "," : "", (unsigned long long)starts[r]);
        printf("]}\n");
    }
    return mismatched_runs ? 4 : 0;
}
