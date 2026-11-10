// F-ASTRA board FM service (FM-1 of docs/f-astra-fault-management.md) for a DirectRTL harness with the
// FM guard (f_astra_fm_guard.v at GUARD_BASE). Modes, each a separate run of one configuration file:
//   initial     health check and golden input-output consistency (Initial Mode) -> standby or safehold
//   throughput  initial, then the whole corpus as one transfer with every guard check on (FM cost)
//   mission     initial, then input batches with FM detection and the v2 recovery ladder per item,
//               recovery counts kept in a state file (L2-G1-01), Safehold with Health Telemetry
//   swdt        L0-H3-01 alone (no PL): the PS system watchdog armed, fed while the service is healthy
//               (swdt_healthy_s seconds), then the feeding stops (the injection): the SWDT resets the board
//   supervise:MODE  L1-S1-01/02: MODE (initial, throughput or mission) runs in a worker process, this binary with the
//               same configuration, and this process supervises it (see supervise())
//   sem         L0-H2-01/03 on a bitstream with the SEM controller (sem_base, f_astra_sem.v): the PS hands the ICAP
//               to the controller (devcfg PCAP_PR = 0), waits for its observation state, injects one bit at sem_lfa
//               (word sem_word, bit sem_bit) and checks the repair (L0-H2-01); with sem_uncorrectable two bits of
//               one word, which the controller cannot repair (L0-H2-03: Safehold, the PL is reconfigured from storage
//               by the session or the ground). The controller's monitor text goes to `output`. A frame the controller
//               does not scan takes the bit without a detection: the bit is flipped back and the next frame, sem_lfa_step
//               further, is tried (sem_tries frames at most); the uncorrectable test uses the frame that was repaired.
//               L0-H2-02: without classification (no essential-bits data) every correction counts as essential: the
//               AI output is held, the PL IP reset and the model verified on its golden frames. The run first goes
//               through the first model's setup and Initial Mode like a mission.
//               L0-H2-03 (v2: whole PL reconfiguration, a System Reset when it fails): the output held, the PL
//               written again from its stored image (F_ASTRA_PL_FIRMWARE, a name in /lib/firmware that the session
//               exports; after an uncorrectable upset INIT_B stays low, so PROG_B is pulsed first), the DMA and guard
//               set up again and the golden frames verified. Without a stored image the run ends in Safehold.
// PS system watchdog (L0-H3-01, swdt_timeout_s > 0, off by default): /dev/watchdog is fed with every heartbeat, so
// a hung or dead service resets the board; every orderly exit (done, safehold, setup failure) disarms it first (magic
// close). A killed service is not an orderly exit: the board resets, which is the point of the item.
// Faults can be injected per batch to test detection and recovery: inject=BATCH:KIND[,...] with KIND one of
// guard-range guard-format guard-stop guard-skip-input heartbeat-stop dma-late-s2mm dma-short timeout crc stale
// length late result pll parity temperature, and -1:golden for the Initial golden reference.
// FM-2 checks in this service: XADC temperature/supply before each batch (L0-H4-04 -> Safehold), PLL lock before
// each batch (L0-H4-02 -> PL IP reset and retry) and the Healthy Bit across Initial runs (L2-G1-02).
// Within the current platform (no DDR ECC, Linux in the secure state) the FM data this service relies on (the
// golden input-output) lives in MMU read-only pages with a CRC: a CRC scrub before each batch stands in for DDR
// ECC (L0-H4-01: detect, safe output, reload from storage, re-verify) and a blocked write to those pages stands
// in for a TrustZone violation (L0-H4-03: the MMU blocks it, the cause is recorded, the operation is isolated).
// Injections: scrub (flip a protected bit outside the MMU check) and protect (attempt a write).
// L0-H4-01 parity (R3, image r10): the L2 (PL310) and OCM parity must be on at Initial (else Safehold); before each
// batch new L2 raw parity bits (cleared after reading) or OCM parity interrupts classify L0-H4-01 (safe output, reload,
// reverify). No parity error can be made in hardware: the injection parity sets PARRD in the value read.
// Engine-fed stream model (engine_base set; the GRU tail v2 behind the aligner, docs/gru-50gops-design.md): the model
// takes its input records from the shared engine's G4 stream and its other input (the hidden codes) from its DMA, so
// every transfer also runs the engine's program on the batch's own window (engine_frames windows of engine_steps steps,
// placed once at engine_pool_offset; a batch, the golden frames and a throughput transfer are one window). The engine
// starts after the model's DMA (the hybrid runner's order). The shell keeps its own FM hooks (run deadline, WDT with a
// period above the heartbeat-stop injection, memory window). Every failed transfer, interface init and PL IP reset
// leaves the engine reset and released; an engine abort or fault while the DMA and the guard saw nothing fails the
// transfer (engine_fault event), one that follows a DMA or guard fault is that fault's consequence and only logged.
// A fault the shell's own FM hooks recorded (FAULTS) classifies the batch by the shell's item when it is the cause
// (docs/moderntcn-engine.md 13.2): deadline L0-H1-03, WDT L0-H3-02, R/B error response and a refused START L0-H4-05,
// access outside the WINDOW L0-H4-03 (the stand-in), bus not drained L1-S2-01. It is the cause when the DMA saw no
// error and the tail's guard at most a frame deadline (the tail starved behind the stopped engine); a guard fault of
// its own (range, format, stall, WDT, stale) stopped the stream first and the engine stalled behind it. Engine
// injections (F5, the engine shell on the board): engine-freeze (frozen read address -> run deadline), engine-wdt
// (a 1000-cycle WDT for the run), engine-read-error and engine-write-error (one R or B error response, shell INJECT)
// and engine-window (a 4 KiB WINDOW for the run).
// Supervision injections (inject=BATCH:KIND, a mission worker only, its first inject_attempts attempts, default 1):
// hang (the worker stops itself with SIGSTOP: no heartbeat), crash (abort), memory and fd (allocate or open until the
// supervisor's rlimit_data_mb or rlimit_nofile stops it: exit 4).
// OBC reporting (F4, SIR01600): the FM events the OBC needs (detected, recovered, safehold, the limits, the ground
// reset, Initial and mission ends) are also appended to /run/f-astra-fm/events.jsonl and every health record is copied
// to /run/f-astra-fm/health.json; the CAN CMD node (tools/te0715-csp-v1-node.c) forwards the events as TM 0xC020 and
// answers the FM health TC from the copy. A ground reset sent over CAN (TC 0x0021) is the SD file
// f-astra-fm/ground-reset: the next Initial applies it as ground_reset=1 and removes it.
// Set models (F7, a FINN model with a set-index port such as the PD LayerNorm sets, docs/f-astra-fault-management.md
// 7.1): feeder_base names the model's set feeder (f_astra_set_feeder.v); every transfer is one frame (one whole
// LayerNorm operation) and writes its set index first: a batch's from its header (reserved field), the golden
// frame's from golden_set. A beat still waiting or a refused write fails the transfer (the PL IP reset clears the
// feeder with the accelerator). The feeder sits in the accelerator's reset domain: while the guard holds the
// accelerator in reset (a WDT hold, guard STATUS bit 8) its AXI-Lite port does not answer and an access would stall
// the PS on the bus (a board hang, 6.64.0), so the transfer fails without touching it. output_pool_offset places the outputs in the pool's arena instead of its output region
// (frames larger than that region); the inputs then end below it.
// Exit codes: 0 done, 3 safehold, 2 setup failure, 4 resource exhausted (a worker), 64 usage.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/watchdog.h>
#include <time.h>
#include <unistd.h>

#include "f_astra_fm_core.h"
#include "f_astra_fm_regs.h"

struct pool_info { uint64_t dma_address; uint32_t bytes; uint32_t output_offset; };
#define POOL_INFO _IOR('F', 1, struct pool_info)
#define APERTURE 0x10000u
#include "f_astra_pl_io.h"          // rd, wr, map_registers: /dev/mem or the secure monitor
#define SLCR_BASE 0xF8000000u
#define SWDT_BASE 0xF8005000u
#define SLCR_ARM_PLL_CTRL 0x100u
#define SLCR_PLL_STATUS 0x10Cu
#define SLCR_ARM_CLK_CTRL 0x120u
#define SLCR_REBOOT_STATUS 0x258u
enum { MM2S_CR = 0x00, MM2S_SR = 0x04, MM2S_SA = 0x18, MM2S_LEN = 0x28,
       S2MM_CR = 0x30, S2MM_SR = 0x34, S2MM_DA = 0x48, S2MM_LEN = 0x58 };
enum { SR_HALTED = 0x1, SR_DEC_ERR = 0x40, SR_IOC = 0x1000, SR_ERR_IRQ = 0x4000 };
#define MAX_INJECT 32
// engine shell (mtcn_shell.v) and engine registers, as the hybrid runner uses them
enum { E_CTRL = 0x00, E_PROG = 0x08, E_DDR = 0x0C, E_ID = 0x1C };
enum { S_ID = 0x80, S_CTRL = 0x84, S_STATUS = 0x88, S_FAULTS = 0x8C, S_DEADLINE = 0x90, S_WDT = 0x94, S_HEARTBEAT = 0x98,
       S_WINDOW = 0x9C, S_DONES = 0xA4, S_INJECT = 0xB8, S_ENGINE_STATUS = 0xBC, S_BASE = 0xC8 };
// shell FAULTS bits (f_astra_mtcn_shell.v) and the INJECT key
enum { SF_DEADLINE = 1 << 0, SF_WDT = 1 << 1, SF_R_ERROR = 1 << 2, SF_B_ERROR = 1 << 3, SF_WIDTH = 1 << 4,
       SF_DESCRIPTOR = 1 << 5, SF_WINDOW_R = 1 << 6, SF_WINDOW_W = 1 << 7, SF_STATE = 1 << 8, SF_DRAIN = 1 << 9 };
#define SHELL_INJECT_KEY 0xF1A5u
enum { ST_RUNNING = 1, ST_DONE = 2, ST_ISOLATED = 4, ST_RESET = 8, ST_DRAINED = 16 };
enum { C_HOLD = 1, C_WDT = 4, C_SOFT_RESET = 0x100, C_CLEAR = 0x200 };
#define SHELL_ID 0x4D545348u
#define ENGINE_ID 0x4D54434Eu
#define ENGINE_WDT_CYCLES (1u << 30)   // 8 s at 133 MHz: above the heartbeat-stop injection's pause (2 guard WDT periods)
#define ENGINE_GRACE_NS 50000000ull    // after a clean DMA transfer the engine has finished too (its stream fed it)

struct config {
    char pool[128], input[256], golden_input[256], golden_expected[256], output[256], telemetry[256], health[256],
        state[256], inject[512], engine_windows[256], spool[256], spool_out[256], hybrid_plan[256], hybrid_image[256],
        golden_manifest[256], pl_windows[256], serve_yield[512], warm_marker[256];
    uint32_t guard_base, dma_base, gpio_base, frames_total, batch_frames, in_frame_bytes, out_elems_per_frame, out_elem_bytes,
        out_elem_bits, out_frame_bytes,
        out_signed, out_beat_bytes, golden_frames, deadline_cycles, wdt_cycles, stall_cycles, batch_timeout_ms,
        inference_deadline_us, recovery_limit, repeat, clock_khz, ground_reset, boot_limit, swdt_timeout_s, swdt_healthy_s,
        heartbeat_deadline_ms, restart_limit, rlimit_data_mb, rlimit_nofile, inject_attempts, feeder_base,
        output_pool_offset, golden_set, sem_base, sem_lfa, sem_word, sem_bit, sem_uncorrectable, sem_lfa_step, sem_tries,
        sem_reconfigure_fail,
        engine_base, engine_window_bytes, engine_ddr_offset, engine_program_offset, engine_steps, engine_frames,
        engine_pool_offset, engine_deadline, idle_check_ms, golden_every_s, serve_feed_ms, state_every,
        engine_rows_bytes, engine_block_offset, tz, tz_probe, gru_step, gru_x_lane, gru_nx, gru_hidden,
        step_check_every, secure_golden, golden_slot, secure_safehold, secure_watch, hybrid_in_offset, hybrid_out_offset,
        engine_weight_offset, hybrid_windows, serve_idle_exit_ms, serve_turn_ms, telemetry_max_bytes, warm_golden_s,
        poll_sleep_us, clock_gates_addr, clock_gates_on, clock_gates_all, soft_start_us, soft_start_mask, soft_start_from;
    int64_t min, max, safe;
    double ops_per_frame, temp_limit_c, vccint_min, vccint_max;
};
struct injection { long batch; char kind[24]; };

static struct config cfg;
static struct injection inject[MAX_INJECT];
static int n_inject;
static volatile uint32_t *dma, *gpio, *guard, *slcr, *shell, *feeder;
static volatile uint32_t *cgate;               // the accelerator clock gate register (route_models clock_gates)
static volatile uint32_t *l2c, *ocm;          // L0-H4-01 parity (R3): the PL310 L2 and OCM registers
static struct fm_parity parity_last;          // the registers at the last parity check
static uint32_t out_region, out_capacity, in_limit;   // the output region (pool offset, bytes) and the input limit
static uint32_t transfer_set;                       // the set index of the next transfer (set models)
static uint32_t engine_frame, engine_dones;   // the window of the next transfer; the shell's DONES count
static int engine_fault;                      // the last transfer's engine aborted or faulted on a clean DMA transfer
static uint32_t engine_fault_bits;            // the shell FAULTS of the last transfer's engine run (0: none)
static const char *engine_inject;             // the engine injection of the next run (F5), or NULL
static uint64_t tz_in_addr, tz_out_addr;      // a TrustZone probe transfer's DMA addresses (0: the pool's)
static int skip_env_checks;                   // a GRU step between its environment and memory checks
static uint8_t *pool;
static struct pool_info info;
static FILE *telemetry;
static int swdt_fd = -1;
static int swdt_monitor;                      // the secure monitor owns the PS watchdog (non-secure kernel, T2b)
static uint64_t swdt_last_feed_unix_ns, swdt_last_diag_ns;
static int swdt_last_feed_errno, swdt_worker_attempt;
static char swdt_attempt_id[80];
static int heartbeat_fd = -1;                 // a supervised worker's heartbeat pipe (F_ASTRA_FM_HEARTBEAT_FD)
static FILE *obc_events;                      // the OBC's FM event list (F4), or NULL
#define OBC_DIR "/run/f-astra-fm"
#define GROUND_RESET_FILE "/media/sd-mmcblk0p1/f-astra-fm/ground-reset"
#define RESET_RECORD_DIR "/media/sd-mmcblk0p1/f-astra-fm"     // what a System Reset leaves for the next boot
static int worker_attempt;                    // a supervised worker's attempt (F_ASTRA_FM_ATTEMPT), 0 unsupervised
#define MAX_HYBRID_WINDOWS 4            // a hybrid model's frames in flight (hybrid_windows)
static struct fm_state state;
static uint64_t t0;
static char last_cause[64] = "";

// Accelerator clock gates (route_models clock_gates; clock_gates_addr in the configuration): this model's clocks run
// (clock_gates_on), every other gated accelerator stops while the service runs; all run again when it ends, and the
// mask is written again after a PL reload (the reloaded register starts with every bit 1).
static void clock_gates_write(uint32_t mask)
{
    if (cgate) wr(cgate, 0x0, mask);
}

static void clock_gates_restore(void)
{
    clock_gates_write(cfg.clock_gates_all);
}

static uint64_t now_ns(void);
// Soft start of an engine step (soft_start_us, soft_start_mask: the engine clock island's gate bits, a single-pumped
// engine; soft_start_from percent): right after the start the island's clock is dithered by a first-order sigma-delta,
// its on share rising as a smoothstep (or from soft_start_from as an ease-out) to 1 over the ramp, as the hybrid
// runner does (directrtl_hybrid_runner.c SOFT_START): a hard engine start rang the 12 V input. No register of the
// engine is touched during the ramp; the island ends switched on.
static void engine_soft_start(void)
{
    if (!cgate || !cfg.soft_start_us) return;
    uint64_t ramp = (uint64_t)cfg.soft_start_us * 1000u, s0 = now_ns(), tt = s0;
    double acc = 0.0, from = cfg.soft_start_from / 100.0, share = from;
    int on = 1;
    unsigned pass = 0;
    while (tt < s0 + ramp) {
        if (!(pass++ & 15)) {
            double x;
            tt = now_ns();
            x = tt >= s0 + ramp ? 1.0 : (double)(tt - s0) / (double)ramp;
            share = cfg.soft_start_from ? 1.0 - (1.0 - from) * (1.0 - x) * (1.0 - x) : x * x * (3.0 - 2.0 * x);
        }
        acc += share;
        int want = acc >= 1.0;
        if (want) acc -= 1.0;
        if (want != on) {
            clock_gates_write(want ? cfg.clock_gates_on : cfg.clock_gates_on & ~cfg.soft_start_mask);
            on = want;
        }
    }
    if (!on) clock_gates_write(cfg.clock_gates_on);
}

// Protected FM data: golden input and expected output in read-only pages, with their CRC32.
struct protected_data { uint8_t *in, *out; size_t in_bytes, out_bytes, in_pages, out_pages; uint32_t in_crc, out_crc; };
static struct protected_data golden;
static sigjmp_buf protect_jump;
static volatile sig_atomic_t protect_armed;
static volatile void *protect_fault;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t unix_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void swdt_disarm(void);
static void swdt_diag_maybe(int force);
static void worker_supervision_injections(long batch);

// FM stage D window mode, warm turns (warm_marker, the scheduler's window folder): the marker is "boot <boot id>" and
// one "<spool> <boot seconds of its last golden pass>" line per model whose last turn ended cleanly. A start takes its
// line out (a turn cut short leaves none: the next start is cold) and a clean stop writes it back. A warm start keeps
// the windows as the window's first (cold) start wrote and checked them (no other model writes them: add_schedule
// keeps the models' pool regions apart; the scrub watches them during serve) and runs the start's golden check only
// when the last one is warm_golden_s old. A Safehold, a setup failure or a PL reload drops the whole marker: every
// model's next start is cold.
static int warm_turn;
static uint64_t warm_golden_at;
static void warm_drop(void) { if (cfg.warm_marker[0]) unlink(cfg.warm_marker); }
static uint64_t boot_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    return (uint64_t)t.tv_sec;
}

// the marker's lines of this boot other than this model's (into `kept`), and this model's golden time if it is there
static int warm_read(char *boot, size_t boot_size, char *kept, size_t kept_size, uint64_t *golden_at)
{
    char line[400], spool[300];
    unsigned long long at;
    size_t used = 0;
    int mine = 0, same = 0;
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    snprintf(boot, boot_size, "boot ");
    if (f) {
        if (!fgets(boot + 5, (int)(boot_size - 5), f)) boot[5] = 0;
        fclose(f);
    }
    boot[strcspn(boot, "\n")] = 0;
    kept[0] = 0;
    if (!(f = fopen(cfg.warm_marker, "r"))) return 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "boot ", 5)) { same = !strcmp(line, boot); continue; }
        if (!same || sscanf(line, "%299s %llu", spool, &at) != 2) continue;
        if (!strcmp(spool, cfg.spool)) { mine = 1; *golden_at = at; continue; }
        if (used + strlen(line) + 2 < kept_size) used += (size_t)snprintf(kept + used, kept_size - used, "%s\n", line);
    }
    fclose(f);
    return mine;
}

static void warm_write(const char *boot, const char *kept, int mine)
{
    FILE *f = fopen(cfg.warm_marker, "w");
    if (!f) return;
    fprintf(f, "%s\n%s", boot, kept);
    if (mine) fprintf(f, "%s %llu\n", cfg.spool, (unsigned long long)warm_golden_at);
    fclose(f);
}

// a serve start: this model's line taken out of the marker; warm when it was there
static void warm_take(void)
{
    char boot[64], kept[4096];
    if (!cfg.warm_marker[0]) return;
    warm_turn = warm_read(boot, sizeof(boot), kept, sizeof(kept), &warm_golden_at);
    warm_write(boot, kept, 0);
}

// a clean serve stop: this model's line back
static void warm_give(void)
{
    char boot[64], kept[4096];
    uint64_t ignored;
    if (!cfg.warm_marker[0]) return;
    warm_read(boot, sizeof(boot), kept, sizeof(kept), &ignored);
    warm_write(boot, kept, 1);
}

// serve mode's test feeder (a forked child; it holds the pool's descriptor and mappings like its parent)
static pid_t serve_feeder;

// 6.213.0: every exit of the service also ends the feeder. A Safehold in serve mode (r36v2 FM, guard594) left it
// feeding, so the next FM runs found the pool busy (EBUSY) until it finished.
static void feeder_stop(void)
{
    if (serve_feeder > 0) {
        kill(serve_feeder, SIGTERM);
        waitpid(serve_feeder, NULL, 0);
        serve_feeder = 0;
    }
}

static void fail(const char *what)
{
    feeder_stop();
    warm_drop();
    fprintf(stderr, "F_ASTRA_FM_SETUP_FAILED %s errno=%d\n", what, errno);
    swdt_disarm();
    exit(2);
}

static int obc_event(const char *ev)
{
    static const char *const list[] = {"detected", "recovered", "safehold", "recovery_limit", "boot_limit", "ground_reset",
                                       "initial_pass", "mission_complete", "secure_release_challenge",
                                       "secure_safehold_released", "secure_release_refused"};
    for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); i++)
        if (!strcmp(ev, list[i])) return 1;
    return 0;
}

// The SD copy of the SWDT diagnostics (FAT, also the boot partition) is kept below SD_DIAG_MAX: past it, the newest
// SD_DIAG_KEEP bytes (from a line start) are written back in place. No rename: on FAT a rename cut by a reset left
// two directory entries with one short name (2026-10-14).
#define SD_DIAG_MAX (256u * 1024u)
#define SD_DIAG_KEEP (128u * 1024u)
static void sd_diag_trim(const char *path)
{
    struct stat st;
    FILE *f;
    char *keep, *start;
    size_t got;
    if (stat(path, &st) || (uint64_t)st.st_size <= SD_DIAG_MAX) return;
    if (!(f = fopen(path, "rb"))) return;
    keep = malloc(SD_DIAG_KEEP + 1);
    if (!keep || fseek(f, (long)(st.st_size - SD_DIAG_KEEP), SEEK_SET)) { free(keep); fclose(f); return; }
    got = fread(keep, 1, SD_DIAG_KEEP, f);
    fclose(f);
    keep[got] = 0;
    start = memchr(keep, '\n', got);              // drop the partial first line
    start = start ? start + 1 : keep + got;
    if ((f = fopen(path, "wb"))) {
        if (fwrite(start, 1, (size_t)(keep + got - start), f) != (size_t)(keep + got - start) || fflush(f)
            || fsync(fileno(f)))
            fprintf(stderr, "F_ASTRA_FM_SWDT_DIAG_SD_TRIM_FAILED errno=%d\n", errno);
        fclose(f);
    }
    free(keep);
}

// A record that keeps growing in operation (always-on service, FM R6): past `limit` bytes the file moves to NAME.1
// (the one before is dropped) and a new one starts. The OBC's CSP node follows a new file (inode, size). A test build
// may set a smaller OBC limit (-DOBC_EVENTS_MAX=...) to see rotations in minutes.
#ifndef OBC_EVENTS_MAX
#define OBC_EVENTS_MAX (4u << 20)
#endif
// Two processes may write one record (supervise: the supervisor and its worker): a handle whose file is no longer
// at `path` (the other one rotated it) moves to the new file instead of rotating again; a failed open keeps the old
// handle, never a closed one.
static FILE *rotated(FILE *f, const char *path, uint32_t limit)
{
    struct stat mine, now;
    char older[600];
    FILE *g;
    if (!f || !limit || fstat(fileno(f), &mine)) return f;
    if (stat(path, &now) || now.st_ino != mine.st_ino || now.st_dev != mine.st_dev) {
        if (!(g = fopen(path, "a"))) return f;
        fclose(f);
        return g;
    }
    if (mine.st_size < (off_t)limit) return f;
    snprintf(older, sizeof(older), "%s.1", path);
    if (rename(path, older) || !(g = fopen(path, "a"))) return f;
    fclose(f);
    return g;
}

static void event(long batch, const char *ev, const char *item, const char *detail)
{
    char line[1024];
    telemetry = rotated(telemetry, cfg.telemetry, cfg.telemetry_max_bytes);   // 0: the whole run (a campaign's record)
    obc_events = rotated(obc_events, OBC_DIR "/events.jsonl", OBC_EVENTS_MAX);
    if (swdt_fd >= 0 && strcmp(ev, "swdt_diag") && strcmp(ev, "swdt_armed")) swdt_diag_maybe(0);
    if (fm_event_json(line, sizeof(line), now_ns() - t0, batch, ev, item, detail, &state) > 0 && telemetry) {
        fprintf(telemetry, "%s\n", line);
        fflush(telemetry);
        if (swdt_fd >= 0) fsync(fileno(telemetry));   // a watchdog reset may follow: keep the record
        if (!strcmp(ev, "swdt_diag")) {
            // A local bundle lives in /run; keep this PS-only diagnostic across a watchdog reset on SD too.
            if (mkdir(RESET_RECORD_DIR, 0755) == 0 || errno == EEXIST) {
                sd_diag_trim(RESET_RECORD_DIR "/swdt-diag.jsonl");
                FILE *sd = fopen(RESET_RECORD_DIR "/swdt-diag.jsonl", "a");
                if (sd) {
                    int dir;
                    if (fprintf(sd, "%s\n", line) < 0 || fflush(sd) || fsync(fileno(sd)))
                        fprintf(stderr, "F_ASTRA_FM_SWDT_DIAG_SD_FAILED errno=%d\n", errno);
                    fclose(sd);
                    dir = open(RESET_RECORD_DIR, O_RDONLY | O_DIRECTORY);
                    if (dir >= 0) { fsync(dir); close(dir); }
                } else fprintf(stderr, "F_ASTRA_FM_SWDT_DIAG_SD_FAILED errno=%d\n", errno);
            } else fprintf(stderr, "F_ASTRA_FM_SWDT_DIAG_SD_FAILED errno=%d\n", errno);
        }
        if (obc_events && obc_event(ev)) { fprintf(obc_events, "%s\n", line); fflush(obc_events); }
    }
}

static int ground_requested(void) { return access(GROUND_RESET_FILE, F_OK) == 0; }

// The persisted state (f_astra_fm_core.c: two slots written in place). A file whose two slots are both damaged cannot
// say whether a Safehold was latched, so it counts as one (a ground reset clears it).
static void load_state(void)
{
    if (fm_state_load(cfg.state, &state) == 0) return;
    state.safehold = 1;
    snprintf(state.cause, sizeof(state.cause), "state-damaged");
    event(-1, "state_damaged", "", cfg.state);
}

// ---------------------------------------------------------------- PS system watchdog (L0-H3-01)
static void swdt_diag_maybe(int force)
{
    const uint64_t period = 5000000000ull;
    uint64_t now = now_ns();
    uint32_t mode = 0, ccr = 0, sr = 0, clk = 0;
    int read_errno = 0;
    char detail[320];
    int mem;
    volatile uint32_t *wdt, *clock_regs;
    if (swdt_fd < 0 || (!force && now - swdt_last_diag_ns < period)) return;
    swdt_last_diag_ns = now;
    if (swdt_monitor) {                            // through the monitor (/dev/f-astra-sec): SWDT and SLCR words
        // a failed read is reported here, not through fail(): this also runs inside fail()'s disarm
        const uint32_t at[4] = {SWDT_BASE, SWDT_BASE + 0x4u, SWDT_BASE + 0xCu, SLCR_BASE + 0x304u};
        uint32_t *to[4] = {&mode, &ccr, &sr, &clk};
        for (int i = 0; i < 4; i++)
            if (pread64(swdt_fd, to[i], 4, (off64_t)at[i]) != 4 && !read_errno) read_errno = errno;
        goto report;
    }
    // These mappings cover only the PS SWDT and SLCR pages. Never read the PL while it may be unavailable.
    mem = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (mem < 0) read_errno = errno;
    else {
        wdt = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mem, 0xF8005000u);
        clock_regs = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mem, 0xF8000000u);
        if (wdt == MAP_FAILED || clock_regs == MAP_FAILED) read_errno = errno;
        else {
            mode = wdt[0];                         // 0xF8005000 SWDT MODE
            ccr = wdt[1];                          // 0xF8005004 SWDT CCR
            sr = wdt[3];                           // 0xF800500C SWDT SR
            clk = clock_regs[0x304u / 4u];         // 0xF8000304 WDT_CLK_SEL
        }
        if (wdt != MAP_FAILED) munmap((void *)wdt, 4096);
        if (clock_regs != MAP_FAILED) munmap((void *)clock_regs, 4096);
        close(mem);
    }
report:
    snprintf(detail, sizeof(detail),
             "attempt_id=%s worker_attempt=%d mode=0x%08x ccr=0x%08x sr=0x%08x wdt_clk_sel=0x%08x "
             "last_feed_unix_ns=%llu last_feed_errno=%d ps_read_errno=%d",
             swdt_attempt_id, swdt_worker_attempt, mode, ccr, sr, clk,
             (unsigned long long)swdt_last_feed_unix_ns, swdt_last_feed_errno, read_errno);
    event(-1, "swdt_diag", "L0-H3-01", detail);
}

static void swdt_arm(void)
{
    char detail[64];
    char boot_id[40] = "unknown";
    int timeout = (int)cfg.swdt_timeout_s;
    if (!cfg.swdt_timeout_s) return;
    FILE *boot = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (boot) { if (!fgets(boot_id, sizeof(boot_id), boot)) strcpy(boot_id, "unknown"); fclose(boot); }
    boot_id[strcspn(boot_id, "\r\n")] = 0;
    snprintf(swdt_attempt_id, sizeof(swdt_attempt_id), "%.8s-%ld-%llu", boot_id, (long)getpid(),
             (unsigned long long)t0);
    swdt_last_feed_unix_ns = 0;
    swdt_last_feed_errno = 0;
    swdt_last_diag_ns = 0;
    swdt_fd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
    if (swdt_fd < 0 && pl_io_secure_monitor()) {
        // non-secure kernel (T2b): the monitor armed the watchdog at boot and keeps its mode; this sets its timeout
        // (CPU_1x 111.1 MHz / 4096 / 4096 per restart-value step, rounded up) and restarts it, through
        // /dev/f-astra-sec. A dup of the device stands for the watchdog descriptor.
        uint32_t crv = (uint32_t)(((uint64_t)timeout * 111111111u / 4096u + 4095u) / 4096u), ccr;
        crv = crv > 1u ? crv - 1u : 1u;
        if (crv > 3975u) crv = 3975u;
        ccr = 0x00920000u | (crv << 2) | 3u;
        swdt_fd = fcntl(pl_io_fd, F_DUPFD_CLOEXEC, 0);
        swdt_monitor = swdt_fd >= 0;               // set first: a failure below disarms through the monitor too
        if (swdt_fd < 0 || pwrite64(swdt_fd, &ccr, 4, (off64_t)(SWDT_BASE + 4u)) != 4) fail("monitor watchdog timeout");
        timeout = (int)(((uint64_t)(crv + 1u) * 4096u * 4096u + 55555555u) / 111111111u);
    }
    if (swdt_fd < 0) fail("/dev/watchdog");
    if (!swdt_monitor && (ioctl(swdt_fd, WDIOC_SETTIMEOUT, &timeout) || ioctl(swdt_fd, WDIOC_GETTIMEOUT, &timeout)))
        fail("watchdog timeout");
    snprintf(detail, sizeof(detail), "timeout_s=%d%s", timeout, swdt_monitor ? " owner=monitor" : "");
    event(-1, "swdt_armed", "L0-H3-01", detail);
    swdt_diag_maybe(1);
}

static void swdt_feed(void)
{
    if (swdt_fd >= 0) {
        uint32_t key = 0x1999u;
        if (swdt_monitor ? pwrite64(swdt_fd, &key, 4, (off64_t)(SWDT_BASE + 8u)) == 4 : ioctl(swdt_fd, WDIOC_KEEPALIVE, 0) == 0) {
            swdt_last_feed_unix_ns = unix_ns();
            swdt_last_feed_errno = 0;
        } else swdt_last_feed_errno = errno;
        swdt_diag_maybe(0);
    }
}

static void swdt_disarm(void)
{
    int monitor = swdt_monitor;
    if (swdt_fd < 0) return;
    swdt_diag_maybe(1);                       // the final successful feed is retained even for a run under 5 s
    if (monitor) {
        // the non-secure world cannot stop the monitor's watchdog: back to its boot timeout (600 s), restarted;
        // whatever runs the board next keeps feeding it. Under the T3c watch that restart needs a heartbeat first
        // (written directly: this also runs inside fail())
        uint32_t ccr = 0x00920000u | (3975u << 2) | 3u, beat = 1;
        if (cfg.secure_watch && pwrite64(swdt_fd, &beat, 4, (off64_t)(cfg.guard_base + FMG_HEARTBEAT)) != 4)
            fprintf(stderr, "F_ASTRA_FM_SWDT_HEARTBEAT_FAILED errno=%d\n", errno);
        if (pwrite64(swdt_fd, &ccr, 4, (off64_t)(SWDT_BASE + 4u)) != 4)
            fprintf(stderr, "F_ASTRA_FM_SWDT_RESTORE_FAILED errno=%d\n", errno);
    } else if (write(swdt_fd, "V", 1) != 1) fprintf(stderr, "F_ASTRA_FM_SWDT_MAGIC_CLOSE_FAILED errno=%d\n", errno);
    close(swdt_fd);
    swdt_fd = -1;
    swdt_monitor = 0;
    swdt_last_diag_ns = 0;
    event(-1, "swdt_disarmed", "L0-H3-01", monitor ? "owner=monitor timeout_s=600" : "");
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

// ---------------------------------------------------------------- configuration
static void read_config(const char *path, int swdt_only)
{
    char line[600], key[64], value[520];
    FILE *f = fopen(path, "r");
    if (!f) fail(path);
    memset(&cfg, 0, sizeof(cfg));
    cfg.guard_base = 0x43C00000u; cfg.dma_base = 0x40400000u; cfg.gpio_base = 0x41200000u;
    cfg.recovery_limit = 3; cfg.batch_timeout_ms = 2000; cfg.temp_limit_c = 100.0; cfg.clock_khz = 133333;
    cfg.out_elems_per_frame = 1;
    cfg.idle_check_ms = 1000; cfg.state_every = 1; cfg.step_check_every = 1; cfg.hybrid_windows = 1;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || sscanf(line, "%63[^=]=%519[^\n]", key, value) != 2) continue;
#define S(k) if (!strcmp(key, #k)) { if (strlen(value) >= sizeof(cfg.k)) { errno = ENAMETOOLONG; fail(key); }                                      memcpy(cfg.k, value, strlen(value) + 1); continue; }
#define U(k) if (!strcmp(key, #k)) { cfg.k = (uint32_t)strtoul(value, NULL, 0); continue; }
#define I(k) if (!strcmp(key, #k)) { cfg.k = strtoll(value, NULL, 0); continue; }
#define D(k) if (!strcmp(key, #k)) { cfg.k = strtod(value, NULL); continue; }
        S(pool) S(input) S(golden_input) S(golden_expected) S(output) S(telemetry) S(health) S(state) S(inject)
        S(engine_windows) S(spool) S(spool_out) S(hybrid_plan) S(hybrid_image) S(golden_manifest) S(pl_windows) S(serve_yield) S(warm_marker) U(warm_golden_s)
        U(hybrid_in_offset) U(hybrid_out_offset)
        U(engine_weight_offset) U(hybrid_windows) U(serve_idle_exit_ms) U(serve_turn_ms) U(telemetry_max_bytes)
        U(poll_sleep_us) U(clock_gates_addr) U(clock_gates_on) U(clock_gates_all)
        U(soft_start_us) U(soft_start_mask) U(soft_start_from)
        U(idle_check_ms) U(golden_every_s) U(serve_feed_ms) U(state_every)
        U(engine_base) U(engine_window_bytes) U(engine_ddr_offset) U(engine_program_offset)
        U(engine_steps) U(engine_frames) U(engine_pool_offset) U(engine_deadline) U(engine_rows_bytes) U(engine_block_offset)
        U(tz) U(tz_probe) U(gru_step) U(gru_x_lane) U(gru_nx) U(gru_hidden) U(step_check_every)
        U(secure_golden) U(golden_slot) U(secure_safehold) U(secure_watch)
        U(guard_base) U(dma_base) U(gpio_base) U(frames_total) U(batch_frames) U(in_frame_bytes) U(out_elems_per_frame) U(out_elem_bytes)
        U(out_elem_bits) U(out_signed) U(out_beat_bytes) U(golden_frames) U(deadline_cycles) U(wdt_cycles)
        U(stall_cycles) U(batch_timeout_ms) U(inference_deadline_us) U(recovery_limit) U(repeat) U(clock_khz)
        U(ground_reset) U(boot_limit) U(swdt_timeout_s) U(swdt_healthy_s)
        U(heartbeat_deadline_ms) U(restart_limit) U(rlimit_data_mb) U(rlimit_nofile) U(inject_attempts)
        U(feeder_base) U(output_pool_offset) U(golden_set) U(sem_base) U(sem_lfa) U(sem_word) U(sem_bit)
        U(sem_uncorrectable) U(sem_lfa_step) U(sem_tries) U(sem_reconfigure_fail)
        I(min) I(max) I(safe) D(ops_per_frame) D(temp_limit_c) D(vccint_min) D(vccint_max)
        fprintf(stderr, "unknown config key %s\n", key);
        errno = EINVAL;
        fail("config");
    }
    fclose(f);
    if (cfg.swdt_timeout_s > 60) { errno = EINVAL; fail("swdt_timeout_s (1..60)"); }
    if (swdt_only) return;
    if (!cfg.batch_frames || !cfg.in_frame_bytes || !cfg.out_elem_bytes || !cfg.out_elem_bits || !cfg.out_beat_bytes
        || !cfg.golden_frames || !cfg.out_elems_per_frame || cfg.out_elem_bits > 8 * cfg.out_elem_bytes || cfg.min > cfg.max)
        { errno = EINVAL; fail("config values"); }
    cfg.out_frame_bytes = cfg.out_elems_per_frame * cfg.out_elem_bytes;
    if (cfg.out_frame_bytes % cfg.out_beat_bytes) { errno = EINVAL; fail("output frame is not whole beats"); }
    if (cfg.engine_base && !cfg.hybrid_plan[0] && (!cfg.engine_windows[0] || !cfg.engine_steps
                            || (cfg.gru_step ? cfg.batch_frames < cfg.engine_steps : cfg.batch_frames != cfg.engine_steps)
                            || cfg.golden_frames != cfg.engine_steps
                            || (!cfg.gru_step && cfg.frames_total != cfg.engine_frames * cfg.engine_steps)
                            || !cfg.engine_window_bytes
                            || cfg.engine_window_bytes % 4096 || cfg.engine_pool_offset % 4096 || !cfg.engine_deadline))
        { errno = EINVAL; fail("engine-fed configuration (one window a batch, the golden frames and every transfer)"); }
    // batches with their engine rows (serve): [engine rows][tail input] of every frame go into the batch window at
    // engine_block_offset, the window's program image and weights stay those of the corpus's first window
    if (cfg.engine_rows_bytes && (!cfg.engine_base || (uint64_t)cfg.engine_block_offset
                                  + (uint64_t)cfg.engine_steps * (cfg.engine_rows_bytes + cfg.in_frame_bytes) > cfg.engine_window_bytes))
        { errno = EINVAL; fail("engine rows in the batches: an engine-fed model, the rows inside one window"); }
    if (cfg.gru_step && (!cfg.engine_rows_bytes || !cfg.gru_nx || !cfg.gru_hidden || cfg.gru_hidden > cfg.gru_x_lane
                         || (cfg.gru_x_lane + cfg.gru_nx) * 2 > cfg.engine_rows_bytes || cfg.gru_hidden * 2 != cfg.in_frame_bytes
                         || cfg.gru_hidden * 2 > cfg.out_frame_bytes || cfg.gru_hidden > 256 || !cfg.step_check_every))
        { errno = EINVAL; fail("GRU step loop: engine rows, h on lanes 0.., x on gru_x_lane.., h' first in the output"); }
    // a hybrid model (FM stage C, the PD hybrid): the engine runs its program's segments and the set model (the feeder
    // stream) the operations between them on one window, one frame after the other
    if (cfg.hybrid_plan[0] && (!cfg.engine_base || !cfg.hybrid_image[0] || !cfg.feeder_base || !cfg.engine_window_bytes
                               || cfg.engine_window_bytes % 4096 || cfg.engine_pool_offset % 4096 || !cfg.engine_deadline
                               || cfg.engine_windows[0] || cfg.gru_step || cfg.engine_rows_bytes
                               || (uint64_t)cfg.hybrid_in_offset + cfg.in_frame_bytes > cfg.engine_window_bytes
                               || (uint64_t)cfg.hybrid_out_offset + cfg.out_frame_bytes > cfg.engine_window_bytes))
        { errno = EINVAL; fail("hybrid configuration (engine shell, window image, set feeder, frame regions in the window)"); }
    if (cfg.engine_weight_offset && !cfg.engine_base)
        { errno = EINVAL; fail("engine_weight_offset: the weights of an engine window (an engine-fed or hybrid model)"); }
    if (cfg.feeder_base && ((cfg.batch_frames != 1 && !cfg.hybrid_plan[0]) || cfg.golden_frames != 1 || cfg.golden_set > 255))
        { errno = EINVAL; fail("set model: one frame a transfer (batch and golden frames 1), set index 0..255"); }
    // a hybrid model's frames in flight (FM stage C pipelining): one window each, a batch of any size
    if (cfg.poll_sleep_us > 100000) { errno = EINVAL; fail("poll_sleep_us: 0 to 100000 microseconds"); }
    if (cfg.clock_gates_addr && (!cfg.clock_gates_all || (cfg.clock_gates_on & ~cfg.clock_gates_all)))
        { errno = EINVAL; fail("clock gates: clock_gates_on must be part of clock_gates_all"); }
    if (cfg.soft_start_us && (!cfg.clock_gates_addr || !cfg.soft_start_mask || (cfg.soft_start_mask & ~cfg.clock_gates_on)
                              || cfg.soft_start_us > 100000 || cfg.soft_start_from > 95))
        { errno = EINVAL; fail("soft start: the engine island's gate bits among clock_gates_on, 1..100000 us, from 0..95 %"); }
    if (cfg.hybrid_windows < 1 || cfg.hybrid_windows > MAX_HYBRID_WINDOWS || (cfg.hybrid_windows > 1 && !cfg.hybrid_plan[0]))
        { errno = EINVAL; fail("hybrid_windows: 1 to 4 windows of a hybrid model"); }
    for (char *tok = strtok(cfg.inject, ","); tok && n_inject < MAX_INJECT; tok = strtok(NULL, ",")) {
        if (sscanf(tok, "%ld:%23s", &inject[n_inject].batch, inject[n_inject].kind) == 2) n_inject++;
    }
}

static int injected(long batch, const char *kind)
{
    for (int i = 0; i < n_inject; i++)
        if (inject[i].batch == batch && !strcmp(inject[i].kind, kind)) return 1;
    return 0;
}

// ---------------------------------------------------------------- L0-H4-01 parity (R3, f_astra_fm_core.h)
static uint32_t parity_reg(void *ctx, uint32_t address)
{
    (void)ctx;
    return address >= FM_L2C_BASE ? rd(l2c, address - FM_L2C_BASE) : rd(ocm, address - FM_OCM_BASE);
}

static uint64_t ocm_irqs(void)
{
    static char text[16384];
    size_t n = 0;
    FILE *f = fopen("/proc/interrupts", "r");
    if (f) { n = fread(text, 1, sizeof(text) - 1, f); fclose(f); }
    text[n] = 0;
    return fm_irq_count(text, "f800c000.ocmc");
}

static void parity_read(struct fm_parity *p)
{
    fm_parity_read(parity_reg, 0, p);
    p->ocm_irqs = ocm_irqs();
}

// New L2 or OCM parity errors since the last check (an event with the registers); the L2 raw parity bits are cleared.
// No parity error can be made in hardware: the injection sets PARRD in the value read, the register is not written.
static unsigned parity_check(long batch, int inject_l2)
{
    struct fm_parity now;
    char text[192];
    unsigned errors;
    parity_read(&now);
    if (now.l2_raw & (FM_L2C_PARRT | FM_L2C_PARRD)) wr(l2c, FM_L2C_INTR_CLEAR, now.l2_raw & (FM_L2C_PARRT | FM_L2C_PARRD));
    if (inject_l2) now.l2_raw |= FM_L2C_PARRD;
    errors = fm_parity_errors(&now, &parity_last);
    if (errors) {
        fm_parity_text(text, sizeof(text), &now);
        event(batch, "parity", fm_names[FM_L0_H4_01], text);
    }
    parity_last = now;
    parity_last.l2_raw = 0;
    return errors;
}

// a supervision injection applies to the worker's first inject_attempts attempts, so a restarted worker recovers
static int supervision_injected(long batch, const char *kind)
{
    return injected(batch, kind) && worker_attempt <= (int)(cfg.inject_attempts ? cfg.inject_attempts : 1);
}

// ---------------------------------------------------------------- PL guard and DMA
static int reset_dma(void)
{
    uint64_t start = now_ns();
    wr(dma, MM2S_CR, 4);
    while (rd(dma, MM2S_CR) & 4)
        if (now_ns() - start > 100000000ull) return 0;
    return (rd(dma, MM2S_SR) & SR_HALTED) && (rd(dma, S2MM_SR) & SR_HALTED);
}

// ---------------------------------------------------------------- engine-fed model
static int engine_on(void) { return cfg.engine_base != 0; }

static int shell_wait(uint32_t mask, uint32_t want, uint64_t limit_ns)
{
    uint64_t start = now_ns();
    while ((rd(shell, S_STATUS) & mask) != want)
        if (now_ns() - start > limit_ns) return 0;
    return 1;
}

// Hold, abort and reset the engine (the shell drains its bus first), clear the faults, then release it: window 0,
// run deadline, WDT and the program's DDR base. Returns 1 when the engine is idle, released and fault-free.
static int engine_reset(void)
{
    if (!engine_on()) return 1;
    wr(shell, S_CTRL, C_HOLD | C_SOFT_RESET | C_CLEAR);
    if (!shell_wait(ST_ISOLATED | ST_RESET | ST_DRAINED, ST_ISOLATED | ST_RESET | ST_DRAINED, 1000000000ull)) return 0;
    wr(shell, S_INJECT, 0);
    wr(shell, S_BASE, (uint32_t)(info.dma_address + cfg.engine_pool_offset));
    wr(shell, S_WINDOW, cfg.engine_window_bytes);
    wr(shell, S_DEADLINE, cfg.engine_deadline);
    wr(shell, S_WDT, ENGINE_WDT_CYCLES);
    wr(shell, S_HEARTBEAT, 1);
    wr(shell, S_CTRL, C_WDT);
    if (!shell_wait(ST_ISOLATED | ST_RESET, 0, 100000000ull) || rd(shell, E_ID) != ENGINE_ID) return 0;
    wr(shell, E_DDR, cfg.engine_ddr_offset);
    engine_dones = rd(shell, S_DONES);
    return rd(shell, S_FAULTS) == 0;
}

static void engine_injection(void);

// Start the engine's program on window `frame` (BASE may change between runs).
static void engine_start(uint32_t frame)
{
    wr(shell, S_BASE, (uint32_t)(info.dma_address + cfg.engine_pool_offset + (uint64_t)frame * cfg.engine_window_bytes));
    wr(shell, E_PROG, cfg.engine_program_offset);
    engine_injection();
    wr(shell, S_HEARTBEAT, 1);
    wr(shell, E_CTRL, 1);
    engine_soft_start();
}

// the engine injection of the next run (F5), written before its start
static void engine_injection(void)
{
    if (engine_inject) {
        if (!strcmp(engine_inject, "engine-read-error")) wr(shell, S_INJECT, (SHELL_INJECT_KEY << 16) | 1u);
        else if (!strcmp(engine_inject, "engine-write-error")) wr(shell, S_INJECT, (SHELL_INJECT_KEY << 16) | 2u);
        else if (!strcmp(engine_inject, "engine-freeze")) wr(shell, S_INJECT, (SHELL_INJECT_KEY << 16) | 4u);
        else if (!strcmp(engine_inject, "engine-wdt")) wr(shell, S_WDT, 1000);
        else if (!strcmp(engine_inject, "engine-window")) wr(shell, S_WINDOW, 4096);
    }
}

// After the model's DMA: 1 when the engine finished its run cleanly within `limit_ns`; otherwise it is reset.
// The PS's polling pause (poll_sleep_us, 0: look at the PL back to back): every wait loop of a transfer sleeps that
// long between two looks, and the pipelined hybrid after a pass that changed nothing. The PS then idles instead of
// spinning on PL registers while the engine and the DMA work (PS power during inference); a completion is seen at
// most poll_sleep_us late.
static void poll_pause(void)
{
    if (cfg.poll_sleep_us) {
        struct timespec ts = {0, (long)cfg.poll_sleep_us * 1000L};
        nanosleep(&ts, NULL);
    }
}

static int engine_finish(uint64_t limit_ns, const char *after)
{
    uint64_t start = now_ns();
    uint32_t st = 0;
    for (;;) {
        st = rd(shell, S_STATUS);
        if (st & ST_ISOLATED) break;
        if (!(st & ST_RUNNING) && (st & ST_DONE) && rd(shell, S_DONES) == engine_dones + 1) {
            engine_dones++;
            if (rd(shell, S_FAULTS) == 0) return 1;
            break;
        }
        if (now_ns() - start > limit_ns) break;
        poll_pause();
    }
    {
        char detail[160];
        engine_fault_bits |= rd(shell, S_FAULTS);
        snprintf(detail, sizeof(detail), "after=%s status=0x%x faults=0x%x engine_status=0x%x window=%u", after, st,
                 rd(shell, S_FAULTS), rd(shell, S_ENGINE_STATUS), engine_frame);
        event(-1, "engine_reset", "", detail);
    }
    if (!engine_reset()) { errno = EIO; fail("engine reset"); }
    return 0;
}

static uint32_t control_bits(void)
{
    return FMG_CTRL_CHECKS | FMG_CTRL_SAFE_OUTPUT | FMG_CTRL_IRQ | (cfg.wdt_cycles ? FMG_CTRL_WDT : 0);
}

static void guard_configure(void)
{
    wr(guard, FMG_MIN, (uint32_t)cfg.min);
    wr(guard, FMG_MAX, (uint32_t)cfg.max);
    wr(guard, FMG_DEADLINE, cfg.deadline_cycles);
    wr(guard, FMG_WDT, cfg.wdt_cycles);
    wr(guard, FMG_STALL, cfg.stall_cycles);
    wr(guard, FMG_SAFE, (uint32_t)cfg.safe);
    wr(guard, FMG_INJECT, 0);
    wr(guard, FMG_CONTROL, control_bits() | FMG_CTRL_SOFT_RESET | FMG_CTRL_CLEAR);
    usleep(1000);
}

// a supervised worker's sign of life (a full pipe drops it: the supervisor reads the next one)
static void worker_beat(void)
{
    if (heartbeat_fd >= 0 && write(heartbeat_fd, "h", 1) < 0 && errno != EAGAIN) heartbeat_fd = -1;
}

static void heartbeat(void)
{
    worker_beat();
    swdt_feed();
    wr(guard, FMG_HEARTBEAT, 1);
    if (engine_on()) wr(shell, S_HEARTBEAT, 1);
}

static int interface_init(void)
{
    int ok = engine_reset();
    ok = reset_dma() && ok;
    wr(dma, MM2S_SR, SR_IOC | SR_ERR_IRQ);
    wr(dma, S2MM_SR, SR_IOC | SR_ERR_IRQ);
    return ok;
}

// PL IP reset: the DMA and the accelerator (through the guard), then the guard's frame state; a fresh WDT period.
// The DMA is reset once more after the accelerator: a reset while a frame was still streaming (a transfer stopped in the
// middle of a 221,184-beat PD LN frame) left the S2MM channel failing the next transfer (DMAIntErr, F7).
static int pl_ip_reset(void)
{
    int ok = engine_reset();     // the producer first: the engine stops feeding the aligner
    ok = reset_dma() && ok;
    wr(guard, FMG_INJECT, 0);
    wr(guard, FMG_CONTROL, control_bits() | FMG_CTRL_SOFT_RESET | FMG_CTRL_CLEAR);
    usleep(1000);
    ok = reset_dma() && ok;      // the streams are quiet now
    heartbeat();
    return ok && (rd(guard, FMG_STATUS) & 0x3u) == 0 && rd(guard, FMG_FAULTS) == 0 && rd(gpio, 0x8) == 0;
}

// ---------------------------------------------------------------- hybrid model (FM stage C: the PD hybrid)
// The plan (the hybrid runner's file, moderntcn_engine_rtl.hybrid_plan): "E PROGRAM_OFFSET" an engine segment, "S SET
// SRC DST IN_BYTES OUT_BYTES" a set-model operation from the window region SRC to DST (offsets after the window's DDR
// base). The service runs a frame's steps in order on its one window (no frames in flight, unlike the runner): each
// engine segment must finish cleanly, each operation's DMA must complete and the set model's guard must see no fault.
// The window's image (program, weights) is written once; a frame's input goes to hybrid_in_offset in the window and its
// output is read from hybrid_out_offset.
#define MAX_HYBRID_STEPS 64
struct hybrid_step { char kind; uint32_t program_offset, set, src, dst, in_bytes, out_bytes; };
static struct hybrid_step hsteps[MAX_HYBRID_STEPS];
static int nhsteps;
static int hybrid_on(void) { return cfg.hybrid_plan[0] != 0; }

static void hybrid_read_plan(void)
{
    FILE *f = fopen(cfg.hybrid_plan, "r");
    int engines = 0;
    char kind;
    if (!f) fail(cfg.hybrid_plan);
    while (nhsteps < MAX_HYBRID_STEPS && fscanf(f, " %c", &kind) == 1) {
        struct hybrid_step *s = &hsteps[nhsteps];
        memset(s, 0, sizeof(*s));
        s->kind = kind;
        if (kind == 'E') {
            if (fscanf(f, "%u", &s->program_offset) != 1) fail("hybrid plan engine step");
            engines++;
        } else if (kind == 'S') {
            if (fscanf(f, "%u %u %u %u %u", &s->set, &s->src, &s->dst, &s->in_bytes, &s->out_bytes) != 5)
                fail("hybrid plan stream step");
            if (s->set > 255 || s->out_bytes % cfg.out_beat_bytes || s->in_bytes >= (1u << 23) || s->out_bytes >= (1u << 23)
                || (uint64_t)cfg.engine_ddr_offset + s->src + s->in_bytes > cfg.engine_window_bytes
                || (uint64_t)cfg.engine_ddr_offset + s->dst + s->out_bytes > cfg.engine_window_bytes)
                { errno = EINVAL; fail("hybrid plan stream step bounds"); }
        } else {
            errno = EINVAL;
            fail("hybrid plan step kind (E or S)");
        }
        nhsteps++;
    }
    fclose(f);
    if (!engines) { errno = EINVAL; fail("hybrid plan without an engine segment"); }
}

// the output beats a frame of the set model's guard (FMG_PARAMS): those of the plan's stream operations
static uint32_t hybrid_stream_beats(void)
{
    for (int k = 0; k < nhsteps; k++)
        if (hsteps[k].kind == 'S') return hsteps[k].out_bytes / cfg.out_beat_bytes;
    return 0;
}

// L0-H4-01 on the engine windows (the hybrid's one window; an engine-fed model's corpus windows and its batch window):
// a window's program [0, engine_ddr_offset) and weights [engine_weight_offset, window end) are written once and never
// change (the engine, the plan's operations and the service's rows write between them). Each memory check compares
// the next 256 KiB slice with the CRC recorded when the windows were written, so a full pass takes one check a slice
// (the PD window: 58 slices; the GRU tail's 21 windows: 42); the pool is mapped uncached, a slice goes through a cached
// buffer. A mismatch reloads every slice from the window file.
#define WEIGHT_SLICE (256u * 1024u)
struct weight_slice { uint32_t window, offset, bytes, crc; };
static struct weight_slice *wslices;
static uint32_t nwslices, wslice_next;
static uint8_t wbuffer[64 * 1024];

static int weights_on(void) { return engine_on() && cfg.engine_weight_offset; }
static uint32_t weight_windows(void) { return hybrid_on() ? cfg.hybrid_windows : cfg.engine_frames + (cfg.engine_rows_bytes != 0); }
static uint8_t *window_at(uint32_t window) { return pool + cfg.engine_pool_offset + (size_t)window * cfg.engine_window_bytes; }

static uint32_t window_crc(uint32_t window, uint32_t offset, uint32_t bytes)
{
    const uint8_t *src = window_at(window) + offset;
    uint32_t crc = 0;
    for (uint32_t done = 0; done < bytes;) {
        uint32_t n = bytes - done < sizeof(wbuffer) ? bytes - done : (uint32_t)sizeof(wbuffer);
        memcpy(wbuffer, src + done, n);
        crc = fm_crc32_add(crc, wbuffer, n);
        done += n;
    }
    return crc;
}

// the bytes a window's slice was written with: the hybrid image (zero past its end), or the engine-fed windows file
// (the batch window is a copy of the first)
static void window_source(const uint8_t *file, size_t file_bytes, const struct weight_slice *s, uint8_t *dst)
{
    size_t at = (hybrid_on() || s->window >= cfg.engine_frames ? 0 : (size_t)s->window * cfg.engine_window_bytes) + s->offset;
    size_t have = at >= file_bytes ? 0 : file_bytes - at < s->bytes ? file_bytes - at : s->bytes;
    memcpy(dst, file + at, have);
    memset(dst + have, 0, s->bytes - have);
}

// the windows' static ranges in slices, with their CRCs from the windows just written and read back (`from_pool`; else
// the caller sets them); the regions the service and the plan write must stay below the weights
static void weights_index(int from_pool)
{
    uint32_t woff = cfg.engine_weight_offset, end = cfg.engine_window_bytes, windows = weight_windows(), k = 0;
    uint32_t ranges[2][2] = {{0, cfg.engine_ddr_offset}, {woff, end}}, per = 0;
    if (woff < cfg.engine_ddr_offset || woff >= end
        || (hybrid_on() && ((uint64_t)cfg.hybrid_in_offset + cfg.in_frame_bytes > woff
                            || (uint64_t)cfg.hybrid_out_offset + cfg.out_frame_bytes > woff
                            || cfg.hybrid_in_offset < cfg.engine_ddr_offset || cfg.hybrid_out_offset < cfg.engine_ddr_offset))
        || (cfg.engine_rows_bytes && ((uint64_t)cfg.engine_block_offset
                                      + (uint64_t)cfg.engine_steps * (cfg.engine_rows_bytes + cfg.in_frame_bytes) > woff
                                      || cfg.engine_block_offset < cfg.engine_ddr_offset)))
        { errno = EINVAL; fail("engine_weight_offset: the frame and row regions between the program and the weights"); }
    for (int j = 0; j < nhsteps; j++)
        if (hsteps[j].kind == 'S' && (uint64_t)cfg.engine_ddr_offset + hsteps[j].dst + hsteps[j].out_bytes > woff)
            { errno = EINVAL; fail("engine_weight_offset: a plan operation writes into the weights"); }
    for (int r = 0; r < 2; r++) per += (ranges[r][1] - ranges[r][0] + WEIGHT_SLICE - 1) / WEIGHT_SLICE;
    free(wslices);      // a warm start whose stored slices did not load builds the table again (no leak of the first)
    if (!(wslices = calloc((size_t)windows * per, sizeof(*wslices)))) fail("weight slices");
    for (uint32_t w = 0; w < windows; w++)
        for (int r = 0; r < 2; r++)
            for (uint32_t off = ranges[r][0]; off < ranges[r][1]; off += WEIGHT_SLICE, k++) {
                wslices[k].window = w;
                wslices[k].offset = off;
                wslices[k].bytes = ranges[r][1] - off < WEIGHT_SLICE ? ranges[r][1] - off : WEIGHT_SLICE;
                wslices[k].crc = from_pool ? window_crc(w, off, wslices[k].bytes) : 0;
            }
    nwslices = k;
    wslice_next = 0;
}

// warm turns: the scrub's slice table (window, offset, bytes, CRC) a cold start computed, saved next to the marker as
// "<warm_marker>.<spool>.slices"; a warm start takes it when it lists the same slices weights_index(0) lays out
static void warm_slices_path(char *path, size_t size) { snprintf(path, size, "%s.%s.slices", cfg.warm_marker, cfg.spool); }

static void warm_slices_save(void)
{
    char path[600];
    FILE *f;
    if (!cfg.warm_marker[0] || !nwslices) return;
    warm_slices_path(path, sizeof(path));
    if (!(f = fopen(path, "wb"))) return;
    if (fwrite(wslices, sizeof(*wslices), nwslices, f) != nwslices) { fclose(f); unlink(path); return; }
    fclose(f);
}

static int warm_slices_load(void)
{
    char path[600];
    FILE *f;
    struct weight_slice *saved;
    int ok = 0;
    if (!cfg.warm_marker[0]) return 0;
    weights_index(0);
    warm_slices_path(path, sizeof(path));
    if (!nwslices || !(f = fopen(path, "rb"))) return 0;
    if ((saved = calloc((size_t)nwslices + 1, sizeof(*saved))) && fread(saved, sizeof(*saved), nwslices + 1, f) == nwslices) {
        ok = 1;
        for (uint32_t k = 0; k < nwslices && ok; k++)
            ok = saved[k].window == wslices[k].window && saved[k].offset == wslices[k].offset
                 && saved[k].bytes == wslices[k].bytes;
        for (uint32_t k = 0; ok && k < nwslices; k++) wslices[k].crc = saved[k].crc;
    }
    free(saved);
    fclose(f);
    return ok;
}

// FM stage D: a hybrid window that still holds the image (this model's earlier turn) keeps every static slice whose
// CRC matches the image's; any slice another model's turn overwrote is written again (and read back by CRC). The
// dynamic region [engine_ddr_offset, engine_weight_offset) is written from the image every start, as a fresh window
// would be. The scrub's slices and CRCs come out of the same pass. Returns the bytes written.
static size_t hybrid_windows_refresh(const uint8_t *image, size_t bytes, int warm)
{
    uint8_t *src = malloc(WEIGHT_SLICE);
    uint32_t woff = cfg.engine_weight_offset, ddr = cfg.engine_ddr_offset;
    size_t written = 0, copy = (woff < bytes ? woff : bytes) > ddr ? (woff < bytes ? woff : bytes) - ddr : 0;
    if (!src) fail("weight slice");
    weights_index(0);
    uint32_t per = nwslices / cfg.hybrid_windows;           // every window holds the same image: its CRCs once
    for (uint32_t k = 0; k < nwslices; k++) {
        struct weight_slice *s = &wslices[k];
        window_source(image, bytes, s, src);
        s->crc = k < per ? fm_crc32(src, s->bytes) : wslices[k % per].crc;
        if (warm || window_crc(s->window, s->offset, s->bytes) == s->crc) continue;   // warm: as the cold start left them
        memcpy(window_at(s->window) + s->offset, src, s->bytes);
        __sync_synchronize();
        written += s->bytes;
        if (window_crc(s->window, s->offset, s->bytes) != s->crc) { errno = EIO; fail("hybrid window slice readback"); }
    }
    for (uint32_t w = 0; w < cfg.hybrid_windows; w++) {
        uint8_t *at = window_at(w) + ddr;
        memcpy(at, image + ddr, copy);
        memset(at + copy, 0, woff - ddr - copy);
        __sync_synchronize();
        if (memcmp(at, image + ddr, copy)) { errno = EIO; fail("hybrid window readback"); }
        written += woff - ddr;
    }
    free(src);
    return written;
}

// the next slice against its CRC: 0 on a mismatch (L0-H4-01)
static int weights_scrub(long batch)
{
    const struct weight_slice *s;
    uint32_t crc;
    char detail[128];
    if (!weights_on()) return 1;
    s = &wslices[wslice_next];
    wslice_next = (wslice_next + 1) % nwslices;
    if ((crc = window_crc(s->window, s->offset, s->bytes)) == s->crc) return 1;
    snprintf(detail, sizeof(detail), "window %u offset %u, %u bytes: crc=0x%08x expected=0x%08x", s->window, s->offset,
             s->bytes, crc, s->crc);
    event(batch, "weights_mismatch", "L0-H4-01", detail);
    return 0;
}

// a bit flipped in the slice checked next (stands in for a DDR upset under the window); the scrub must find it
static void corrupt_weights(void)
{
    const struct weight_slice *s;
    if (!weights_on()) return;
    s = &wslices[wslice_next];
    window_at(s->window)[s->offset + s->bytes / 2] ^= 0x10;
    __sync_synchronize();
}

// every slice again from the window file (which must still give the recorded CRCs), each read back; the heartbeat
// every 8 slices keeps the guard's WDT (the PD window: ~0.67 s)
static int weights_reload(void)
{
    size_t bytes;
    uint8_t *file, *slice;
    int ok = 1;
    if (!weights_on()) return 1;
    file = load(hybrid_on() ? cfg.hybrid_image : cfg.engine_windows, &bytes);
    if (!(slice = malloc(WEIGHT_SLICE))) fail("weight slice");
    for (uint32_t k = 0; k < nwslices && ok; k++) {
        window_source(file, bytes, &wslices[k], slice);
        ok = fm_crc32(slice, wslices[k].bytes) == wslices[k].crc;
        if (!ok) break;
        memcpy(window_at(wslices[k].window) + wslices[k].offset, slice, wslices[k].bytes);
        __sync_synchronize();
        ok = window_crc(wslices[k].window, wslices[k].offset, wslices[k].bytes) == wslices[k].crc;
        if (k % 8 == 7) heartbeat();
    }
    free(slice);
    free(file);
    return ok;
}

// A started engine segment, without waiting: 1 finished cleanly, 0 still running, -1 a fault or `expired` (the engine
// reset, as engine_finish).
static int engine_poll(int expired, const char *after)
{
    uint32_t st = rd(shell, S_STATUS);
    char detail[160];
    if (!expired && !(st & ST_ISOLATED)) {
        if ((st & ST_RUNNING) || !(st & ST_DONE) || rd(shell, S_DONES) != engine_dones + 1) return 0;
        engine_dones++;
        if (rd(shell, S_FAULTS) == 0) return 1;
    }
    engine_fault_bits |= rd(shell, S_FAULTS);
    snprintf(detail, sizeof(detail), "after=%s status=0x%x faults=0x%x engine_status=0x%x window=%u", after, st,
             rd(shell, S_FAULTS), rd(shell, S_ENGINE_STATUS), engine_frame);
    event(-1, "engine_reset", "", detail);
    if (!engine_reset()) { errno = EIO; fail("engine reset"); }
    return -1;
}

static uint8_t *hybrid_window(uint32_t k) { return pool + cfg.engine_pool_offset + (size_t)k * cfg.engine_window_bytes; }

// FM stage C pipelining (hybrid_windows N >= 2): up to N frames in flight, each in its own window (the same image), the
// engine and the set model on different frames at once (list scheduling by polling, as the hybrid runner). A frame
// takes a free window (its input copied in), runs its plan's steps in order, and gives its output back when the last
// step is done; the engine and the set model each start the oldest frame whose next step is theirs. Returns as
// transfer_hybrid: elapsed ns, or 0 (a DMA error, a refused set index, the batch timeout with no engine segment
// running). An engine segment that did not finish cleanly (or still runs at the timeout) sets engine_fault; a guard
// fault ends the transfer at once. The engine injection acts on the batch's first segment, the DMA injections on its
// first operation.
static uint64_t transfer_hybrid_pipelined(uint32_t in_offset, uint32_t frames, uint64_t timeout_ns, uint32_t *mm2s,
                                          uint32_t *s2mm, int late_s2mm, int short_s2mm)
{
    int frame[MAX_HYBRID_WINDOWS], step[MAX_HYBRID_WINDOWS], engine_k = -1, stream_k = -1, first_engine = 1;
    uint32_t next = 0, done = 0, n = cfg.hybrid_windows;
    uint64_t start = now_ns();
    char detail[80];
    engine_fault = 0;
    engine_fault_bits = 0;
    *mm2s = *s2mm = 0;
    for (uint32_t k = 0; k < n; k++) frame[k] = -1;
    while (done < frames) {
        int r, progress = 0;
        if (now_ns() - start > timeout_ns) {
            // a stream operation still out: the batch timed out (its recovery resets the engine too); an engine
            // segment alone still running is the engine's fault
            if (engine_k < 0 || stream_k >= 0) return 0;
            engine_poll(1, "hybrid_segment");
            engine_fault = 1;
            return now_ns() - start;
        }
        if (engine_k >= 0 && (r = engine_poll(0, "hybrid_segment")) != 0) {
            if (r < 0) { engine_fault = 1; return now_ns() - start; }
            step[engine_k]++;
            engine_k = -1;
            progress = 1;
        }
        if (stream_k >= 0) {
            *mm2s = rd(dma, MM2S_SR);
            *s2mm = rd(dma, S2MM_SR);
            if ((*mm2s | *s2mm) & FM_DMA_ERRORS) return 0;
            if (rd(guard, FMG_FAULTS)) return now_ns() - start;
            if ((*mm2s & SR_IOC) && (*s2mm & SR_IOC)) {
                wr(dma, MM2S_SR, SR_IOC | SR_ERR_IRQ);
                wr(dma, S2MM_SR, SR_IOC | SR_ERR_IRQ);
                if (rd(guard, FMG_FAULTS)) return now_ns() - start;
                step[stream_k]++;
                stream_k = -1;
                progress = 1;
            }
        }
        for (uint32_t k = 0; k < n; k++) {
            if (frame[k] >= 0 && step[k] == nhsteps) {      // a frame done: its output, its window free
                memcpy(pool + out_region + (size_t)frame[k] * cfg.out_frame_bytes, hybrid_window(k) + cfg.hybrid_out_offset,
                       cfg.out_frame_bytes);
                frame[k] = -1;
                done++;
                progress = 1;
            }
            if (frame[k] < 0 && next < frames) {             // the next frame into a free window
                memcpy(hybrid_window(k) + cfg.hybrid_in_offset, pool + in_offset + (size_t)next * cfg.in_frame_bytes,
                       cfg.in_frame_bytes);
                __sync_synchronize();
                frame[k] = (int)next++;
                step[k] = 0;
                progress = 1;
            }
        }
        for (int resource = 0; resource < 2; resource++) {
            char kind = resource ? 'S' : 'E';
            int pick = -1;
            if ((resource ? stream_k : engine_k) >= 0) continue;
            for (uint32_t k = 0; k < n; k++)
                if (frame[k] >= 0 && step[k] < nhsteps && hsteps[step[k]].kind == kind && (int)k != engine_k
                    && (int)k != stream_k && (pick < 0 || frame[k] < frame[pick]))
                    pick = (int)k;
            if (pick < 0) continue;
            const struct hybrid_step *s = &hsteps[step[pick]];
            uint32_t window = (uint32_t)(info.dma_address + cfg.engine_pool_offset + (uint64_t)pick * cfg.engine_window_bytes);
            uint32_t base = window + cfg.engine_ddr_offset;
            if (kind == 'E') {
                wr(shell, S_BASE, window);
                wr(shell, E_PROG, s->program_offset);
                if (first_engine) engine_injection();
                first_engine = 0;
                wr(shell, S_HEARTBEAT, 1);
                wr(shell, E_CTRL, 1);
                engine_soft_start();
                engine_k = pick;
                progress = 1;
                continue;
            }
            if (rd(guard, FMG_STATUS) & (1u << 8)) {     // the accelerator and its feeder are held in reset
                event(-1, "feeder_held", "", "the accelerator is in reset: the feeder is not accessed");
                return 0;
            }
            if (rd(feeder, 0x4) & 1u) {
                snprintf(detail, sizeof(detail), "set=%u status=0x%x (a beat still waits)", s->set, rd(feeder, 0x4));
                event(-1, "feeder_refused", "", detail);
                return 0;
            }
            wr(feeder, 0x0, s->set);
            if (rd(feeder, 0x4) & 2u) {
                snprintf(detail, sizeof(detail), "set=%u status=0x%x (refused)", s->set, rd(feeder, 0x4));
                event(-1, "feeder_refused", "", detail);
                return 0;
            }
            wr(gpio, 0x0, s->out_bytes / cfg.out_beat_bytes);
            wr(dma, S2MM_CR, 1);
            wr(dma, MM2S_CR, 1);
            if (!late_s2mm) { wr(dma, S2MM_DA, base + s->dst); wr(dma, S2MM_LEN, short_s2mm ? s->out_bytes / 2 : s->out_bytes); }
            wr(dma, MM2S_SA, base + s->src);
            wr(dma, MM2S_LEN, s->in_bytes);
            if (late_s2mm) { usleep(2000); wr(dma, S2MM_DA, base + s->dst); wr(dma, S2MM_LEN, s->out_bytes); }
            late_s2mm = short_s2mm = 0;
            stream_k = pick;
            progress = 1;
        }
        if (!progress) poll_pause();
    }
    return now_ns() - start;
}

// `frames` frames of the hybrid model, one after the other; returns elapsed ns, or 0 (an operation's DMA error, a
// refused set index, the batch timeout). An engine segment that did not finish cleanly sets engine_fault; a guard fault
// ends the transfer at once (the caller classifies both, as for an engine-fed model). The DMA injections act on the
// first operation.
static uint64_t transfer_hybrid(uint32_t in_offset, uint32_t frames, uint64_t timeout_ns, uint32_t *mm2s, uint32_t *s2mm,
                                int late_s2mm, int short_s2mm)
{
    uint8_t *win = pool + cfg.engine_pool_offset;
    uint32_t window = (uint32_t)(info.dma_address + cfg.engine_pool_offset), base = window + cfg.engine_ddr_offset;
    uint64_t start = now_ns();
    char detail[80];
    engine_fault = 0;
    engine_fault_bits = 0;
    *mm2s = *s2mm = 0;
    for (uint32_t f = 0; f < frames; f++) {
        memcpy(win + cfg.hybrid_in_offset, pool + in_offset + (size_t)f * cfg.in_frame_bytes, cfg.in_frame_bytes);
        __sync_synchronize();
        for (int k = 0; k < nhsteps; k++) {
            const struct hybrid_step *s = &hsteps[k];
            uint64_t used = now_ns() - start;
            if (used > timeout_ns) return 0;
            if (s->kind == 'E') {
                wr(shell, S_BASE, window);
                wr(shell, E_PROG, s->program_offset);
                if (f == 0 && k == 0) engine_injection();
                wr(shell, S_HEARTBEAT, 1);
                wr(shell, E_CTRL, 1);
                engine_soft_start();
                if (!engine_finish(timeout_ns - used, "hybrid_segment")) { engine_fault = 1; return now_ns() - start; }
                continue;
            }
            if (rd(guard, FMG_STATUS) & (1u << 8)) {     // the accelerator and its feeder are held in reset
                event(-1, "feeder_held", "", "the accelerator is in reset: the feeder is not accessed");
                return 0;
            }
            if (rd(feeder, 0x4) & 1u) {
                snprintf(detail, sizeof(detail), "set=%u status=0x%x (a beat still waits)", s->set, rd(feeder, 0x4));
                event(-1, "feeder_refused", "", detail);
                return 0;
            }
            wr(feeder, 0x0, s->set);
            if (rd(feeder, 0x4) & 2u) {
                snprintf(detail, sizeof(detail), "set=%u status=0x%x (refused)", s->set, rd(feeder, 0x4));
                event(-1, "feeder_refused", "", detail);
                return 0;
            }
            wr(gpio, 0x0, s->out_bytes / cfg.out_beat_bytes);
            wr(dma, S2MM_CR, 1);
            wr(dma, MM2S_CR, 1);
            if (!late_s2mm) { wr(dma, S2MM_DA, base + s->dst); wr(dma, S2MM_LEN, short_s2mm ? s->out_bytes / 2 : s->out_bytes); }
            wr(dma, MM2S_SA, base + s->src);
            wr(dma, MM2S_LEN, s->in_bytes);
            if (late_s2mm) { usleep(2000); wr(dma, S2MM_DA, base + s->dst); wr(dma, S2MM_LEN, s->out_bytes); }
            late_s2mm = short_s2mm = 0;
            for (;;) {
                *mm2s = rd(dma, MM2S_SR);
                *s2mm = rd(dma, S2MM_SR);
                if ((*mm2s | *s2mm) & FM_DMA_ERRORS) return 0;
                if ((*mm2s & SR_IOC) && (*s2mm & SR_IOC)) break;
                // a guard fault (the operation's deadline, a stall, stale input) ends the operation at once: the
                // stream it stopped would hold the DMA until the batch timeout, past the guard's WDT
                if (rd(guard, FMG_FAULTS)) return now_ns() - start;
                if (now_ns() - start > timeout_ns) return 0;
                poll_pause();
            }
            wr(dma, MM2S_SR, SR_IOC | SR_ERR_IRQ);
            wr(dma, S2MM_SR, SR_IOC | SR_ERR_IRQ);
            if (rd(guard, FMG_FAULTS)) return now_ns() - start;
        }
        memcpy(pool + out_region + (size_t)f * cfg.out_frame_bytes, win + cfg.hybrid_out_offset, cfg.out_frame_bytes);
    }
    return now_ns() - start;
}

// One transfer of `frames` frames starting `in_offset` bytes into the pool input, the output at the start of the pool
// output region; returns elapsed ns or 0 (timeout or DMA error).
static uint64_t transfer_at(uint32_t in_offset, uint32_t frames, uint64_t timeout_ns, uint32_t *mm2s, uint32_t *s2mm,
                            int late_s2mm, int short_s2mm)
{
    uint32_t in_addr = tz_in_addr ? (uint32_t)tz_in_addr : (uint32_t)info.dma_address + in_offset;
    uint32_t out_addr = tz_out_addr ? (uint32_t)tz_out_addr : (uint32_t)(info.dma_address + out_region);
    uint32_t out_bytes = frames * cfg.out_frame_bytes, beats = out_bytes / cfg.out_beat_bytes;
    uint64_t start = now_ns(), end;
    if (hybrid_on() && cfg.hybrid_windows > 1)
        return transfer_hybrid_pipelined(in_offset, frames, timeout_ns, mm2s, s2mm, late_s2mm, short_s2mm);
    if (hybrid_on()) return transfer_hybrid(in_offset, frames, timeout_ns, mm2s, s2mm, late_s2mm, short_s2mm);
    engine_fault = 0;
    engine_fault_bits = 0;
    if (engine_on() && (frames != cfg.engine_steps || engine_frame >= cfg.engine_frames + (cfg.engine_rows_bytes != 0)))
        { errno = EINVAL; fail("engine window"); }
    if (feeder) {                // the frame's set index first: a waiting beat or a refused write fails the transfer
        char detail[64];
        if (rd(guard, FMG_STATUS) & (1u << 8)) {     // the accelerator and its feeder are held in reset
            event(-1, "feeder_held", "", "the accelerator is in reset: the feeder is not accessed");
            return 0;
        }
        if (frames != 1 || (rd(feeder, 0x4) & 1u)) {
            snprintf(detail, sizeof(detail), "set=%u status=0x%x (a beat still waits)", transfer_set, rd(feeder, 0x4));
            event(-1, "feeder_refused", "", detail);
            return 0;
        }
        wr(feeder, 0x0, transfer_set);
        if (rd(feeder, 0x4) & 2u) {
            snprintf(detail, sizeof(detail), "set=%u status=0x%x (refused)", transfer_set, rd(feeder, 0x4));
            event(-1, "feeder_refused", "", detail);
            return 0;
        }
    }
    wr(gpio, 0x0, beats);
    wr(dma, S2MM_CR, 1);
    wr(dma, MM2S_CR, 1);
    if (!late_s2mm) { wr(dma, S2MM_DA, out_addr); wr(dma, S2MM_LEN, short_s2mm ? out_bytes / 2 : out_bytes); }
    wr(dma, MM2S_SA, in_addr);
    wr(dma, MM2S_LEN, frames * cfg.in_frame_bytes);
    if (engine_on()) engine_start(engine_frame);
    if (late_s2mm) { usleep(2000); wr(dma, S2MM_DA, out_addr); wr(dma, S2MM_LEN, out_bytes); }
    for (;;) {
        *mm2s = rd(dma, MM2S_SR);
        *s2mm = rd(dma, S2MM_SR);
        if ((*mm2s | *s2mm) & FM_DMA_ERRORS) { if (engine_on()) engine_finish(0, "dma_error"); return 0; }
        // the deadline before the completion: a transfer seen complete only after its deadline is late (a GRU step
        // is shorter than the set-up, so with the completion first an injected 1 us timeout was missed at random)
        if (now_ns() - start > timeout_ns) { if (engine_on()) engine_finish(0, "timeout"); return 0; }
        if ((*mm2s & SR_IOC) && (*s2mm & SR_IOC)) break;
        poll_pause();
    }
    end = now_ns();
    wr(dma, MM2S_SR, SR_IOC | SR_ERR_IRQ);
    wr(dma, S2MM_SR, SR_IOC | SR_ERR_IRQ);
    // a clean DMA transfer: the engine must have finished cleanly too, unless the guard stopped the stream (its fault
    // is the batch's; the engine stalled behind it and is reset)
    if (engine_on() && !engine_finish(ENGINE_GRACE_NS, "transfer") && rd(guard, FMG_FAULTS) == 0) engine_fault = 1;
    return end - start;
}

static uint64_t transfer(uint32_t frames, uint64_t timeout_ns, uint32_t *mm2s, uint32_t *s2mm, int late_s2mm, int short_s2mm)
{
    return transfer_at(0, frames, timeout_ns, mm2s, s2mm, late_s2mm, short_s2mm);
}

// ---------------------------------------------------------------- health and Initial Mode
// L0-H4-02: the PLLs the PS runs on are locked (PLL_STATUS bits 0..2: ARM, DDR, IO). The DDR and IO PLLs always; the ARM
// PLL unless the power profile (image r14) bypassed and powered it down (ARM_PLL_CTRL bits 4 and 1) with the CPU on the
// DDR or IO PLL (ARM_CLK_CTRL SRCSEL 2 or 3): then nothing runs on it and its lock bit is down by design.
static int plls_locked(uint32_t status)
{
    uint32_t need = 0x6u;
    int arm_off = (rd(slcr, SLCR_ARM_PLL_CTRL) & 0x12u) == 0x12u && (rd(slcr, SLCR_ARM_CLK_CTRL) & 0x30u) >= 0x20u;
    if (!arm_off) need |= 0x1u;
    return (status & need) == need;
}

static double xadc(const char *channel)
{
    char path[160], text[64];
    double raw = 0, offset = 0, scale = 0;
    const char *part[] = {"raw", "offset", "scale"};
    double *value[] = {&raw, &offset, &scale};
    for (int i = 0; i < 3; i++) {
        FILE *f;
        snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device0/in_%s_%s", channel, part[i]);
        if (!(f = fopen(path, "r"))) { if (i == 1) continue; return -1e9; }
        if (fgets(text, sizeof(text), f)) *value[i] = strtod(text, NULL);
        fclose(f);
    }
    return (raw + offset) * scale / 1000.0;
}

// Set while the PL is being written or after its write failed: the FPGA manager holds the PL interfaces in reset with
// the PL-to-PS level shifters off, and a PL register read then never returns (the PS stalls on the bus; the SWDT did not
// reset the board from that state, 2026-10-14). Nothing reads the PL then, the health record included.
static int pl_unavailable;

static void health_record(FILE *f, const char *mode)
{
    char fpga[32] = "?";
    uint32_t g[7] = {0};
    struct fm_parity par;
    FILE *s = fopen("/sys/class/fpga_manager/fpga0/state", "r");
    memset(&par, 0, sizeof(par));
    if (l2c && ocm) fm_parity_read(parity_reg, 0, &par);
    if (s) { if (fscanf(s, "%31s", fpga) != 1) strcpy(fpga, "?"); fclose(s); }
    if (guard && !pl_unavailable) {
        g[0] = rd(guard, FMG_STATUS); g[1] = rd(guard, FMG_FAULTS); g[2] = rd(guard, FMG_FAULT_FRAME);
        g[3] = rd(guard, FMG_FAULT_COUNT); g[4] = rd(guard, FMG_STARTED); g[5] = rd(guard, FMG_OUT_FRAMES);
        g[6] = rd(guard, FMG_MAX_LATENCY);
    }
    fprintf(f, "{\"schema\":\"f-astra/fm-health-telemetry/v1\",\"mode\":\"%s\",\"cause\":\"%s\",\"fpga\":\"%s\","
               "\"pll_status\":%u,\"reboot_status\":%u,\"temp_c\":%.2f,\"vccint_v\":%.4f,"
               "\"guard\":{\"status\":%u,\"faults\":%u,\"fault_frame\":%u,\"fault_count\":%u,\"started\":%u,\"out_frames\":%u,"
               "\"max_latency_cycles\":%u},\"parity\":{\"enabled\":%u,\"l2_aux\":%u,\"l2_raw\":%u,\"ocm_ctrl\":%u,"
               "\"ocm_status\":%u,\"ocm_address\":%u},\"recovery_counts\":{",
            mode, last_cause, fpga, rd(slcr, SLCR_PLL_STATUS), rd(slcr, SLCR_REBOOT_STATUS), xadc("temp0"), xadc("voltage0_vccint"),
            g[0], g[1], g[2], g[3], g[4], g[5], g[6], fm_parity_enabled(&par), par.l2_aux, par.l2_raw, par.ocm_ctrl,
            par.ocm_status, par.ocm_address);
    for (int i = 0; i < FM_COUNT; i++) fprintf(f, "%s\"%s\":%u", i ? "," : "", fm_names[i], state.count[i]);
    fprintf(f, "},\"safehold\":%u,\"safehold_cause\":\"%s\",\"boot_pending\":%u}\n", state.safehold, state.cause,
            state.boot_pending);
}

static void write_health(const char *mode)
{
    FILE *f = fopen(cfg.health, "w");
    if (f) {
        health_record(f, mode);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    if (obc_events && (f = fopen(OBC_DIR "/health.json.tmp", "w"))) {   // the OBC's copy (F4), replaced whole
        health_record(f, mode);
        if (fclose(f) == 0) rename(OBC_DIR "/health.json.tmp", OBC_DIR "/health.json");
    }
}

// ---------------------------------------------------------------- TrustZone T3b: the monitor's FM services
// Through /dev/f-astra-sec (non-secure kernel): the Safehold latch (no PL or devcfg write until a reset) and the secure
// golden copy per slot (the first load of the boot wins; a compare checks a PL output against it).
struct fastra_sec_call { uint32_t fid, a1, a2, a3, r0, r1, r2, r3; };
#define FASTRA_SEC_CALL _IOWR('F', 0x40, struct fastra_sec_call)
#define SEC_FM_STATE 0x82000F10u
#define SEC_FM_SAFEHOLD 0x82000F11u
#define SEC_FM_GOLDEN_LOAD 0x82000F12u
#define SEC_FM_GOLDEN_COMPARE 0x82000F13u
#define SEC_FM_WATCH 0x82000F14u
#define SEC_FM_GOLDEN_SIGNED 0x82000F15u     // T3d (a monitor built with the ground's key)
#define SEC_FM_RELEASE_CHALLENGE 0x82000F16u
#define SEC_FM_RELEASE 0x82000F17u
#define GROUND_RELEASE_FILE OBC_DIR "/ground-release"   // the ground's release tag, 64 hex digits (applies once)
static int secure_golden_loaded;

static int sec_call(uint32_t fid, uint32_t a1, uint32_t a2, uint32_t a3, struct fastra_sec_call *c)
{
    memset(c, 0, sizeof(*c));
    c->fid = fid; c->a1 = a1; c->a2 = a2; c->a3 = a3;
    if (!pl_io_secure_monitor()) { errno = ENODEV; return -1; }
    return ioctl(pl_io_fd, FASTRA_SEC_CALL, c);
}

static void safehold(long batch, const char *cause)
{
    struct fastra_sec_call c;
    warm_drop();
    snprintf(last_cause, sizeof(last_cause), "%s", cause);
    swdt_disarm();                                              // Safehold waits for the ground, not for a reset
    if (engine_on()) wr(shell, S_CTRL, C_HOLD);                 // the engine-fed model's engine stops with it
    reset_dma();
    wr(guard, FMG_CONTROL, control_bits() | FMG_CTRL_HOLD);   // AI output blocked until a ground command
    if (cfg.secure_safehold) {      // T3b: the last PL writes are done; the monitor refuses any further one
        int rc = sec_call(SEC_FM_SAFEHOLD, 0, 0, 0, &c);
        event(batch, "secure_safehold", cause, rc ? strerror(errno) : "latched in the secure monitor");
    }
    event(batch, "safehold", cause, "AI processing stopped; output held");
    write_health("SAFEHOLD");
    feeder_stop();
    exit(3);
}

// T3b: a Safehold the monitor latched earlier in this boot holds every later run too; the PL stays as it was left
// (the monitor refuses PL writes, so Safehold's own PL steps are not repeated)
// T3c: the monitor restarts the watchdog only after a heartbeat of this service (its guard watched, the PL mapped):
// one heartbeat now, so the watchdog's timeout and restart that follow are allowed
static void secure_watch_start(void)
{
    struct fastra_sec_call c;
    char detail[64];
    int rc = sec_call(SEC_FM_WATCH, cfg.guard_base, 0, 0, &c);
    snprintf(detail, sizeof(detail), "%s guard=0x%08x", rc || c.r0 ? "refused" : "on", cfg.guard_base);
    event(-1, "secure_watch", "L0-H3-01", detail);
    if (rc || c.r0) { errno = EPERM; fail("secure watch"); }
    wr(guard, FMG_HEARTBEAT, 1);
}

// T3d: the ground's release of the secure latch, a tag in GROUND_RELEASE_FILE over the challenge this boot reported
// (RELEASE_CHALLENGE_FILE keeps it: later latched starts report the same one, so a tag on its way still fits). The tag
// file is consumed (one try; the monitor disarms the challenge either way). A release is the ground's decision: the
// service's own persisted Safehold and counts are cleared as by a ground reset. 1 when the monitor took the tag.
#define RELEASE_CHALLENGE_FILE OBC_DIR "/release-challenge"
static int secure_release_try(void)
{
    char hex[80] = "";
    uint8_t tag[32];
    unsigned v;
    struct fastra_sec_call c;
    FILE *f = fopen(GROUND_RELEASE_FILE, "r");
    int ok;
    memset(&c, 0, sizeof(c));
    if (!f) return 0;
    ok = fgets(hex, sizeof(hex), f) != NULL;
    fclose(f);
    unlink(GROUND_RELEASE_FILE);
    unlink(RELEASE_CHALLENGE_FILE);                      // tried: the next latched start asks for a new challenge
    for (int i = 0; ok && i < 32; i++) {
        ok = sscanf(hex + 2 * i, "%2x", &v) == 1;
        tag[i] = (uint8_t)v;
    }
    if (ok) {
        memcpy(pool + out_region, tag, sizeof(tag));     // the monitor reads it from non-secure DDR
        __asm__ volatile("dsb" ::: "memory");
        ok = !sec_call(SEC_FM_RELEASE, (uint32_t)info.dma_address + out_region, 0, 0, &c) && c.r0 == 0;
    }
    snprintf(hex, sizeof(hex), "r0=0x%08x", ok ? 0u : c.r0);
    event(-1, ok ? "secure_safehold_released" : "secure_release_refused", "", hex);
    if (ok) {
        memset(&state, 0, sizeof(state));
        fm_state_save(cfg.state, &state);
        event(-1, "ground_reset", "", "secure_release");
    }
    return ok;
}

static void secure_safehold_check(void)
{
    struct fastra_sec_call c;
    char detail[64], kept[32] = "";
    FILE *f;
    if (!cfg.secure_safehold || sec_call(SEC_FM_STATE, 0, 0, 0, &c) || !(c.r1 & 1u)) return;
    int keyed = (c.r1 & 2u) != 0;
    if (keyed && secure_release_try()) return;           // T3d: released by the ground, the start goes on
    snprintf(last_cause, sizeof(last_cause), "INITIAL:secure-safehold");
    swdt_disarm();
    event(-1, "safehold", "INITIAL:secure-safehold", "latched in the secure monitor; the PL stays as it was left");
    if (keyed && (f = fopen(RELEASE_CHALLENGE_FILE, "r"))) {      // this boot's challenge, still armed
        if (!fgets(kept, sizeof(kept), f)) kept[0] = 0;
        fclose(f);
        kept[strcspn(kept, "\n")] = 0;
    }
    if (keyed && strlen(kept) == 16) {
        snprintf(detail, sizeof(detail), "challenge=%s", kept);
        event(-1, "secure_release_challenge", "", detail);
    } else if (keyed && !sec_call(SEC_FM_RELEASE_CHALLENGE, 0, 0, 0, &c) && c.r0 == 0) {
        // the challenge for the ground's tag, little-endian words: bytes in order
        snprintf(kept, sizeof(kept), "%02x%02x%02x%02x%02x%02x%02x%02x", c.r1 & 0xFFu, (c.r1 >> 8) & 0xFFu,
                 (c.r1 >> 16) & 0xFFu, c.r1 >> 24, c.r2 & 0xFFu, (c.r2 >> 8) & 0xFFu, (c.r2 >> 16) & 0xFFu, c.r2 >> 24);
        if ((f = fopen(RELEASE_CHALLENGE_FILE, "w"))) { fprintf(f, "%s\n", kept); fclose(f); }
        snprintf(detail, sizeof(detail), "challenge=%s", kept);
        event(-1, "secure_release_challenge", "", detail);
    }
    write_health("SAFEHOLD");
    exit(3);
}

// T3d: the bitstream's PL register windows (pl_windows: the ground's signed manifest, ground_auth.py pl-windows). A
// keyed monitor refuses PL accesses outside them, and every PROG_B pulse clears them: the service registers them at
// its start (as the session does after its PL load) and again after its own PL reload (an SEM uncorrectable).
#define SEC_PL_WINDOWS 0x82000F18u
#define PL_WINDOWS_BYTES (4u + 8u * 32u + 32u)   // the largest manifest (32 windows); the monitor reads this much
static void secure_pl_windows(void)
{
    struct fastra_sec_call c;
    char detail[64];
    size_t bytes;
    uint8_t *m;
    int rc;
    if (!cfg.pl_windows[0]) return;
    m = load(cfg.pl_windows, &bytes);
    if (bytes > PL_WINDOWS_BYTES || PL_WINDOWS_BYTES > out_capacity) { errno = EINVAL; fail("pl_windows"); }
    memset(pool + out_region, 0, PL_WINDOWS_BYTES);
    memcpy(pool + out_region, m, bytes);
    free(m);
    __asm__ volatile("dsb" ::: "memory");
    memset(&c, 0, sizeof(c));
    rc = sec_call(SEC_PL_WINDOWS, (uint32_t)info.dma_address + out_region, 0, 0, &c);
    snprintf(detail, sizeof(detail), "r0=0x%08x errno=%d", c.r0, rc ? errno : 0);
    event(-1, rc || c.r0 ? "pl_windows_refused" : "pl_windows", "", detail);
}

static uint8_t *protected_copy(const char *path, size_t *bytes, size_t *pages)
{
    size_t size;
    uint8_t *data = load(path, &size), *page;
    long unit = sysconf(_SC_PAGESIZE);
    *pages = (size + (size_t)unit - 1) / (size_t)unit * (size_t)unit;
    page = mmap(NULL, *pages ? *pages : (size_t)unit, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) fail("protected pages");
    memcpy(page, data, size);
    free(data);
    *bytes = size;
    return page;
}

// Load (or reload) the golden data into read-only pages; a reload must reproduce the CRC first recorded.
static int protect_golden(int reload)
{
    uint32_t in_crc = golden.in_crc, out_crc = golden.out_crc;
    if (reload) { munmap(golden.in, golden.in_pages); munmap(golden.out, golden.out_pages); }
    golden.in = protected_copy(cfg.golden_input, &golden.in_bytes, &golden.in_pages);
    golden.out = protected_copy(cfg.golden_expected, &golden.out_bytes, &golden.out_pages);
    golden.in_crc = fm_crc32(golden.in, golden.in_bytes);
    golden.out_crc = fm_crc32(golden.out, golden.out_bytes);
    if (mprotect(golden.in, golden.in_pages, PROT_READ) || mprotect(golden.out, golden.out_pages, PROT_READ)) fail("mprotect");
    return !reload || (golden.in_crc == in_crc && golden.out_crc == out_crc);
}

static int scrub_golden(void)
{
    return fm_crc32(golden.in, golden.in_bytes) == golden.in_crc && fm_crc32(golden.out, golden.out_bytes) == golden.out_crc;
}

static void on_segv(int sig, siginfo_t *info_, void *context)
{
    (void)context;
    if (protect_armed) { protect_fault = info_->si_addr; siglongjmp(protect_jump, 1); }
    signal(sig, SIG_DFL);
    raise(sig);
}

// A write into protected FM data (injection): returns 1 when the MMU blocked it and the data is unchanged.
static int attempt_protected_write(void)
{
    volatile uint8_t *target = golden.out;
    struct sigaction action, previous;
    volatile int blocked = 0;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_segv;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, &previous);
    if (sigsetjmp(protect_jump, 1) == 0) {
        protect_armed = 1;
        target[0] ^= 0x5A;
        protect_armed = 0;
    } else {
        protect_armed = 0;
        blocked = (uint8_t *)protect_fault == golden.out;
    }
    sigaction(SIGSEGV, &previous, NULL);
    return blocked && scrub_golden();
}

// Corrupt one protected bit behind the MMU's back (stands in for a memory upset); the scrub must find it.
static void corrupt_golden(void)
{
    mprotect(golden.out, golden.out_pages, PROT_READ | PROT_WRITE);
    golden.out[golden.out_bytes / 2] ^= 0x10;
    mprotect(golden.out, golden.out_pages, PROT_READ);
}

static int golden_check(long batch, int corrupt)
{
    size_t in_bytes = golden.in_bytes, out_bytes = golden.out_bytes;
    uint32_t mm2s, s2mm;
    int ok;
    struct fastra_sec_call c;
    char detail[96];
    if (in_bytes != (size_t)cfg.golden_frames * cfg.in_frame_bytes || out_bytes != (size_t)cfg.golden_frames * cfg.out_frame_bytes)
        return 0;
    if (cfg.secure_golden && !secure_golden_loaded) {
        // T3b: the reference goes to the monitor's copy through the pool (uncached) before any injection can touch it;
        // a copy loaded earlier in this boot must have the same bytes. T3d (golden_manifest, a monitor with the
        // ground's key): the ground's manifest (SHA-256, tag) goes first and the monitor takes only the signed data.
        size_t manifest = cfg.golden_manifest[0] ? 64 : 0;
        if (manifest) {
            size_t bytes;
            uint8_t *m = load(cfg.golden_manifest, &bytes);
            if (bytes != manifest || manifest + out_bytes > out_capacity) { errno = EINVAL; fail("golden_manifest"); }
            memcpy(pool + out_region, m, manifest);
            free(m);
        }
        memcpy(pool + out_region + manifest, golden.out, out_bytes);
        __asm__ volatile("dsb" ::: "memory");
        if (sec_call(manifest ? SEC_FM_GOLDEN_SIGNED : SEC_FM_GOLDEN_LOAD, cfg.golden_slot,
                     (uint32_t)info.dma_address + out_region, (uint32_t)out_bytes, &c)
            || c.r0 > 1u || c.r1 != golden.out_crc) {
            snprintf(detail, sizeof(detail), "r0=0x%08x crc=0x%08x expected=0x%08x errno=%d", c.r0, c.r1, golden.out_crc, errno);
            event(batch, "secure_golden_changed", "L0-H4-01", detail);
            return 0;
        }
        secure_golden_loaded = 1;
        snprintf(detail, sizeof(detail), "slot=%u crc=0x%08x %s", cfg.golden_slot, c.r1, c.r0 ? "already loaded" : "copied");
        event(batch, "secure_golden_loaded", "", detail);
    }
    if (corrupt) corrupt_golden();
    memcpy(pool, golden.in, in_bytes);
    memset(pool + out_region, 0xA5, out_bytes);
    transfer_set = cfg.golden_set;
    heartbeat();
    engine_frame = 0;            // the golden frames are the corpus's first: the first engine window
    ok = transfer(cfg.golden_frames, (uint64_t)cfg.batch_timeout_ms * 1000000ull, &mm2s, &s2mm, 0, 0) != 0
         && !engine_fault && rd(guard, FMG_FAULTS) == 0 && !memcmp(pool + out_region, golden.out, out_bytes)
         && scrub_golden();
    if (cfg.secure_golden && secure_golden_loaded) {
        // T3b: the PL output against the secure copy as well; a reference corrupted in the non-secure world fails it
        int rc = sec_call(SEC_FM_GOLDEN_COMPARE, cfg.golden_slot, (uint32_t)info.dma_address + out_region,
                          (uint32_t)out_bytes, &c);
        snprintf(detail, sizeof(detail), "secure_compare=%s first_difference=%u", rc || c.r0 ? "error" : c.r1 ? "differs" : "equal",
                 c.r1);
        event(batch, "secure_golden_compare", "", detail);
        if (rc || c.r0 || c.r1) ok = 0;
    }
    event(batch, ok ? "golden_pass" : "golden_fail", "", corrupt ? "golden reference corrupted (injection)" : "");
    if (ok) warm_golden_at = boot_seconds();
    return ok;
}

// ---------------------------------------------------------------- TrustZone T1 (PL masters non-secure)
// The bitstream marks every PL access to the PS memory non-secure (f_astra_axi_ns before each HP port) and the session
// opened exactly the pool's 64 MB DDR sections (f-astra-tz open). A PL access anywhere else ends in DECERR.
static uint32_t tz_pool_mask(void) { return fm_tz_ddr_mask(info.dma_address, info.bytes); }

// the physical address of a locked page of this process (/proc/self/pagemap, root), or 0
static uint64_t page_physical(const void *page)
{
    uint64_t entry = 0, v = (uint64_t)(uintptr_t)page;
    FILE *f = fopen("/proc/self/pagemap", "rb");
    if (!f) return 0;
    if (fseek(f, (long)(v / 4096 * 8), SEEK_SET) || fread(&entry, sizeof(entry), 1, f) != 1) entry = 0;
    fclose(f);
    if (!(entry >> 63) || !(entry & ((1ull << 55) - 1))) return 0;
    return (entry & ((1ull << 55) - 1)) * 4096 + v % 4096;
}

// Initial (tz_probe): one-frame transfers that read from, then write to, a locked page of this process in a secure
// section. Each must end in DECERR (the page keeps its pattern); after a PL IP reset the golden frames pass again.
// The probe frame's output fits in the page, so a fence that is not there costs this page only.
static void tz_probes(void)
{
    uint8_t *page = NULL;
    uint64_t phys = 0;
    if (cfg.out_frame_bytes > 4096) { event(-1, "tz_probe_skipped", "L0-H4-03", "an output frame exceeds one page"); return; }
    for (int i = 0; i < 64 && !phys; i++) {   // a page in an open section stays locked, so the next try gets another
        if (posix_memalign((void **)&page, 4096, 4096) || mlock(page, 4096)) fail("tz probe page");
        memset(page, 0x5A, 4096);
        uint64_t p = page_physical(page);
        if (p && !(fm_tz_ddr_mask(p, 4096) & tz_pool_mask())) phys = p;
    }
    if (!phys) safehold(-1, "INITIAL:tz-probe");
    for (int write = 0; write < 2; write++) {
        uint32_t mm2s = 0, s2mm = 0, intact = 1;
        char detail[160];
        memcpy(pool, golden.in, cfg.in_frame_bytes);
        memset(pool + out_region, 0xA5, cfg.out_frame_bytes);
        heartbeat();
        engine_frame = 0;
        transfer_set = cfg.golden_set;
        if (write) tz_out_addr = phys; else tz_in_addr = phys;
        uint64_t t = transfer(1, 100000000ull, &mm2s, &s2mm, 0, 0);
        tz_in_addr = tz_out_addr = 0;
        for (int i = 0; i < 4096; i++) intact &= page[i] == 0x5A;
        int blocked = !t && ((write ? s2mm : mm2s) & SR_DEC_ERR) && intact;
        snprintf(detail, sizeof(detail), "phys=0x%llx mm2s=0x%x s2mm=0x%x page_intact=%u", (unsigned long long)phys, mm2s,
                 s2mm, intact);
        event(-1, write ? (blocked ? "tz_write_blocked" : "tz_write_open") : (blocked ? "tz_read_blocked" : "tz_read_open"),
              "L0-H4-03", detail);
        if (!pl_ip_reset() || !interface_init() || !blocked || !golden_check(-1, 0)) safehold(-1, "INITIAL:tz-probe");
    }
}

static void initial(void)
{
    char fpga[32] = "";
    FILE *s = fopen("/sys/class/fpga_manager/fpga0/state", "r");
    double temp = xadc("temp0");
    uint32_t pll = rd(slcr, SLCR_PLL_STATUS), params = rd(guard, FMG_PARAMS);
    if (s) { if (fscanf(s, "%31s", fpga) != 1) fpga[0] = 0; fclose(s); }
    int ground = cfg.ground_reset || ground_requested();
    event(-1, "initial_start", "", "");
    if (state.safehold && !ground) safehold(-1, state.cause[0] ? state.cause : "persisted");
    if (ground) {
        memset(&state, 0, sizeof(state));
        fm_state_save(cfg.state, &state);
        if (!cfg.ground_reset) unlink(GROUND_RESET_FILE);   // a ground reset the OBC sent over CAN applies once
        event(-1, "ground_reset", "", cfg.ground_reset ? "configuration" : "obc");
    }
    if (cfg.boot_limit && state.boot_pending >= cfg.boot_limit) {        // L2-G1-02: stop using this image
        state.safehold = 1;
        snprintf(state.cause, sizeof(state.cause), "L2-G1-02");
        fm_state_save(cfg.state, &state);
        event(-1, "boot_limit", "L2-G1-02", "Healthy Bit missing after repeated Initial runs");
        safehold(-1, "L2-G1-02");
    }
    state.boot_pending++;
    fm_state_save(cfg.state, &state);
    if (strcmp(fpga, "operating")) safehold(-1, "INITIAL:fpga");
    // the guard's frame (a 16-bit field): the model's output frame, or a hybrid model's stream operation
    if (rd(guard, FMG_ID) != FMG_ID_VALUE
        || (params & 0xFFFFu) != ((hybrid_on() ? hybrid_stream_beats() : cfg.out_frame_bytes / cfg.out_beat_bytes) & 0xFFFFu))
        safehold(-1, "INITIAL:guard");
    if (!plls_locked(pll)) safehold(-1, "INITIAL:pll");
    {   // L0-H4-01: L2 and OCM parity on; errors from before the start are recorded and cleared, not counted
        char text[192];
        parity_read(&parity_last);
        fm_parity_text(text, sizeof(text), &parity_last);
        event(-1, fm_parity_enabled(&parity_last) == (FM_PARITY_L2 | FM_PARITY_OCM) ? "parity_on" : "parity_off",
              fm_names[FM_L0_H4_01], text);
        if (parity_last.l2_raw & (FM_L2C_PARRT | FM_L2C_PARRD))
            wr(l2c, FM_L2C_INTR_CLEAR, parity_last.l2_raw & (FM_L2C_PARRT | FM_L2C_PARRD));
        parity_last.l2_raw = 0;
        if (fm_parity_enabled(&parity_last) != (FM_PARITY_L2 | FM_PARITY_OCM)) safehold(-1, "INITIAL:parity");
    }
    if (temp > cfg.temp_limit_c) safehold(-1, "INITIAL:temperature");
    if (cfg.tz) {   // T1: exactly the pool's sections are open to the PL
        char text[96];
        uint32_t ddr = rd(slcr, FM_SLCR_TZ_DDR_RAM - SLCR_BASE), want = tz_pool_mask();
        snprintf(text, sizeof(text), "tz_ddr_ram=0x%x pool=0x%llx+%u want=0x%x", ddr, (unsigned long long)info.dma_address,
                 info.bytes, want);
        event(-1, ddr == want ? "tz_on" : "tz_off", "L0-H4-03", text);
        if (ddr != want) safehold(-1, "INITIAL:tz");
    }
    if (!interface_init()) safehold(-1, "INITIAL:dma");
    protect_golden(0);
    guard_configure();
    // a warm turn (window mode) checks the golden frames only when the last check of this model is warm_golden_s old
    int golden_due = !warm_turn || !cfg.warm_golden_s || injected(-1, "golden")
                     || boot_seconds() - warm_golden_at >= cfg.warm_golden_s;
    if (golden_due && !golden_check(-1, injected(-1, "golden"))) safehold(-1, "INITIAL:golden");
    if (cfg.tz_probe) tz_probes();
    state.boot_pending = 0;                                               // Healthy Bit
    fm_state_save(cfg.state, &state);
    event(-1, "initial_pass", "", !warm_turn ? "standby" : golden_due ? "standby, warm" : "standby, warm, golden not due");
}

// ---------------------------------------------------------------- throughput (FM cost)
// Every frame of the batch file in the pool input at once, then repeats of transfers of at most `step` frames: as many
// as the pool output region holds (all of them when it holds all). A repeat's time is the sum of its transfers.
static void throughput(void)
{
    size_t bytes;
    const uint8_t *batches = load(cfg.input, &bytes);
    uint32_t frames = 0, mm2s, s2mm, step;
    size_t offset = 0, out_bytes;
    uint64_t total = 0, best = ~0ull;
    if (feeder) { errno = EINVAL; fail("throughput: a set model moves one frame a transfer (mission mode)"); }
    while (offset + sizeof(struct fm_batch_header) <= bytes) {
        struct fm_batch_header h;
        memcpy(&h, batches + offset, sizeof(h));
        if (h.payload_bytes > bytes - offset - sizeof(h) || h.payload_bytes != (uint64_t)h.frames * cfg.in_frame_bytes
            || ((uint64_t)frames + h.frames) * cfg.in_frame_bytes > in_limit) { errno = EINVAL; fail("throughput input"); }
        memcpy(pool + (size_t)frames * cfg.in_frame_bytes, batches + offset + sizeof(h), h.payload_bytes);
        frames += h.frames;
        offset += sizeof(h) + h.payload_bytes;
    }
    step = out_capacity / cfg.out_frame_bytes;
    if (step > frames) step = frames;
    if (engine_on() && !hybrid_on()) {
        if (cfg.engine_steps > step || frames % cfg.engine_steps) { errno = EINVAL; fail("throughput engine windows"); }
        step = cfg.engine_steps;
    }
    if (!frames || !step) { errno = EINVAL; fail("throughput frames"); }
    out_bytes = (size_t)frames * cfg.out_frame_bytes;
    uint8_t *first = malloc(out_bytes);
    if (!first) fail("throughput output");
    unsigned mismatched = 0;
    for (uint32_t r = 0; r < cfg.repeat + 1; r++) {
        uint64_t elapsed = 0;
        int differs = 0;
        for (uint32_t start = 0; start < frames; start += step) {
            uint32_t n = frames - start < step ? frames - start : step;
            size_t part = (size_t)n * cfg.out_frame_bytes;
            memset(pool + out_region, 0xA5, part);
            heartbeat();
            engine_frame = start / step;
            uint64_t t = transfer_at(start * cfg.in_frame_bytes, n, (uint64_t)cfg.batch_timeout_ms * 1000000ull, &mm2s, &s2mm, 0, 0);
            if (!t || engine_fault || rd(guard, FMG_FAULTS)) { event(r, "throughput_fault", "", ""); safehold(r, "THROUGHPUT"); }
            elapsed += t;
            if (r == 0) memcpy(first + (size_t)start * cfg.out_frame_bytes, pool + out_region, part);
            else differs |= memcmp(first + (size_t)start * cfg.out_frame_bytes, pool + out_region, part) != 0;
        }
        if (r == 0) {
            FILE *o = fopen(cfg.output, "wb");
            if (!o || fwrite(first, 1, out_bytes, o) != out_bytes || fclose(o)) fail(cfg.output);
            continue;   // warmup
        }
        mismatched += differs;
        total += elapsed;
        if (elapsed < best) best = elapsed;
    }
    double mean_s = (double)total / cfg.repeat / 1e9;
    printf("{\"schema\":\"f-astra/fm-throughput/v1\",\"frames\":%u,\"transfer_frames\":%u,\"repeat\":%u,\"mean_ns\":%.1f,"
           "\"min_ns\":%llu,\"gops_mean\":%.6f,\"mismatched_runs\":%u,\"guard_checks\":true,\"guard_faults\":%u,"
           "\"max_latency_cycles\":%u}\n",
           frames, step, cfg.repeat, (double)total / cfg.repeat, (unsigned long long)best,
           frames * cfg.ops_per_frame / mean_s / 1e9, mismatched, rd(guard, FMG_FAULTS), rd(guard, FMG_MAX_LATENCY));
    event(-1, "throughput_done", "", "");
}

// ---------------------------------------------------------------- one batch (mission and serve)
// One batch through the input, environment and memory checks, the transfer with the guard, the result rules and, on a
// fault, the v2 recovery ladder of its first item (one retry when the ladder says so). `result` receives the frames'
// outputs or the safe value; returns the outcome: "ok", "retried", "safe", "discarded" or "blocked". `written` is the
// frame count before this batch (an engine-fed model's window), `last_sequence` the newest sequence taken so far.
static const char *run_batch(long batch, const struct fm_batch_header *hp, uint8_t *payload, size_t available,
                             uint8_t *result, uint32_t written, uint64_t last_sequence)
{
    struct fm_batch_header h = *hp;
    uint32_t frames = h.frames <= cfg.batch_frames ? h.frames : cfg.batch_frames;
    uint32_t elements = frames * cfg.out_elems_per_frame;
    const char *outcome = "ok";
    int attempt = 0;
    again:
        {
            struct fm_obs o;
            struct fm_batch_header seen = h;
            uint32_t mm2s = 0, s2mm = 0, bad_index = 0;
            uint64_t elapsed = 0;
            enum fm_id ids[FM_COUNT];
            const char *why = 0;
            int first = attempt == 0, tz_decerr = 0;
            memset(&o, 0, sizeof(o));
            if (first && injected(batch, "crc")) payload[0] ^= 0x5A;
            if (first && injected(batch, "stale")) seen.sequence = last_sequence;
            if (first && injected(batch, "length")) seen.frames += 1;
            why = fm_check_batch(&seen, payload, available, cfg.in_frame_bytes + cfg.engine_rows_bytes, cfg.batch_frames,
                                 last_sequence);
            if (!why && seen.kind != (cfg.engine_rows_bytes ? FM_KIND_ENGINE_ROWS : 0)) why = "kind";
            if (first && injected(batch, "crc")) payload[0] ^= 0x5A;   // the stored batch stays intact
            o.input_bad = why != 0;
            if (!skip_env_checks) {
                double temp = xadc("temp0"), vcc = xadc("voltage0_vccint");
                double limit = first && injected(batch, "temperature") ? temp - 1.0 : cfg.temp_limit_c;
                o.environment_alarm = temp > limit || (cfg.vccint_max > 0 && (vcc < cfg.vccint_min || vcc > cfg.vccint_max));
                o.pll_unlocked = !plls_locked(rd(slcr, SLCR_PLL_STATUS)) || (first && injected(batch, "pll"));
                if (first && injected(batch, "scrub")) corrupt_golden();
                if (first && injected(batch, "weights")) corrupt_weights();
                o.memory_corrupt = !scrub_golden();
                if (!weights_scrub(batch)) o.memory_corrupt = 1;
                o.parity_errors = parity_check(batch, first && injected(batch, "parity"));
                if (first && injected(batch, "protect")) o.protection_violation = attempt_protected_write();
                if (cfg.tz && rd(slcr, FM_SLCR_TZ_DDR_RAM - SLCR_BASE) != tz_pool_mask()) o.protection_violation = 1;
            }
            if (!o.input_bad && !o.environment_alarm && !o.pll_unlocked && !o.memory_corrupt && !o.parity_errors
                && !o.protection_violation) {
                uint32_t inj = 0;
                if (first && injected(batch, "guard-range")) inj |= FMG_INJECT_RANGE;
                if (first && injected(batch, "guard-format")) inj |= FMG_INJECT_FORMAT;
                if (first && injected(batch, "guard-stop")) inj |= FMG_INJECT_STOP_OUTPUT;
                if (first && injected(batch, "guard-skip-input")) inj |= FMG_INJECT_SKIP_INPUT_START;
                wr(guard, FMG_INJECT, inj ? (FMG_INJECT_KEY << 16) | inj : 0);
                if (first && injected(batch, "heartbeat-stop")) {
                    usleep((useconds_t)((uint64_t)cfg.wdt_cycles * 1000ull / cfg.clock_khz * 2 + 1000));
                } else heartbeat();
                if (cfg.engine_rows_bytes) {
                    // the batch's engine rows and tail input into the batch window (after the corpus's windows), the
                    // tail input also at the pool start for the model's DMA
                    size_t rows = (size_t)frames * cfg.engine_rows_bytes;
                    memcpy(pool + cfg.engine_pool_offset + (size_t)cfg.engine_frames * cfg.engine_window_bytes
                           + cfg.engine_block_offset, payload, rows + (size_t)frames * cfg.in_frame_bytes);
                    memcpy(pool, payload + rows, (size_t)frames * cfg.in_frame_bytes);
                    engine_frame = cfg.engine_frames;
                } else {
                    memcpy(pool, payload, (size_t)frames * cfg.in_frame_bytes);
                    engine_frame = engine_on() && !hybrid_on() ? written / cfg.engine_steps : 0;
                }
                memset(pool + out_region, 0xA5, (size_t)frames * cfg.out_frame_bytes);
                transfer_set = h.reserved;                  // a set model's frame: its set index
                engine_inject = NULL;
                if (first && engine_on()) {
                    static const char *const kinds[] = {"engine-freeze", "engine-wdt", "engine-read-error",
                                                        "engine-write-error", "engine-window"};
                    for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++)
                        if (injected(batch, kinds[k])) engine_inject = kinds[k];
                }
                uint64_t timeout_ns = first && injected(batch, "timeout") ? 1000ull : (uint64_t)cfg.batch_timeout_ms * 1000000ull;
                elapsed = transfer(frames, timeout_ns, &mm2s, &s2mm,
                                   first && injected(batch, "dma-late-s2mm"), first && injected(batch, "dma-short"));
                wr(guard, FMG_INJECT, 0);
                engine_inject = NULL;
                o.dma_status_errors = (mm2s | s2mm) & FM_DMA_ERRORS;
                if (cfg.tz && ((mm2s | s2mm) & SR_DEC_ERR)) {   // T1: a PL access outside the open DDR sections
                    tz_decerr = 1;
                    o.protection_violation = 1;
                    o.dma_status_errors &= ~(uint32_t)SR_DEC_ERR;
                }
                o.transfer_timeout = (elapsed == 0 && !o.dma_status_errors) || engine_fault;
                o.guard_faults = rd(guard, FMG_FAULTS);
                if (engine_fault_bits && !(o.guard_faults & ~FM_GUARD_DEADLINE) && !o.dma_status_errors) {
                    // the engine stopped first: the tail's timeout and frame deadline behind it are the consequence
                    uint32_t f = engine_fault_bits;
                    char detail[64];
                    o.transfer_timeout = (f & SF_DRAIN) != 0;
                    o.dma_status_errors = (f & (SF_R_ERROR | SF_B_ERROR | SF_STATE)) ? 0x40u : 0;
                    o.guard_faults = ((f & SF_DEADLINE) ? FM_GUARD_DEADLINE : 0) | ((f & SF_WDT) ? FM_GUARD_WDT : 0);
                    o.protection_violation = (f & (SF_WINDOW_R | SF_WINDOW_W)) != 0;
                    o.result_bad = (f & SF_WIDTH) != 0;
                    o.input_bad = (f & SF_DESCRIPTOR) != 0;
                    snprintf(detail, sizeof(detail), "shell_faults=0x%x", f);
                    event(batch, "engine_fault", "", detail);
                }
                if (elapsed && first && injected(batch, "late")) o.inference_late = 1;   // deadline shortened to zero
                else if (elapsed && cfg.inference_deadline_us && elapsed > (uint64_t)cfg.inference_deadline_us * 1000ull)
                    o.inference_late = 1;
                if (elapsed && !o.guard_faults && !o.dma_status_errors) {
                    if (first && injected(batch, "result")) fm_fill_safe(pool + out_region, 1, cfg.out_elem_bytes, cfg.max + 1);
                    o.result_bad = fm_check_result(pool + out_region, (size_t)frames * cfg.out_frame_bytes, elements,
                                                   cfg.out_elem_bytes, cfg.out_elem_bits, (int)cfg.out_signed, cfg.min, cfg.max,
                                                   &bad_index);
                }
            }
            int n = fm_classify(&o, ids, FM_COUNT);
            if (o.protection_violation) {
                char where[64];
                snprintf(where, sizeof(where), "%s", tz_decerr ? "PL access outside the open DDR sections (TrustZone DECERR)"
                         : engine_fault_bits & (SF_WINDOW_R | SF_WINDOW_W) ? "engine access outside its WINDOW (shell)"
                         : cfg.tz && rd(slcr, FM_SLCR_TZ_DDR_RAM - SLCR_BASE) != tz_pool_mask() ? "TZ_DDR_RAM changed"
                         : "blocked write at golden+0 (MMU read-only)");
                event(batch, "protection", fm_names[FM_L0_H4_03], where);
            }
            if (n == 0) {
                memcpy(result, pool + out_region, (size_t)frames * cfg.out_frame_bytes);
                if (attempt) { outcome = "retried"; event(batch, "retry_pass", "", ""); }
            } else {
                char detail[200];
                int count, len = snprintf(detail, sizeof(detail), "%s guard=0x%x dma=0x%x elapsed_ns=%llu", why ? why : "",
                                          o.guard_faults, o.dma_status_errors, (unsigned long long)elapsed);
                for (int i = 1; i < n && len > 0 && (size_t)len < sizeof(detail); i++)
                    len += snprintf(detail + len, sizeof(detail) - (size_t)len, " also=%s", fm_names[ids[i]]);
                event(batch, "detected", fm_names[ids[0]], detail);
                if (fm_count_recovery(&state, ids[0], cfg.recovery_limit)) {
                    fm_state_save(cfg.state, &state);
                    event(batch, "recovery_limit", fm_names[ids[0]], "L2-G1-01");
                    safehold(batch, fm_names[ids[0]]);
                }
                fm_state_save(cfg.state, &state);
                const enum fm_action *plan = fm_plan(ids[0], &count);
                int retry = 0;
                for (int a = 0; a < count; a++) {
                    switch (plan[a]) {
                    case ACT_DISCARD: fm_fill_safe(result, elements, cfg.out_elem_bytes, cfg.safe); outcome = "discarded"; break;
                    case ACT_SAFE_OUTPUT: fm_fill_safe(result, elements, cfg.out_elem_bytes, cfg.safe); outcome = "safe"; break;
                    case ACT_BLOCK:
                        wr(guard, FMG_CONTROL, control_bits() | FMG_CTRL_HOLD);
                        fm_fill_safe(result, elements, cfg.out_elem_bytes, cfg.safe);
                        outcome = "blocked";
                        break;
                    case ACT_INTERFACE_INIT: if (!interface_init()) safehold(batch, "interface_init"); break;
                    case ACT_PL_IP_RESET: if (!pl_ip_reset()) safehold(batch, "pl_ip_reset"); break;
                    case ACT_REVERIFY: if (!golden_check(batch, 0)) safehold(batch, "reverify"); break;
                    case ACT_RETRY: retry = 1; break;
                    case ACT_SAFEHOLD: event(batch, "action", fm_names[ids[0]], "safehold"); safehold(batch, fm_names[ids[0]]); break;
                    case ACT_RELOAD: if (!protect_golden(1) || !weights_reload() || !scrub_golden()) safehold(batch, "reload"); break;
                    default: break;
                    }
                    event(batch, "action", fm_names[ids[0]], fm_action_names[plan[a]]);
                }
                if ((rd(guard, FMG_STATUS) & 0x3u) || rd(guard, FMG_FAULTS)) pl_ip_reset();   // leave a clean guard
                event(batch, "recovered", fm_names[ids[0]], outcome);
                if (retry && attempt == 0) { attempt = 1; goto again; }
            }
        }
    return outcome;
}

// ---------------------------------------------------------------- GRU step loop (FM R6 B2)
// A batch of kind FM_KIND_GRU_STEPS carries the x(t) codes of `frames` steps (gru_nx int16 each). The service keeps the
// hidden state h (gru_hidden int16; zero at the start) and runs every step as a batch of the engine-fed model through
// run_batch, so each step gets the checks and the recovery ladder: row 0 of the batch window is [h on lanes 0..,
// x(t) on lanes gru_x_lane..] and hidden row 0 is h, the program's other rows stay zero. Row 0 of the tail's output
// ([h', indices]) is the step's output and h' is fed back. A step that does not end ok or retried leaves its safe
// output and resets h to zero (event gru_state_reset: the encoder starts a fresh block). Environment and memory checks
// run every step_check_every steps. Step injections count steps from the start of the run (`step_count`).
static int16_t gru_h[256];
static long step_count;
static FILE *step_flags;                      // the test feed's per-step verdicts ("<output>.steps")

static int outcome_rank(const char *o)
{
    static const char *const order[] = {"ok", "retried", "safe", "discarded", "blocked"};
    for (int i = 0; i < 5; i++) if (!strcmp(o, order[i])) return i;
    return 5;
}

static const char *run_steps(long batch, const struct fm_batch_header *hp, uint8_t *payload, size_t available,
                             uint8_t *result, uint64_t last_sequence)
{
    static uint8_t *sub, *rows_out;
    size_t erows = (size_t)cfg.engine_steps * cfg.engine_rows_bytes;
    size_t block = erows + (size_t)cfg.engine_steps * cfg.in_frame_bytes;
    uint32_t frames = hp->frames <= cfg.batch_frames ? hp->frames : cfg.batch_frames;
    const char *why = fm_check_batch(hp, payload, available, cfg.gru_nx * 2, cfg.batch_frames, last_sequence), *worst = "ok";
    if (!why && hp->kind != FM_KIND_GRU_STEPS) why = "kind";
    if (why) {   // L1-S2-02: the batch is not taken and h stays (discard, interface init)
        int count;
        const enum fm_action *plan = fm_plan(FM_L1_S2_02, &count);
        event(batch, "detected", fm_names[FM_L1_S2_02], why);
        fm_fill_safe(result, frames * cfg.out_elems_per_frame, cfg.out_elem_bytes, cfg.safe);
        for (int a = 0; a < count; a++) {
            if (plan[a] == ACT_INTERFACE_INIT && !interface_init()) safehold(batch, "interface_init");
            event(batch, "action", fm_names[FM_L1_S2_02], fm_action_names[plan[a]]);
        }
        event(batch, "recovered", fm_names[FM_L1_S2_02], "discarded");
        return "discarded";
    }
    if (!sub && (!(sub = malloc(block)) || !(rows_out = malloc((size_t)cfg.engine_steps * cfg.out_frame_bytes))))
        fail("GRU step buffers");
    for (uint32_t t = 0; t < frames; t++) {
        struct fm_batch_header sh;
        memset(sub, 0, block);
        memcpy(sub, gru_h, cfg.gru_hidden * 2);                                      // engine row 0: h
        memcpy(sub + cfg.gru_x_lane * 2, payload + (size_t)t * cfg.gru_nx * 2, cfg.gru_nx * 2);   //   and x(t)
        memcpy(sub + erows, gru_h, cfg.gru_hidden * 2);                              // hidden row 0: h
        memset(&sh, 0, sizeof(sh));
        sh.magic = FM_BATCH_MAGIC; sh.version = 1; sh.kind = FM_KIND_ENGINE_ROWS; sh.frames = cfg.engine_steps;
        sh.payload_bytes = (uint32_t)block; sh.sequence = 1; sh.crc32 = fm_crc32(sub, block);
        skip_env_checks = step_count % cfg.step_check_every != 0;
        const char *o = run_batch(step_count, &sh, sub, block, rows_out, 0, 0);
        skip_env_checks = 0;
        memcpy(result + (size_t)t * cfg.out_frame_bytes, rows_out, cfg.out_frame_bytes);
        if (!strcmp(o, "ok") || !strcmp(o, "retried")) memcpy(gru_h, rows_out, cfg.gru_hidden * 2);
        else {
            memset(gru_h, 0, sizeof(gru_h));
            event(step_count, "gru_state_reset", "", o);
        }
        if (outcome_rank(o) > outcome_rank(worst)) worst = o;
        if (step_flags) fprintf(step_flags, "%ld 1 %s\n", step_count, o);
        step_count++;
    }
    return worst;
}

// ---------------------------------------------------------------- mission
static void mission(void)
{
    size_t bytes, offset = 0;
    uint8_t *batches = load(cfg.input, &bytes);
    uint8_t *output = calloc((size_t)cfg.frames_total + 1, cfg.out_frame_bytes);
    FILE *flags = NULL;
    char flags_path[300];
    uint64_t last_sequence = 0;
    uint32_t written = 0;
    long batch = 0;
    snprintf(flags_path, sizeof(flags_path), "%s.batches", cfg.output);
    flags = fopen(flags_path, "w");
    if (!output || !flags) fail("mission buffers");
    for (; offset + sizeof(struct fm_batch_header) <= bytes; batch++) {
        struct fm_batch_header h;
        uint8_t *payload = batches + offset + sizeof(h);
        memcpy(&h, batches + offset, sizeof(h));
        size_t available = bytes - offset - sizeof(h);
        uint32_t frames = h.frames <= cfg.batch_frames ? h.frames : cfg.batch_frames;
        if (frames > cfg.frames_total - written) {
            // a header claiming more frames than the campaign has left (corrupt or replayed input): run_batch would
            // fill its safe output past the end of the buffer, so the mission ends here with what it has
            fprintf(flags, "%ld %u beyond_frames_total\n", batch, frames);
            event(batch, "input", "L1-S2-02", "beyond_frames_total");
            break;
        }
        uint8_t *result = output + (size_t)written * cfg.out_frame_bytes;
        offset += sizeof(h) + (h.payload_bytes <= available ? h.payload_bytes : available);
        worker_supervision_injections(batch);
        const char *outcome = run_batch(batch, &h, payload, available, result, written, last_sequence);
        if (h.sequence > last_sequence) last_sequence = h.sequence;
        fprintf(flags, "%ld %u %s\n", batch, frames, outcome);
        written += frames;
    }
    fclose(flags);
    FILE *o = fopen(cfg.output, "wb");
    if (!o || fwrite(output, cfg.out_frame_bytes, written, o) != written || fclose(o)) fail(cfg.output);
    event(-1, "mission_complete", "", "");
    write_health("STANDBY");
    printf("{\"schema\":\"f-astra/fm-mission/v1\",\"batches\":%ld,\"frames\":%u,\"safehold\":%u}\n", batch, written, state.safehold);
}

// ---------------------------------------------------------------- serve (always-on operation, FM R6)
// The FM service as the operational path: batches arrive one file each in `spool` ("<sequence>.batch": one batch
// header and its payload, written under another name and renamed into place by the producer; the spool is a tmpfs
// folder). They are taken in sequence order and each goes through run_batch, the same checks and recovery ladder as a
// mission batch. Its output frames go to "<sequence>.out" and its verdict line ("<batch> <frames> <outcome>
// <latency_ns>") to "<sequence>.fm" in `spool_out` (written as ".part" and renamed), and the batch file is removed.
// Between batches the service keeps the PS watchdog, the PL heartbeat and its supervisor fed, checks the environment,
// the PLLs and the memory (scrub, parity) every idle_check_ms with the items and plans of a batch, and the golden
// frames every golden_every_s. The newest sequence taken is kept in the state file (state_every batches), so a batch
// replayed after a restart is stale. A "STOP" file in the spool or SIGTERM ends the service after the current batch;
// serve_idle_exit_ms ends it once the spool has stayed empty that long (one model's turn, f_astra_fm_schedule.sh), and
// serve_turn_ms after the batch that reaches that turn length (a model with steady arrivals cannot keep the PL).
// serve_yield (the other models' spools, comma separated) ends the turn as soon as the own spool is empty while one of
// them holds a batch, instead of waiting out serve_idle_exit_ms.
// An engine-fed model is served with batches of kind FM_KIND_ENGINE_ROWS (engine_rows_bytes): every frame's engine row,
// then every frame's tail input, written into a window of their own after the corpus's windows (the golden frames
// keep the first), so the engine's program runs on the batch's own rows.
// Test feed (serve_feed_ms > 0): a child process writes the batches of `input` into the spool, one every serve_feed_ms;
// the service stops once they are all answered and also writes `output` and its ".batches" verdicts, as a mission
// does, so a campaign judges the run the same way.
static volatile sig_atomic_t serve_stop;
static void on_serve_stop(int sig) { (void)sig; serve_stop = 1; }

// the batch file of the lowest sequence in the spool, or 0 when there is none
static uint64_t spool_next(char *name, size_t size)
{
    DIR *d = opendir(cfg.spool);
    struct dirent *e;
    uint64_t best = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char *end;
        unsigned long long s = strtoull(e->d_name, &end, 10);
        if (end != e->d_name && !strcmp(end, ".batch") && s && (!best || s < best)) {
            best = s;
            snprintf(name, size, "%.48s", e->d_name);
        }
    }
    closedir(d);
    return best;
}

// serve_yield: does another model's spool hold a batch (sequence 1 or above, as the scheduler counts)?
static int yield_waiting(void)
{
    char list[sizeof(cfg.serve_yield)], *save = NULL;
    memcpy(list, cfg.serve_yield, sizeof(list));
    for (char *folder = strtok_r(list, ",", &save); folder; folder = strtok_r(NULL, ",", &save)) {
        DIR *d = opendir(folder);
        struct dirent *e;
        int found = 0;
        if (!d) continue;
        while (!found && (e = readdir(d))) {
            char *end;
            unsigned long long s = strtoull(e->d_name, &end, 10);
            found = e->d_name[0] >= '1' && e->d_name[0] <= '9' && !strcmp(end, ".batch") && s;
        }
        closedir(d);
        if (found) return 1;
    }
    return 0;
}

static int spool_write(const char *folder, const char *name, const void *data, size_t bytes)
{
    char path[600], part[608];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", folder, name);
    snprintf(part, sizeof(part), "%s.part", path);
    if (!(f = fopen(part, "wb")) || (bytes && fwrite(data, 1, bytes, f) != bytes) || fclose(f)) return -1;
    return rename(part, path);
}

// the test feeder: the batches of `input`, one file every serve_feed_ms (a child process)
static void serve_feed(void)
{
    size_t bytes, offset = 0;
    uint8_t *batches = load(cfg.input, &bytes);
    while (offset + sizeof(struct fm_batch_header) <= bytes) {
        struct fm_batch_header h;
        char name[64];
        memcpy(&h, batches + offset, sizeof(h));
        size_t n = sizeof(h) + h.payload_bytes;
        if (n > bytes - offset) n = bytes - offset;
        snprintf(name, sizeof(name), "%llu.batch", (unsigned long long)h.sequence);
        if (spool_write(cfg.spool, name, batches + offset, n)) _exit(2);
        offset += n;
        usleep((useconds_t)cfg.serve_feed_ms * 1000u);
    }
    _exit(0);
}

// between batches: environment, PLLs and memory with the items and v2 plans of a batch (no output to hold)
static void idle_checks(long batch)
{
    struct fm_obs o;
    enum fm_id ids[FM_COUNT];
    int n, count;
    memset(&o, 0, sizeof(o));
    {
        double temp = xadc("temp0"), vcc = xadc("voltage0_vccint");
        o.environment_alarm = temp > cfg.temp_limit_c || (cfg.vccint_max > 0 && (vcc < cfg.vccint_min || vcc > cfg.vccint_max));
    }
    o.pll_unlocked = !plls_locked(rd(slcr, SLCR_PLL_STATUS));
    o.memory_corrupt = !scrub_golden();
    if (!weights_scrub(batch)) o.memory_corrupt = 1;
    o.parity_errors = parity_check(batch, 0);
    if (!(n = fm_classify(&o, ids, FM_COUNT))) return;
    event(batch, "detected", fm_names[ids[0]], "between batches");
    if (fm_count_recovery(&state, ids[0], cfg.recovery_limit)) {
        fm_state_save(cfg.state, &state);
        event(batch, "recovery_limit", fm_names[ids[0]], "L2-G1-01");
        safehold(batch, fm_names[ids[0]]);
    }
    fm_state_save(cfg.state, &state);
    const enum fm_action *plan = fm_plan(ids[0], &count);
    for (int a = 0; a < count; a++) {
        switch (plan[a]) {
        case ACT_INTERFACE_INIT: if (!interface_init()) safehold(batch, "interface_init"); break;
        case ACT_PL_IP_RESET: if (!pl_ip_reset()) safehold(batch, "pl_ip_reset"); break;
        case ACT_REVERIFY: if (!golden_check(batch, 0)) safehold(batch, "reverify"); break;
        case ACT_RELOAD: if (!protect_golden(1) || !weights_reload() || !scrub_golden()) safehold(batch, "reload"); break;
        case ACT_SAFEHOLD: event(batch, "action", fm_names[ids[0]], "safehold"); safehold(batch, fm_names[ids[0]]); break;
        default: continue;   // safe output, discard, block, retry: no batch in flight
        }
        event(batch, "action", fm_names[ids[0]], fm_action_names[plan[a]]);
    }
    event(batch, "recovered", fm_names[ids[0]], "between batches");
}

static void serve(void)
{
    uint8_t *output = calloc((size_t)cfg.batch_frames + 1, cfg.out_frame_bytes);
    FILE *flags = NULL, *all = NULL;
    char flags_path[300], stop_path[300], detail[160];
    uint32_t written = 0;
    long batch = 0;
    pid_t feeder = 0;
    int fed = 0;
    uint64_t last_check = now_ns(), last_golden = now_ns(), started = now_ns(), latency_sum = 0, latency_max = 0;
    uint64_t last_batch = now_ns();     // serve_idle_exit_ms: the scheduler's turn ends when the spool stays empty
    if (!output || !cfg.spool[0] || !cfg.spool_out[0]) { errno = EINVAL; fail("serve: spool and spool_out"); }
    if (engine_on() && !hybrid_on() && !cfg.engine_rows_bytes)
        { errno = EINVAL; fail("serve: an engine-fed model's batches carry its engine rows (engine_rows_bytes)"); }
    if ((mkdir(cfg.spool, 0755) && errno != EEXIST) || (mkdir(cfg.spool_out, 0755) && errno != EEXIST)) fail("serve: spool");
    snprintf(stop_path, sizeof(stop_path), "%s/STOP", cfg.spool);
    signal(SIGTERM, on_serve_stop);
    signal(SIGINT, on_serve_stop);
    if (cfg.serve_feed_ms) {
        snprintf(flags_path, sizeof(flags_path), "%s.batches", cfg.output);
        if (!(flags = fopen(flags_path, "w")) || !(all = fopen(cfg.output, "wb"))) fail("serve: test output");
        if (cfg.gru_step) {
            snprintf(flags_path, sizeof(flags_path), "%s.steps", cfg.output);
            if (!(step_flags = fopen(flags_path, "w"))) fail("serve: test output");
        }
        fflush(NULL);
        if ((feeder = fork()) < 0) fail("serve: feeder");
        if (feeder == 0) {
            // a backstop: the feeder ends with the service even when the service is killed
            if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() == 1) _exit(2);
            serve_feed();
        }
        serve_feeder = feeder;
    }
    snprintf(detail, sizeof(detail), "spool=%.100s last_sequence=%llu", cfg.spool, (unsigned long long)state.last_sequence);
    event(-1, "serve_start", "", detail);
    write_health("SERVE");
    for (;;) {
        char name[64], path[600];
        uint64_t now = now_ns(), seq = spool_next(name, sizeof(name));
        if (!seq) {
            int st;
            if (feeder && !fed && waitpid(feeder, &st, WNOHANG) == feeder) {
                fed = 1;
                if (!WIFEXITED(st) || WEXITSTATUS(st)) { errno = EIO; fail("serve: feeder"); }
            }
            if (serve_stop || access(stop_path, F_OK) == 0 || (feeder && fed && !spool_next(name, sizeof(name)))) break;
            if (cfg.serve_yield[0] && batch && yield_waiting()) break;      // another model waits: hand over at once
            if (cfg.serve_idle_exit_ms && now - last_batch >= (uint64_t)cfg.serve_idle_exit_ms * 1000000ull) break;
            if (cfg.serve_turn_ms && now - started >= (uint64_t)cfg.serve_turn_ms * 1000000ull) break;
            if (now - last_check >= (uint64_t)cfg.idle_check_ms * 1000000ull) { idle_checks(batch); last_check = now_ns(); }
            if (cfg.golden_every_s && now - last_golden >= (uint64_t)cfg.golden_every_s * 1000000000ull) {
                if (!golden_check(batch, 0)) safehold(batch, "golden");
                last_golden = now_ns();
            }
            heartbeat();
            usleep(5000);
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", cfg.spool, name);
        {
            size_t bytes;
            uint8_t *data = load(path, &bytes);
            struct fm_batch_header h;
            char line[160], out_name[64];
            memset(&h, 0, sizeof(h));
            memcpy(&h, data, bytes < sizeof(h) ? bytes : sizeof(h));
            size_t available = bytes > sizeof(h) ? bytes - sizeof(h) : 0;
            uint32_t frames = h.frames <= cfg.batch_frames ? h.frames : cfg.batch_frames;
            worker_supervision_injections(batch);
            uint8_t *body = data + (bytes > sizeof(h) ? sizeof(h) : bytes);
            const char *outcome = cfg.gru_step ? run_steps(batch, &h, body, available, output, state.last_sequence)
                                               : run_batch(batch, &h, body, available, output, written, state.last_sequence);
            uint64_t latency = now_ns() - now;
            if (h.sequence > state.last_sequence) {
                state.last_sequence = h.sequence;
                if (cfg.state_every && (batch + 1) % cfg.state_every == 0) fm_state_save(cfg.state, &state);
            }
            snprintf(out_name, sizeof(out_name), "%llu.out", (unsigned long long)seq);
            if (spool_write(cfg.spool_out, out_name, output, (size_t)frames * cfg.out_frame_bytes)) fail("serve: output");
            snprintf(line, sizeof(line), "%ld %u %s %llu\n", batch, frames, outcome, (unsigned long long)latency);
            snprintf(out_name, sizeof(out_name), "%llu.fm", (unsigned long long)seq);
            if (spool_write(cfg.spool_out, out_name, line, strlen(line))) fail("serve: verdict");
            if (unlink(path)) fail("serve: consume");
            if (flags) {
                fprintf(flags, "%ld %u %s\n", batch, frames, outcome);
                if (fwrite(output, cfg.out_frame_bytes, frames, all) != frames) fail("serve: test output");
            }
            free(data);
            latency_sum += latency;
            if (latency > latency_max) latency_max = latency;
            written += frames;
            batch++;
            last_check = last_batch = now_ns();      // a batch ran the same checks
            // serve_turn_ms: the turn ends after this batch, so a model with steady arrivals leaves the others theirs
            if (cfg.serve_turn_ms && last_batch - started >= (uint64_t)cfg.serve_turn_ms * 1000000ull) break;
        }
    }
    if (feeder && !fed) { kill(feeder, SIGTERM); waitpid(feeder, NULL, 0); }
    serve_feeder = 0;
    fm_state_save(cfg.state, &state);
    if (flags && fclose(flags)) fail("serve: test output");
    if (step_flags && fclose(step_flags)) fail("serve: test output");
    if (all && fclose(all)) fail(cfg.output);
    snprintf(detail, sizeof(detail), "batches=%ld last_sequence=%llu", batch, (unsigned long long)state.last_sequence);
    event(-1, "serve_stop", "", detail);
    write_health("STANDBY");
    printf("{\"schema\":\"f-astra/fm-serve/v1\",\"batches\":%ld,\"frames\":%u,\"last_sequence\":%llu,\"latency_mean_ns\":%.0f,"
           "\"latency_max_ns\":%llu,\"seconds\":%.3f,\"safehold\":%u}\n", batch, written,
           (unsigned long long)state.last_sequence, batch ? (double)latency_sum / batch : 0.0,
           (unsigned long long)latency_max, (double)(now_ns() - started) / 1e9, state.safehold);
}

// ---------------------------------------------------------------- service supervision (L1-S1-01, L1-S1-02)
// supervise:MODE runs MODE in a worker process (this binary, the same configuration) and watches it:
// - L1-S1-01: the worker's heartbeats arrive through a pipe within heartbeat_deadline_ms (default 10 s); a hung or
//   stopped worker is killed.
// - L1-S1-02: its end: a signal, or an exit code other than done (0), safehold (3) and setup failure (2), e.g. 4 for
//   an exhausted rlimit_data_mb or rlimit_nofile, which the supervisor sets on the worker.
// A failed worker is restarted (action restart, after a PL IP reset). More than restart_limit recoveries of an item, counted in the state
// file like every recovery (L2-G1-01), escalate to Safehold: the supervisor holds the PL output as safehold() does.
// While it supervises, the supervisor owns the PS watchdog (the worker's swdt_timeout_s is ignored) and feeds it only
// while the worker's heartbeat is on time.
static void worker_supervision_injections(long batch)
{
    char detail[64];
    if (supervision_injected(batch, "hang")) {
        event(batch, "inject", fm_names[FM_L1_S1_01], "hang (SIGSTOP)");
        raise(SIGSTOP);
    }
    if (supervision_injected(batch, "crash")) {
        event(batch, "inject", fm_names[FM_L1_S1_02], "crash (abort)");
        fflush(telemetry);
        abort();
    }
    if (supervision_injected(batch, "memory")) {
        struct rlimit r;
        size_t total = 0;
        if (getrlimit(RLIMIT_DATA, &r) || r.rlim_cur == RLIM_INFINITY || r.rlim_cur > (512ull << 20)) {
            errno = EPERM;
            fail("memory injection needs rlimit_data_mb <= 512 (a supervised worker)");
        }
        event(batch, "inject", fm_names[FM_L1_S1_02], "memory");
        for (;;) {
            uint8_t *p = malloc(16u << 20);
            if (!p) break;
            memset(p, 1, 16u << 20);
            total += 16u << 20;
        }
        snprintf(detail, sizeof(detail), "memory allocated_mb=%zu", total >> 20);
        event(batch, "resource_exhausted", fm_names[FM_L1_S1_02], detail);
        exit(4);
    }
    if (supervision_injected(batch, "fd")) {
        struct rlimit r;
        int opened = 0;
        if (getrlimit(RLIMIT_NOFILE, &r) || r.rlim_cur > 4096) { errno = EPERM; fail("fd injection needs rlimit_nofile <= 4096"); }
        event(batch, "inject", fm_names[FM_L1_S1_02], "fd");
        while (open("/dev/null", O_RDONLY) >= 0) opened++;
        snprintf(detail, sizeof(detail), "fd opened=%d errno=%d", opened, errno);
        event(batch, "resource_exhausted", fm_names[FM_L1_S1_02], detail);
        exit(4);
    }
}

static void worker_exec(const char *self, const char *path, const char *mode, int hb[2], int attempt)
{
    char fd_text[16], attempt_text[16];
    struct rlimit r;
    close(hb[0]);
    if (swdt_fd >= 0) close(swdt_fd);        // the watchdog stays with the supervisor (O_CLOEXEC as well)
    if (fcntl(hb[1], F_SETFL, O_NONBLOCK)) _exit(2);
    if (cfg.rlimit_data_mb) {
        r.rlim_cur = r.rlim_max = (rlim_t)cfg.rlimit_data_mb << 20;
        if (setrlimit(RLIMIT_DATA, &r)) _exit(2);
    }
    if (cfg.rlimit_nofile) {
        r.rlim_cur = r.rlim_max = cfg.rlimit_nofile;
        if (setrlimit(RLIMIT_NOFILE, &r)) _exit(2);
    }
    snprintf(fd_text, sizeof(fd_text), "%d", hb[1]);
    snprintf(attempt_text, sizeof(attempt_text), "%d", attempt);
    if (setenv("F_ASTRA_FM_HEARTBEAT_FD", fd_text, 1) || setenv("F_ASTRA_FM_ATTEMPT", attempt_text, 1)) _exit(2);
    execl("/proc/self/exe", self, path, mode, (char *)NULL);
    _exit(127);
}

static void map_pl(int mem_fd);

static int supervise(const char *self, const char *path, const char *mode)
{
    uint64_t deadline = (uint64_t)(cfg.heartbeat_deadline_ms ? cfg.heartbeat_deadline_ms : 10000) * 1000000ull;
    char detail[160];
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) fail("/dev/mem");
    map_pl(mem_fd);                           // for the Safehold of an escalation (output held, health record)
    load_state();
    if (state.safehold && !cfg.ground_reset && !ground_requested()) safehold(-1, state.cause[0] ? state.cause : "persisted");
    if (cfg.secure_watch) secure_watch_start();   // T3c: the workers' heartbeats keep the watchdog going
    swdt_arm();
    for (int attempt = 1;; attempt++) {
        swdt_worker_attempt = attempt;
        int hb[2], status = 0, hung = 0;
        long beats = 0;
        enum fm_id id;
        if (pipe(hb)) fail("heartbeat pipe");
        fflush(telemetry);
        pid_t pid = fork();
        if (pid < 0) fail("fork");
        if (pid == 0) worker_exec(self, path, mode, hb, attempt);
        close(hb[1]);
        snprintf(detail, sizeof(detail), "attempt=%d pid=%d mode=%s", attempt, (int)pid, mode);
        event(-1, "worker_start", "", detail);
        uint64_t last = now_ns();
        for (;;) {
            struct pollfd p = {hb[0], POLLIN, 0};
            if (poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
                char b[64];
                ssize_t n = read(hb[0], b, sizeof(b));
                if (n > 0) { beats += n; last = now_ns(); }
            }
            if (waitpid(pid, &status, WNOHANG) == pid) break;
            if (now_ns() - last > deadline) {    // L1-S1-01: no heartbeat within the deadline (hung or stopped)
                hung = 1;
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            swdt_feed();                          // the worker's heartbeat is on time
        }
        close(hb[0]);
        if (hung) {
            id = FM_L1_S1_01;
            snprintf(detail, sizeof(detail), "worker_hang attempt=%d beats=%ld deadline_ms=%llu", attempt, beats,
                     (unsigned long long)(deadline / 1000000ull));
        } else if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            snprintf(detail, sizeof(detail), "attempt=%d code=%d beats=%ld", attempt, code, beats);
            if (code == 0 || code == 3 || code == 2 || code == 64 || code == 127) {
                event(-1, code == 0 ? "worker_done" : code == 3 ? "worker_safehold" : "worker_setup_failed", "", detail);
                swdt_disarm();
                return code == 0 || code == 3 ? code : 2;
            }
            id = FM_L1_S1_02;
            snprintf(detail, sizeof(detail), "worker_exit attempt=%d code=%d beats=%ld", attempt, code, beats);
        } else {
            id = FM_L1_S1_02;
            snprintf(detail, sizeof(detail), "worker_signal attempt=%d signal=%d beats=%ld", attempt,
                     WIFSIGNALED(status) ? WTERMSIG(status) : 0, beats);
        }
        event(-1, "detected", fm_names[id], detail);
        snprintf(last_cause, sizeof(last_cause), "%s", fm_names[id]);
        load_state();                            // the worker keeps its own counts in the same file
        if (fm_count_recovery(&state, id, cfg.restart_limit)) {
            fm_state_save(cfg.state, &state);
            event(-1, "recovery_limit", fm_names[id], "L2-G1-01");
            safehold(-1, fm_names[id]);
        }
        fm_state_save(cfg.state, &state);
        // the dead worker may have left a transfer in flight: reset the PL interface it owned before the restart
        if (!pl_ip_reset()) event(-1, "pl_ip_reset_failed", fm_names[id], "before the restart");
        event(-1, "action", fm_names[id], fm_action_names[ACT_RESTART]);
    }
}

// ---------------------------------------------------------------- SEM controller (L0-H2-01, L0-H2-03)
enum { SEM_ID = 0x00, SEM_CTRL = 0x04, SEM_STATUS = 0x08, SEM_CORRECTED = 0x0C, SEM_UNCORRECT = 0x10, SEM_BEATS = 0x14,
       SEM_INJ_LO = 0x18, SEM_INJ_HI = 0x1C, SEM_MON_RX = 0x20, SEM_MON_TX = 0x24, SEM_MON_STAT = 0x28 };
enum { SEM_ST_INIT = 1u << 1, SEM_ST_OBSERVATION = 1u << 2, SEM_ST_INJECTION = 1u << 5, SEM_ST_UNCORRECTABLE = 1u << 7 };
#define SEM_ID_VALUE 0x53454D43u
#define DEVCFG_BASE 0xF8007000u
#define DEVCFG_PCAP_PR (1u << 27)
static volatile uint32_t *sem, *devcfg;
static FILE *sem_text;

static void sem_drain(void)
{
    for (int i = 0; i < 8192; i++) {
        uint32_t v = rd(sem, SEM_MON_RX);
        if (!(v & 0x100u)) break;
        fputc((int)(v & 0xFFu), sem_text);
    }
    fflush(sem_text);
}

// one monitor command line (the controller takes one byte at a time)
static void sem_command(const char *text)
{
    for (const char *c = text; *c; c++) {
        wr(sem, SEM_MON_TX, (uint32_t)(unsigned char)*c);
        usleep(2000);
        sem_drain();
    }
    fprintf(sem_text, "\n[F-ASTRA sent %s]\n", text[0] == 'I' ? "I (idle)" : text[0] == 'O' ? "O (observation)" : text);
}

// poll the controller until (status & mask) == want, or a counter passes `above`; feeds the PS watchdog and the
// guard's heartbeat meanwhile (the first model stays armed next to the SEM run)
static int sem_wait(uint32_t mask, uint32_t want, unsigned counter, uint32_t above, uint64_t limit_ms)
{
    uint64_t start = now_ns();
    while (now_ns() - start < limit_ms * 1000000ull) {
        swdt_feed();
        heartbeat();
        sem_drain();
        if (counter ? rd(sem, counter) > above : (rd(sem, SEM_STATUS) & mask) == want) return 1;
        usleep(1000);
    }
    return 0;
}

// the injection command through the pins: linear frame address, word, bit (7 series, 40 bits)
static void sem_inject(uint32_t lfa, uint32_t word, uint32_t bit)
{
    uint64_t address = (0xCull << 36) | ((uint64_t)(lfa & 0x1FFFFu) << 12) | ((word & 0x7Fu) << 5) | (bit & 0x1Fu);
    char detail[96];
    wr(sem, SEM_INJ_LO, (uint32_t)address);
    wr(sem, SEM_INJ_HI, (uint32_t)(address >> 32) | 0x80000000u);
    usleep(20000);
    sem_drain();
    snprintf(detail, sizeof(detail), "address=0x%010llx lfa=0x%x word=%u bit=%u", (unsigned long long)address, lfa, word, bit);
    event(-1, "sem_inject", "", detail);
    fprintf(sem_text, "\n[F-ASTRA injected %s]\n", detail);
}

// ---------------------------------------------------------------- PL reconfiguration and System Reset (L0-H2-03)
// The whole PL written again from its stored image through the FPGA manager. A configuration CRC error (an upset the
// SEM controller cannot correct) holds INIT_B low, and the Zynq FPGA manager waits for INIT_B high before it pulses
// PROG_B ("Timeout waiting for PCFG_INIT"): PROG_B (devcfg CTRL bit 30) is pulsed here first. Nothing may touch the
// PL while it is written.
// PROG_B (CTRL bit 30) held low for a second, then INIT_B (devcfg STATUS bit 4) high and steady: the configuration
// logic cleared as in the session's recovery. After an uncorrectable upset the FPGA manager's own short PROG_B pulse
// left INIT_B low (twice on 2026-10-14: once without this pulse while INIT_B was still high, once after a 1 ms pulse).
// ``always``: also when INIT_B is high (before the FPGA manager rewrites the PL after an upset).
static int pl_init_release(int always)
{
    char detail[64];
    if (!always && (rd(devcfg, 0x14) & 0x10u)) return 1;
    uint32_t ctrl = rd(devcfg, 0x0);
    wr(devcfg, 0x0, ctrl & ~(1u << 30));
    for (int i = 0; i < 10; i++) { swdt_feed(); usleep(100000); }
    wr(devcfg, 0x0, ctrl | (1u << 30));
    for (int i = 0; i < 20 && !(rd(devcfg, 0x14) & 0x10u); i++) { swdt_feed(); usleep(100000); }
    usleep(100000);                                                       // and it stays high
    snprintf(detail, sizeof(detail), "status=0x%08x", rd(devcfg, 0x14));
    event(-1, "pl_init_released", "L0-H2-03", detail);
    return (rd(devcfg, 0x14) & 0x10u) != 0;
}

static int pl_reconfigure(const char *image)
{
    char state[32] = "";
    FILE *f;
    char detail[128];
    pl_unavailable = 1;                       // from the PROG_B pulse until the FPGA manager reports the PL operating
    warm_drop();                              // the other models' next starts are cold (Initial on the new PL)
    if (!pl_init_release(1)) return 0;
    swdt_feed();
    if (!(f = fopen("/sys/class/fpga_manager/fpga0/flags", "w")) || fputs("0", f) < 0 || fclose(f)) return 0;
    if (!(f = fopen("/sys/class/fpga_manager/fpga0/firmware", "w"))) return 0;
    int bad = fputs(image, f) < 0;
    if (fclose(f) != 0 || bad) return 0;                                  // the write returns when the PL is done
    swdt_feed();
    if (!(f = fopen("/sys/class/fpga_manager/fpga0/state", "r"))) return 0;
    if (fscanf(f, "%31s", state) != 1) state[0] = 0;
    fclose(f);
    snprintf(detail, sizeof(detail), "image=%.80s state=%s", image, state);
    event(-1, "pl_reconfigured", "L0-H2-03", detail);
    if (strcmp(state, "operating")) return 0;
    pl_unavailable = 0;
    clock_gates_write(cfg.clock_gates_on);
    return 1;
}

// The last resort of the recovery ladder: the whole board restarts (the boot path checks its health again). The SWDT,
// left without feeding, resets the board should the reboot not happen.
static void system_reset(const char *item, const char *why)
{
    event(-1, "action", item, "system_reset");
    snprintf(last_cause, sizeof(last_cause), "%s", why);
    pl_unavailable = 1;                       // the PL is in an unknown state: never read it again
    // INIT_B released before the reset, so that the FSBL's PL configuration never meets it low. Whether a PS reset
    // clears that state by itself is not verified (UG585 lists the PL among what a system reset resets); the reboot
    // after this release did boot with the PL configured (2026-10-14)
    pl_init_release(0);
    write_health("SYSTEM_RESET");
    if (mkdir(RESET_RECORD_DIR, 0755) == 0 || errno == EEXIST) {   // /run does not survive the reset: a copy on SD
        char line[1024];
        FILE *in = fopen(cfg.telemetry, "r"), *out = fopen(RESET_RECORD_DIR "/last-system-reset.jsonl", "w");
        if (telemetry) fflush(telemetry);
        if (in && out) while (fgets(line, sizeof(line), in)) fputs(line, out);
        if (in) fclose(in);
        if (out) { fflush(out); fsync(fileno(out)); fclose(out); }
    }
    fflush(NULL);
    sync();
    reboot(RB_AUTOBOOT);
    for (;;) pause();
}

static void sem_mode(int mem_fd)
{
    char detail[160];
    uint32_t before, ctrl;
    sem = map_registers(mem_fd, cfg.sem_base);
    devcfg = map_registers(mem_fd, DEVCFG_BASE);
    if (rd(sem, SEM_ID) != SEM_ID_VALUE) { errno = ENODEV; fail("SEM controller id"); }
    sem_text = fopen(cfg.output, "w");
    if (!sem_text) fail(cfg.output);
    // the ICAP to the controller: fpga-manager left the configuration port on the PCAP
    ctrl = rd(devcfg, 0x0);
    wr(devcfg, 0x0, ctrl & ~DEVCFG_PCAP_PR);
    wr(sem, SEM_CTRL, 2u);                    // counters cleared, grant off
    wr(sem, SEM_CTRL, 1u);                    // grant: the controller initializes
    snprintf(detail, sizeof(detail), "devcfg_ctrl=0x%08x->0x%08x", ctrl, rd(devcfg, 0x0));
    event(-1, "sem_grant", "", detail);
    if (!sem_wait(SEM_ST_OBSERVATION, SEM_ST_OBSERVATION, 0, 0, 60000)) {
        snprintf(last_cause, sizeof(last_cause), "SEM:init");
        snprintf(detail, sizeof(detail), "status=0x%02x beats=%u", rd(sem, SEM_STATUS), rd(sem, SEM_BEATS));
        event(-1, "sem_init_timeout", "", detail);
        write_health("ERROR");
        swdt_disarm();
        exit(2);
    }
    snprintf(detail, sizeof(detail), "status=0x%02x beats=%u", rd(sem, SEM_STATUS), rd(sem, SEM_BEATS));
    event(-1, "sem_observation", "", detail);
    // L0-H2-01: one bit, repaired by the controller (a whole scan takes some 35 ms)
    uint32_t lfa = cfg.sem_lfa, repaired = 0, beats;
    for (uint32_t t = 0; t < (cfg.sem_tries ? cfg.sem_tries : 1u) && !repaired; t++, lfa += cfg.sem_lfa_step) {
        sem_command("I\r");
        if (!sem_wait(SEM_ST_OBSERVATION | SEM_ST_INIT, 0, 0, 0, 5000)) { event(-1, "sem_idle_timeout", "", ""); }
        before = rd(sem, SEM_CORRECTED);
        sem_inject(lfa, cfg.sem_word, cfg.sem_bit);
        beats = rd(sem, SEM_BEATS);
        sem_command("O\r");
        if (sem_wait(0, 0, SEM_CORRECTED, before, 3000)) {
            snprintf(detail, sizeof(detail), "corrected=%u lfa=0x%x", rd(sem, SEM_CORRECTED), lfa);
            event(-1, "detected", "L0-H2-01", detail);
            event(-1, "action", "L0-H2-01", "sem_repair");
            if (sem_wait(SEM_ST_OBSERVATION, SEM_ST_OBSERVATION, 0, 0, 10000)) event(-1, "recovered", "L0-H2-01", "observation");
            repaired = 1;
            cfg.sem_lfa = lfa;
            // L0-H2-02: no classification, so the corrected bit may have been essential: Safe Output, PL IP reset,
            // functional verification on the golden frames
            event(-1, "detected", "L0-H2-02", "essential unknown (no classification): treated as essential");
            wr(guard, FMG_CONTROL, control_bits() | FMG_CTRL_HOLD);
            event(-1, "action", "L0-H2-02", "safe_output");
            if (!pl_ip_reset()) safehold(-1, "pl_ip_reset");
            event(-1, "action", "L0-H2-02", "pl_ip_reset");
            if (!golden_check(-1, 0)) safehold(-1, "L0-H2-02");
            event(-1, "action", "L0-H2-02", "reverify");
            event(-1, "recovered", "L0-H2-02", "golden");
            break;
        }
        snprintf(detail, sizeof(detail), "lfa=0x%x status=0x%02x beats=%u->%u", lfa, rd(sem, SEM_STATUS), beats,
                 rd(sem, SEM_BEATS));
        event(-1, "sem_no_correction", "L0-H2-01", detail);
        sem_command("I\r");                  // not scanned: the bit back as it was
        sem_wait(SEM_ST_OBSERVATION | SEM_ST_INIT, 0, 0, 0, 5000);
        sem_inject(lfa, cfg.sem_word, cfg.sem_bit);
        sem_command("O\r");
        sem_wait(SEM_ST_OBSERVATION, SEM_ST_OBSERVATION, 0, 0, 5000);
    }
    if (cfg.sem_uncorrectable && repaired) {  // L0-H2-03: two bits of one word
        sem_command("I\r");
        sem_wait(SEM_ST_OBSERVATION | SEM_ST_INIT, 0, 0, 0, 5000);
        before = rd(sem, SEM_UNCORRECT);
        sem_inject(cfg.sem_lfa, cfg.sem_word, cfg.sem_bit);
        sem_inject(cfg.sem_lfa, cfg.sem_word, (cfg.sem_bit + 1u) & 31u);
        sem_command("O\r");
        if (sem_wait(0, 0, SEM_UNCORRECT, before, 10000)) {
            const char *image = getenv("F_ASTRA_PL_FIRMWARE");
            if (cfg.sem_reconfigure_fail && image && *image) {   // injection: an image the FPGA manager cannot find
                image = "f-astra-fm-missing-image.bit.bin";
                event(-1, "inject", "L0-H2-03", "reconfigure_fail");
            }
            snprintf(detail, sizeof(detail), "uncorrectable=%u status=0x%02x", rd(sem, SEM_UNCORRECT), rd(sem, SEM_STATUS));
            event(-1, "detected", "L0-H2-03", detail);
            sem_drain();
            fclose(sem_text);
            wr(guard, FMG_CONTROL, control_bits() | FMG_CTRL_HOLD);
            event(-1, "action", "L0-H2-03", "safe_output");
            if (!image || !*image) {             // no stored image: the ground reconfigures the PL
                event(-1, "action", "L0-H2-03", "safehold");
                safehold(-1, "L0-H2-03");
            }
            if (!pl_reconfigure(image)) system_reset("L0-H2-03", "PL reconfiguration failed");
            secure_pl_windows();     // T3d: the reload's PROG_B cleared the monitor's windows
            event(-1, "action", "L0-H2-03", "pl_reconfigure");
            if (!interface_init()) system_reset("L0-H2-03", "DMA after the PL reconfiguration");
            guard_configure();
            if (!golden_check(-1, 0)) system_reset("L0-H2-03", "golden after the PL reconfiguration");
            event(-1, "action", "L0-H2-03", "reverify");
            event(-1, "recovered", "L0-H2-03", "pl_reconfigured");
            write_health("STANDBY");
            return;
        }
        event(-1, "sem_no_uncorrectable", "L0-H2-03", "");
    }
    sem_drain();
    fclose(sem_text);
    write_health("STANDBY");
}

// L0-H3-01 alone: armed, fed for swdt_healthy_s seconds, then no more feeding. The board should reset within the
// timeout; a board still running after three timeouts is a failure (event swdt_no_reset, disarmed, exit 3).
static void swdt_test(void)
{
    char detail[96];
    if (!cfg.swdt_timeout_s) { errno = EINVAL; fail("swdt mode needs swdt_timeout_s"); }
    swdt_arm();
    for (uint32_t i = 0; i < cfg.swdt_healthy_s; i++) { swdt_feed(); sleep(1); }
    swdt_feed();
    snprintf(detail, sizeof(detail), "fed_s=%u timeout_s=%u unix=%lld", cfg.swdt_healthy_s, cfg.swdt_timeout_s,
             (long long)time(NULL));
    event(-1, "swdt_feed_stopped", "L0-H3-01", detail);
    for (uint32_t i = 0; i < cfg.swdt_timeout_s * 3; i++) { sleep(1); swdt_diag_maybe(0); }
    event(-1, "swdt_no_reset", "L0-H3-01", "the SWDT did not reset the board");
    swdt_disarm();
    exit(3);
}

// the PL windows (DMA, GPIO, guard, SLCR and, engine-fed, the engine shell) and the L2 and OCM registers (parity)
static void map_pl(int mem_fd)
{
    dma = map_registers(mem_fd, cfg.dma_base);
    gpio = map_registers(mem_fd, cfg.gpio_base);
    guard = map_registers(mem_fd, cfg.guard_base);
    slcr = map_registers(mem_fd, SLCR_BASE);
    l2c = map_registers(mem_fd, FM_L2C_BASE);
    ocm = map_registers(mem_fd, FM_OCM_BASE);
    if (engine_on()) {
        shell = map_registers(mem_fd, cfg.engine_base);
        if (rd(shell, S_ID) != SHELL_ID) { errno = ENODEV; fail("engine shell id"); }
    }
    if (cfg.feeder_base) feeder = map_registers(mem_fd, cfg.feeder_base);
    if (cfg.clock_gates_addr) cgate = map_registers(mem_fd, cfg.clock_gates_addr);
}

int main(int argc, char **argv)
{
    int pool_fd, mem_fd;
    const char *mode = argc == 3 ? argv[2] : "";
    int supervised = !strncmp(mode, "supervise:", 10);
    const char *run = supervised ? mode + 10 : mode;
    if (argc != 3 || (strcmp(run, "initial") && strcmp(run, "throughput") && strcmp(run, "mission") && strcmp(run, "serve")
                      && (supervised || (strcmp(run, "swdt") && strcmp(run, "sem"))))) {
        fprintf(stderr, "usage: %s CONFIG initial|throughput|mission|serve|swdt|supervise:initial|supervise:throughput|"
                        "supervise:mission|supervise:serve\n", argv[0]);
        return 64;
    }
    t0 = now_ns();
    if (!supervised && getenv("F_ASTRA_FM_HEARTBEAT_FD") && getenv("F_ASTRA_FM_ATTEMPT")) {
        heartbeat_fd = atoi(getenv("F_ASTRA_FM_HEARTBEAT_FD"));
        worker_attempt = atoi(getenv("F_ASTRA_FM_ATTEMPT"));
    }
    read_config(argv[1], !strcmp(run, "swdt") || supervised);
    if (worker_attempt) cfg.swdt_timeout_s = 0;   // a supervised worker: the supervisor owns the PS watchdog
    // always-on operation rotates its telemetry (it is on tmpfs and swdt_diag lines come every 5 s); a campaign keeps
    // its whole record (0) unless its configuration says otherwise
    if ((supervised || !strcmp(run, "serve")) && !cfg.telemetry_max_bytes) cfg.telemetry_max_bytes = 4u << 20;
    telemetry = fopen(cfg.telemetry, "a");
    if (!telemetry) fail(cfg.telemetry);
    if (mkdir(OBC_DIR, 0755) == 0 || errno == EEXIST) obc_events = fopen(OBC_DIR "/events.jsonl", "a");
    worker_beat();
    if (supervised) return supervise(argv[0], argv[1], run);
    if (!strcmp(run, "swdt")) swdt_test();
    if (!strcmp(run, "sem") && !cfg.sem_base) { errno = EINVAL; fail("sem_base"); }
    load_state();
    pool_fd = open(cfg.pool, O_RDWR);
    if (pool_fd < 0 || ioctl(pool_fd, POOL_INFO, &info)) fail(cfg.pool);
    // a batch (and the golden frames) must fit the pool's input and output regions; output_pool_offset moves the
    // outputs into the arena (the inputs end below it)
    if (info.output_offset > info.bytes || cfg.output_pool_offset >= info.output_offset) { errno = ENOSPC; fail("pool regions"); }
    out_region = cfg.output_pool_offset ? cfg.output_pool_offset : info.output_offset;
    out_capacity = cfg.output_pool_offset ? info.output_offset - cfg.output_pool_offset : info.bytes - info.output_offset;
    in_limit = cfg.output_pool_offset ? cfg.output_pool_offset : info.output_offset;
    if ((uint64_t)(cfg.batch_frames > cfg.golden_frames ? cfg.batch_frames : cfg.golden_frames) * cfg.in_frame_bytes > in_limit
        || (uint64_t)(cfg.batch_frames > cfg.golden_frames ? cfg.batch_frames : cfg.golden_frames) * cfg.out_frame_bytes
           > out_capacity) { errno = ENOSPC; fail("pool regions"); }
    pool = mmap(NULL, info.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd, 0);
    if (pool == MAP_FAILED) fail("mmap pool");
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) fail("/dev/mem");
    if (!cfg.secure_watch) swdt_arm();
    map_pl(mem_fd);
    secure_pl_windows();            // T3d: the bitstream's PL windows first (a keyed monitor refuses PL access without)
    secure_safehold_check();        // T3b: before any PL write (a latched monitor refuses them; reads stay)
    if (cgate) {
        clock_gates_write(cfg.clock_gates_on);
        atexit(clock_gates_restore);
    }
    if (cfg.secure_watch) {
        secure_watch_start();
        swdt_arm();
    }
    if (!strcmp(run, "serve")) warm_take();
    if (hybrid_on()) {
        // FM stage C: the hybrid model's one window (its image, the rest zero) clear of the regions the batches use
        hybrid_read_plan();
        if (warm_turn && weights_on() && warm_slices_load()) {
            // a warm turn: the windows as the window's cold start left them, program, weights and dynamic region alike
            // (one serve process runs batch after batch on them the same way); the scrub's table from that start
            event(-1, "hybrid_windows", "", "kept, warm");
        } else {
            size_t bytes;
            uint8_t *image = load(cfg.hybrid_image, &bytes);
            uint32_t frames = cfg.batch_frames > cfg.golden_frames ? cfg.batch_frames : cfg.golden_frames;
            uint64_t in_end = (uint64_t)frames * cfg.in_frame_bytes, out_end = out_region + (uint64_t)frames * cfg.out_frame_bytes;
            uint64_t lo = cfg.engine_pool_offset, hi = lo + (uint64_t)cfg.hybrid_windows * cfg.engine_window_bytes;
            if (bytes > cfg.engine_window_bytes || hi > info.bytes || lo < in_end || (lo < out_end && hi > out_region))
                { errno = ENOSPC; fail("hybrid window"); }
            if (weights_on()) {        // the windows' static slices kept when they still hold the image (FM stage D)
                char detail[96];
                uint64_t t = now_ns();
                size_t written = hybrid_windows_refresh(image, bytes, warm_turn);
                snprintf(detail, sizeof(detail), "written=%zu of %llu bytes in %.0f ms%s", written,
                         (unsigned long long)cfg.hybrid_windows * cfg.engine_window_bytes, (now_ns() - t) / 1e6,
                         warm_turn ? ", warm" : "");
                event(-1, "hybrid_windows", "", detail);
                warm_slices_save();
            } else for (uint32_t k = 0; k < cfg.hybrid_windows; k++) {     // a window for every frame in flight
                uint8_t *w = pool + lo + (size_t)k * cfg.engine_window_bytes;
                memset(w, 0, cfg.engine_window_bytes);
                memcpy(w, image, bytes);
                __sync_synchronize();
                if (memcmp(w, image, bytes)) { errno = EIO; fail("hybrid window readback"); }
            }
            free(image);
        }
        if (!engine_reset()) { errno = EIO; fail("engine release"); }
    } else if (engine_on()) {
        if (!(warm_turn && weights_on() && warm_slices_load())) {   // warm: the windows and the table as the cold start left them
            size_t bytes;
            uint8_t *windows = load(cfg.engine_windows, &bytes);
            uint32_t slots = cfg.engine_frames + (cfg.engine_rows_bytes != 0);   // and the batch window
            uint64_t end = (uint64_t)cfg.engine_pool_offset + (uint64_t)slots * cfg.engine_window_bytes;
            if (bytes != (size_t)cfg.engine_frames * cfg.engine_window_bytes || end > info.output_offset
                || (uint64_t)cfg.frames_total * cfg.in_frame_bytes > cfg.engine_pool_offset) { errno = ENOSPC; fail("engine windows"); }
            if (warm_turn && weights_on()) {     // as the cold start wrote them; the scrub's CRCs from the file, not the pool
                uint8_t *src = malloc(WEIGHT_SLICE);
                if (!src) fail("weight slice");
                weights_index(0);
                for (uint32_t k = 0; k < nwslices; k++) {
                    window_source(windows, bytes, &wslices[k], src);
                    wslices[k].crc = fm_crc32(src, wslices[k].bytes);
                }
                free(src);
            } else {
                memcpy(pool + cfg.engine_pool_offset, windows, bytes);
                if (cfg.engine_rows_bytes) memcpy(pool + cfg.engine_pool_offset + bytes, windows, cfg.engine_window_bytes);
                __sync_synchronize();
                if (memcmp(pool + cfg.engine_pool_offset, windows, bytes)) { errno = EIO; fail("engine windows readback"); }
                if (cfg.engine_rows_bytes && memcmp(pool + cfg.engine_pool_offset + bytes, windows, cfg.engine_window_bytes))
                    { errno = EIO; fail("engine batch window readback"); }
                if (weights_on()) weights_index(1);     // L0-H4-01: the windows' program and weights as written
            }
            if (weights_on()) warm_slices_save();
            free(windows);
        }
        if (!engine_reset()) { errno = EIO; fail("engine release"); }
    }
    initial();
    if (!strcmp(run, "throughput")) throughput();
    else if (!strcmp(run, "mission")) mission();
    else if (!strcmp(run, "serve")) {
        serve();
        warm_give();                // a clean stop: the next turn of this model in the window may start warm
    }
    else if (!strcmp(run, "sem")) sem_mode(mem_fd);   // the PL holds the SEM controller next to the first model
    else write_health("STANDBY");
    if (engine_on()) wr(shell, S_CTRL, C_HOLD);   // the engine held and idle
    swdt_disarm();
    reset_dma();
    wr(guard, FMG_CONTROL, 0);   // checks off and WDT stopped: the session may reprogram the PL next
    fclose(telemetry);
    return 0;
}
