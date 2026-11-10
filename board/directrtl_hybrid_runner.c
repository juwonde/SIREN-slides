// F-ASTRA board runner of a hybrid model (docs/plan-short-term.md section 4): the unified ModernTCN engine runs the
// segments of its program (moderntcn_engine_rtl.hybrid_plan) and a FINN streaming accelerator runs the operations
// between them (LayerNorm with one coefficient set per operation), FRAMES_IN_FLIGHT frames at a time. Every frame in
// flight has its own engine window (a copy of the window image) in the models pool: the runner switches the shell's
// BASE to the frame's window (allowed between runs), writes PROG_BASE = the segment's program offset and starts the
// engine; a stream operation writes the set index into the set feeder (one stream beat), the expected output beats
// into the accelerator's framing GPIO and moves the operation's rows from the frame's window through the accelerator
// back into the window with the model's AXI DMA. The engine and the accelerator run concurrently on different frames
// (list scheduling by polling). Outputs are compared byte for byte with the expected file (the integer reference);
// the timer covers every step of every measured frame (from the first start to the last completion of a repeat).
// PS load (S2 PL scheduler question): the run reports the runner's CPU time (getrusage, user + system) over its wall time
// from the first repeat to the last; POLL_SLEEP_US (environment, 0 = off) sleeps that long after a scheduling pass that
// changed nothing (no step started or finished, no input or compare piece), so the PS polls less often.
// Progress and failures go to stderr; exit 0 pass, 2 setup failure, 3 timeout or abort, 4 mismatch, 5 an FM fault.
// SIGTERM or SIGINT (for example the session's thermal guard) stops the run at once: the unfinished repeat is dropped,
// the engine and the DMA are quiesced and the finished repeats are reported ("stopped": true, exit 3).
// Thermal stop (environment THERMAL_PACE_HIGH_C and THERMAL_PACE_LOW_C, both set): a die at or above the high mark,
// read between repeats and every 0.5 s inside one, stops the run: no new step starts, the started ones finish, and the
// run waits (engine idle, watchdog fed) until the die is below the low mark, then goes on. A pause between repeats
// changes no measurement. A measured repeat interrupted inside is not timed and, when the run has more than one
// repeat, runs again (at most MAX_THERMAL_RETRIES times; then it counts as done without a time). Pauses, interrupted
// and untimed repeats are reported.
// Chunks (optional fourth field CHUNK of FEEDER:BEAT:INFLIGHT:CHUNK): a repeat runs its frames in chunks of CHUNK
// frames; the pipeline drains completely at every chunk end (its fill and drain stay inside the timed span), and with
// thermal pacing the run cools below the low mark before every chunk (the cooling is not timed). A repeat whose
// chunks all ran without a thermal stop is timed: the sum of its chunk spans. A repeat longer than the heating span
// from the low to the high mark is measurable this way; every frame is still compared with the integer reference.
// A plan may end with a stream step (the stream output is then the model output), but it needs an engine step.
// Concurrent steps ('C', the GRU G4 stream: docs/gru-50gops-design.md): the engine runs a program whose results leave on
// its G4 stream through the aligner into the stream model, while the model's DMA reads the step's other input (the
// hidden codes) and writes its output; the step ends when the engine and both DMA channels are done. Static inputs
// (sixth field STATIC = 1 of FEEDER:BEAT:INFLIGHT:CHUNK:STATIC, frames = inflight): every frame's input is placed in
// its own window once before the run and its output region is filled with 0xA5 before every repeat; the outputs are
// compared after the repeat's timer stops (as the stream models' runner does), so a repeat times the PL work alone.
// PS work in pieces: a frame's input copy into its window and its output compare (both on the uncached pool) run
// PIECE_BYTES at a time between polls, so a short engine run (the GRU predictor: 0.4 ms) is started again at once
// instead of waiting for a whole copy or compare. A frame takes part in the schedule once its input is complete; its
// slot is free once its compare is complete. Both stay inside the timed span.
// Digest mode (environment DIGEST_PLAN and DIGEST_OUT, I6-2 D3 per-structure equivalence): DIGEST_PLAN lists
// "STEP OFFSET BYTES" lines (a plan step and a window region it wrote). When that step of a frame finishes, the runner
// writes the SHA-256 of the region (32 bytes) to DIGEST_OUT, frame by frame in plan order. It needs one frame in flight,
// no static inputs, no warmup and one repeat; the run is an equivalence run, not a measurement.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct pool_info { uint64_t dma_address; uint32_t bytes; uint32_t output_offset; };
#define POOL_INFO _IOR('F', 1, struct pool_info)

#define APERTURE 0x10000u
#include "f_astra_pl_io.h"          // rd, wr, map_registers: /dev/mem or the secure monitor
enum { E_CTRL = 0x00, E_STATUS = 0x04, E_PROG = 0x08, E_DDR = 0x0C, E_CYC_LO = 0x10, E_CYC_HI = 0x14, E_ID = 0x1C,
       E_PC = 0x2C };
enum { S_ID = 0x80, S_CTRL = 0x84, S_STATUS = 0x88, S_FAULTS = 0x8C, S_DEADLINE = 0x90, S_WDT = 0x94, S_HEARTBEAT = 0x98,
       S_WINDOW = 0x9C, S_RUNS = 0xA0, S_DONES = 0xA4, S_INJECT = 0xB8, S_BASE = 0xC8 };
// engine STATUS bits of the lane-memory SECDED ECC: 12 a double-bit error (also the error bit), 13 a corrected one
enum { E_ST_ECC_UNCORRECTABLE = 1u << 12, E_ST_ECC_CORRECTED = 1u << 13 };
enum { ST_RUNNING = 1, ST_DONE = 2, ST_ISOLATED = 4, ST_RESET = 8, ST_DRAINED = 16 };
enum { C_HOLD = 1, C_WDT = 4, C_SOFT_RESET = 0x100, C_CLEAR = 0x200 };
enum { MM2S_CR = 0x00, MM2S_SR = 0x04, MM2S_SA = 0x18, MM2S_LEN = 0x28,
       S2MM_CR = 0x30, S2MM_SR = 0x34, S2MM_DA = 0x48, S2MM_LEN = 0x58 };
enum { SR_HALTED = 0x1, SR_ERRORS = 0x70, SR_IOC = 0x1000, SR_ERR_IRQ = 0x4000 };
#define SHELL_ID 0x4D545348u
#define ENGINE_ID 0x4D54434Eu
#define WDT_CYCLES (1u << 27)
#define MAX_STEPS 64
#define MAX_INFLIGHT 4
#define MAX_TIMES 65536
#define MAX_THERMAL_RETRIES 3
#define PIECE_BYTES 2048u

struct step { char kind; uint32_t program_offset, set, src, dst, in_bytes, out_bytes; };
struct slot { long frame; int step, busy, differs, cpart; uint64_t started; uint32_t copied, checked; };

static volatile uint32_t *shell, *dma, *gpio, *feeder;

// Step clock gates (environment GATE_STEPS=ADDR:RUN:ENGINE:STREAM, the route's clock gate register and masks): the run
// starts with its mask RUN; inside a repeat the ENGINE bits are on only while an engine step runs or the shell is
// accessed, the STREAM bits only while a stream step runs (one frame in flight: the engine waits during the stream
// operations and the stream model during the engine segments). The engine island gate stops the shell's AXI port as
// well, so every shell access turns the ENGINE bits on first; a hold, the thermal cooling and the exit put RUN back.
static volatile uint32_t *gates;
static uint32_t gate_run, gate_engine, gate_stream, gate_now;
static unsigned long gate_writes;
// Pacing (environment PACE_GOPS): frames start no closer than ops_per_frame / PACE_GOPS apart, so the run delivers that
// throughput and no more; between frames nothing runs (with GATE_STEPS every gated block stops) and the PS sleeps.
static uint64_t pace_period_ns;
// Step gap (environment STEP_GAP_US): an engine step that follows a stream step starts no earlier than that long after
// the stream step ended, so the two show apart in a power trace (a diagnostic of where the 12 V peaks sit).
static uint64_t gap_ns, gap_until;
// Engine clocks kept on (environment GATE_KEEP_ENGINE=1, with GATE_STEPS): while a frame is in flight the engine's
// gates stay on through its stream steps; only the stream model's gates follow its steps, and every gate stops between
// frames.
static int keep_engine;
// Gap mask (environment GATE_GAP_MASK, with GATE_KEEP_ENGINE=2; a power-trace diagnostic): gates turned on while no
// frame is in flight, off while one is, so a frame start is a smaller load step on the 12 V input.
static uint32_t gap_mask;
// Soft start (environment SOFT_START=RAMP_US:STEPS:MASK, with GATE_STEPS): after an engine step starts, the MASK gate
// bits (inside ENGINE; the bundle allows only an engine clock island of single-pumped slices, mac_pump 1, where every
// engine register shares one gated clock and the clock converters' async FIFOs are the only crossings) are switched
// off and on in STEPS slots over RAMP_US with the on share rising from 1/STEPS to 1: the engine's current rises over
// the ramp instead of in one step. No shell access happens during the ramp.
static uint64_t soft_ramp_ns;
static unsigned soft_steps;
static unsigned soft_from;        // dither (STEPS 1) with a start share in percent: an ease-out from it (0: smoothstep)
static uint32_t soft_mask;
static unsigned long soft_ramps;
static uint64_t now_ns(void);
static void spin_until(uint64_t t) { while (now_ns() < t) { } }

// whether n bytes of the uncached (bufferable) pool at p differ from q: 64-byte NEON loads instead of memcmp's word
// loads, each of which waits for DDR (a 565 KB GRU output took 37 ms, so the PL idled that long between repeats and
// every repeat began with a load step from idle)
static int pool_differs(const uint8_t *p, const uint8_t *q, size_t n)
{
    size_t i = 0;
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
    if (!(((uintptr_t)p | (uintptr_t)q) & 3)) {
        uint32_t any[4] = {0, 0, 0, 0};
        __asm__ volatile("vmov.i32 q0, #0\n\t" : : : "d0", "d1");
        for (; i + 64 <= n; i += 64)
            __asm__ volatile("vldmia %0, {d16-d23}\n\t"
                             "vldmia %1, {d24-d31}\n\t"
                             "veor q8, q8, q12\n\t"
                             "veor q9, q9, q13\n\t"
                             "veor q10, q10, q14\n\t"
                             "veor q11, q11, q15\n\t"
                             "vorr q8, q8, q9\n\t"
                             "vorr q10, q10, q11\n\t"
                             "vorr q0, q0, q8\n\t"
                             "vorr q0, q0, q10\n\t"
                             : : "r"(p + i), "r"(q + i)
                             : "d0", "d1", "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "d24", "d25", "d26",
                               "d27", "d28", "d29", "d30", "d31", "memory");
        __asm__ volatile("vst1.32 {d0-d1}, [%0]\n\t" : : "r"(any) : "memory");
        if (any[0] | any[1] | any[2] | any[3]) return 1;
    }
#endif
    return i < n && memcmp(p + i, q + i, n - i) != 0;
}
// The static output check of one window: n bytes of the uncached pool at p against q (pool_compare, the faster of two
// ways measured at the start), then the region filled with 0xA5 for the next repeat (a missing write then shows).
// Several uncached loads in flight before their data is used: one 64-byte load at a time read 98 MB/s, two 275 MB/s.
// Mixing the 0xA5 stores into the same loop serialized the loads again (97 MB/s), so the fill is a separate memset
// (1.3 GB/s). Fewer milliseconds here, a shorter PL pause between repeats: every long pause made the next start a
// load step with an excess 0.5-0.75 ms long.
static int pool_compare2(const uint8_t *p, const uint8_t *q, size_t n)
{
    // two halves read side by side, 64 bytes of each per step
    size_t half = n / 2 / 64 * 64, i = 0;
    int differs = 0;
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
    if (!(((uintptr_t)p | (uintptr_t)q) & 7)) {
        uint32_t any[4] = {0, 0, 0, 0};
        __asm__ volatile("vmov.i32 q0, #0\n\t" : : : "d0", "d1");
        for (; i + 64 <= half; i += 64)
            __asm__ volatile("vldmia %0, {d16-d23}\n\t"
                             "vldmia %1, {d24-d31}\n\t"
                             "vldmia %2, {d2-d9}\n\t"
                             "veor q8, q8, q1\n\t"
                             "veor q9, q9, q2\n\t"
                             "veor q10, q10, q3\n\t"
                             "veor q11, q11, q4\n\t"
                             "vldmia %3, {d2-d9}\n\t"
                             "veor q12, q12, q1\n\t"
                             "veor q13, q13, q2\n\t"
                             "veor q14, q14, q3\n\t"
                             "veor q15, q15, q4\n\t"
                             "vorr q8, q8, q9\n\t"
                             "vorr q10, q10, q11\n\t"
                             "vorr q12, q12, q13\n\t"
                             "vorr q14, q14, q15\n\t"
                             "vorr q8, q8, q10\n\t"
                             "vorr q12, q12, q14\n\t"
                             "vorr q0, q0, q8\n\t"
                             "vorr q0, q0, q12\n\t"
                             : : "r"(p + i), "r"(p + half + i), "r"(q + i), "r"(q + half + i)
                             : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9", "d16", "d17", "d18", "d19",
                               "d20", "d21", "d22", "d23", "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31",
                               "memory");
        __asm__ volatile("vst1.32 {d0-d1}, [%0]\n\t" : : "r"(any) : "memory");
        differs = (any[0] | any[1] | any[2] | any[3]) != 0;
    } else half = 0;
#else
    half = 0;
#endif
    if (i < half) differs |= memcmp(p + i, q + i, half - i) != 0;
    if (half + i < n) differs |= memcmp(p + half + i, q + half + i, n - half - i) != 0;
    return differs;
}

static int pool_compare4(const uint8_t *p, const uint8_t *q, size_t n)
{
    // four quarters read side by side, 32 bytes of each per step (four loads in flight)
    size_t quarter = n / 4 / 32 * 32, i = 0;
    int differs = 0;
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
    if (quarter && !(((uintptr_t)p | (uintptr_t)q) & 7)) {
        uint32_t any[4] = {0, 0, 0, 0};
        __asm__ volatile("vmov.i32 q0, #0\n\t" : : : "d0", "d1");
        for (; i + 32 <= quarter; i += 32) {
            const uint8_t *a = p + i, *b = q + i;
            __asm__ volatile("vld1.64 {d16-d19}, [%0:64], %2\n\t"
                             "vld1.64 {d20-d23}, [%0:64], %2\n\t"
                             "vld1.64 {d24-d27}, [%0:64], %2\n\t"
                             "vld1.64 {d28-d31}, [%0:64]\n\t"
                             "vld1.64 {d2-d5}, [%1:64], %2\n\t"
                             "veor q8, q8, q1\n\t"
                             "veor q9, q9, q2\n\t"
                             "vld1.64 {d2-d5}, [%1:64], %2\n\t"
                             "veor q10, q10, q1\n\t"
                             "veor q11, q11, q2\n\t"
                             "vld1.64 {d2-d5}, [%1:64], %2\n\t"
                             "veor q12, q12, q1\n\t"
                             "veor q13, q13, q2\n\t"
                             "vld1.64 {d2-d5}, [%1:64]\n\t"
                             "veor q14, q14, q1\n\t"
                             "veor q15, q15, q2\n\t"
                             "vorr q8, q8, q9\n\t"
                             "vorr q10, q10, q11\n\t"
                             "vorr q12, q12, q13\n\t"
                             "vorr q14, q14, q15\n\t"
                             "vorr q8, q8, q10\n\t"
                             "vorr q12, q12, q14\n\t"
                             "vorr q0, q0, q8\n\t"
                             "vorr q0, q0, q12\n\t"
                             : "+r"(a), "+r"(b) : "r"(quarter)
                             : "d0", "d1", "d2", "d3", "d4", "d5", "d16", "d17", "d18", "d19", "d20", "d21", "d22",
                               "d23", "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31", "memory");
        }
        __asm__ volatile("vst1.32 {d0-d1}, [%0]\n\t" : : "r"(any) : "memory");
        differs = (any[0] | any[1] | any[2] | any[3]) != 0;
    } else quarter = 0;
#else
    quarter = 0;
#endif
    for (size_t k = 0; k < 4 && quarter; k++)
        if (i < quarter) differs |= memcmp(p + k * quarter + i, q + k * quarter + i, quarter - i) != 0;
    if (4 * quarter < n) differs |= memcmp(p + 4 * quarter, q + 4 * quarter, n - 4 * quarter) != 0;
    return differs;
}

static int (*pool_compare)(const uint8_t *, const uint8_t *, size_t) = pool_compare2;
static int compare_way = 2;

static int pool_check_fill(uint8_t *p, const uint8_t *q, size_t n)
{
    int differs = pool_compare(p, q, n);
    memset(p, 0xA5, n);
    return differs;
}

static void pool_bench_fill(uint8_t *p, size_t n)
{
    uint64_t t = now_ns();
    memset(p, 0xA5, n);
    __sync_synchronize();
    t = now_ns() - t;
    fprintf(stderr, "F_ASTRA_HYBRID_POOL_BENCH memset_fill %.1f MB/s\n", (double)n / 1e6 / ((double)t / 1e9));
}

// Uncached read rates (environment POOL_BENCH, a diagnostic): ways of reading the pool timed on the first window's
// image, MB/s on stderr (F_ASTRA_HYBRID_POOL_BENCH), to choose the output compare.
static uint32_t bench_ldm(const uint8_t *p, size_t n)
{
    uint32_t acc = 0;
    size_t i;
    for (i = 0; i + 32 <= n; i += 32) {
        uint32_t x;
        __asm__ volatile("ldmia %1, {r4, r5, r6, r7, r8, r9, r10, ip}\n\t"
                         "eor %0, r4, r5\n\t"
                         "eor %0, %0, r6\n\t"
                         "eor %0, %0, r7\n\t"
                         "eor %0, %0, r8\n\t"
                         "eor %0, %0, r9\n\t"
                         "eor %0, %0, r10\n\t"
                         "eor %0, %0, ip\n\t"
                         : "=&r"(x) : "r"(p + i) : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "ip", "memory");
        acc ^= x;
    }
    return acc;
}

static uint32_t bench_neon2(const uint8_t *p, size_t n)
{
    // two streams half a region apart, each load issued before the other's data is used
    uint32_t any[4] = {0, 0, 0, 0};
    size_t half = n / 2 / 64 * 64, i;
#if defined(__ARM_NEON__) || defined(__ARM_NEON)
    __asm__ volatile("vmov.i32 q0, #0\n\t" : : : "d0", "d1");
    for (i = 0; i + 64 <= half; i += 64)
        __asm__ volatile("vldmia %0, {d16-d23}\n\t"
                         "vldmia %1, {d24-d31}\n\t"
                         "veor q8, q8, q12\n\t"
                         "vorr q0, q0, q8\n\t"
                         : : "r"(p + i), "r"(p + half + i)
                         : "d0", "d1", "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23", "d24", "d25", "d26",
                           "d27", "d28", "d29", "d30", "d31", "memory");
    __asm__ volatile("vst1.32 {d0-d1}, [%0]\n\t" : : "r"(any) : "memory");
#else
    (void)p; (void)half; (void)i;
#endif
    return any[0] ^ any[3];
}

static void pool_bench(const uint8_t *win, const uint8_t *image, size_t n)
{
    uint64_t t;
    volatile uint32_t sink = 0;
    double mb = (double)n / 1e6;
    pid_t child;
    t = now_ns(); sink ^= (uint32_t)memcmp(win, image, n); t = now_ns() - t;
    fprintf(stderr, "F_ASTRA_HYBRID_POOL_BENCH memcmp %.1f MB/s\n", mb / ((double)t / 1e9));
    t = now_ns(); sink ^= (uint32_t)pool_differs(win, image, n); t = now_ns() - t;
    fprintf(stderr, "F_ASTRA_HYBRID_POOL_BENCH neon64_compare %.1f MB/s\n", mb / ((double)t / 1e9));
    t = now_ns(); sink ^= bench_ldm(win, n); t = now_ns() - t;
    fprintf(stderr, "F_ASTRA_HYBRID_POOL_BENCH ldm32_read %.1f MB/s\n", mb / ((double)t / 1e9));
    t = now_ns(); sink ^= bench_neon2(win, n); t = now_ns() - t;
    fprintf(stderr, "F_ASTRA_HYBRID_POOL_BENCH neon64x2_read %.1f MB/s\n", mb / ((double)t / 1e9));
    t = now_ns();
    child = fork();
    if (child == 0) _exit(pool_differs(win + n / 2, image + n / 2, n - n / 2));
    sink ^= (uint32_t)pool_differs(win, image, n / 2);
    if (child > 0) waitpid(child, NULL, 0);
    t = now_ns() - t;
    fprintf(stderr, "F_ASTRA_HYBRID_POOL_BENCH neon64_compare_two_processes %.1f MB/s (sink %u)\n",
            mb / ((double)t / 1e9), (unsigned)sink);
}

// Concurrent-step engine delay (environment C_ENGINE_DELAY_US, a power-trace diagnostic): the engine of a 'C' step
// starts that long after the stream model's DMA, so the two starts show apart in a trace.
static uint64_t c_delay_ns;
// Rest before each repeat (environment REPEAT_REST_US, a power-trace diagnostic): after the static output fill the
// PS sleeps that long before the repeat starts, so a busy PS (output compare, fill) and an idle PL show apart.
static uint64_t rest_ns;
// Quiet polling of concurrent steps (environment POLL_QUIET_US): after a 'C' step's engine start the PS sleeps that
// long before it polls again (the GRU frame takes 4.9 ms; polling every 50 us kept the PS awake through it).
static uint64_t quiet_ns;
// Static outputs checked every Nth measured repeat only (environment STATIC_CHECK_EVERY, a power-trace diagnostic: the
// repeats then follow each other without the PS compare between them; the warmup, every Nth and the last repeat are
// still compared, the others counted in static_unchecked).
static unsigned long check_every, unchecked;
static unsigned long gaps;
static void gate_write(uint32_t mask)
{
    if (gates && mask != gate_now) { wr(gates, 0, mask); gate_now = mask; gate_writes++; }
}
static void engine_on(void) { gate_write(gate_now | gate_engine); }
// Fault injection (environment ECC_INJECT=1 one bit, 2 two bits, 3 TMR): the shell flips that many bits of the next data
// word the engine stores into lane memory 0 (3: inverts copy 1 of the triplicated sequencer registers for one cycle),
// once, at the first measured repeat; every engine step's STATUS is read after its completion and the steps with a
// corrected or an uncorrectable lane-memory read are counted (ecc_* in the record).
static int ecc_inject;
static unsigned long ecc_corrected_steps, ecc_uncorrectable_steps;
static void gates_restore(void) { if (gates) gate_write(gate_run); }
// Island PWM (environment ISLAND_DUTY=D, 1..255, with GATE_STEPS on a bitstream with route_models island_pwm): the gate
// register's second channel runs the engine island's clock on D/256 of its cycles for the whole run (the engine's
// throughput and switching follow the share; the enable bit, the step gates and the soft start work on top of it);
// 0 again at the exit.
static unsigned island_duty;
static void duty_off(void) { if (gates && island_duty) gates[2] = 0; }
// Overlap duty (environment OVERLAP_DUTY=D, 1..255, needs GATE_STEPS on an island_pwm bitstream): while an engine
// step and a stream step of two frames in flight run at the same time, the island's share is D/256 instead of
// ISLAND_DUTY; the moment one of them ends it is ISLAND_DUTY (or the full clock) again. With two frames in flight
// the two blocks running together were the only 100 Hz samples above 5 W (10/29: every one a single 10 ms
// sample), so only those moments are slowed, not the whole run. The soft-start ramp's writes are left alone (the
// ramp ends at ISLAND_DUTY; the next transition writes the overlap share if the stream is still busy).
static unsigned overlap_duty;
static unsigned long overlap_writes;
static int overlap_on;
static void overlap_update(int engine_busy_now, int stream_busy_now) {
    int both;
    if (!overlap_duty) return;
    both = engine_busy_now && stream_busy_now;
    if (both == overlap_on) return;
    gates[2] = both ? overlap_duty : island_duty;
    overlap_on = both;
    overlap_writes++;
}
// Soft start through the island PWM (environment SOFT_PWM=1, with a dithered SOFT_START on an island_pwm bitstream):
// the ramp writes the PWM duty (FROM of the full clock, or of ISLAND_DUTY, rising as the same ease-out) in
// SOFT_PWM_WRITES steps and the PS sleeps between the writes, so the 12 V input does not see a busy PS core on top
// of the engine's start (the dithered ramp kept a core in a tight loop for the whole ramp at every engine step: the
// highest PD samples were those). At the end of the ramp the duty is ISLAND_DUTY again (0: the full clock).
static int soft_pwm;
static unsigned long soft_pwm_writes_total;
#define SOFT_PWM_WRITES 8

static void fail(const char *what)
{
    fprintf(stderr, "F_ASTRA_HYBRID_RUNNER_FAILED %s errno=%d\n", what, errno);
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
    fprintf(stderr, "F_ASTRA_HYBRID_RUNNER_PHASE %s %ld.%02ld\n", what, (long)up.tv_sec, (long)(up.tv_nsec / 10000000));
}

static volatile sig_atomic_t stop_requested;
static void on_stop(int sig) { (void)sig; stop_requested = 1; }

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
    gates_restore();
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

// plan: one step per line, "E PROGRAM_OFFSET", "S SET SRC DST IN_BYTES OUT_BYTES" or "C PROGRAM_OFFSET SRC DST IN_BYTES
// OUT_BYTES" (offsets in the window)
static int read_plan(const char *path, struct step *steps)
{
    FILE *f = fopen(path, "r");
    int n = 0;
    char kind;
    if (!f) fail(path);
    while (n < MAX_STEPS && fscanf(f, " %c", &kind) == 1) {
        struct step *s = &steps[n];
        memset(s, 0, sizeof(*s));
        s->kind = kind;
        if (kind == 'E') {
            if (fscanf(f, "%u", &s->program_offset) != 1) fail("plan engine step");
        } else if (kind == 'S') {
            if (fscanf(f, "%u %u %u %u %u", &s->set, &s->src, &s->dst, &s->in_bytes, &s->out_bytes) != 5) fail("plan stream step");
        } else if (kind == 'C') {
            if (fscanf(f, "%u %u %u %u %u", &s->program_offset, &s->src, &s->dst, &s->in_bytes, &s->out_bytes) != 5)
                fail("plan concurrent step");
        } else {
            errno = EINVAL;
            fail("plan step kind");
        }
        n++;
    }
    fclose(f);
    {
        int k, engine_steps = 0;
        for (k = 0; k < n; k++) engine_steps += steps[k].kind == 'E' || steps[k].kind == 'C';
        if (!engine_steps) { errno = EINVAL; fail("plan needs an engine segment"); }
    }
    return n;
}

// XADC die temperature in degrees C (sysfs iio), or a negative value when it cannot be read
static double die_temp_c(void)
{
    // the XADC offset and scale are constants: read once; the raw value each time (three sysfs opens per call took
    // a large part of the PL pause between repeats)
    static const char *part[3] = {"raw", "offset", "scale"};
    static double v[3];
    static int constants;
    int k;
    for (k = 0; k < (constants ? 1 : 3); k++) {
        char path[96];
        FILE *f;
        snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device0/in_temp0_%s", part[k]);
        f = fopen(path, "r");
        if (!f) return -1.0;
        if (fscanf(f, "%lf", &v[k]) != 1) { fclose(f); return -1.0; }
        fclose(f);
    }
    constants = 1;
    return (v[0] + v[1]) * v[2] / 1000.0;
}

// waits until the die is below `low` (engine idle, the shell watchdog fed every 100 ms); returns that temperature
static double cool_below(double low)
{
    uint64_t beat = now_ns();
    double t;
    gates_restore();                    // the heartbeat below reaches the shell
    while ((t = die_temp_c()) >= low && !stop_requested) {
        struct timespec rest = {0, 200000000};
        if (now_ns() - beat > 100000000ull) { wr(shell, S_HEARTBEAT, 1); beat = now_ns(); }
        nanosleep(&rest, NULL);
    }
    return t;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// SHA-256 (FIPS 180-4) of the digest mode
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t *hash, const uint8_t *p)
{
    uint32_t w[64], v[8], t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7]
               + (ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10));
    memcpy(v, hash, sizeof(v));
    for (i = 0; i < 64; i++) {
        t1 = v[7] + (ROR32(v[4], 6) ^ ROR32(v[4], 11) ^ ROR32(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + K256[i] + w[i];
        t2 = (ROR32(v[0], 2) ^ ROR32(v[0], 13) ^ ROR32(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof(v[0]));
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++) hash[i] += v[i];
}

static void sha256(const uint8_t *data, size_t n, uint8_t out[32])
{
    uint32_t hash[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t tail[128];
    size_t i, rest = n % 64, end = n % 64 < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (i = 0; i + 64 <= n; i += 64) sha256_block(hash, data + i);
    memset(tail, 0, sizeof(tail));
    memcpy(tail, data + n - rest, rest);
    tail[rest] = 0x80;
    for (i = 0; i < 8; i++) tail[end - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256_block(hash, tail);
    if (end == 128) sha256_block(hash, tail + 64);
    for (i = 0; i < 32; i++) out[i] = (uint8_t)(hash[i / 4] >> (24 - 8 * (i % 4)));
}

#define MAX_DIGESTS 32
struct digest { uint32_t step, offset, bytes; };
static struct digest digests[MAX_DIGESTS];
static int ndigests;
static unsigned long digests_written;
static FILE *digest_out;
static uint8_t *digest_buffer;

// digest plan: one "STEP OFFSET BYTES" line per region (offsets in the window)
static void read_digests(const char *path, int nsteps, uint32_t window)
{
    FILE *f = fopen(path, "r");
    uint32_t largest = 0;
    struct digest d;
    if (!f) fail(path);
    while (fscanf(f, "%u %u %u", &d.step, &d.offset, &d.bytes) == 3) {
        if (ndigests == MAX_DIGESTS || d.step >= (uint32_t)nsteps || !d.bytes || (uint64_t)d.offset + d.bytes > window) {
            errno = EINVAL;
            fail("digest plan");
        }
        digests[ndigests++] = d;
        if (d.bytes > largest) largest = d.bytes;
    }
    fclose(f);
    if (!ndigests) { errno = EINVAL; fail("digest plan is empty"); }
    digest_buffer = malloc(largest);
    if (!digest_buffer) fail("digest buffer");
}

// the regions a finished step wrote: copied out of the uncached pool, hashed, appended to DIGEST_OUT
static void digest_step(int step, const uint8_t *win)
{
    uint8_t out[32];
    int k;
    if (!digest_out) return;
    __sync_synchronize();
    for (k = 0; k < ndigests; k++)
        if (digests[k].step == (uint32_t)step) {
            memcpy(digest_buffer, win + digests[k].offset, digests[k].bytes);
            sha256(digest_buffer, digests[k].bytes, out);
            if (fwrite(out, 1, sizeof(out), digest_out) != sizeof(out)) fail("digest write");
            digests_written++;
        }
}

int main(int argc, char **argv)
{
    const char *pool_path, *image_path, *plan_path, *input_path, *expected_path, *output_path;
    unsigned long frames, warmup, repeat, timeout_ms, r, n = 0, mismatched_runs = 0, mismatched_frames = 0, faulted = 0;
    unsigned long chunk = 0, statics = 0;
    int fields;
    uint32_t shell_base, window, ddr_offset, in_offset, in_bytes_frame, out_offset, out_bytes_frame, weight_offset;
    uint32_t deadline, status, faults_seen = 0, window_offset, dma_base, gpio_base, feeder_base, beat_bytes, inflight;
    double ops_per_frame;
    size_t image_bytes, in_bytes, out_bytes, i;
    uint8_t *image, *input, *expected, *pool, *saved;
    struct pool_info info;
    struct step steps[MAX_STEPS];
    int nsteps, pool_fd, mem_fd, image_intact = 1;
    static uint64_t times[MAX_TIMES], sorted[MAX_TIMES], starts[MAX_TIMES];
    // per plan step over the measured repeats: engine cycles (engine steps) or wall time in ns (stream steps)
    static uint64_t step_sum[MAX_STEPS];
    static unsigned long step_count[MAX_STEPS];
    uint64_t total = 0, total_all = 0, engine_busy = 0, stream_busy = 0, compare_ns = 0;
    unsigned long compares = 0;
    int stopped = 0;
    double pace_high = 0.0, pace_low = 0.0, pause_s = 0.0;
    uint64_t repeat_temp_at = 0;
    struct timespec poll_sleep = {0, 0};
    struct rusage usage0, usage1;
    uint64_t wall0 = 0, wall1 = 0;
    unsigned long pauses = 0, interruptions = 0, excluded = 0, retries = 0;
    struct sigaction stop_action;

    if (argc != 25) {
        fprintf(stderr, "usage: %s POOL IMAGE PLAN INPUT EXPECTED OUTPUT FRAMES OPS_PER_FRAME WARMUP REPEAT TIMEOUT_MS "
                        "SHELL_BASE WINDOW_BYTES DDR_OFFSET IN_OFFSET IN_BYTES OUT_OFFSET OUT_BYTES WEIGHT_OFFSET "
                        "DEADLINE_CYCLES WINDOW_OFFSET DMA_BASE GPIO_BASE FEEDER_BASE:BEAT_BYTES:INFLIGHT[:CHUNK]\n", argv[0]);
        return 64;
    }
    pool_path = argv[1]; image_path = argv[2]; plan_path = argv[3]; input_path = argv[4]; expected_path = argv[5];
    output_path = argv[6]; frames = strtoul(argv[7], NULL, 10); ops_per_frame = strtod(argv[8], NULL);
    warmup = strtoul(argv[9], NULL, 10); repeat = strtoul(argv[10], NULL, 10); timeout_ms = strtoul(argv[11], NULL, 10);
    shell_base = (uint32_t)strtoul(argv[12], NULL, 0); window = (uint32_t)strtoul(argv[13], NULL, 0);
    ddr_offset = (uint32_t)strtoul(argv[14], NULL, 0); in_offset = (uint32_t)strtoul(argv[15], NULL, 0);
    in_bytes_frame = (uint32_t)strtoul(argv[16], NULL, 0); out_offset = (uint32_t)strtoul(argv[17], NULL, 0);
    out_bytes_frame = (uint32_t)strtoul(argv[18], NULL, 0); weight_offset = (uint32_t)strtoul(argv[19], NULL, 0);
    deadline = (uint32_t)strtoul(argv[20], NULL, 0); window_offset = (uint32_t)strtoul(argv[21], NULL, 0);
    dma_base = (uint32_t)strtoul(argv[22], NULL, 0); gpio_base = (uint32_t)strtoul(argv[23], NULL, 0);
    fields = sscanf(argv[24], "%i:%u:%u:%lu:%lu", (int *)&feeder_base, &beat_bytes, &inflight, &chunk, &statics);
    if (fields < 3) { errno = EINVAL; fail("feeder"); }
    if (fields == 3) chunk = frames;
    if (statics > 1 || (statics && (frames != inflight || chunk != frames))) { errno = EINVAL; fail("static inputs"); }
    if (!frames || ops_per_frame <= 0 || !repeat || frames * repeat > MAX_TIMES || !timeout_ms || !window || (window & 4095)
        || !chunk || chunk > frames
        || (ddr_offset & 4095) || (window_offset & 4095) || !inflight || inflight > MAX_INFLIGHT || !beat_bytes
        || in_offset + (uint64_t)in_bytes_frame > window || out_offset + (uint64_t)out_bytes_frame > window
        || weight_offset < ddr_offset || weight_offset > window) {
        errno = EINVAL;
        fail("arguments");
    }
    memset(&stop_action, 0, sizeof(stop_action));
    stop_action.sa_handler = on_stop;
    if (sigaction(SIGTERM, &stop_action, NULL) || sigaction(SIGINT, &stop_action, NULL)) fail("signal handler");
    phase("load");
    image = load(image_path, &image_bytes);
    input = load(input_path, &in_bytes);
    expected = load(expected_path, &out_bytes);
    nsteps = read_plan(plan_path, steps);
    if (image_bytes > window || image_bytes < weight_offset || in_bytes != frames * in_bytes_frame
        || out_bytes != frames * out_bytes_frame) { errno = EINVAL; fail("sizes"); }
    for (i = 0; i < (size_t)nsteps; i++)
        if ((steps[i].kind == 'S' || steps[i].kind == 'C') && (ddr_offset + (uint64_t)steps[i].src + steps[i].in_bytes > window
                                     || ddr_offset + (uint64_t)steps[i].dst + steps[i].out_bytes > window
                                     || steps[i].out_bytes % beat_bytes || steps[i].in_bytes >= (1u << 23)
                                     || steps[i].out_bytes >= (1u << 23))) { errno = EINVAL; fail("stream step bounds"); }
    if (getenv("DIGEST_PLAN") || getenv("DIGEST_OUT")) {
        if (!getenv("DIGEST_PLAN") || !getenv("DIGEST_OUT") || inflight != 1 || statics || warmup || repeat != 1) {
            errno = EINVAL;
            fail("digest mode: DIGEST_PLAN and DIGEST_OUT, one frame in flight, no static inputs, no warmup, one repeat");
        }
        read_digests(getenv("DIGEST_PLAN"), nsteps, window);
        digest_out = fopen(getenv("DIGEST_OUT"), "wb");
        if (!digest_out) fail(getenv("DIGEST_OUT"));
    }

    phase("pool");
    pool_fd = strncmp(pool_path, "fd:", 3) ? open(pool_path, O_RDWR) : atoi(pool_path + 3);
    if (pool_fd < 0 || ioctl(pool_fd, POOL_INFO, &info)) fail(pool_path);
    if ((uint64_t)window_offset + (uint64_t)inflight * window > info.output_offset || (info.dma_address & 4095)
        || info.dma_address + info.bytes > 0x40000000ull) { errno = ENOSPC; fail("pool size"); }
    pool = mmap(NULL, info.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, 0);
    if (pool == MAP_FAILED) fail("mmap pool");
    saved = malloc(out_bytes);
    if (!saved) fail("output buffer");

    phase("registers");
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) fail("/dev/mem");
    shell = map_registers(mem_fd, shell_base);
    dma = map_registers(mem_fd, dma_base);
    gpio = map_registers(mem_fd, gpio_base);
    feeder = feeder_base ? map_registers(mem_fd, feeder_base) : NULL;   // 0: a stream model without set port
    {
        // the run-time GRU aligner form (ZIP_MODE=ADDR:0|1, route engine_feed sign_free 2): set while nothing
        // streams, read back
        const char *z = getenv("ZIP_MODE");
        if (z && *z) {
            char *end;
            unsigned long addr = strtoul(z, &end, 0), value = *end == ':' ? strtoul(end + 1, &end, 0) : 2;
            if (!addr || (addr & 0xFFF) || *end || value > 1) { errno = EINVAL; fail("ZIP_MODE=ADDR:0|1"); }
            volatile uint32_t *zip = map_registers(mem_fd, (uint32_t)addr);
            wr(zip, 0, (uint32_t)value);
            if ((rd(zip, 0) & 1u) != value) { errno = EIO; fail("ZIP_MODE did not read back"); }
        }
    }
    {
        const char *g = getenv("GATE_STEPS");
        if (g && *g) {
            unsigned long v[4] = {0, 0, 0, 0};
            char *end = (char *)g;
            int k, ok = 1;
            for (k = 0; k < 4 && ok; k++) {
                v[k] = strtoul(end, &end, 0);
                ok = *end == (k < 3 ? ':' : '\0');
                if (k < 3) end++;
            }
            unsigned long addr = v[0], run = v[1], eng = v[2], str = v[3];
            if (!ok || !addr || (eng & ~run) || (str & ~run) || (eng & str) || !eng) {
                errno = EINVAL;
                fail("GATE_STEPS: ADDR:RUN:ENGINE:STREAM with ENGINE and STREAM inside RUN, apart, ENGINE not empty");
            }
            gates = map_registers(mem_fd, (uint32_t)addr);
            gate_run = (uint32_t)run; gate_engine = (uint32_t)eng; gate_stream = (uint32_t)str;
            gate_now = gate_run;
            wr(gates, 0, gate_run);
            atexit(gates_restore);
        }
        const char *d = getenv("ISLAND_DUTY");
        if (d && *d) {
            island_duty = (unsigned)strtoul(d, NULL, 10);
            if (!gates || !island_duty || island_duty > 255) { errno = EINVAL; fail("ISLAND_DUTY 1..255 needs GATE_STEPS"); }
            gates[2] = island_duty;
            atexit(duty_off);
        }
        const char *od = getenv("OVERLAP_DUTY");
        if (od && *od) {
            overlap_duty = (unsigned)strtoul(od, NULL, 10);
            if (!gates || !overlap_duty || overlap_duty > 255) { errno = EINVAL; fail("OVERLAP_DUTY 1..255 needs GATE_STEPS"); }
            if (!island_duty) atexit(duty_off);
        }
        const char *ei = getenv("ECC_INJECT");
        if (ei && *ei) {
            ecc_inject = (int)strtol(ei, NULL, 10);
            if (ecc_inject < 1 || ecc_inject > 3) { errno = EINVAL; fail("ECC_INJECT 1 (one bit), 2 (two bits) or 3 (TMR)"); }
        }
        const char *sp = getenv("SOFT_PWM");
        if (sp && *sp) {
            soft_pwm = (int)strtol(sp, NULL, 10);
            if (!gates || soft_pwm != 1) { errno = EINVAL; fail("SOFT_PWM=1 needs GATE_STEPS (and a dithered SOFT_START)"); }
            if (!island_duty) atexit(duty_off);
        }
    }
    if (rd(shell, S_ID) != SHELL_ID) { errno = ENODEV; fail("shell id"); }
    if (!hold(C_SOFT_RESET | C_CLEAR, &status)) { errno = EIO; fail("engine did not quiesce"); }
    if (!reset_dma()) { errno = EIO; fail("dma reset"); }
    if (rd(gpio, 0x8) != 0) { errno = EIO; fail("framing counter not idle"); }

    phase("image");
    for (i = 0; i < inflight; i++) {
        uint8_t *win = pool + window_offset + i * window;
        // the image, then zeros over the rest of the window (the pool is uncached: every byte written or read back costs;
        // 10/28: the old pass wrote the window twice and read the image about eight times over, 2 s of every run)
        memcpy(win, image, image_bytes);
        if (window > image_bytes) memset(win + image_bytes, 0, window - image_bytes);
        __sync_synchronize();
        if (i == 0) {
            // the output compares' NEON paths must see one changed byte anywhere: checked on the first window's image
            // copy (uncached, before a static input goes in) against the image. Every byte of the first 256 (all lanes
            // of two 128-byte steps), the bytes around the 4 KB edge and the far tail are flipped in turn; the faster of
            // the two static compares is taken (timed on the first MB). The flips are undone and the whole image is
            // read back once below with the compare taken.
            int (*ways[3])(const uint8_t *, const uint8_t *, size_t) = {pool_differs, pool_compare2, pool_compare4};
            size_t span = image_bytes < 4096 + 64 ? image_bytes : 4096 + 64;
            size_t timed = image_bytes < (1u << 20) ? image_bytes : (1u << 20);
            size_t edge[6] = {4031, 4032, 4095, 4096, 4097, span - 1}, k, w;
            double rate[3] = {0, 0, 0};
            for (w = 0; w < 3; w++) {
                uint64_t t0 = now_ns();
                if (ways[w](win, image, timed)) { errno = EIO; fail("pool compare: equal regions differ"); }
                rate[w] = (double)timed / 1e6 / ((double)(now_ns() - t0) / 1e9);
                for (k = 0; k < 256 + 6 + 1; k++) {
                    size_t at = k < 256 ? k : k < 256 + 6 ? edge[k - 256] : image_bytes - 1;
                    int seen;
                    if (at >= image_bytes) continue;
                    win[at] ^= 0x10;
                    __sync_synchronize();
                    seen = ways[w](win, image, image_bytes < span + 64 ? image_bytes : span + 64);
                    if (at == image_bytes - 1) seen = ways[w](win, image, image_bytes);
                    win[at] ^= 0x10;
                    __sync_synchronize();
                    if (!seen) { errno = EIO; fail("pool compare missed a changed byte"); }
                }
            }
            if (rate[2] > rate[1]) { pool_compare = pool_compare4; compare_way = 4; }
            fprintf(stderr, "F_ASTRA_HYBRID_POOL_COMPARE one %.1f two %.1f four %.1f MB/s, using %d\n", rate[0], rate[1],
                    rate[2], compare_way);
        }
        if (pool_compare(win, image, image_bytes)) { errno = EIO; fail("window image readback"); }
        if (i == 0) {
            if (getenv("POOL_BENCH") && *getenv("POOL_BENCH")) pool_bench(win, image, image_bytes);
            if (statics) {
                // the static check-and-fill pass (pool_check_fill) on the output region: an equal region passes and
                // is left all 0xA5, and one changed byte at the edges of its 128-byte steps or at the end shows
                uint8_t *o = win + out_offset, *fill = malloc(out_bytes_frame);
                size_t n4 = out_bytes_frame / 4, at[11] = {0, 63, 64, 127, 128, 4095, n4, 2 * n4 - 1, 2 * n4, 3 * n4 + 31,
                                                       out_bytes_frame - 1};
                uint64_t c0;
                size_t k;
                if (!fill) fail("malloc");
                memset(fill, 0xA5, out_bytes_frame);
                memcpy(o, expected, out_bytes_frame);
                __sync_synchronize();
                c0 = now_ns();
                if (pool_check_fill(o, expected, out_bytes_frame)) { errno = EIO; fail("check-fill: equal region differs"); }
                __sync_synchronize();
                c0 = now_ns() - c0;
                if (memcmp(o, fill, out_bytes_frame)) { errno = EIO; fail("check-fill left the region unfilled"); }
                if (getenv("POOL_BENCH") && *getenv("POOL_BENCH")) {
                    fprintf(stderr, "F_ASTRA_HYBRID_POOL_BENCH check_fill %.1f MB/s\n",
                            (double)out_bytes_frame / 1e6 / ((double)c0 / 1e9));
                    pool_bench_fill(o, out_bytes_frame);
                }
                for (k = 0; k < 11; k++) {
                    if (at[k] >= out_bytes_frame) continue;
                    memcpy(o, expected, out_bytes_frame);
                    o[at[k]] ^= 0x10;
                    __sync_synchronize();
                    if (!pool_check_fill(o, expected, out_bytes_frame)) { errno = EIO; fail("check-fill missed a changed byte"); }
                }
                free(fill);
            }
        }
        if (statics) {
            memcpy(win + in_offset, input + (size_t)i * in_bytes_frame, in_bytes_frame);
            __sync_synchronize();
            if (memcmp(win + in_offset, input + (size_t)i * in_bytes_frame, in_bytes_frame)) { errno = EIO; fail("static input readback"); }
        }
    }
    wr(shell, S_BASE, (uint32_t)(info.dma_address + window_offset));
    wr(shell, S_WINDOW, window);
    wr(shell, S_DEADLINE, deadline);
    wr(shell, S_WDT, WDT_CYCLES);
    wr(shell, S_CTRL, C_WDT);
    if (!wait_status(ST_ISOLATED | ST_RESET, 0, 100000000ull, &status)) { errno = EIO; fail("release"); }
    if (rd(shell, E_ID) != ENGINE_ID) { errno = ENODEV; fail("engine id"); }
    wr(shell, E_DDR, ddr_offset);

    if (getenv("THERMAL_PACE_HIGH_C") && getenv("THERMAL_PACE_LOW_C")) {
        pace_high = strtod(getenv("THERMAL_PACE_HIGH_C"), NULL);
        pace_low = strtod(getenv("THERMAL_PACE_LOW_C"), NULL);
        if (!(pace_low > 0.0 && pace_low < pace_high && pace_high <= 100.0) || die_temp_c() < 0.0) {
            errno = EINVAL;
            fail("thermal pacing marks or die temperature");
        }
    }
    if (getenv("POLL_SLEEP_US")) {
        long us = strtol(getenv("POLL_SLEEP_US"), NULL, 10);
        if (us < 0 || us > 100000) { errno = EINVAL; fail("POLL_SLEEP_US 0..100000"); }
        poll_sleep.tv_sec = us / 1000000; poll_sleep.tv_nsec = (us % 1000000) * 1000;
    }
    if (getenv("GATE_KEEP_ENGINE") && *getenv("GATE_KEEP_ENGINE")) {
        keep_engine = atoi(getenv("GATE_KEEP_ENGINE"));
        if (keep_engine < 0 || keep_engine > 2) { errno = EINVAL; fail("GATE_KEEP_ENGINE 0, 1 (through a frame) or 2 (hold)"); }
        if (keep_engine && !gates) { errno = EINVAL; fail("GATE_KEEP_ENGINE needs GATE_STEPS"); }
    }
    if (getenv("SOFT_START") && *getenv("SOFT_START")) {
        unsigned long us = 0, steps = 0, mask = 0, from = 0;
        int got = sscanf(getenv("SOFT_START"), "%lu:%lu:%li:%lu", &us, &steps, (long *)&mask, &from);
        if ((got != 3 && got != 4) || us < 10 || us > 100000 || steps < 1 || steps > 1000 || !gates || !mask
            || (mask & ~(unsigned long)gate_engine) || from > 95 || (from && steps != 1)) {
            errno = EINVAL;
            fail("SOFT_START=RAMP_US:STEPS:MASK[:FROM] (10..100000 us, 1 (dither) or 2..1000 steps, MASK inside the engine "
                 "bits of GATE_STEPS, FROM 0..95 percent with dither)");
        }
        soft_ramp_ns = (uint64_t)us * 1000u; soft_steps = (unsigned)steps; soft_mask = (uint32_t)mask;
        soft_from = (unsigned)from;
    }
    if (getenv("GATE_GAP_MASK") && *getenv("GATE_GAP_MASK")) {
        unsigned long v = strtoul(getenv("GATE_GAP_MASK"), NULL, 0);
        if (!gates || keep_engine != 2 || (v & gate_run) || v > 0xFFFFFFFFul) {
            errno = EINVAL;
            fail("GATE_GAP_MASK: gates outside the run mask, with GATE_KEEP_ENGINE=2");
        }
        gap_mask = (uint32_t)v;
    }
    if (getenv("C_ENGINE_DELAY_US") && *getenv("C_ENGINE_DELAY_US")) {
        long us = strtol(getenv("C_ENGINE_DELAY_US"), NULL, 10);
        if (us < 0 || us > 100000) { errno = EINVAL; fail("C_ENGINE_DELAY_US 0..100000"); }
        c_delay_ns = (uint64_t)us * 1000u;
    }
    if (getenv("REPEAT_REST_US") && *getenv("REPEAT_REST_US")) {
        long us = strtol(getenv("REPEAT_REST_US"), NULL, 10);
        if (us < 0 || us > 1000000) { errno = EINVAL; fail("REPEAT_REST_US 0..1000000"); }
        rest_ns = (uint64_t)us * 1000u;
    }
    if (getenv("POLL_QUIET_US") && *getenv("POLL_QUIET_US")) {
        long us = strtol(getenv("POLL_QUIET_US"), NULL, 10);
        if (us < 0 || us > 100000) { errno = EINVAL; fail("POLL_QUIET_US 0..100000"); }
        quiet_ns = (uint64_t)us * 1000u;
    }
    if (getenv("STATIC_CHECK_EVERY") && *getenv("STATIC_CHECK_EVERY")) {
        long every = strtol(getenv("STATIC_CHECK_EVERY"), NULL, 10);
        if (every < 1 || every > 1000000) { errno = EINVAL; fail("STATIC_CHECK_EVERY 1..1000000"); }
        check_every = (unsigned long)every;
    }
    if (getenv("STEP_GAP_US") && *getenv("STEP_GAP_US")) {
        long us = strtol(getenv("STEP_GAP_US"), NULL, 10);
        if (us < 0 || us > 100000) { errno = EINVAL; fail("STEP_GAP_US 0..100000"); }
        gap_ns = (uint64_t)us * 1000u;
    }
    if (getenv("PACE_GOPS") && *getenv("PACE_GOPS")) {
        double gops = strtod(getenv("PACE_GOPS"), NULL);
        if (!(gops > 0.0 && gops < 10000.0)) { errno = EINVAL; fail("PACE_GOPS > 0"); }
        pace_period_ns = (uint64_t)(ops_per_frame / gops);       // ops / (G ops/s) = ns
    }
    phase("run");
    getrusage(RUSAGE_SELF, &usage0);
    wall0 = now_ns();
    for (r = 0; r < warmup + repeat; r++) {
        struct slot slots[MAX_INFLIGHT];
        if (ecc_inject && r == warmup) {
            engine_on();
            wr(shell, S_INJECT, (0xF1A5u << 16) | (ecc_inject == 1 ? 8u : ecc_inject == 2 ? 16u : 32u));
        }
        if (statics && r == 0) {
            // static inputs: fresh output regions (outside the timed span), so a missing write cannot pass; later
            // repeats find them filled by the previous repeat's check (pool_check_fill)
            for (i = 0; i < inflight; i++) memset(pool + window_offset + i * window + out_offset, 0xA5, out_bytes_frame);
            __sync_synchronize();
        }
        if (rest_ns) {
            struct timespec ts = {(time_t)(rest_ns / 1000000000ull), (long)(rest_ns % 1000000000ull)};
            nanosleep(&ts, NULL);
        }
        long next = 0, completed = 0, chunk_end = (long)chunk;
        int engine_slot = -1, stream_slot = -1;
        uint32_t dones = rd(shell, S_DONES);
        uint64_t run_start = now_ns(), engine_t0 = 0, stream_t0 = 0, last_beat = run_start;
        uint64_t engine_busy0 = engine_busy, stream_busy0 = stream_busy, paused = 0, temp_checked = run_start;
        uint64_t pace_next = run_start, quiet_until = 0;
        unsigned long bad = 0;
        int hot = 0, interrupted = 0;
        for (i = 0; i < inflight; i++) {
            slots[i].frame = -1; slots[i].busy = 0;
        }
        if (pace_high > 0.0 && chunk < frames && die_temp_c() >= pace_low) {
            // chunked: the first chunk starts below the low mark as well (the cooling is not timed)
            uint64_t pause_start = now_ns();
            double cooled = cool_below(pace_low);
            paused += now_ns() - pause_start;
            pause_s += (double)(now_ns() - pause_start) / 1e9;
            pauses++;
            temp_checked = now_ns();
            fprintf(stderr, "F_ASTRA_HYBRID_CHUNK_COOL run=%lu completed=0 temp_c=%.2f\n", r, cooled);
        }
        while (completed < (long)frames) {
            uint64_t t = now_ns();
            int progress = 0;
            if (stop_requested) {
                stopped = 1;
                break;
            }
            if (pace_high > 0.0 && !hot && t - temp_checked > 500000000ull) {
                temp_checked = t;
                if (die_temp_c() >= pace_high) {
                    hot = 1;
                    fprintf(stderr, "F_ASTRA_HYBRID_THERMAL_STOP run=%lu completed=%ld\n", r, completed);
                }
            }
            if (hot && engine_slot < 0 && stream_slot < 0) {
                // every started step has finished: cool with the engine and the stream idle, then go on
                uint64_t pause_start = now_ns();
                double cooled = cool_below(pace_low);
                paused += now_ns() - pause_start;
                pause_s += (double)(now_ns() - pause_start) / 1e9;
                pauses++;
                hot = 0;
                interrupted = 1;
                temp_checked = now_ns();
                fprintf(stderr, "F_ASTRA_HYBRID_THERMAL_RESUME run=%lu temp_c=%.2f\n", r, cooled);
                continue;
            }
            if (completed == chunk_end && chunk_end < (long)frames) {
                // a chunk end: every frame of the chunk finished, nothing runs; cool before the next chunk
                chunk_end = completed + (long)chunk < (long)frames ? completed + (long)chunk : (long)frames;
                if (pace_high > 0.0 && die_temp_c() >= pace_low) {
                    uint64_t pause_start = now_ns();
                    double cooled = cool_below(pace_low);
                    paused += now_ns() - pause_start;
                    pause_s += (double)(now_ns() - pause_start) / 1e9;
                    pauses++;
                    temp_checked = now_ns();
                    fprintf(stderr, "F_ASTRA_HYBRID_CHUNK_COOL run=%lu completed=%ld temp_c=%.2f\n", r, completed, cooled);
                    continue;
                }
            }
            // refill idle slots with the next frames of the chunk; the input reaches the slot's window in pieces (below)
            for (i = 0; i < inflight; i++)
                if (!hot && slots[i].frame < 0 && next < chunk_end && (!pace_period_ns || t >= pace_next)) {
                    if (pace_period_ns)          // on schedule: the next start one period later; behind: from now
                        pace_next = t - pace_next < pace_period_ns ? pace_next + pace_period_ns : t + pace_period_ns;
                    slots[i].frame = next++; slots[i].step = 0; slots[i].busy = 0; slots[i].started = now_ns();
                    slots[i].copied = statics ? in_bytes_frame : 0; slots[i].checked = 0; slots[i].differs = 0;
                    slots[i].cpart = 0;
                }
            {
                // one piece of input copy per pass: the oldest frame whose input is not complete
                int best = -1;
                for (i = 0; i < inflight; i++)
                    if (slots[i].frame >= 0 && slots[i].copied < in_bytes_frame
                        && (best < 0 || slots[i].frame < slots[best].frame)) best = (int)i;
                if (best >= 0) {
                    struct slot *sl = &slots[best];
                    uint8_t *win = pool + window_offset + (size_t)best * window;
                    uint32_t part = in_bytes_frame - sl->copied < PIECE_BYTES ? in_bytes_frame - sl->copied : PIECE_BYTES;
                    memcpy(win + in_offset + sl->copied, input + (size_t)sl->frame * in_bytes_frame + sl->copied, part);
                    sl->copied += part;
                    progress = 1;
                    if (sl->copied == in_bytes_frame) __sync_synchronize();
                }
            }
            // engine: completion, then the next engine step of the frame waiting longest
            if (engine_slot >= 0) {
                status = rd(shell, S_STATUS);
                if (status & ST_ISOLATED) {
                    fprintf(stderr, "F_ASTRA_HYBRID_ENGINE_ABORTED run=%lu frame=%ld step=%d faults=0x%x\n", r,
                            slots[engine_slot].frame, slots[engine_slot].step, rd(shell, S_FAULTS));
                    hold(0, &status);
                    return 3;
                }
                if (!(status & ST_RUNNING) && (status & ST_DONE) && rd(shell, S_DONES) == dones + 1) {
                    dones++;
                    {
                        uint32_t es = rd(shell, E_STATUS);
                        if (es & E_ST_ECC_CORRECTED) ecc_corrected_steps++;
                        if (es & E_ST_ECC_UNCORRECTABLE) ecc_uncorrectable_steps++;
                    }
                    progress = 1;
                    engine_busy += now_ns() - engine_t0;
                    overlap_update(0, stream_slot >= 0);
                    if (r >= warmup) {
                        step_sum[slots[engine_slot].step] += rd(shell, E_CYC_LO) | ((uint64_t)rd(shell, E_CYC_HI) << 32);
                        step_count[slots[engine_slot].step]++;
                    }
                    if (steps[slots[engine_slot].step].kind == 'C' && --slots[engine_slot].cpart) engine_slot = -1;
                    else {
                        if (steps[slots[engine_slot].step].kind == 'E')
                            digest_step(slots[engine_slot].step, pool + window_offset + (size_t)engine_slot * window);
                        slots[engine_slot].step++; slots[engine_slot].busy = 0; engine_slot = -1;
                    }
                }
            }
            if (engine_slot < 0 && !hot && (!gap_ns || now_ns() >= gap_until)) {
                int best = -1;
                for (i = 0; i < inflight; i++)
                    if (slots[i].frame >= 0 && slots[i].copied == in_bytes_frame && !slots[i].busy && slots[i].step < nsteps
                        && (steps[slots[i].step].kind == 'E' || (steps[slots[i].step].kind == 'C' && stream_slot < 0))
                        && (best < 0 || slots[i].frame < slots[best].frame)) best = (int)i;
                if (best >= 0) {
                    engine_on();
                    if (steps[slots[best].step].kind == 'C') {
                        gate_write(gate_now | gate_stream);
                        // the stream model's DMA first (its output and the hidden codes), then the engine feeding it
                        struct step *s = &steps[slots[best].step];
                        uint32_t base = (uint32_t)(info.dma_address + window_offset + (uint64_t)best * window) + ddr_offset;
                        wr(gpio, 0x0, s->out_bytes / beat_bytes);
                        wr(dma, S2MM_CR, 1);
                        wr(dma, MM2S_CR, 1);
                        wr(dma, S2MM_DA, base + s->dst);
                        wr(dma, S2MM_LEN, s->out_bytes);
                        stream_t0 = now_ns();
                        wr(dma, MM2S_SA, base + s->src);
                        wr(dma, MM2S_LEN, s->in_bytes);
                        stream_slot = best; slots[best].cpart = 2;
                        // C_ENGINE_DELAY_US (diagnostic): the engine starts that long after the stream model's DMA
                        if (c_delay_ns) spin_until(now_ns() + c_delay_ns);
                    }
                    wr(shell, S_BASE, (uint32_t)(info.dma_address + window_offset + (uint64_t)best * window));
                    wr(shell, E_PROG, steps[slots[best].step].program_offset);
                    wr(shell, S_HEARTBEAT, 1);
                    engine_t0 = now_ns();
                    wr(shell, E_CTRL, 1);
                    overlap_update(1, stream_slot >= 0 || steps[slots[best].step].kind == 'C');
                    if (quiet_ns && steps[slots[best].step].kind == 'C') quiet_until = engine_t0 + quiet_ns;
                    if (soft_ramp_ns && soft_steps == 1 && soft_pwm) {
                        // the ramp through the island PWM: the duty rises from FROM (of the run duty, or of the full
                        // clock) as the ease-out, one write per step, the PS asleep between the writes
                        uint64_t s0 = now_ns(), step = soft_ramp_ns / SOFT_PWM_WRITES;
                        unsigned full = island_duty ? island_duty : 255;
                        double from = soft_from ? soft_from / 100.0 : 0.05;
                        for (unsigned k = 0; k < SOFT_PWM_WRITES; k++) {
                            double x = (double)k / (double)SOFT_PWM_WRITES;
                            double share = 1.0 - (1.0 - from) * (1.0 - x) * (1.0 - x);
                            unsigned d = (unsigned)(full * share + 0.5);
                            gates[2] = d ? d : 1;
                            soft_pwm_writes_total++;
                            uint64_t at = s0 + step * (k + 1), t = now_ns();
                            if (at > t) {
                                struct timespec ts = {0, (long)(at - t)};
                                nanosleep(&ts, NULL);
                            }
                        }
                        gates[2] = island_duty;
                        soft_ramps++;
                    } else if (soft_ramp_ns && soft_steps == 1) {
                        // dithered ramp (STEPS 1): the island's on share rises over the ramp as a smoothstep
                        // x^2 (3 - 2x) (flat at both ends: the linear ramp's corner at full rate still kicked the
                        // supply), decided anew at every pass (a few microseconds) by a first-order sigma-delta, so
                        // the supply sees the average rising instead of the island's full on/off steps of slot PWM.
                        // With a start share (FROM) the ramp opens at that share (a small step at a low level) and
                        // ends in an ease-out 1 - (1 - FROM)(1 - x)^2, flat at full rate: a longer, gentler top for
                        // the same lost time (the share missing over the ramp is (1 - FROM) / 3 of it)
                        // The share is read from the clock every 16 passes only, so a pass is a few register-free
                        // operations and the island toggles at MHz rates (a clock read every pass toggled about every
                        // 6 us, and the supply still saw some of the pattern)
                        uint64_t s0 = now_ns(), tt = s0;
                        double acc = 0.0, share = soft_from / 100.0, from = soft_from / 100.0;
                        int on = 1;
                        unsigned pass = 0;
                        while (tt < s0 + soft_ramp_ns) {
                            if (!(pass++ & 15)) {
                                double x;
                                tt = now_ns();
                                x = tt >= s0 + soft_ramp_ns ? 1.0 : (double)(tt - s0) / (double)soft_ramp_ns;
                                share = soft_from ? 1.0 - (1.0 - from) * (1.0 - x) * (1.0 - x) : x * x * (3.0 - 2.0 * x);
                            }
                            acc += share;
                            int want = acc >= 1.0;
                            if (want) acc -= 1.0;
                            if (want != on) { gate_write(want ? gate_now | soft_mask : gate_now & ~soft_mask); on = want; }
                        }
                        if (!on) gate_write(gate_now | soft_mask);
                        soft_ramps++;
                    } else if (soft_ramp_ns) {
                        // the started engine's island off and on in slots, the on share rising to 1 (no shell access)
                        uint64_t slot = soft_ramp_ns / soft_steps, s0 = now_ns();
                        for (unsigned k = 1; k < soft_steps; k++) {
                            uint64_t at = s0 + (k - 1) * slot;
                            spin_until(at + slot * k / soft_steps);
                            gate_write(gate_now & ~soft_mask);
                            spin_until(at + slot);
                            gate_write(gate_now | soft_mask);
                        }
                        soft_ramps++;
                    }
                    slots[best].busy = 1; engine_slot = best;
                    progress = 1;
                }
            }
            // stream accelerator: completion, then the next stream step
            if (stream_slot >= 0) {
                uint32_t mm2s = rd(dma, MM2S_SR), s2mm = rd(dma, S2MM_SR);
                if ((mm2s | s2mm) & SR_ERRORS) {
                    fprintf(stderr, "F_ASTRA_HYBRID_DMA_FAILED run=%lu frame=%ld mm2s_sr=%u s2mm_sr=%u\n", r,
                            slots[stream_slot].frame, mm2s, s2mm);
                    reset_dma();
                    hold(0, &status);
                    return 3;
                }
                if ((mm2s & SR_IOC) && (s2mm & SR_IOC)) {
                    progress = 1;
                    wr(dma, MM2S_SR, SR_IOC | SR_ERR_IRQ);
                    wr(dma, S2MM_SR, SR_IOC | SR_ERR_IRQ);
                    stream_busy += now_ns() - stream_t0;
                    overlap_update(engine_slot >= 0, 0);
                    if (steps[slots[stream_slot].step].kind == 'C') {
                        // a concurrent step reports the engine's cycles; it ends with the later of its two parts
                        if (--slots[stream_slot].cpart) stream_slot = -1;
                        else { slots[stream_slot].step++; slots[stream_slot].busy = 0; stream_slot = -1; }
                    } else {
                        if (r >= warmup) {
                            step_sum[slots[stream_slot].step] += now_ns() - stream_t0;
                            step_count[slots[stream_slot].step]++;
                        }
                        digest_step(slots[stream_slot].step, pool + window_offset + (size_t)stream_slot * window);
                        slots[stream_slot].step++; slots[stream_slot].busy = 0; stream_slot = -1;
                        if (gap_ns) { gap_until = now_ns() + gap_ns; gaps++; }
                    }
                }
            }
            if (stream_slot < 0 && !hot) {
                int best = -1;
                for (i = 0; i < inflight; i++)
                    if (slots[i].frame >= 0 && slots[i].copied == in_bytes_frame && !slots[i].busy && slots[i].step < nsteps
                        && steps[slots[i].step].kind == 'S'
                        && (best < 0 || slots[i].frame < slots[best].frame)) best = (int)i;
                if (best >= 0) {
                    struct step *s = &steps[slots[best].step];
                    uint32_t base = (uint32_t)(info.dma_address + window_offset + (uint64_t)best * window) + ddr_offset;
                    gate_write(gate_now | gate_stream);
                    // the previous operation took its set beat; a refused write (sticky bit 1) stops the run
                    if (feeder) {
                        if (rd(feeder, 0x4) & 1) { errno = EIO; fail("set feeder still holds a beat"); }
                        wr(feeder, 0x0, s->set);
                        if (rd(feeder, 0x4) & 2) { errno = EIO; fail("set feeder refused the set index"); }
                    }
                    wr(gpio, 0x0, s->out_bytes / beat_bytes);
                    wr(dma, S2MM_CR, 1);
                    wr(dma, MM2S_CR, 1);
                    wr(dma, S2MM_DA, base + s->dst);
                    wr(dma, S2MM_LEN, s->out_bytes);
                    stream_t0 = now_ns();
                    wr(dma, MM2S_SA, base + s->src);
                    wr(dma, MM2S_LEN, s->in_bytes);
                    slots[best].busy = 1; stream_slot = best;
                    progress = 1;
                }
            }
            {
                // finished frames: one piece of output compare per pass (the oldest finished frame); a complete compare
                // records the frame and frees its slot
                int best = -1;
                for (i = 0; i < inflight; i++)
                    if (slots[i].frame >= 0 && slots[i].step == nsteps && (best < 0 || slots[i].frame < slots[best].frame))
                        best = (int)i;
                if (best >= 0) progress = 1;
                if (best >= 0 && statics) {
                    // static inputs: compared after the timed span (below)
                    engine_on();
                    if (rd(shell, S_FAULTS)) { faults_seen |= rd(shell, S_FAULTS); faulted++; }
                    slots[best].frame = -1;
                    completed++;
                } else if (best >= 0) {
                    struct slot *sl = &slots[best];
                    uint8_t *win = pool + window_offset + (size_t)best * window;
                    size_t base = (size_t)sl->frame * out_bytes_frame + sl->checked;
                    uint32_t part = out_bytes_frame - sl->checked < PIECE_BYTES ? out_bytes_frame - sl->checked : PIECE_BYTES;
                    if (!sl->checked) __sync_synchronize();
                    sl->differs |= memcmp(win + out_offset + sl->checked, expected + base, part) != 0;
                    if (r == 0) memcpy(saved + base, win + out_offset + sl->checked, part);   // the warmup output (or the first)
                    sl->checked += part;
                    if (sl->checked == out_bytes_frame) {
                        bad += (unsigned long)sl->differs;
                        engine_on();
                        if (rd(shell, S_FAULTS)) { faults_seen |= rd(shell, S_FAULTS); faulted++; }
                        sl->frame = -1;
                        completed++;
                    }
                }
            }
            int in_flight = 0;
            for (i = 0; i < inflight; i++) in_flight |= slots[i].frame >= 0;
            if (gates) {
                // the blocks without a running step stop until their next step (or shell access); with
                // GATE_KEEP_ENGINE the engine's clocks stay on while a frame is in flight, so a stream step's end and
                // the next engine step are not one load step (the 12 V input rang after LayerNorm -> pw starts)
                // (GATE_KEEP_ENGINE=2 holds every gate of the run on, between frames as well: the gates then only
                // serve the soft start, e.g. for short back-to-back GRU frames whose every start was a load step)
                uint32_t m = gate_now;
                if (keep_engine == 2) m = gate_run | (in_flight ? 0u : gap_mask);
                else {
                    if (engine_slot < 0 && !(keep_engine && in_flight)) m &= ~gate_engine;
                    else if (keep_engine && in_flight) m |= gate_engine;
                    if (stream_slot < 0) m &= ~gate_stream;
                }
                gate_write(m);
            }
            if (!progress && pace_period_ns && !in_flight && next < chunk_end && t < pace_next) {
                // no frame in flight until the next paced start: sleep up to it (at most 10 ms, the heartbeat and
                // the thermal check stay on time); a frame in flight keeps the polling pace (a step gap, say)
                uint64_t wait = pace_next - t < 10000000ull ? pace_next - t : 10000000ull;
                struct timespec ts = {(time_t)(wait / 1000000000ull), (long)(wait % 1000000000ull)};
                nanosleep(&ts, NULL);
            } else if (!progress && t < quiet_until) {
                // POLL_QUIET_US: a concurrent step takes about as long every frame; the PS sleeps through most of it
                // instead of waking every poll (at most 10 ms at a time, the heartbeat and the thermal check go on)
                uint64_t wait = quiet_until - t < 10000000ull ? quiet_until - t : 10000000ull;
                struct timespec ts = {(time_t)(wait / 1000000000ull), (long)(wait % 1000000000ull)};
                nanosleep(&ts, NULL);
            } else if (!progress && (poll_sleep.tv_sec || poll_sleep.tv_nsec)) nanosleep(&poll_sleep, NULL);
            if (t - last_beat > 1000000ull && (gate_now & gate_engine) == gate_engine) {
                wr(shell, S_HEARTBEAT, 1);
                last_beat = t;
            }
            if (t - run_start - paused > (uint64_t)timeout_ms * 1000000ull * frames) {
                gates_restore();
                fprintf(stderr, "F_ASTRA_HYBRID_TIMEOUT run=%lu completed=%ld engine_slot=%d stream_slot=%d pc=%u\n", r,
                        completed, engine_slot, stream_slot, rd(shell, E_PC));
                reset_dma();
                hold(0, &status);
                return 3;
            }
        }
        gates_restore();                // the shell is read below and at the next repeat's start
        if (stopped) {
            // the unfinished repeat does not count; the final block quiesces the engine and the DMA
            engine_busy = engine_busy0;
            stream_busy = stream_busy0;
            fprintf(stderr, "F_ASTRA_HYBRID_STOPPED run=%lu completed=%ld\n", r, completed);
            break;
        }
        {
            uint64_t elapsed = now_ns() - run_start - paused;
            total_all += elapsed;
            if (r >= warmup && !interrupted) {
                starts[n] = run_start;
                times[n++] = elapsed;
                total += elapsed;
            }
            if (statics && check_every && r >= warmup && r + 1 < warmup + repeat && (r - warmup) % check_every) {
                // STATIC_CHECK_EVERY (diagnostic): this repeat's outputs are not compared (counted as unchecked)
                unchecked++;
            } else if (statics) {
                // frame i ran in window i
                uint64_t c0 = now_ns();
                __sync_synchronize();
                for (i = 0; i < frames; i++) {
                    uint8_t *win = pool + window_offset + i * window;
                    if (r == 0) memcpy(saved + i * out_bytes_frame, win + out_offset, out_bytes_frame);
                    // compared and filled with 0xA5 for the next repeat in one pass
                    if (pool_check_fill(win + out_offset, expected + i * out_bytes_frame, out_bytes_frame)) bad++;
                }
                __sync_synchronize();
                compare_ns += now_ns() - c0;
                compares++;
            }
            fprintf(stderr, "F_ASTRA_HYBRID_RUN run=%lu frames=%lu ns=%llu bad=%lu interrupted=%d\n", r, frames,
                    (unsigned long long)elapsed, bad, interrupted);
        }
        if (r == 0) {
            FILE *o = fopen(output_path, "wb");
            if (!o || fwrite(saved, 1, out_bytes, o) != out_bytes || fclose(o)) fail(output_path);
        }
        mismatched_frames += bad;
        mismatched_runs += bad != 0;
        interruptions += (unsigned long)interrupted;
        if (interrupted && r >= warmup) {
            // an interrupted measured repeat has no time: run it again while retries remain (not for a single repeat)
            if (repeat > 1 && retries < MAX_THERMAL_RETRIES) {
                retries++;
                r--;
            } else {
                excluded++;
                retries = 0;
            }
        } else {
            retries = 0;
        }
        // between repeats the die is read at most every 100 ms (a read each repeat kept the PL idle longer)
        if (pace_high > 0.0 && r + 1 < warmup + repeat && now_ns() - repeat_temp_at >= 100000000ull
            && (repeat_temp_at = now_ns(), die_temp_c() >= pace_high)) {
            uint64_t pause_start = now_ns();
            double t;
            pauses++;
            fprintf(stderr, "F_ASTRA_HYBRID_THERMAL_PAUSE run=%lu\n", r);
            t = cool_below(pace_low);
            pause_s += (double)(now_ns() - pause_start) / 1e9;
            fprintf(stderr, "F_ASTRA_HYBRID_THERMAL_RESUME run=%lu temp_c=%.2f\n", r, t);
            if (stop_requested) {
                stopped = 1;
                break;
            }
        }
    }
    for (i = 0; i < inflight; i++) {
        uint8_t *win = pool + window_offset + i * window;
        image_intact &= !memcmp(win, image, ddr_offset)
                        && !memcmp(win + weight_offset, image + weight_offset, image_bytes - weight_offset);
    }
    {
        uint32_t final_faults = rd(shell, S_FAULTS);
        wall1 = now_ns();
        getrusage(RUSAGE_SELF, &usage1);
        double cpu_s = (double)(usage1.ru_utime.tv_sec - usage0.ru_utime.tv_sec + usage1.ru_stime.tv_sec
                                - usage0.ru_stime.tv_sec)
                       + (double)(usage1.ru_utime.tv_usec - usage0.ru_utime.tv_usec + usage1.ru_stime.tv_usec
                                  - usage0.ru_stime.tv_usec) / 1e6;
        double wall_s = (double)(wall1 - wall0) / 1e9;
        // means over the measured repeats that finished (all of them unless stopped)
        double mean_frame_s = n ? (double)total / n / frames / 1e9 : 0.0;
        int held = hold(0, &status), halted = reset_dma();
        memcpy(sorted, times, n * sizeof(times[0]));
        qsort(sorted, n, sizeof(sorted[0]), cmp_u64);
        if (!n) sorted[0] = 0;
        printf("{\"schema\":\"f-astra/moderntcn-hybrid-run/v1\",\"track\":\"experimental: unified ModernTCN engine segments with "
               "a FINN streaming accelerator between them\",\"frames\":%lu,\"warmup\":%lu,\"repeat\":%lu,\"inflight\":%u,"
               "\"chunk_frames\":%lu,\"static_inputs\":%s,\"steps\":%d,\"window_bytes\":%u,\"window_offset\":%u,\"pool_dma_address\":%llu,\"ops_per_frame\":%.0f,"
               "\"mean_frame_ns\":%.1f,\"mean_run_ns\":%.1f,\"min_run_ns\":%llu,\"median_run_ns\":%llu,\"max_run_ns\":%llu,"
               "\"gops_mean\":%.6f,\"frames_per_second_mean\":%.4f,\"engine_busy_fraction\":%.4f,"
               "\"stream_busy_fraction\":%.4f,\"mismatched_runs\":%lu,\"mismatched_frames\":%lu,\"faulted_frames\":%lu,"
               "\"faults_seen\":%u,\"final_faults\":%u,\"image_intact\":%s,\"held_after\":%s,\"dma_halted_after\":%s,"
               "\"measured_repeats\":%lu,\"stopped\":%s,\"thermal_pauses\":%lu,\"thermal_pause_s\":%.1f,"
               "\"thermal_interrupted_repeats\":%lu,\"timing_excluded_repeats\":%lu,"
               "\"timer\":\"CLOCK_MONOTONIC_RAW from the first step to the last completion of every measured repeat\","
               "\"times_ns\":[",
               frames, warmup, repeat, inflight, chunk, statics ? "true" : "false", nsteps, window, window_offset, (unsigned long long)info.dma_address,
               ops_per_frame, mean_frame_s * 1e9, n ? (double)total / n : 0.0, (unsigned long long)sorted[0],
               (unsigned long long)sorted[n / 2], (unsigned long long)sorted[n ? n - 1 : 0],
               mean_frame_s > 0 ? ops_per_frame / mean_frame_s / 1e9 : 0.0, mean_frame_s > 0 ? 1.0 / mean_frame_s : 0.0,
               (double)engine_busy / (double)(total_all ? total_all : 1), (double)stream_busy / (double)(total_all ? total_all : 1),
               mismatched_runs, mismatched_frames, faulted, faults_seen, final_faults, image_intact ? "true" : "false",
               held ? "true" : "false", halted ? "true" : "false", n, stopped ? "true" : "false", pauses, pause_s,
               interruptions, excluded);
        for (i = 0; i < n; i++) printf("%s%llu", i ? "," : "", (unsigned long long)times[i]);
        // the measured repeats' starts (CLOCK_MONOTONIC_RAW), to place a power trace's samples inside the repeats
        printf("],\"starts_ns\":[");
        for (i = 0; i < n; i++) printf("%s%llu", i ? "," : "", (unsigned long long)starts[i]);
        // per plan step means over the measured repeats: "E" engine cycles (the engine's own counter), "S" DMA ns
        printf("],\"step_means\":[");
        for (i = 0; i < (size_t)nsteps; i++)
            printf("%s{\"kind\":\"%c\",\"mean\":%.1f,\"unit\":\"%s\"}", i ? "," : "", steps[i].kind,
                   step_count[i] ? (double)step_sum[i] / step_count[i] : 0.0, steps[i].kind == 'S' ? "ns" : "cycles");
        // inject_left: the shell's pending INJECT bits after the run (an injection the engine consumed is cleared)
        printf("],\"ecc_inject\":%d,\"ecc_corrected_steps\":%lu,\"ecc_uncorrectable_steps\":%lu,\"inject_left\":%u",
               ecc_inject, ecc_corrected_steps, ecc_uncorrectable_steps, rd(shell, S_INJECT) & 0xFFu);
        printf(",\"digest_mode\":%s,\"digest_regions\":%d,\"digests_written\":%lu,\"ps_cpu_s\":%.3f,\"ps_wall_s\":%.3f,"
               "\"ps_cpu_fraction\":%.4f,\"poll_sleep_us\":%ld,\"gate_steps\":%s,\"gate_writes\":%lu,\"pace_period_ns\":%llu,"
               "\"step_gap_ns\":%llu,\"step_gaps\":%lu,\"gate_keep_engine\":%d,\"soft_start_ns\":%llu,"
               "\"soft_start_steps\":%u,\"soft_start_mask\":%lu,\"soft_start_ramps\":%lu,\"static_compare_ns\":%.0f,\"repeat_rest_ns\":%llu,\"poll_quiet_ns\":%llu,\"compare_way\":%d,\"static_unchecked\":%lu,\"soft_start_from\":%u,\"soft_pwm\":%d,\"soft_pwm_writes\":%lu,\"overlap_duty\":%u,\"overlap_writes\":%lu}\n",
               digest_out ? "true" : "false", ndigests, digests_written,
               cpu_s, wall_s, wall_s > 0 ? cpu_s / wall_s : 0.0,
               (long)poll_sleep.tv_sec * 1000000L + poll_sleep.tv_nsec / 1000L, gates ? "true" : "false", gate_writes,
               (unsigned long long)pace_period_ns, (unsigned long long)gap_ns, gaps, keep_engine,
               (unsigned long long)soft_ramp_ns, soft_steps, (unsigned long)soft_mask, soft_ramps,
               compares ? (double)compare_ns / compares : 0.0, (unsigned long long)rest_ns,
               (unsigned long long)quiet_ns, compare_way, unchecked, soft_from, soft_pwm, soft_pwm_writes_total, overlap_duty, overlap_writes);
        if (faulted || final_faults) return 5;
    }
    if (digest_out && fclose(digest_out)) fail("digest close");
    if (stopped) return 3;
    if (digest_out && digests_written != frames * (unsigned long)ndigests) return 4;
    return (mismatched_runs || !image_intact || n + excluded != repeat) ? 4 : 0;
}
