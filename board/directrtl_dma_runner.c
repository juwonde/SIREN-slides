// F-ASTRA DirectRTL board runner for the complete harness (directrtl_harness.py):
// AXI DMA (simple mode) at 0x40400000 feeds the FINN stream design, the framing
// block at GPIO 0x41200000 ends the output transfer after a programmed beat count.
// Input and output live in the private coherent pool of f_astra_finn_dma_pool.
// The timer covers register programming, the whole-corpus transfer and completion
// polling; it excludes loading files and comparing outputs.
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

#define DMA_BASE 0x40400000u
#define GPIO_BASE 0x41200000u
#define APERTURE 0x10000u
#include "f_astra_pl_io.h"          // rd, wr, map_registers: /dev/mem or the secure monitor
enum { MM2S_CR = 0x00, MM2S_SR = 0x04, MM2S_SA = 0x18, MM2S_LEN = 0x28,
       S2MM_CR = 0x30, S2MM_SR = 0x34, S2MM_DA = 0x48, S2MM_LEN = 0x58 };
enum { SR_HALTED = 0x1, SR_IDLE = 0x2, SR_ERRORS = 0x70, SR_IOC = 0x1000, SR_ERR_IRQ = 0x4000 };
#define MAX_REPEAT 1000

static volatile uint32_t *dma, *gpio;

static void fail(const char *what)
{
    fprintf(stderr, "F_ASTRA_DMA_RUNNER_FAILED %s errno=%d\n", what, errno);
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

static int reset_dma(void)
{
    uint64_t start = now_ns();
    wr(dma, MM2S_CR, 4);
    while (rd(dma, MM2S_CR) & 4)
        if (now_ns() - start > 100000000ull) return 0;
    return (rd(dma, MM2S_SR) & SR_HALTED) && (rd(dma, S2MM_SR) & SR_HALTED);
}

// One whole-corpus transfer. Returns elapsed ns, or 0 on timeout/error (status in *mm2s, *s2mm).
static uint64_t transfer(uint32_t in_addr, uint32_t in_bytes, uint32_t out_addr, uint32_t out_bytes,
                         uint32_t beats, uint64_t timeout_ns, uint32_t *mm2s, uint32_t *s2mm)
{
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
    }
    end = now_ns();
    wr(dma, MM2S_SR, SR_IOC | SR_ERR_IRQ);
    wr(dma, S2MM_SR, SR_IOC | SR_ERR_IRQ);
    return end - start;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    const char *pool_path, *input_path, *expected_path, *output_path;
    unsigned long frames, warmup, repeat, beat_bytes, timeout_ms;
    double ops_per_frame;
    size_t in_bytes, out_bytes;
    uint8_t *input, *expected, *pool;
    struct pool_info info;
    int pool_fd, mem_fd;
    uint64_t times[MAX_REPEAT], sorted[MAX_REPEAT], total = 0;
    unsigned long mismatched_runs = 0, mismatched_frames = 0, r;
    uint32_t mm2s = 0, s2mm = 0, in_addr, out_addr, beats;

    if (argc == 3 && !strcmp(argv[1], "--quiesce")) {
        mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (mem_fd < 0) fail("/dev/mem");
        dma = map_registers(mem_fd, DMA_BASE);
        printf("{\"schema\":\"f-astra/directrtl-dma-quiesce/v1\",\"halted\":%s,\"mm2s_sr\":%u,\"s2mm_sr\":%u}\n",
               reset_dma() ? "true" : "false", rd(dma, MM2S_SR), rd(dma, S2MM_SR));
        return 0;
    }
    if (argc != 11) {
        fprintf(stderr, "usage: %s POOL INPUT EXPECTED OUTPUT FRAMES BEAT_BYTES OPS_PER_FRAME WARMUP REPEAT TIMEOUT_MS\n"
                        "       %s --quiesce now\n", argv[0], argv[0]);
        return 64;
    }
    pool_path = argv[1]; input_path = argv[2]; expected_path = argv[3]; output_path = argv[4];
    frames = strtoul(argv[5], NULL, 10); beat_bytes = strtoul(argv[6], NULL, 10);
    ops_per_frame = strtod(argv[7], NULL); warmup = strtoul(argv[8], NULL, 10);
    repeat = strtoul(argv[9], NULL, 10); timeout_ms = strtoul(argv[10], NULL, 10);
    if (!frames || !beat_bytes || ops_per_frame <= 0 || !repeat || repeat > MAX_REPEAT || !timeout_ms) {
        errno = EINVAL;
        fail("arguments");
    }
    input = load(input_path, &in_bytes);
    expected = load(expected_path, &out_bytes);
    if (in_bytes % frames || out_bytes % frames || out_bytes % beat_bytes) { errno = EINVAL; fail("sizes"); }
    beats = (uint32_t)(out_bytes / beat_bytes);

    pool_fd = open(pool_path, O_RDWR);
    if (pool_fd < 0 || ioctl(pool_fd, POOL_INFO, &info)) fail(pool_path);
    if (in_bytes > info.output_offset || out_bytes > info.bytes - info.output_offset || in_bytes >= (1u << 23)
        || info.dma_address + info.bytes > 0x40000000ull) { errno = ENOSPC; fail("pool size"); }
    pool = mmap(NULL, info.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, 0);
    if (pool == MAP_FAILED) fail("mmap pool");
    memcpy(pool, input, in_bytes);
    if (memcmp(pool, input, in_bytes)) { errno = EIO; fail("pool input readback"); }
    in_addr = (uint32_t)info.dma_address;
    out_addr = (uint32_t)(info.dma_address + info.output_offset);

    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) fail("/dev/mem");
    dma = map_registers(mem_fd, DMA_BASE);
    gpio = map_registers(mem_fd, GPIO_BASE);
    if (!reset_dma()) { errno = EIO; fail("dma reset"); }
    if (rd(gpio, 0x8) != 0) { errno = EIO; fail("framing counter not idle"); }

    for (r = 0; r < warmup + repeat; r++) {
        uint8_t *out = pool + info.output_offset;
        uint64_t elapsed;
        unsigned long f, bad = 0, frame_bytes = out_bytes / frames;
        memset(out, 0xA5, out_bytes);
        elapsed = transfer(in_addr, (uint32_t)in_bytes, out_addr, (uint32_t)out_bytes, beats,
                           (uint64_t)timeout_ms * 1000000ull, &mm2s, &s2mm);
        if (!elapsed) {
            int halted = reset_dma();
            fprintf(stderr, "F_ASTRA_DMA_TRANSFER_FAILED run=%lu mm2s_sr=%u s2mm_sr=%u halted_after_reset=%d\n",
                    r, mm2s, s2mm, halted);
            return 3;
        }
        if (rd(dma, S2MM_LEN) != out_bytes) { errno = EIO; fail("S2MM received length"); }
        for (f = 0; f < frames; f++)
            bad += memcmp(out + f * frame_bytes, expected + f * frame_bytes, frame_bytes) != 0;
        if (r == warmup) {
            FILE *o = fopen(output_path, "wb");
            if (!o || fwrite(out, 1, out_bytes, o) != out_bytes || fclose(o)) fail(output_path);
        }
        if (r >= warmup) {
            times[r - warmup] = elapsed;
            total += elapsed;
            mismatched_frames += bad;
            mismatched_runs += bad != 0;
        } else if (bad) {
            mismatched_runs++;
            mismatched_frames += bad;
        }
    }
    if (!reset_dma()) { errno = EIO; fail("final dma reset"); }
    memcpy(sorted, times, repeat * sizeof(times[0]));
    qsort(sorted, repeat, sizeof(sorted[0]), cmp_u64);
    {
        double mean_s = (double)total / repeat / 1e9;
        printf("{\"schema\":\"f-astra/directrtl-dma-run/v1\",\"frames\":%lu,\"input_bytes\":%zu,\"output_bytes\":%zu,"
               "\"output_beats\":%u,\"pool_dma_address\":%llu,\"warmup\":%lu,\"repeat\":%lu,"
               "\"ops_per_frame\":%.0f,\"mean_ns\":%.1f,\"min_ns\":%llu,\"median_ns\":%llu,\"max_ns\":%llu,"
               "\"gops_mean\":%.6f,\"frames_per_second_mean\":%.3f,"
               "\"mismatched_runs\":%lu,\"mismatched_frames\":%lu,\"final_mm2s_sr\":%u,\"final_s2mm_sr\":%u,"
               "\"timer\":\"CLOCK_MONOTONIC_RAW around register programming, whole-corpus DMA and completion polling\","
               "\"times_ns\":[",
               frames, in_bytes, out_bytes, beats, (unsigned long long)info.dma_address, warmup, repeat,
               ops_per_frame, (double)total / repeat, (unsigned long long)sorted[0],
               (unsigned long long)sorted[repeat / 2], (unsigned long long)sorted[repeat - 1],
               frames * ops_per_frame / mean_s / 1e9, frames / mean_s,
               mismatched_runs, mismatched_frames, rd(dma, MM2S_SR), rd(dma, S2MM_SR));
        for (r = 0; r < repeat; r++) printf("%s%llu", r ? "," : "", (unsigned long long)times[r]);
        printf("]}\n");
    }
    return mismatched_runs ? 4 : 0;
}
