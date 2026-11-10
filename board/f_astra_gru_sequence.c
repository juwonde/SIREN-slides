// F-ASTRA board runner of the SIREN Model 3 predictor's real-time path (I5-b, docs/gru-50gops-design.md): one GRU
// step per run of the shared engine's G4 program and the FINN tail behind the aligner, the PS feeding the tail's new
// hidden codes back into the next step. Per step t it writes the engine row [h(t-1) on lanes 0..H-1, x(t) on lanes
// X_LANE..X_LANE+NX-1] and the hidden row h(t-1) into the window (row 0 of a G4 program of ROWS rows; the other rows
// stay zero and their tail outputs are not used), starts the tail's DMA (hidden codes in, [h', indices] out) and the
// engine as the hybrid runner's concurrent step does, waits for the engine and both DMA channels, and takes row 0 of
// the output: h(t) = its first H codes (fed back), the indices = the next C codes. The outputs of every step are
// written to OUT_PREFIX-output.bin (int16 LE) and compared with EXPECTED (the integer reference of the same day, from
// the zero state; the run goes on through a mismatch, so the board's own feedback decides every later step); the
// indices go to OUT_PREFIX-index.i32 (int32 LE, the encoder's --index). The x codes come from XCODES (int16 LE,
// STEPS x NX). Timing per step covers the row writes, the run and the output read (CLOCK_MONOTONIC_RAW).
// Exit 0 all steps equal, 2 setup failure, 3 timeout or abort, 4 mismatch.
//   f-astra-gru-sequence POOL IMAGE XCODES EXPECTED OUT_PREFIX STEPS SHELL_BASE DMA_BASE GPIO_BASE WINDOW_BYTES
//       DDR_OFFSET IN_OFFSET HIDDEN_OFFSET OUT_OFFSET WEIGHT_OFFSET DEADLINE_CYCLES TIMEOUT_MS WINDOW_OFFSET
//       ROWS:IN_CHANNELS:X_LANE:NX:HIDDEN:CHANNELS:BEAT_BYTES
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
enum { E_CTRL = 0x00, E_STATUS = 0x04, E_PROG = 0x08, E_DDR = 0x0C, E_CYC_LO = 0x10, E_CYC_HI = 0x14, E_ID = 0x1C };
enum { S_ID = 0x80, S_CTRL = 0x84, S_STATUS = 0x88, S_FAULTS = 0x8C, S_DEADLINE = 0x90, S_WDT = 0x94, S_HEARTBEAT = 0x98,
       S_WINDOW = 0x9C, S_DONES = 0xA4, S_BASE = 0xC8 };
enum { ST_RUNNING = 1, ST_DONE = 2, ST_ISOLATED = 4, ST_RESET = 8, ST_DRAINED = 16 };
enum { C_HOLD = 1, C_WDT = 4, C_SOFT_RESET = 0x100, C_CLEAR = 0x200 };
enum { MM2S_CR = 0x00, MM2S_SR = 0x04, MM2S_SA = 0x18, MM2S_LEN = 0x28,
       S2MM_CR = 0x30, S2MM_SR = 0x34, S2MM_DA = 0x48, S2MM_LEN = 0x58 };
enum { SR_HALTED = 0x1, SR_ERRORS = 0x70, SR_IOC = 0x1000, SR_ERR_IRQ = 0x4000 };
#define SHELL_ID 0x4D545348u
#define ENGINE_ID 0x4D54434Eu
#define WDT_CYCLES (1u << 27)
#define MAX_LANES 256
#define MAX_OUT 256

static volatile uint32_t *shell, *dma, *gpio;
static uint32_t rd(volatile uint32_t *base, unsigned off) { return base[off / 4]; }
static void wr(volatile uint32_t *base, unsigned off, uint32_t v) { base[off / 4] = v; }

static void fail(const char *what)
{
    fprintf(stderr, "F_ASTRA_GRU_SEQUENCE_FAILED %s errno=%d\n", what, errno);
    exit(2);
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
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

static void *map_registers(int fd, uint32_t base)
{
    void *p = mmap(NULL, APERTURE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (p == MAP_FAILED) fail("mmap registers");
    return p;
}

static int wait_status(uint32_t mask, uint32_t want, uint64_t limit_ns, uint32_t *status)
{
    uint64_t start = now_ns();
    for (;;) {
        *status = rd(shell, S_STATUS);
        if ((*status & mask) == want) return 1;
        if (now_ns() - start > limit_ns) return 0;
    }
}

static int hold(uint32_t extra, uint32_t *status)
{
    wr(shell, S_CTRL, C_HOLD | extra);
    return wait_status(ST_ISOLATED | ST_RESET | ST_DRAINED, ST_ISOLATED | ST_RESET | ST_DRAINED, 1000000000ull, status);
}

static int reset_dma(void)
{
    uint64_t start = now_ns();
    wr(dma, MM2S_CR, 4);
    while (rd(dma, MM2S_CR) & 4)
        if (now_ns() - start > 100000000ull) return 0;
    return (rd(dma, MM2S_SR) & SR_HALTED) && (rd(dma, S2MM_SR) & SR_HALTED);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    const char *pool_path, *image_path, *x_path, *expected_path, *prefix;
    unsigned long steps, t, mismatched = 0;
    long first_mismatch = -1;
    uint32_t shell_base, dma_base, gpio_base, window, ddr_offset, in_offset, hidden_offset, out_offset, weight_offset;
    uint32_t deadline, timeout_ms, window_offset, rows, in_channels, x_lane, nx, hidden, channels, beat, status;
    size_t image_bytes, x_bytes, expected_bytes, k;
    uint8_t *image, *xcodes, *expected, *pool, *win;
    int16_t h[MAX_LANES], row[MAX_LANES], out[MAX_OUT];
    uint64_t *times, total = 0, cycles = 0;
    struct pool_info info;
    int pool_fd, mem_fd;
    FILE *output_file, *index_file;
    char name[512];

    if (argc != 20) {
        fprintf(stderr, "usage: %s POOL IMAGE XCODES EXPECTED OUT_PREFIX STEPS SHELL_BASE DMA_BASE GPIO_BASE WINDOW_BYTES "
                        "DDR_OFFSET IN_OFFSET HIDDEN_OFFSET OUT_OFFSET WEIGHT_OFFSET DEADLINE_CYCLES TIMEOUT_MS "
                        "WINDOW_OFFSET ROWS:IN_CHANNELS:X_LANE:NX:HIDDEN:CHANNELS:BEAT_BYTES\n", argv[0]);
        return 64;
    }
    pool_path = argv[1]; image_path = argv[2]; x_path = argv[3]; expected_path = argv[4]; prefix = argv[5];
    steps = strtoul(argv[6], NULL, 10);
    shell_base = (uint32_t)strtoul(argv[7], NULL, 0); dma_base = (uint32_t)strtoul(argv[8], NULL, 0);
    gpio_base = (uint32_t)strtoul(argv[9], NULL, 0); window = (uint32_t)strtoul(argv[10], NULL, 0);
    ddr_offset = (uint32_t)strtoul(argv[11], NULL, 0); in_offset = (uint32_t)strtoul(argv[12], NULL, 0);
    hidden_offset = (uint32_t)strtoul(argv[13], NULL, 0); out_offset = (uint32_t)strtoul(argv[14], NULL, 0);
    weight_offset = (uint32_t)strtoul(argv[15], NULL, 0); deadline = (uint32_t)strtoul(argv[16], NULL, 0);
    timeout_ms = (uint32_t)strtoul(argv[17], NULL, 0); window_offset = (uint32_t)strtoul(argv[18], NULL, 0);
    if (sscanf(argv[19], "%u:%u:%u:%u:%u:%u:%u", &rows, &in_channels, &x_lane, &nx, &hidden, &channels, &beat) != 7) {
        errno = EINVAL;
        fail("geometry");
    }
    if (!steps || !rows || !timeout_ms || !window || (window & 4095) || (ddr_offset & 4095) || (window_offset & 4095)
        || in_channels > MAX_LANES || x_lane + nx > in_channels || hidden > x_lane || hidden + channels > MAX_OUT || !beat
        || (rows * (hidden + channels) * 2) % beat || in_offset + (uint64_t)rows * in_channels * 2 > hidden_offset
        || hidden_offset + (uint64_t)rows * hidden * 2 > out_offset || out_offset + (uint64_t)rows * (hidden + channels) * 2 > window
        || weight_offset < ddr_offset || weight_offset > window || hidden_offset < ddr_offset || out_offset < ddr_offset) {
        errno = EINVAL;
        fail("arguments");
    }
    image = load(image_path, &image_bytes);
    xcodes = load(x_path, &x_bytes);
    expected = load(expected_path, &expected_bytes);
    if (image_bytes > window || image_bytes < weight_offset || x_bytes != steps * nx * 2
        || expected_bytes != steps * (hidden + channels) * 2) { errno = EINVAL; fail("sizes"); }
    times = calloc(steps, sizeof(*times));
    if (!times) fail("times");

    pool_fd = strncmp(pool_path, "fd:", 3) ? open(pool_path, O_RDWR) : atoi(pool_path + 3);
    if (pool_fd < 0 || ioctl(pool_fd, POOL_INFO, &info)) fail(pool_path);
    if ((uint64_t)window_offset + window > info.output_offset || (info.dma_address & 4095)
        || info.dma_address + info.bytes > 0x40000000ull) { errno = ENOSPC; fail("pool size"); }
    pool = mmap(NULL, info.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, 0);
    if (pool == MAP_FAILED) fail("mmap pool");
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) fail("/dev/mem");
    shell = map_registers(mem_fd, shell_base);
    dma = map_registers(mem_fd, dma_base);
    gpio = map_registers(mem_fd, gpio_base);
    if (rd(shell, S_ID) != SHELL_ID) { errno = ENODEV; fail("shell id"); }
    if (!hold(C_SOFT_RESET | C_CLEAR, &status)) { errno = EIO; fail("engine did not quiesce"); }
    if (!reset_dma()) { errno = EIO; fail("dma reset"); }
    if (rd(gpio, 0x8) != 0) { errno = EIO; fail("framing counter not idle"); }

    // the window: the G4 program and weights, the input and hidden rows zero (rows 1.. stay zero)
    win = pool + window_offset;
    memset(win, 0, window);
    memcpy(win, image, image_bytes);
    memset(win + in_offset, 0, (size_t)rows * in_channels * 2);
    memset(win + hidden_offset, 0, (size_t)rows * hidden * 2);
    __sync_synchronize();
    if (memcmp(win, image, image_bytes < in_offset ? image_bytes : in_offset)) { errno = EIO; fail("window readback"); }
    wr(shell, S_BASE, (uint32_t)(info.dma_address + window_offset));
    wr(shell, S_WINDOW, window);
    wr(shell, S_DEADLINE, deadline);
    wr(shell, S_WDT, WDT_CYCLES);
    wr(shell, S_CTRL, C_WDT);
    if (!wait_status(ST_ISOLATED | ST_RESET, 0, 100000000ull, &status)) { errno = EIO; fail("release"); }
    if (rd(shell, E_ID) != ENGINE_ID) { errno = ENODEV; fail("engine id"); }
    wr(shell, E_DDR, ddr_offset);

    snprintf(name, sizeof(name), "%s-output.bin", prefix);
    output_file = fopen(name, "wb");
    snprintf(name, sizeof(name), "%s-index.i32", prefix);
    index_file = fopen(name, "wb");
    if (!output_file || !index_file) fail("output files");
    memset(h, 0, sizeof(h));                                         // the zero state
    memset(row, 0, sizeof(row));
    {
        uint32_t base = (uint32_t)(info.dma_address + window_offset) + ddr_offset;
        uint32_t out_bytes = rows * (hidden + channels) * 2, hidden_bytes = rows * hidden * 2;
        uint32_t dones = rd(shell, S_DONES);
        for (t = 0; t < steps; t++) {
            uint64_t start = now_ns(), limit = (uint64_t)timeout_ms * 1000000ull;
            int engine_done = 0, dma_done = 0;
            // row 0: [h(t-1), x(t)] and the hidden row h(t-1)
            memcpy(row, h, hidden * 2);
            memcpy(row + x_lane, xcodes + (size_t)t * nx * 2, nx * 2);
            memcpy(win + in_offset, row, in_channels * 2);
            memcpy(win + hidden_offset, h, hidden * 2);
            __sync_synchronize();
            // the tail's DMA first (as the hybrid runner's concurrent step), then the engine feeding it
            wr(gpio, 0x0, out_bytes / beat);
            wr(dma, S2MM_CR, 1);
            wr(dma, MM2S_CR, 1);
            wr(dma, S2MM_DA, base + (out_offset - ddr_offset));
            wr(dma, S2MM_LEN, out_bytes);
            wr(dma, MM2S_SA, base + (hidden_offset - ddr_offset));
            wr(dma, MM2S_LEN, hidden_bytes);
            wr(shell, S_BASE, (uint32_t)(info.dma_address + window_offset));
            wr(shell, E_PROG, 0);
            wr(shell, S_HEARTBEAT, 1);
            wr(shell, E_CTRL, 1);
            while (!(engine_done && dma_done)) {
                if (!engine_done) {
                    status = rd(shell, S_STATUS);
                    if (status & ST_ISOLATED) {
                        fprintf(stderr, "F_ASTRA_GRU_SEQUENCE_ENGINE_ABORTED step=%lu faults=0x%x\n", t, rd(shell, S_FAULTS));
                        hold(0, &status);
                        return 3;
                    }
                    if (!(status & ST_RUNNING) && (status & ST_DONE) && rd(shell, S_DONES) == dones + 1) {
                        dones++;
                        engine_done = 1;
                        cycles += rd(shell, E_CYC_LO) | ((uint64_t)rd(shell, E_CYC_HI) << 32);
                    }
                }
                if (!dma_done) {
                    uint32_t mm2s = rd(dma, MM2S_SR), s2mm = rd(dma, S2MM_SR);
                    if ((mm2s | s2mm) & SR_ERRORS) {
                        fprintf(stderr, "F_ASTRA_GRU_SEQUENCE_DMA_FAILED step=%lu mm2s_sr=%u s2mm_sr=%u\n", t, mm2s, s2mm);
                        reset_dma();
                        hold(0, &status);
                        return 3;
                    }
                    if ((mm2s & SR_IOC) && (s2mm & SR_IOC)) {
                        wr(dma, MM2S_SR, SR_IOC | SR_ERR_IRQ);
                        wr(dma, S2MM_SR, SR_IOC | SR_ERR_IRQ);
                        dma_done = 1;
                    }
                }
                if (now_ns() - start > limit) {
                    fprintf(stderr, "F_ASTRA_GRU_SEQUENCE_TIMEOUT step=%lu engine=%d dma=%d\n", t, engine_done, dma_done);
                    reset_dma();
                    hold(0, &status);
                    return 3;
                }
            }
            __sync_synchronize();
            memcpy(out, win + out_offset, (hidden + channels) * 2);   // row 0
            memcpy(h, out, hidden * 2);                               // fed back into the next step
            times[t] = now_ns() - start;
            total += times[t];
            if (memcmp(out, expected + (size_t)t * (hidden + channels) * 2, (hidden + channels) * 2)) {
                if (first_mismatch < 0) first_mismatch = (long)t;
                mismatched++;
            }
            if (fwrite(out, 2, hidden + channels, output_file) != hidden + channels) fail("output write");
            for (k = 0; k < channels; k++) {
                int32_t index = out[hidden + k];
                if (fwrite(&index, 4, 1, index_file) != 1) fail("index write");
            }
        }
    }
    if (fclose(output_file) || fclose(index_file)) fail("output close");
    hold(0, &status);
    qsort(times, steps, sizeof(*times), cmp_u64);
    printf("{\"schema\":\"f-astra/gru-sequence-run/v1\",\"steps\":%lu,\"rows_per_run\":%u,\"mismatched_steps\":%lu,"
           "\"first_mismatch\":%ld,\"step_ns_min\":%llu,\"step_ns_median\":%llu,\"step_ns_p99\":%llu,\"step_ns_max\":%llu,"
           "\"step_ns_mean\":%.1f,\"total_s\":%.4f,\"engine_cycles_mean\":%.1f,"
           "\"timer\":\"CLOCK_MONOTONIC_RAW per step: row writes, DMA and engine start, completion polling, output read\"}\n",
           steps, rows, mismatched, first_mismatch, (unsigned long long)times[0], (unsigned long long)times[steps / 2],
           (unsigned long long)times[(steps * 99) / 100], (unsigned long long)times[steps - 1], (double)total / steps,
           (double)total / 1e9, (double)cycles / steps);
    return mismatched ? 4 : 0;
}
