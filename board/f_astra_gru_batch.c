// F-ASTRA board runner of the SIREN Model 3 predictor in segments (weekly NN window): every day is cut into segments
// of L steps whose hidden state starts at zero (gru_predictor.segmented, an inference optimization: the packets stay
// within 0.01 %), and all segments of all days are the ROWS rows of one G4 program, so one run of the shared engine's
// G4 program and the FINN tail behind the aligner computes one step of every segment at the engine's batched rate.
// LAYOUT=window (11/04, gru_predictor.windowed; the per-orbit NN, whose 47 segments an orbit left the engine's fixed
// cost of a run larger than its rows): one row per step instead, every step's prediction the last step of its own
// window of the L steps up to it (from the day's first step for the first L - 1), the hidden state zero at the window
// start; run k computes step t - L + 1 + k of row t (a row idles in a run before its window, its hidden state zero).
// Per step: the island's clock ramps in (SOFT_START, the hybrid runner's dithered ease-out; with ISLAND_DUTY the PWM
// duty itself ramps and the PS sleeps between its writes) after the engine starts, the PS sleeps (QUIET_US from the
// start) and then polls with POLL_US naps (the PS idle while the engine finishes: the 12 V input must not see the PS
// on top of the engine at either end of a run) for the engine and both tail DMA channels, then (THREADS PS threads)
// takes each row's [h', indices]:
// h' goes into the row's engine input lanes and hidden row for the next step together with the next x codes, the
// indices and outputs are kept. Every run's rate (valid row-steps x OPS_PER_STEP over the time from the engine start to
// the last DMA completion) is reported. After the last step every day's outputs (int16 [steps][H + C]) are compared
// with its expected file and written with its indices (int32, the encoder's --index) as OUT_PREFIX-NAME-output.bin and
// OUT_PREFIX-NAME-index.i32, with OUT_PREFIX-NAME.json (the sequence result record); the run record goes to stdout.
// A day without its expected file (gexpected-NAME.bin absent: a mission bundle whose expected outputs do not fit the
// board, directrtl_weekly.comp_batch_days) is written unchecked ("checked":false); the session checks the written
// files against the host's hashes after the runs (gbatch-check.sha256).
// IMAGE is HEAD:WEIGHTS, the window image without its zero middle: HEAD goes to the window's start (the program),
// WEIGHTS to WEIGHT_OFFSET. ROWS may exceed the segments (a G4 program's rows are a multiple of 4): the rest stay zero.
// Delta inputs (DELTA in the geometry, gru_predictor.extend_delta): after the NX x codes of a step the row carries
// x(t) - x(t-1) of the first DELTA codes (0 at the day's first step; a segment start keeps them: only h resets).
// Context inputs (CTX:SHIFT:FRAC after DELTA = 0, gru_predictor source.context; 11/02): after the NX x codes the row
// carries, per code k < CTX, the day's integer moving average of x_k formed here once per day before the runs:
// E(0) = x(0) 2^FRAC, E(t) = E(t-1) + floor((x(t) 2^FRAC - E(t-1)) / 2^SHIFT), code rne(E / 2^FRAC). It crosses the
// segment starts (only h resets), so the segments keep the day's history.
// Throughput share of the engine island (the engine runs at it, its rate and switching follow): ISLAND_DUTY=D (1..255)
// writes D to the clock gate register's second channel (route_models island_pwm: the island's clock runs on D/256 of
// its cycles, the PS sleeps), or DITHER_SHARE=S (0 < S < 1, a bitstream without it) has the PS dither the island's
// enable bit in time (on time = S x elapsed, the DMA status ends the run) through every run.
// Exit 0 all equal, 2 setup failure, 3 timeout or abort, 4 mismatch.
//   f-astra-gru-batch POOL HEAD:WEIGHTS OUT_PREFIX L SHELL_BASE DMA_BASE GPIO_BASE WINDOW_BYTES DDR_OFFSET IN_OFFSET
//       HIDDEN_OFFSET OUT_OFFSET WEIGHT_OFFSET DEADLINE_CYCLES TIMEOUT_MS WINDOW_OFFSET OPS_PER_STEP
//       ROWS:IN_CHANNELS:X_LANE:NX:HIDDEN:CHANNELS:BEAT_BYTES[:DELTA[:CTX:SHIFT:FRAC]] NAME:STEPS...
//       (files gxcodes-NAME.bin, NX codes
//       a step, gexpected-NAME.bin)
//   environment: THREADS (1 or 2, default 2), GATES=ADDR:RUN:ENGINE (clock gate register, the run mask held on through
//   the whole run as the hybrid runner's GATE_KEEP_ENGINE=2, the engine bits), SOFT_START=RAMP_US:MASK:FROM (MASK inside
//   ENGINE, switched only during a ramp; every shell access happens with RUN on), QUIET_US (the PS sleeps that long from
//   an engine start before it polls), ISLAND_DUTY or DITHER_SHARE (with GATES and SOFT_START's MASK as the island),
//   ZIP_MODE=ADDR:VALUE (11/02: the run-time form of the GRU aligner, 1 sign-free for a 4-bit grid program, 0 plain;
//   written and read back before the first run),
//   FEED_GOVERNOR (a cpufreq governor, e.g. performance: the PS clock of the feeds only; the governor found is written
//   back before every engine start, so the PS sleeps through the runs at the clock found, and at exit),
//   FEED_GATES=MASK (with GATES: the clock gates during the feeds, e.g. 0: the islands are idle while the PS writes the
//   next rows; RUN again before any shell access. 11/03: RUN open through the feeds kept 0.59 W under the runs' 10 ms
//   samples, the Comp batch's 5.04-5.52 W), LAYOUT (segments, the default, or window: the rows above),
//   KEEP=1 (11/04, an engine with G4 weights resident, entry r03: every run starts with CTRL bit 1, so a tile whose
//   weights its lane-memory bank already holds skips the fill), PRELOAD=HEAD:ROWS (with KEEP: before the runs, the
//   head image HEAD, the program with its row count cut to ROWS, gru_engine.preload_program_g4, runs once with keep and
//   tags the banks; its outputs are not kept and its time is not a run's: the Comp model's weight load of a power-on),
//   CHUNKS=FILE:N (11/04, with KEEP; the user's 8-bit gates: a run of all ROWS rows kept the engine busy about 5 ms, one
//   100 Hz sample of the 12 V input above 5 W with 8-bit gates): every run goes as chunks of N rows (the last one the
//   rest), FILE the chunks' program heads (gru_engine.chunk_programs_g4: the program with its row count N and its input
//   rows from row c N on, the weights where they are), each chunk's outputs taken and its next rows written right after
//   it (the rows are independent), so the engine runs a chunk at a time with the PS's feed in between; a chunk is a
//   computation of its own: chunk_gops_min / chunk_gops_max over the chunks, run_gops over a run's chunks' times),
//   RT_PRIORITY=P (1..99: the runs and feeds at SCHED_FIFO priority P, back to SCHED_OTHER before the outputs; 11/04:
//   one chunk of 54 took 155 us more than the others with the same engine cycles, the polling thread preempted),
//   RUN_CPU=N (11/04, 6.210.0: the runner pinned to CPU N from the runs on; the GIC sends every device interrupt to CPU 0),
//   CHUNK_TRACE=1 (every run's (chunk's) global timer marks into the log after the runs: its start, the poll that saw
//   the engine done and the one that saw both DMA channels done, its end, the polls, its engine cycles and CPU, and
//   /proc/interrupts and /proc/softirqs before and after the runs, and the local timer's count at its start; the times
//   of the runs are the same without it),
//   TICK_GUARD_US=G (11/04, 6.210.0, with RUN_CPU and the global timer: the chunk trace showed one chunk a session
//   130-240 us long with the same engine cycles, no device interrupt and no context switch during the runs, only the
//   kernel's tick on the runner's CPU (twd, the Cortex-A9 private timer, HZ 100); before a run starts the PS reads its
//   CPU's private timer and, when the tick is due within G us, waits until it has fired and been set again, so the
//   tick lands in the feed instead of the run; the waits are counted, tick_waits and tick_wait_ns).
// 6.210.0: a chunked run reads the CPU clock once at its start and checks it after its last chunk (no sysfs read
// around every chunk), FEED_GOVERNOR equal to the governor found writes nothing; a run's timeout is checked on the global timer (clock_gettime is a system call of a few us at the PS's
// 333 MHz, once before the run now, no longer every poll), the engine cycles are read and the DMA status cleared after
// the run's end, and a chunk after a run's first one starts with START alone (window, program and heartbeat hold).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
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
#define MAX_DAYS 16
#define DMA_LIMIT (1u << 23)

static volatile uint32_t *shell, *dma, *gpio, *gates;
static uint32_t gate_now;
static uint32_t rd(volatile uint32_t *base, unsigned off) { return base[off / 4]; }
static void wr(volatile uint32_t *base, unsigned off, uint32_t v) { base[off / 4] = v; }
static uint32_t gate_run;
static void gate_write(uint32_t m) { if (gates && m != gate_now) { gates[0] = m; gate_now = m; } }
static void gates_restore(void) { if (gates) gate_write(gate_run); }
static unsigned island_duty;                    // ISLAND_DUTY: the island PWM's duty while the runner runs
#define RAMP_STEP_NS 250000ull                  // the duty ramp's write period (ISLAND_DUTY soft start)
#define POLL_NS_DEFAULT 200000ull               // POLL_US default: the PS's nap between polls after the quiet sleep
static void duty_off(void) { if (gates && island_duty) gates[2] = 0; }
#define GOVERNOR "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
static char governor_found[32];                 // FEED_GOVERNOR: the governor found, back before every run
static const char *feed_governor;
static int feed_gates_set;                      // FEED_GATES: the gate mask during the feeds
static uint32_t feed_gates;
static void fail(const char *what);
static int feed_switch;                         // FEED_GOVERNOR differs from the governor found: written around feeds
static void governor_write(const char *g)
{
    FILE *f = fopen(GOVERNOR, "w");
    if (!f || fputs(g, f) < 0 || fclose(f)) fail("FEED_GOVERNOR write");
}
static void governor_restore(void)
{
    FILE *f = feed_governor ? fopen(GOVERNOR, "w") : NULL;
    if (f) { fputs(governor_found, f); fclose(f); }
}

static void fail(const char *what)
{
    fprintf(stderr, "F_ASTRA_GRU_BATCH_FAILED %s errno=%d\n", what, errno);
    exit(2);
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// The run timer (11/04, f_astra_gt.h): every run also on the Cortex-A9 global timer (clock_gettime moves in 18.4 us
// steps on image r23); RUN_TIMER=clock: clock_gettime only. A run whose CPU clock changed keeps the clock_gettime time
// and is counted in gt_changed.
#include "f_astra_gt.h"

// the phase's start on the session's timeline clock (/proc/uptime = CLOCK_BOOTTIME): the runner's setup, runs
// and output work can be told apart in the session's timeline.txt
static void phase(const char *what)
{
    struct timespec up;
#ifdef CLOCK_BOOTTIME
    clock_gettime(CLOCK_BOOTTIME, &up);
#else
    clock_gettime(CLOCK_MONOTONIC, &up);
#endif
    fprintf(stderr, "F_ASTRA_GRU_BATCH_PHASE %s %ld.%02ld\n", what, (long)up.tv_sec, (long)(up.tv_nsec / 10000000));
}

// CHUNK_TRACE: a /proc file into the log (before and after the runs: the interrupts that came in between)
static void trace_file(const char *path, const char *when)
{
    char line[1024];
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) fprintf(stderr, "F_ASTRA_GRU_BATCH_PROC %s %s %s", when, path, line);
    fclose(f);
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

// the rows: day d's segment s is row first[d] + s; its step k is the day's step s * L + k (LAYOUT=window: row
// first[d] + t is step t's window, its step k the day's step t - L + 1 + k)
static unsigned days, L, rows, used, nx, nd, nk, kshift, kfrac, hidden, channels, in_channels, x_lane,
                first_row[MAX_DAYS + 1];
static int window_layout;
static uint8_t *preload;                        // PRELOAD: the head image of the weight preload, and its rows
static uint32_t preload_rows;
static uint64_t preload_ns, preload_cycles;
static uint32_t keep_ctrl = 1;                  // E_CTRL at a run's start: START, with KEEP=1 also bit 1 (keep)
static uint8_t *chunk_heads;                    // CHUNKS: the chunk_count program heads, chunk_rows rows each (last: the rest)
static unsigned chunk_rows, chunk_count;
struct chunk_mark { uint64_t t0, engine, dma, t1, cycles; uint32_t polls, khz, twd; int cpu; };
enum { TWD_COUNT = 0x604, TWD_CTRL = 0x608 };   // this CPU's private timer (the kernel's tick) on the GT page
static uint64_t tick_guard_us, tick_wait_ns;    // TICK_GUARD_US: the guard, the PS's time waiting for ticks
static unsigned long tick_waits;
static struct chunk_mark *marks;                // CHUNK_TRACE=1: every run's (chunk's) global timer marks
static unsigned long day_steps[MAX_DAYS];
static int16_t *xcodes[MAX_DAYS];
static int16_t *ctxcodes[MAX_DAYS];             // [steps][nk] context codes of each day (nk > 0)

// the day step row r (of day d) computes in run k, or -1 when the row idles in that run
static long row_step(unsigned d, unsigned r, unsigned k)
{
    unsigned long s = r - first_row[d];
    long t = window_layout ? (long)s - (long)(L - 1) + (long)k : (long)(s * L + k);
    return t >= 0 && (unsigned long)t < day_steps[d] ? t : -1;
}

// floor(v / 2^s) and round half to even of v / 2^s for signed v (no implementation-defined right shift)
static int32_t floor_shift(int32_t v, unsigned s)
{
    return v >= 0 ? v >> s : -(int32_t)((((uint32_t)-v) + ((1u << s) - 1)) >> s);
}

static int32_t rne_shift(int32_t v, unsigned s)
{
    int32_t q = floor_shift(v, s), r = v - q * (1 << s), half = 1 << (s - 1);
    return q + ((r > half || (r == half && (q & 1))) ? 1 : 0);
}

// the context codes of day d (gru_predictor.context_codes): the moving average over the whole day
static void context_day(unsigned d)
{
    int32_t e[MAX_LANES];
    ctxcodes[d] = malloc((size_t)day_steps[d] * nk * 2);
    if (!ctxcodes[d]) fail("context codes");
    for (unsigned long t = 0; t < day_steps[d]; t++) {
        const int16_t *x = xcodes[d] + t * nx;
        for (unsigned k = 0; k < nk; k++) {
            int32_t xs = (int32_t)x[k] * (1 << kfrac);
            e[k] = t ? e[k] + floor_shift(xs - e[k], kshift) : xs;
            ctxcodes[d][t * nk + k] = (int16_t)rne_shift(e[k], kfrac);
        }
    }
}
static uint8_t *win;
static uint32_t in_offset, hidden_offset, out_offset;
static int16_t *outputs;                       // [L][rows][hidden + channels]

// the kept outputs [h', indices] of day d's step t: its segment's run t % L, or the last run of its window
static const int16_t *step_output(unsigned d, unsigned long t)
{
    unsigned r = first_row[d] + (unsigned)(window_layout ? t : t / L), s = window_layout ? L - 1 : (unsigned)(t % L);
    return outputs + ((size_t)s * rows + r) * (hidden + channels);
}

#define FEED_BLOCK 256                          // rows a feed thread assembles in cached memory before one copy out
struct span { unsigned r0, r1, k; int16_t *stage_in, *stage_hidden; };
static int16_t *stages[2][2];                   // per feed thread: [FEED_BLOCK][IN_CHANNELS], [FEED_BLOCK][HIDDEN], zeroed

// after step k (k = -1: before the first step): every row's outputs kept, its next input row and hidden row written.
// The pool is non-cacheable: the step's outputs of the thread's rows come out in one long sequential copy into the kept
// outputs (cached), and the input and hidden rows are assembled FEED_BLOCK at a time in cached memory and copied into
// the pool in one piece each (small copies row by row on the pool took about 100 ms a step, 3x an engine run). Lanes a
// row never writes (between HIDDEN and X_LANE, beyond the x codes) stay zero in the stage as in the pool.
static void *feed(void *arg)
{
    struct span *sp = arg;
    unsigned w = hidden + channels, d = 0;
    const int16_t *kept = NULL;
    if ((int)sp->k >= 0 && sp->r1 > sp->r0) {
        memcpy(outputs + ((size_t)sp->k * rows + sp->r0) * w, win + out_offset + (size_t)sp->r0 * w * 2,
               (size_t)(sp->r1 - sp->r0) * w * 2);
        kept = outputs + (size_t)sp->k * rows * w;
    }
    for (unsigned r0 = sp->r0; r0 < sp->r1; r0 += FEED_BLOCK) {
        unsigned r1 = sp->r1 - r0 > FEED_BLOCK ? r0 + FEED_BLOCK : sp->r1;
        for (unsigned r = r0; r < r1; r++) {
            int16_t *row = sp->stage_in + (size_t)(r - r0) * in_channels;
            while (r >= first_row[d + 1]) d++;
            if (kept && row_step(d, r, sp->k) >= 0) memcpy(row, kept + (size_t)r * w, (size_t)hidden * 2);
            else memset(row, 0, (size_t)hidden * 2);                 // the zero state of every segment and window
            long t = sp->k + 1 < L ? row_step(d, r, sp->k + 1) : -1;
            if (t >= 0) {
                const int16_t *x = xcodes[d] + t * nx, *before = t ? x - nx : x;
                memcpy(row + x_lane, x, (size_t)nx * 2);
                for (unsigned j = 0; j < nd; j++) row[x_lane + nx + j] = (int16_t)(x[j] - before[j]);
                if (nk) memcpy(row + x_lane + nx, ctxcodes[d] + t * nk, (size_t)nk * 2);
            } else {
                memset(row + x_lane, 0, (size_t)(nx + nd + nk) * 2);
            }
            memcpy(sp->stage_hidden + (size_t)(r - r0) * hidden, row, (size_t)hidden * 2);
        }
        memcpy(win + in_offset + (size_t)r0 * in_channels * 2, sp->stage_in, (size_t)(r1 - r0) * in_channels * 2);
        memcpy(win + hidden_offset + (size_t)r0 * hidden * 2, sp->stage_hidden, (size_t)(r1 - r0) * hidden * 2);
    }
    return NULL;
}

// the feed of rows [r0, r1) after step k on this thread (CHUNKS: a chunk's rows right after it)
static void feed_span(unsigned r0, unsigned r1, unsigned k)
{
    struct span a = {r0, r1 < used ? r1 : used, k, stages[0][0], stages[0][1]};
    if (a.r1 > a.r0) feed(&a);
    __sync_synchronize();
}

// the valid row-steps of rows [r0, r1) in run k
static unsigned long valid_span(unsigned r0, unsigned r1, unsigned k)
{
    unsigned long n = 0;
    unsigned d = 0;
    for (unsigned r = r0; r < r1 && r < used; r++) {
        while (r >= first_row[d + 1]) d++;
        n += row_step(d, r, k) >= 0;
    }
    return n;
}

static void feed_all(unsigned k, unsigned threads)
{
    struct span a = {0, used, k, stages[0][0], stages[0][1]}, b = {0, 0, k, stages[1][0], stages[1][1]};
    pthread_t th;
    if (threads > 1) {
        a.r1 = used / 2;
        b.r0 = used / 2; b.r1 = used;
        if (pthread_create(&th, NULL, feed, &b)) fail("thread");
        feed(&a);
        pthread_join(th, NULL);
    } else {
        feed(&a);
    }
    __sync_synchronize();
}

int main(int argc, char **argv)
{
    const char *pool_path, *image_path, *prefix;
    uint32_t shell_base, dma_base, gpio_base, window, ddr_offset, weight_offset, deadline, timeout_ms, window_offset, beat;
    uint32_t status, gate_engine = 0, soft_mask = 0, soft_from = 0;
    uint64_t soft_ramp_ns = 0, quiet_ns = 0, poll_ns = POLL_NS_DEFAULT;
    double dither_share = 0.0;
    double ops_per_step;
    size_t head_bytes, weight_bytes, k;
    uint8_t *head, *weights, *pool;
    unsigned threads = 2;
    struct pool_info info;
    int pool_fd, mem_fd;
    char names[MAX_DAYS][64], path[600];
    phase("load");

    if (argc < 20) {
        fprintf(stderr, "usage: %s POOL IMAGE OUT_PREFIX L SHELL_BASE DMA_BASE GPIO_BASE WINDOW_BYTES DDR_OFFSET IN_OFFSET "
                        "HIDDEN_OFFSET OUT_OFFSET WEIGHT_OFFSET DEADLINE_CYCLES TIMEOUT_MS WINDOW_OFFSET OPS_PER_STEP "
                        "ROWS:IN_CHANNELS:X_LANE:NX:HIDDEN:CHANNELS:BEAT_BYTES NAME:STEPS...\n", argv[0]);
        return 64;
    }
    pool_path = argv[1]; image_path = argv[2]; prefix = argv[3]; L = (unsigned)strtoul(argv[4], NULL, 10);
    shell_base = (uint32_t)strtoul(argv[5], NULL, 0); dma_base = (uint32_t)strtoul(argv[6], NULL, 0);
    gpio_base = (uint32_t)strtoul(argv[7], NULL, 0); window = (uint32_t)strtoul(argv[8], NULL, 0);
    ddr_offset = (uint32_t)strtoul(argv[9], NULL, 0); in_offset = (uint32_t)strtoul(argv[10], NULL, 0);
    hidden_offset = (uint32_t)strtoul(argv[11], NULL, 0); out_offset = (uint32_t)strtoul(argv[12], NULL, 0);
    weight_offset = (uint32_t)strtoul(argv[13], NULL, 0); deadline = (uint32_t)strtoul(argv[14], NULL, 0);
    timeout_ms = (uint32_t)strtoul(argv[15], NULL, 0); window_offset = (uint32_t)strtoul(argv[16], NULL, 0);
    ops_per_step = strtod(argv[17], NULL);
    nd = nk = kshift = kfrac = 0;
    int fields = sscanf(argv[18], "%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u", &rows, &in_channels, &x_lane, &nx, &hidden,
                        &channels, &beat, &nd, &nk, &kshift, &kfrac);
    if (fields < 7 || (fields > 8 && fields != 11)
        || (nk && (nd || nk > nx || kshift > 15 || kfrac < 1 || kfrac > 15))) {
        errno = EINVAL;
        fail("geometry");
    }
    days = (unsigned)(argc - 19);
    if (days < 1 || days > MAX_DAYS) { errno = EINVAL; fail("days"); }
    first_row[0] = 0;
    for (unsigned d = 0; d < days; d++) {
        char *colon = strchr(argv[19 + d], ':');
        if (!colon || colon - argv[19 + d] >= 60) { errno = EINVAL; fail("NAME:STEPS"); }
        memcpy(names[d], argv[19 + d], (size_t)(colon - argv[19 + d]));
        names[d][colon - argv[19 + d]] = 0;
        day_steps[d] = strtoul(colon + 1, NULL, 10);
        if (!day_steps[d] || !L) { errno = EINVAL; fail("steps"); }
    }
    {
        const char *kp = getenv("KEEP");
        if (kp && *kp && strcmp(kp, "0") && strcmp(kp, "1")) { errno = EINVAL; fail("KEEP 0 or 1"); }
        keep_ctrl = kp && !strcmp(kp, "1") ? 3u : 1u;
        const char *y = getenv("LAYOUT");
        if (y && *y && strcmp(y, "segments") && strcmp(y, "window")) { errno = EINVAL; fail("LAYOUT segments or window"); }
        window_layout = y && !strcmp(y, "window");
    }
    for (unsigned d = 0; d < days; d++) {
        if (day_steps[d] >= 1ul << 24) { errno = EINVAL; fail("steps"); }
        first_row[d + 1] = first_row[d] + (unsigned)(window_layout ? day_steps[d] : (day_steps[d] + L - 1) / L);
    }
    {
        const char *t = getenv("THREADS");
        if (t && *t) threads = (unsigned)strtoul(t, NULL, 10);
        if (threads < 1 || threads > 2) { errno = EINVAL; fail("THREADS 1 or 2"); }
    }
    unsigned w = hidden + channels;
    used = first_row[days];
    if (used > rows || rows - used >= 4 || !timeout_ms || !window || (window & 4095) || (ddr_offset & 4095) || (window_offset & 4095)
        || in_channels > MAX_LANES || x_lane + nx + nd + nk > in_channels || nd > nx || hidden > x_lane || w > MAX_OUT || !beat
        || ((uint64_t)rows * w * 2) % beat || (uint64_t)rows * w * 2 >= DMA_LIMIT || (uint64_t)rows * hidden * 2 >= DMA_LIMIT
        || in_offset + (uint64_t)rows * in_channels * 2 > hidden_offset || hidden_offset + (uint64_t)rows * hidden * 2 > out_offset
        || out_offset + (uint64_t)rows * w * 2 > window || weight_offset < ddr_offset || weight_offset > window
        || hidden_offset < ddr_offset || out_offset < ddr_offset || !(ops_per_step > 0)) {
        errno = EINVAL;
        fail("arguments");
    }
    for (unsigned d = 0; d < days; d++) {
        size_t n;
        snprintf(path, sizeof(path), "gxcodes-%.63s.bin", names[d]);
        xcodes[d] = load(path, &n);
        if (n != day_steps[d] * nx * 2) { errno = EINVAL; fail(path); }
        if (nk) context_day(d);
    }
    {
        char head_path[300];
        const char *colon = strchr(image_path, ':');
        if (!colon || colon - image_path >= (long)sizeof(head_path)) { errno = EINVAL; fail("IMAGE: HEAD:WEIGHTS"); }
        memcpy(head_path, image_path, (size_t)(colon - image_path));
        head_path[colon - image_path] = 0;
        head = load(head_path, &head_bytes);
        weights = load(colon + 1, &weight_bytes);
        const char *pl = getenv("PRELOAD");
        if (pl && *pl) {
            char pre_path[300];
            const char *pc = strrchr(pl, ':');
            size_t pre_bytes;
            if (keep_ctrl != 3u || !pc || pc == pl || pc - pl >= (long)sizeof(pre_path)) {
                errno = EINVAL;
                fail("PRELOAD=HEAD:ROWS needs KEEP=1");
            }
            memcpy(pre_path, pl, (size_t)(pc - pl));
            pre_path[pc - pl] = 0;
            preload_rows = (uint32_t)strtoul(pc + 1, NULL, 10);
            preload = load(pre_path, &pre_bytes);
            if (pre_bytes != head_bytes || !preload_rows || preload_rows > rows) { errno = EINVAL; fail("PRELOAD head or rows"); }
        }
        const char *ck = getenv("CHUNKS");
        if (ck && *ck) {
            char chunk_path[300];
            const char *cc = strrchr(ck, ':');
            size_t chunk_bytes;
            if (keep_ctrl != 3u || !cc || cc == ck || cc - ck >= (long)sizeof(chunk_path)) {
                errno = EINVAL;
                fail("CHUNKS=FILE:ROWS needs KEEP=1");
            }
            memcpy(chunk_path, ck, (size_t)(cc - ck));
            chunk_path[cc - ck] = 0;
            chunk_rows = (unsigned)strtoul(cc + 1, NULL, 10);
            chunk_count = chunk_rows ? (rows + chunk_rows - 1) / chunk_rows : 0;
            chunk_heads = load(chunk_path, &chunk_bytes);
            unsigned last = chunk_count ? rows - (chunk_count - 1) * chunk_rows : 0;
            if (!chunk_rows || chunk_rows > rows || chunk_bytes != (size_t)chunk_count * head_bytes
                || ((uint64_t)chunk_rows * (hidden + channels) * 2) % beat || ((uint64_t)last * (hidden + channels) * 2) % beat) {
                errno = EINVAL;
                fail("CHUNKS file or rows");
            }
        }
        if (head_bytes > in_offset || weight_offset + (uint64_t)weight_bytes > window || !weight_bytes) {
            errno = EINVAL;
            fail("image parts");
        }
    }
    outputs = malloc((size_t)L * rows * w * 2);
    for (unsigned j = 0; j < 2; j++) {
        stages[j][0] = calloc((size_t)FEED_BLOCK * in_channels, 2);
        stages[j][1] = calloc((size_t)FEED_BLOCK * hidden, 2);
        if (!stages[j][0] || !stages[j][1]) fail("memory");
    }
    uint64_t *op_ns = calloc(L, sizeof(uint64_t)), *feed_ns = calloc(L + 1, sizeof(uint64_t));
    uint64_t *clock_ns = calloc(L, sizeof(uint64_t));      // every run on clock_gettime as well (the coarse time)
    unsigned long *run_khz = calloc(L, sizeof(unsigned long));
    unsigned gt_changed = 0;
    double chunk_gops_min = 1e30, chunk_gops_max = 0;
    unsigned long *valid = calloc(L, sizeof(unsigned long));
    if (!outputs || !op_ns || !feed_ns || !valid || !clock_ns || !run_khz) fail("memory");
    for (unsigned s = 0; s < L; s++)
        for (unsigned d = 0; d < days; d++)
            for (unsigned r = first_row[d]; r < first_row[d + 1]; r++)
                valid[s] += row_step(d, r, s) >= 0;
    {
        const char *g = getenv("GATES");
        if (g && *g) {
            unsigned long v[3] = {0, 0, 0};
            char *end = (char *)g;
            int ok = 1;
            for (int j = 0; j < 3 && ok; j++) {
                v[j] = strtoul(end, &end, 0);
                ok = *end == (j < 2 ? ':' : '\0');
                if (j < 2) end++;
            }
            if (!ok || !v[0] || (v[2] & ~v[1]) || !v[2]) { errno = EINVAL; fail("GATES=ADDR:RUN:ENGINE, ENGINE inside RUN"); }
            gate_run = (uint32_t)v[1]; gate_engine = (uint32_t)v[2];
            mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
            if (mem_fd < 0) fail("/dev/mem");
            gates = map_registers(mem_fd, (uint32_t)v[0]);
            gate_now = gate_run;
            gates[0] = gate_run;                                       // every shell access below with RUN on
            atexit(gates_restore);
        }
        const char *s = getenv("SOFT_START");
        if (s && *s) {
            unsigned long us = 0, mask = 0, from = 0;
            char *end = (char *)s;
            us = strtoul(end, &end, 10);
            if (*end == ':') mask = strtoul(end + 1, &end, 0);
            if (*end == ':') from = strtoul(end + 1, &end, 10);
            if (!gates || *end || us < 10 || us > 100000 || !mask || (mask & ~gate_engine) || from > 95) {
                errno = EINVAL;
                fail("SOFT_START=RAMP_US:MASK:FROM (with GATES, MASK inside ENGINE)");
            }
            soft_ramp_ns = us * 1000ull; soft_mask = (uint32_t)mask; soft_from = (uint32_t)from;
        }
        const char *fm = getenv("FEED_GATES");
        if (fm && *fm) {
            char *end;
            unsigned long m = strtoul(fm, &end, 0);
            if (!gates || *end || (m >> 16 >> 16) || (m & ~gate_run)) { errno = EINVAL; fail("FEED_GATES=MASK (with GATES, inside RUN)"); }
            feed_gates = (uint32_t)m;
            feed_gates_set = 1;
        }
        const char *fg = getenv("FEED_GOVERNOR");
        if (fg && *fg) {
            FILE *f = fopen(GOVERNOR, "r");
            if (!f || !fgets(governor_found, sizeof(governor_found), f)) fail("FEED_GOVERNOR: " GOVERNOR);
            fclose(f);
            governor_found[strcspn(governor_found, "\n")] = 0;
            feed_governor = fg;
            feed_switch = strcmp(fg, governor_found) != 0;
            atexit(governor_restore);
        }
        const char *q = getenv("QUIET_US");
        if (q && *q) quiet_ns = strtoull(q, NULL, 10) * 1000ull;
        const char *pq = getenv("POLL_US");                                 // 0: busy polling (the runs before 6.145.0)
        if (pq && *pq) poll_ns = strtoull(pq, NULL, 10) * 1000ull;
        if (poll_ns > 10000000ull) { errno = EINVAL; fail("POLL_US at most 10000"); }
        const char *u = getenv("ISLAND_DUTY"), *v = getenv("DITHER_SHARE");
        if (u && *u) island_duty = (unsigned)strtoul(u, NULL, 10);
        if (v && *v) dither_share = strtod(v, NULL);
        if ((island_duty || dither_share > 0.0) && (!gates || !soft_mask || island_duty > 255 || dither_share >= 1.0
                                                    || (island_duty && dither_share > 0.0))) {
            errno = EINVAL;
            fail("ISLAND_DUTY 1..255 or DITHER_SHARE 0..1 (one of them, with GATES and SOFT_START's island mask)");
        }
    }

    phase("pool");
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
    {
        const char *rt = getenv("RUN_TIMER");
        if (rt && *rt && strcmp(rt, "clock") && strcmp(rt, "global")) { errno = EINVAL; fail("RUN_TIMER global or clock"); }
        if (!(rt && !strcmp(rt, "clock")) && gt_cpu_khz()) gt_start(map_registers(mem_fd, GT_PAGE));
    }
    if (rd(shell, S_ID) != SHELL_ID) { errno = ENODEV; fail("shell id"); }
    if (!hold(C_SOFT_RESET | C_CLEAR, &status)) { errno = EIO; fail("engine did not quiesce"); }
    if (!reset_dma()) { errno = EIO; fail("dma reset"); }
    if (rd(gpio, 0x8) != 0) { errno = EIO; fail("framing counter not idle"); }
    {
        // the aligner's run-time form (ZIP_MODE=ADDR:VALUE), set while nothing streams and read back
        const char *z = getenv("ZIP_MODE");
        if (z && *z) {
            char *end;
            unsigned long addr = strtoul(z, &end, 0), value = *end == ':' ? strtoul(end + 1, &end, 0) : 2;
            if (!addr || (addr & 0xFFF) || *end || value > 1) { errno = EINVAL; fail("ZIP_MODE=ADDR:0|1"); }
            volatile uint32_t *zip = map_registers(mem_fd, (uint32_t)addr);
            zip[0] = (uint32_t)value;
            if ((zip[0] & 1u) != value) { errno = EIO; fail("ZIP_MODE did not read back"); }
        }
    }

    win = pool + window_offset;
    memset(win, 0, window);
    memcpy(win, head, head_bytes);
    memcpy(win + weight_offset, weights, weight_bytes);
    __sync_synchronize();
    if (memcmp(win, head, head_bytes) || memcmp(win + weight_offset, weights, weight_bytes)) {
        errno = EIO;
        fail("window readback");
    }
    wr(shell, S_BASE, (uint32_t)(info.dma_address + window_offset));
    wr(shell, S_WINDOW, window);
    wr(shell, S_DEADLINE, deadline);
    wr(shell, S_WDT, WDT_CYCLES);
    wr(shell, S_CTRL, C_WDT);
    if (!wait_status(ST_ISOLATED | ST_RESET, 0, 100000000ull, &status)) { errno = EIO; fail("release"); }
    if (rd(shell, E_ID) != ENGINE_ID) { errno = ENODEV; fail("engine id"); }
    wr(shell, E_DDR, ddr_offset);

    phase("run");
    int rt_priority = 0;
    {
        const char *rp = getenv("RT_PRIORITY");
        if (rp && *rp) {
            char *end;
            long v = strtol(rp, &end, 10);
            struct sched_param sp = {.sched_priority = (int)v};
            if (*end || v < 1 || v > 99) { errno = EINVAL; fail("RT_PRIORITY 1..99"); }
            if (sched_setscheduler(0, SCHED_FIFO, &sp)) fail("RT_PRIORITY: sched_setscheduler");
            rt_priority = (int)v;
        }
    }
    int run_cpu = -1;
    {
        const char *rc = getenv("RUN_CPU");
        if (rc && *rc) {
            char *end;
            long v = strtol(rc, &end, 10);
            cpu_set_t set;
            if (*end || v < 0 || v >= CPU_SETSIZE) { errno = EINVAL; fail("RUN_CPU"); }
            CPU_ZERO(&set);
            CPU_SET((int)v, &set);
            if (sched_setaffinity(0, sizeof(set), &set)) fail("RUN_CPU: sched_setaffinity");
            run_cpu = (int)v;
        }
        const char *tg = getenv("TICK_GUARD_US");
        if (tg && *tg) {
            char *end;
            tick_guard_us = strtoull(tg, &end, 10);
            if (*end || !tick_guard_us || tick_guard_us > 1000 || run_cpu < 0 || !gt) {
                errno = EINVAL;
                fail("TICK_GUARD_US 1..1000 (with RUN_CPU and the global timer)");
            }
        }
        const char *ct = getenv("CHUNK_TRACE");
        if (ct && *ct && strcmp(ct, "0")) {
            if (strcmp(ct, "1")) { errno = EINVAL; fail("CHUNK_TRACE 0 or 1"); }
            marks = calloc((size_t)L * (chunk_count ? chunk_count : 1u), sizeof(*marks));
            if (!marks) fail("memory");
            trace_file("/proc/interrupts", "before");
            trace_file("/proc/softirqs", "before");
        }
    }
    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    uint64_t t_all = now_ns(), cycles = 0;
    unsigned long khz_run = 0;                                         // a chunked run's CPU clock (read at its start)
    if (island_duty) {
        gates[2] = island_duty;                                         // the island PWM (second channel) on
        atexit(duty_off);
    }
    feed_ns[0] = now_ns();
    if (feed_switch) governor_write(feed_governor);
    if (feed_gates_set) gate_write(feed_gates);
    feed_all((unsigned)-1, threads);                                   // step 0's rows: zero state, first x codes
    gates_restore();
    if (feed_switch) governor_write(governor_found);
    feed_ns[0] = now_ns() - feed_ns[0];
    {
        uint32_t base = (uint32_t)(info.dma_address + window_offset) + ddr_offset;
        uint32_t out_bytes = rows * w * 2, hidden_bytes = rows * hidden * 2;
        if (preload) {
            uint32_t pout = preload_rows * w * 2, phid = preload_rows * hidden * 2, d0 = rd(shell, S_DONES), mm2s = 0, s2mm = 0;
            int engine_done = 0, dma_done = 0;
            memcpy(win, preload, head_bytes);
            __sync_synchronize();
            wr(gpio, 0x0, pout / beat);
            wr(dma, S2MM_CR, 1);
            wr(dma, MM2S_CR, 1);
            wr(dma, S2MM_DA, base + (out_offset - ddr_offset));
            wr(dma, S2MM_LEN, pout);
            wr(dma, MM2S_SA, base + (hidden_offset - ddr_offset));
            wr(dma, MM2S_LEN, phid);
            gates_restore();
            unsigned long pkhz = gt ? gt_cpu_khz() : 0;
            uint64_t pg0 = gt_read(), p0 = now_ns();
            wr(shell, S_BASE, (uint32_t)(info.dma_address + window_offset));
            wr(shell, E_PROG, 0);
            wr(shell, S_HEARTBEAT, 1);
            wr(shell, E_CTRL, keep_ctrl);
            while (!(engine_done && dma_done)) {
                status = rd(shell, S_STATUS);
                if (status & ST_ISOLATED) {
                    fprintf(stderr, "F_ASTRA_GRU_BATCH_PRELOAD_ABORTED faults=0x%x\n", rd(shell, S_FAULTS));
                    hold(0, &status);
                    return 3;
                }
                if (!engine_done && !(status & ST_RUNNING) && (status & ST_DONE) && rd(shell, S_DONES) == d0 + 1) {
                    engine_done = 1;
                    preload_cycles = rd(shell, E_CYC_LO) | ((uint64_t)rd(shell, E_CYC_HI) << 32);
                }
                mm2s = rd(dma, MM2S_SR);
                s2mm = rd(dma, S2MM_SR);
                if ((mm2s | s2mm) & SR_ERRORS) {
                    fprintf(stderr, "F_ASTRA_GRU_BATCH_PRELOAD_DMA_FAILED mm2s_sr=%u s2mm_sr=%u\n", mm2s, s2mm);
                    reset_dma();
                    hold(0, &status);
                    return 3;
                }
                dma_done = (mm2s & SR_IOC) && (s2mm & SR_IOC);
                if (now_ns() - p0 > (uint64_t)timeout_ms * 1000000ull) {
                    fprintf(stderr, "F_ASTRA_GRU_BATCH_PRELOAD_TIMEOUT engine=%d dma=%d\n", engine_done, dma_done);
                    reset_dma();
                    hold(0, &status);
                    return 3;
                }
            }
            uint64_t pg1 = gt_read(), pfine;
            preload_ns = now_ns() - p0;
            if (gt && (pfine = gt_ns(pg0, pg1, pkhz)))
                preload_ns = pfine;                                // on the global timer, as the runs (r23's clock: 18.4 us)
            wr(dma, MM2S_SR, SR_IOC | SR_ERR_IRQ);
            wr(dma, S2MM_SR, SR_IOC | SR_ERR_IRQ);
            wr(shell, S_HEARTBEAT, 1);
            memcpy(win, head, head_bytes);                         // the program of the runs again
            __sync_synchronize();
        }
        uint32_t dones = rd(shell, S_DONES);
        unsigned nchunks = chunk_count ? chunk_count : 1u;           // CHUNKS: run s goes as chunks c = q % nchunks
        uint64_t f_run = 0;
        for (unsigned q = 0; q < L * nchunks; q++) {
            unsigned s = q / nchunks, c = q % nchunks;
            uint64_t limit = (uint64_t)timeout_ms * 1000000ull, start;
            int engine_done = 0, dma_done = 0;
            uint32_t r0 = chunk_count ? c * chunk_rows : 0, n = chunk_count ? (c + 1 < chunk_count ? chunk_rows : rows - r0) : rows;
            if (chunk_count) {
                memcpy(win, chunk_heads + (size_t)c * head_bytes, head_bytes);     // the chunk's program head
                __sync_synchronize();
                out_bytes = n * w * 2;
                hidden_bytes = n * hidden * 2;
            }
            wr(gpio, 0x0, out_bytes / beat);
            wr(dma, S2MM_CR, 1);
            wr(dma, MM2S_CR, 1);
            wr(dma, S2MM_DA, base + (out_offset - ddr_offset) + r0 * w * 2);
            wr(dma, S2MM_LEN, out_bytes);
            wr(dma, MM2S_SA, base + (hidden_offset - ddr_offset) + r0 * hidden * 2);
            wr(dma, MM2S_LEN, hidden_bytes);
            gates_restore();                                           // RUN on: the shell is accessed below
            int stable = chunk_count && !feed_switch;                  // the clock read once a run
            unsigned long khz = !gt ? 0 : stable && c ? khz_run : (khz_run = gt_cpu_khz());
            uint32_t twd = 0;
            if (tick_guard_us && khz) {
                // the kernel's tick due within the guard: wait until it has fired and the timer is set again (2 ms
                // at most); the wait is the PS's, outside the run
                uint32_t presc = ((gt[TWD_CTRL / 4] >> 8) & 0xFFu) + 1u;
                uint64_t guard = tick_guard_us * khz / 2000u / presc, w0 = gt_read(), wmax = 2u * khz / gt_div;
                int waited = 0;
                for (;;) {
                    uint32_t ctl = gt[TWD_CTRL / 4], cnt = gt[TWD_COUNT / 4];
                    if (!(ctl & 1u) || cnt > guard || gt_read() - w0 > wmax) break;
                    waited = 1;
                }
                if (waited) {
                    tick_waits++;
                    tick_wait_ns += gt_ns_at(w0, gt_read(), khz);
                }
            }
            if (marks && gt) twd = gt[TWD_COUNT / 4];
            uint64_t gt_limit = gt && khz ? (uint64_t)((double)limit * (double)khz / (1e6 * gt_div)) : 0;
            uint32_t polls = 0;
            start = now_ns();
            uint64_t gt0 = gt_read(), gt_engine = 0, gt_dma = 0;
            if (!c) {                                                  // a chunk after the first: START alone
                wr(shell, S_BASE, (uint32_t)(info.dma_address + window_offset));
                wr(shell, E_PROG, 0);
                wr(shell, S_HEARTBEAT, 1);
            }
            wr(shell, E_CTRL, keep_ctrl);
            if (soft_ramp_ns && island_duty) {
                // the island PWM does the dithering: its duty rises from FROM of the run duty to the run duty as the
                // same ease-out, one write every RAMP_STEP_NS with the PS asleep in between (a busy PS on top of the
                // engine's start was the highest sample of a run)
                uint64_t s0 = now_ns(), tt = s0;
                double from = soft_from / 100.0;
                while (tt < s0 + soft_ramp_ns) {
                    double x = (double)(tt - s0) / (double)soft_ramp_ns;
                    double share = 1.0 - (1.0 - from) * (1.0 - x) * (1.0 - x);
                    unsigned d = (unsigned)(island_duty * share + 0.5);
                    gates[2] = d ? d : 1;
                    struct timespec q = {0, (long)RAMP_STEP_NS};
                    nanosleep(&q, NULL);
                    tt = now_ns();
                }
                gates[2] = island_duty;
            } else if (soft_ramp_ns) {
                // the started engine's island: its on share rises from FROM to 1 as an ease-out
                // 1 - (1 - FROM)(1 - x)^2, a first-order sigma-delta decides every pass (the clock is read every 16
                // passes), so the 12 V input sees the average current rise (the hybrid runner's dithered ramp)
                uint64_t s0 = now_ns(), tt = s0;
                double acc = 0.0, from = soft_from / 100.0, share = from;
                int on = 1;
                unsigned pass = 0;
                while (tt < s0 + soft_ramp_ns) {
                    if (!(pass++ & 15)) {
                        double x;
                        tt = now_ns();
                        x = tt >= s0 + soft_ramp_ns ? 1.0 : (double)(tt - s0) / (double)soft_ramp_ns;
                        share = 1.0 - (1.0 - from) * (1.0 - x) * (1.0 - x);
                    }
                    acc += share;
                    int want = acc >= 1.0;
                    if (want) acc -= 1.0;
                    if (want != on) { gate_write(want ? gate_now | soft_mask : gate_now & ~soft_mask); on = want; }
                }
                if (dither_share > 0.0) {
                    // the island at the share through the run: on time follows the share of the elapsed time (a per-pass
                    // share undershoots: a pass that reads the DMA status is long); the DMA status (not on the island)
                    // ends it, the shell is read only after the island runs again
                    uint64_t t0 = now_ns(), last = t0, on_ns = 0;
                    pass = 0;
                    for (;;) {
                        uint64_t t = now_ns();
                        if (on) on_ns += t - last;
                        last = t;
                        if (!(pass++ & 15)) {
                            uint32_t mm2s = rd(dma, MM2S_SR), s2mm = rd(dma, S2MM_SR);
                            if (((mm2s & SR_IOC) && (s2mm & SR_IOC)) || ((mm2s | s2mm) & SR_ERRORS) || t - start > limit) break;
                        }
                        int want = (double)on_ns < dither_share * (double)(t - t0);
                        if (want != on) { gate_write(want ? gate_now | soft_mask : gate_now & ~soft_mask); on = want; }
                    }
                }
                gates_restore();
            }
            if (quiet_ns && dither_share <= 0.0) {
                // the run takes about as long every step: the PS sleeps through most of it instead of polling
                uint64_t t = now_ns();
                if (t < start + quiet_ns) {
                    uint64_t wait = start + quiet_ns - t;
                    struct timespec q = {(time_t)(wait / 1000000000ull), (long)(wait % 1000000000ull)};
                    nanosleep(&q, NULL);
                }
            }
            while (!(engine_done && dma_done)) {
                polls++;
                if (!engine_done) {
                    status = rd(shell, S_STATUS);
                    if (status & ST_ISOLATED) {
                        fprintf(stderr, "F_ASTRA_GRU_BATCH_ENGINE_ABORTED step=%u faults=0x%x\n", s, rd(shell, S_FAULTS));
                        hold(0, &status);
                        return 3;
                    }
                    if (!(status & ST_RUNNING) && (status & ST_DONE) && rd(shell, S_DONES) == dones + 1) {
                        dones++;
                        engine_done = 1;
                        gt_engine = gt_read();
                    }
                }
                if (!dma_done) {
                    uint32_t mm2s = rd(dma, MM2S_SR), s2mm = rd(dma, S2MM_SR);
                    if ((mm2s | s2mm) & SR_ERRORS) {
                        fprintf(stderr, "F_ASTRA_GRU_BATCH_DMA_FAILED step=%u mm2s_sr=%u s2mm_sr=%u\n", s, mm2s, s2mm);
                        reset_dma();
                        hold(0, &status);
                        return 3;
                    }
                    if ((mm2s & SR_IOC) && (s2mm & SR_IOC)) {
                        dma_done = 1;
                        gt_dma = gt_read();
                    }
                }
                if (gt_limit ? gt_read() - gt0 > gt_limit : now_ns() - start > limit) {
                    fprintf(stderr, "F_ASTRA_GRU_BATCH_TIMEOUT step=%u engine=%d dma=%d\n", s, engine_done, dma_done);
                    reset_dma();
                    hold(0, &status);
                    return 3;
                }
                if (poll_ns && !(engine_done && dma_done)) {
                    struct timespec q = {0, (long)poll_ns};                 // the PS naps between polls
                    nanosleep(&q, NULL);
                }
            }
            uint64_t gt1 = gt_read(), fine, chunk_ns, chunk_clock = now_ns() - start;
            wr(dma, MM2S_SR, SR_IOC | SR_ERR_IRQ);                     // after the run's end: its bookkeeping
            wr(dma, S2MM_SR, SR_IOC | SR_ERR_IRQ);
            uint64_t cyc = rd(shell, E_CYC_LO) | ((uint64_t)rd(shell, E_CYC_HI) << 32);
            cycles += cyc;
            if (marks) {
                struct chunk_mark *m = marks + q;
                m->t0 = gt0; m->engine = gt_engine; m->dma = gt_dma; m->t1 = gt1; m->cycles = cyc;
                m->polls = polls; m->khz = (uint32_t)khz; m->twd = twd; m->cpu = sched_getcpu();
            }
            chunk_ns = chunk_clock;
            if (gt && (fine = stable ? gt_ns_at(gt0, gt1, khz) : gt_ns(gt0, gt1, khz))) {
                chunk_ns = fine;
                run_khz[s] = khz;
            } else if (gt) {
                gt_changed++;
            }
            op_ns[s] += chunk_ns;
            clock_ns[s] += chunk_clock;
            if (stable && gt && c + 1 == nchunks && gt_cpu_khz() != khz) {
                op_ns[s] = clock_ns[s];                                // the run's clock changed: its coarse time
                gt_changed++;
            }
            if (chunk_count) {
                unsigned long v = valid_span(r0, r0 + n, s);
                double g = v * ops_per_step / (double)chunk_ns;
                if (v && g < chunk_gops_min) chunk_gops_min = g;
                if (v && g > chunk_gops_max) chunk_gops_max = g;
            }
            wr(shell, S_HEARTBEAT, 1);
            __sync_synchronize();
            uint64_t f0 = now_ns();
            if (feed_switch) governor_write(feed_governor);              // the feed's clock (the engine is idle)
            if (feed_gates_set) gate_write(feed_gates);                 // the idle islands' clocks off through the feed
            if (chunk_count) feed_span(r0, r0 + n, s);                 // the chunk's outputs kept, its next rows written
            else feed_all(s, threads);                                 // outputs kept, the next step's rows written
            gates_restore();                                           // RUN again: the shell is accessed next
            if (feed_switch) governor_write(governor_found);            // the clock found again before the next run
            f_run += now_ns() - f0;
            if (c + 1 == nchunks) {
                feed_ns[s + 1] = f_run;
                f_run = 0;
            }
        }
    }
    uint64_t all_ns = now_ns() - t_all;
    getrusage(RUSAGE_SELF, &ru1);
    if (marks) {
        // one line a run (chunk): q run chunk cpu khz start_us engine_seen_ns dma_seen_ns end_ns polls cycles
        // twd_count (the times on the global timer from the run's start; start_us from the first run's; twd_count the
        // CPU's private timer at the start, the kernel's next tick)
        unsigned per = chunk_count ? chunk_count : 1u;
        for (unsigned q = 0; q < L * per; q++) {
            const struct chunk_mark *m = marks + q;
            double tick = m->khz ? gt_div * 1e6 / (double)m->khz : 0.0;
            fprintf(stderr, "F_ASTRA_GRU_BATCH_CHUNK %u %u %u %d %u %.3f %.0f %.0f %.0f %u %llu %u\n", q, q / per, q % per,
                    m->cpu, m->khz, (double)(m->t0 - marks[0].t0) * tick / 1e3, (double)(m->engine - m->t0) * tick,
                    (double)(m->dma - m->t0) * tick, (double)(m->t1 - m->t0) * tick, m->polls,
                    (unsigned long long)m->cycles, m->twd);
        }
        trace_file("/proc/interrupts", "after");
        trace_file("/proc/softirqs", "after");
    }
    if (rt_priority) {
        struct sched_param sp = {.sched_priority = 0};
        if (sched_setscheduler(0, SCHED_OTHER, &sp)) fail("RT_PRIORITY: back to SCHED_OTHER");
    }
    if (island_duty) gates[2] = 0;                                     // the plain gate again
    hold(0, &status);
    phase("outputs");

    // every day's outputs in its own step order, compared with its expected file; the post-run work timed apart:
    // the expected file's load and the compare are the test's, the assembly and the writes are the mission's
    unsigned long mismatched_all = 0;
    unsigned unchecked = 0;
    uint64_t post_load_ns = 0, assemble_ns = 0, compare_ns = 0, write_ns = 0, t_post;
    for (unsigned d = 0; d < days; d++) {
        size_t n = 0;
        int16_t *expected = NULL;
        snprintf(path, sizeof(path), "gexpected-%.63s.bin", names[d]);
        if (access(path, F_OK) == 0) {
            t_post = now_ns();
            expected = load(path, &n);
            post_load_ns += now_ns() - t_post;
            if (n != day_steps[d] * w * 2) { errno = EINVAL; fail(path); }
        } else {
            unchecked++;
        }
        int16_t *mine = malloc(day_steps[d] * w * 2);
        int32_t *index = malloc(day_steps[d] * channels * 4);
        if (!mine || !index) fail("memory");
        unsigned long mismatched = 0;
        long first_mismatch = -1;
        t_post = now_ns();
        for (unsigned long t = 0; t < day_steps[d]; t++) {
            memcpy(mine + t * w, step_output(d, t), (size_t)w * 2);
            for (k = 0; k < channels; k++) index[t * channels + k] = mine[t * w + hidden + k];
        }
        assemble_ns += now_ns() - t_post;
        t_post = now_ns();
        for (unsigned long t = 0; expected && t < day_steps[d]; t++)
            if (memcmp(mine + t * w, expected + t * w, (size_t)w * 2)) {
                if (first_mismatch < 0) first_mismatch = (long)t;
                mismatched++;
            }
        compare_ns += now_ns() - t_post;
        FILE *f;
        t_post = now_ns();
        snprintf(path, sizeof(path), "%.200s-%.63s-output.bin", prefix, names[d]);
        if (!(f = fopen(path, "wb")) || fwrite(mine, 2, day_steps[d] * w, f) != day_steps[d] * w || fclose(f)) fail(path);
        snprintf(path, sizeof(path), "%.200s-%.63s-index.i32", prefix, names[d]);
        if (!(f = fopen(path, "wb")) || fwrite(index, 4, day_steps[d] * channels, f) != day_steps[d] * channels || fclose(f))
            fail(path);
        snprintf(path, sizeof(path), "%.200s-%.63s.json", prefix, names[d]);
        if (!(f = fopen(path, "w"))) fail(path);
        fprintf(f, "{\"schema\":\"f-astra/gru-sequence-run/v1\",\"steps\":%lu,\"rows_per_run\":%u,\"%s_steps\":%u,"
                   "\"mismatched_steps\":%lu,\"first_mismatch\":%ld,\"total_s\":%.4f,\"batched\":true,\"checked\":%s}\n",
                day_steps[d], rows, window_layout ? "window" : "segment", L, mismatched, first_mismatch, all_ns / 1e9,
                expected ? "true" : "false");
        fclose(f);
        write_ns += now_ns() - t_post;
        mismatched_all += mismatched;
        free(expected); free(mine); free(index);
    }
    phase("done");
    fprintf(stderr, "F_ASTRA_GRU_BATCH_POST expected_load_ns=%llu assemble_ns=%llu compare_ns=%llu write_ns=%llu\n",
            (unsigned long long)post_load_ns, (unsigned long long)assemble_ns, (unsigned long long)compare_ns,
            (unsigned long long)write_ns);
    // per run: valid row-steps x ops over the run (engine start to the last DMA completion)
    double gops[64], gmin = 1e30, gsum = 0;
    uint64_t sorted[64], feed_total = 0, op_total = 0;
    unsigned long valid_total = 0;
    for (unsigned s = 0; s < L && s < 64; s++) {
        gops[s] = valid[s] * ops_per_step / (double)op_ns[s];
        if (gops[s] < gmin) gmin = gops[s];
        gsum += gops[s];
        sorted[s] = op_ns[s];
        op_total += op_ns[s];
        valid_total += valid[s];
    }
    for (unsigned s = 0; s <= L; s++) feed_total += feed_ns[s];
    qsort(sorted, L < 64 ? L : 64, sizeof(uint64_t), cmp_u64);
    printf("{\"schema\":\"f-astra/gru-batch-run/v1\",\"segment_steps\":%u,\"layout\":\"%s\",\"rows\":%u,\"days\":%u,"
           "\"valid_row_steps\":%lu,\"ops_per_step\":%.0f,\"run_gops\":[", L, window_layout ? "window" : "segments", rows,
           days, valid_total, ops_per_step);
    for (unsigned s = 0; s < L && s < 64; s++) printf(s ? ",%.3f" : "%.3f", gops[s]);
    printf("],\"run_gops_min\":%.3f,\"run_gops_mean\":%.3f,\"run_ns\":[", gmin, gsum / (L < 64 ? L : 64));
    for (unsigned s = 0; s < L && s < 64; s++) printf(s ? ",%llu" : "%llu", (unsigned long long)op_ns[s]);
    printf("],\"run_ns_clock\":[");
    for (unsigned s = 0; s < L && s < 64; s++) printf(s ? ",%llu" : "%llu", (unsigned long long)clock_ns[s]);
    printf("],\"run_cpu_khz\":[");
    for (unsigned s = 0; s < L && s < 64; s++) printf(s ? ",%lu" : "%lu", run_khz[s]);
    printf("],\"run_timer\":\"%s\",\"gt_divider\":%u,\"gt_changed\":%u", gt ? "global timer" : "clock_gettime", gt_div,
           gt_changed);
    printf(",\"keep\":%s,\"preload_rows\":%u,\"preload_ns\":%llu,\"preload_cycles\":%llu", keep_ctrl == 3u ? "true" : "false",
           preload_rows, (unsigned long long)preload_ns, (unsigned long long)preload_cycles);
    if (chunk_count)
        printf(",\"chunk_rows\":%u,\"chunks\":%u,\"chunk_gops_min\":%.3f,\"chunk_gops_max\":%.3f", chunk_rows, chunk_count,
               chunk_gops_min, chunk_gops_max);
    printf(",\"rt_priority\":%d,\"run_cpu\":%d,\"run_switches\":[%ld,%ld],\"chunk_trace\":%s", rt_priority, run_cpu,
           ru1.ru_nvcsw - ru0.ru_nvcsw, ru1.ru_nivcsw - ru0.ru_nivcsw, marks ? "true" : "false");
    printf(",\"tick_guard_us\":%llu,\"tick_waits\":%lu,\"tick_wait_ns\":%llu,\"twd_prescale\":%u",
           (unsigned long long)tick_guard_us, tick_waits, (unsigned long long)tick_wait_ns,
           gt ? ((gt[TWD_CTRL / 4] >> 8) & 0xFFu) + 1u : 0u);
    printf(",\"run_s_total\":%.4f,\"feed_s_total\":%.4f,\"total_s\":%.4f,\"overall_gops\":%.3f,\"engine_cycles\":%llu,"
           "\"threads\":%u,\"soft_start_ns\":%llu,\"soft_start_from\":%u,\"quiet_ns\":%llu,\"poll_ns\":%llu,\"mismatched_row_steps\":%lu,"
           "\"delta_inputs\":%u,\"island_duty\":%u,\"dither_share\":%.4f,\"unchecked_days\":%u,\"feed_governor\":\"%s\","
           "\"timer\":\"run_ns: run_timer (the global timer, else CLOCK_MONOTONIC_RAW as run_ns_clock); a run = engine "
           "start (with its ramp) to the last DMA completion; the feed = the PS taking every row's outputs and writing the "
           "next rows (CLOCK_MONOTONIC_RAW)\"}\n",
           op_total / 1e9, feed_total / 1e9, all_ns / 1e9, valid_total * ops_per_step / (double)all_ns,
           (unsigned long long)cycles, threads, (unsigned long long)soft_ramp_ns, soft_from, (unsigned long long)quiet_ns,
           (unsigned long long)poll_ns,
           mismatched_all, nd, island_duty, dither_share, unchecked, feed_governor ? feed_governor : "");
    (void)sorted;
    return mismatched_all ? 4 : 0;
}
