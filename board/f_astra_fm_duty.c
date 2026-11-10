// F-ASTRA FM duty monitor (user 11/09: Fault Management runs always-on during duty and the PL is never switched off
// then; per-block clock-0 gating is allowed). One process for the whole duty, from the PL load after boot to the
// poweroff, beside the reception, the NN session and the sends. FM items it serves (docs/f-astra-fault-management.md):
//   L0-H2-01  SEM correction (CORRECTED rises): detected and recovered events
//   L0-H2-03  SEM uncorrectable (UNCORRECT rises or STATUS bit 7): the output-held flag file, then, once no session
//             holds the FPGA device lock, PROG_B pulsed, the stored image (firmware) written again with FCLK1 set
//             first, every gate 0, the ICAP granted and the observation state awaited; Safehold when that fails
//   SEM       observation lost or no heartbeat for stall_ticks ticks: the ICAP granted again (detected, recovered)
//   L0-H4-04  XADC die temperature at or above temp_limit_c, VCCINT outside [vccint_min, vccint_max]
//   L0-H4-02  a PS PLL the PS runs on is not locked (an IO PLL that the floor tool powered down is not expected)
// Every tick (period_ms) one JSON line in the duty log and a health record (schema f-astra/fm-health-telemetry/v1,
// mode DUTY or SAFEHOLD) in /run/f-astra-fm/health.json; the OBC's events go to /run/f-astra-fm/events.jsonl in the FM
// service's form (the CAN CMD node forwards them as TM 0xC020).
// The PL is read only while it can answer: fpga-manager operating, the PL reset released, FCLK1 running (FPGA1_THR_CNT
// bit 0 clear) and no clock change in progress (a shared lock on /run/lock/f-astra-pl-clock.lock, which the floor
// tool holds exclusively while it changes FCLK0 or FCLK1). A tick that cannot read the PL is counted as skipped.
// With --load (the image's boot start, once per power-on) the program first makes the duty's PL: `image` (the flight
// bundle's FPGA-manager payload on the SD card) copied to /lib/firmware/`firmware` with its SHA-256, FCLK1 set and
// ungated with the PL reset held, the image written by the FPGA manager, every gate 0, the ICAP granted and the
// observation awaited, then the duty's slow clocks (idle_fclk0, idle_fclk1: halted writes under the clock lock) and
// "keep SHA" in /run/f-astra-pl-state (the NN session's PL_KEEP check); no load happens without `image`.
// Boot stage (11/10): with `boot_bin` (the SD's BOOT.BIN, whose FSBL configured the duty PL about 1 s after power-on;
// the U-Boot early start f_astra_duty_early.c may already have granted the SEM controller) --load adopts that PL
// instead: the FPGA manager operating, the PL partition of boot_bin hashing to the image's SHA-256 (`image_sha256`,
// else the copy's), FCLK1 to the contract word with the clock halted and no reset pulse, every gate 0, the controller
// left observing (granted only when it is not); corrections and uncorrectables counted before the adoption become
// events. With `image_sha256` the copy to /lib/firmware waits for the first reload. Any failed check: the load above.
// usage: f-astra-fm-duty CONF [--load]   key=value lines; SIGTERM, SIGINT or the stop file end it (exit 0; 3 in
//        Safehold; 2 setup or load failure)
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "f_astra_ed25519.h"
#include "f_astra_fm_core.h"

#define FM_DIR "/run/f-astra-fm"
#define PL_STATE "/run/f-astra-pl-state"
#define CLOCK_LOCK "/run/lock/f-astra-pl-clock.lock"
#define DEVICE_LOCK "/run/lock/f-astra-fpga-device.lock"
#define FPGA_STATE "/sys/class/fpga_manager/fpga0/state"
#define SLCR_BASE 0xF8000000u
#define DEVCFG_BASE 0xF8007000u
enum { PLL_STATUS = 0x10C, ARM_PLL = 0x100, DDR_PLL = 0x104, IO_PLL = 0x108, ARM_CLK = 0x120, FPGA0_CLK = 0x170,
       FPGA0_THR = 0x178, FPGA1_CLK = 0x180, FPGA1_THR = 0x188, FPGA_RST = 0x240, REBOOT_STATUS = 0x258 };
enum { DEVCFG_CTRL = 0x00, DEVCFG_STATUS = 0x14 };
enum { SEM_ID = 0x00, SEM_CTRL = 0x04, SEM_STATUS = 0x08, SEM_CORRECTED = 0x0C, SEM_UNCORRECT = 0x10, SEM_BEATS = 0x14,
       SEM_MON_RX = 0x20, SEM_MON_TX = 0x24 };
enum { ST_INIT = 1u << 1, ST_OBSERVATION = 1u << 2, ST_CORRECTION = 1u << 3, ST_INJECTION = 1u << 5,
       ST_UNCORRECTABLE = 1u << 7 };
#define SEM_ID_VALUE 0x53454D43u
#define PCAP_PR (1u << 27)
#define PROG_B (1u << 30)

struct config {
    uint32_t sem_base, gates_addr, fclk1_ctrl, period_ms, stall_ticks, log_max_bytes, observe_ms, idle_fclk0, idle_fclk1;
    double temp_limit_c, vccint_min, vccint_max;
    char firmware[192], log[192], stop_file[192], held_file[192], image[192], boot_bin[192], image_sha256[192];
};
static struct config cfg;
static volatile uint32_t *slcr, *devcfg, *sem, *gates;
static struct fm_state state;
static FILE *dlog, *events;
static uint64_t t0;
static volatile sig_atomic_t stop;
static char cause[16] = "";
static char kept[65]  = "";                   // the SHA-256 of the duty PL in the FPGA (load or adoption)
static uint32_t boot_uncorrect;               // uncorrectables the controller counted before the adoption

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void on_signal(int sig) { (void)sig; stop = 1; }

static void die(const char *what)
{
    fprintf(stderr, "F_ASTRA_FM_DUTY_SETUP_FAILED %s errno=%d\n", what, errno);
    exit(2);
}

static void *map_page(int fd, uint32_t base)
{
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base & ~0xFFFu);
    if (p == MAP_FAILED) die("mmap");
    return (char *)p + (base & 0xFFFu);
}

static uint32_t rd(volatile uint32_t *b, unsigned off) { return b[off / 4]; }
static void wr(volatile uint32_t *b, unsigned off, uint32_t v) { b[off / 4] = v; }

static void read_config(const char *path)
{
    char line[256], key[64], value[192];
    FILE *f = fopen(path, "r");
    if (!f) die("configuration");
    cfg.period_ms     = 1000;
    cfg.stall_ticks   = 5;
    cfg.log_max_bytes = 1u << 20;
    cfg.observe_ms    = 120000;
    cfg.temp_limit_c  = 100.0;
    cfg.vccint_min    = 0.95;
    cfg.vccint_max    = 1.05;
    snprintf(cfg.log, sizeof(cfg.log), FM_DIR "/duty.jsonl");
    snprintf(cfg.stop_file, sizeof(cfg.stop_file), FM_DIR "/duty.stop");
    snprintf(cfg.held_file, sizeof(cfg.held_file), FM_DIR "/output-held");
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || sscanf(line, " %63[^= ] = %191[^\r\n]", key, value) != 2) continue;
#define U(k) if (!strcmp(key, #k)) cfg.k = (uint32_t)strtoul(value, NULL, 0);
#define D(k) if (!strcmp(key, #k)) cfg.k = strtod(value, NULL);
#define S(k) if (!strcmp(key, #k)) snprintf(cfg.k, sizeof(cfg.k), "%s", value);
        U(sem_base) U(gates_addr) U(fclk1_ctrl) U(period_ms) U(stall_ticks) U(log_max_bytes) U(observe_ms)
        U(idle_fclk0) U(idle_fclk1)
        D(temp_limit_c) D(vccint_min) D(vccint_max)
        S(firmware) S(log) S(stop_file) S(held_file) S(image) S(boot_bin) S(image_sha256)
#undef U
#undef D
#undef S
    }
    fclose(f);
    if (!cfg.sem_base || cfg.period_ms < 50) { errno = EINVAL; die("sem_base / period_ms"); }
}

static double xadc(const char *channel)
{
    char path[160], text[64];
    double v[3] = {0, 0, 0};
    const char *part[] = {"raw", "offset", "scale"};
    for (int i = 0; i < 3; i++) {
        FILE *f;
        snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device0/in_%s_%s", channel, part[i]);
        if (!(f = fopen(path, "r"))) { if (i == 1) continue; return -1e9; }
        if (fgets(text, sizeof(text), f)) v[i] = strtod(text, NULL);
        fclose(f);
    }
    return (v[0] + v[1]) * v[2] / 1000.0;
}

static int fpga_operating(void)
{
    char s[32] = "";
    FILE *f = fopen(FPGA_STATE, "r");
    if (!f) return 0;
    if (fscanf(f, "%31s", s) != 1) s[0] = 0;
    fclose(f);
    return !strcmp(s, "operating");
}

// The PLLs the PS runs on are locked: DDR always; ARM and IO unless powered down and bypassed by design (the power
// profile's ARM PLL, the floor tool's IO PLL under reception).
static int plls_locked(uint32_t *status)
{
    uint32_t need = 0x2u;
    *status = rd(slcr, PLL_STATUS);
    if ((rd(slcr, ARM_PLL) & 0x12u) != 0x12u) need |= 0x1u;
    if ((rd(slcr, IO_PLL) & 0x12u) != 0x12u) need |= 0x4u;
    return (*status & need) == need;
}

static FILE *rotated(FILE *f, const char *path, uint32_t limit)
{
    struct stat st;
    char older[200];
    FILE *g;
    if (!f || !limit || fstat(fileno(f), &st) || st.st_size < (off_t)limit) return f;
    snprintf(older, sizeof(older), "%s.1", path);
    if (rename(path, older) || !(g = fopen(path, "a"))) return f;
    fclose(f);
    return g;
}

static int obc_event(const char *ev)
{
    return !strcmp(ev, "detected") || !strcmp(ev, "recovered") || !strcmp(ev, "safehold");
}

static void event(const char *ev, const char *item, const char *detail)
{
    char line[1024];
    if (fm_event_json(line, sizeof(line), now_ns() - t0, -1, ev, item, detail, &state) <= 0) return;
    dlog   = rotated(dlog, cfg.log, cfg.log_max_bytes);
    events = rotated(events, FM_DIR "/events.jsonl", 4u << 20);
    if (dlog) { fprintf(dlog, "%s\n", line); fflush(dlog); }
    if (events && obc_event(ev)) { fprintf(events, "%s\n", line); fflush(events); }
}

struct tick {
    int pl_read, locked;
    uint32_t status, corrected, uncorrect, beats, gate_word, pll;
    double temp, vccint;
};

static void health(const struct tick *t, uint64_t ticks, uint64_t skipped)
{
    FILE *f = fopen(FM_DIR "/health.json.tmp", "w");
    if (!f) return;
    fprintf(f, "{\"schema\":\"f-astra/fm-health-telemetry/v1\",\"mode\":\"%s\",\"cause\":\"%s\",\"fpga\":\"%s\","
               "\"pll_status\":%u,\"reboot_status\":%u,\"temp_c\":%.2f,\"vccint_v\":%.4f,"
               "\"sem\":{\"status\":%u,\"corrected\":%u,\"uncorrectable\":%u,\"beats\":%u,\"read\":%d},\"gates\":%u,"
               "\"ticks\":%llu,\"skipped\":%llu,\"recovery_counts\":{",
            state.safehold ? "SAFEHOLD" : "DUTY", cause, fpga_operating() ? "operating" : "unavailable", t->pll,
            rd(slcr, REBOOT_STATUS), t->temp, t->vccint, t->status, t->corrected, t->uncorrect, t->beats, t->pl_read,
            t->gate_word, (unsigned long long)ticks, (unsigned long long)skipped);
    for (int i = 0; i < FM_COUNT; i++) fprintf(f, "%s\"%s\":%u", i ? "," : "", fm_names[i], state.count[i]);
    fprintf(f, "},\"safehold\":%u,\"safehold_cause\":\"%s\"}\n", state.safehold, state.cause);
    if (fclose(f) == 0) rename(FM_DIR "/health.json.tmp", FM_DIR "/health.json");
}

// the PL can answer a read now: configured, its reset released and FCLK1 running
static int pl_ready(void)
{
    return fpga_operating() && rd(slcr, FPGA_RST) == 0 && !(rd(slcr, FPGA1_THR) & 1u);
}

static int wait_ms(uint64_t ms, uint32_t mask, uint32_t want)
{
    uint64_t end = now_ns() + ms * 1000000ull;
    while (now_ns() < end) {
        if ((rd(sem, SEM_STATUS) & mask) == want) return 1;
        usleep(10000);
    }
    return 0;
}

// the ICAP to the SEM controller (devcfg PCAP_PR = 0), counters cleared, grant, observation awaited
static int sem_observe(void)
{
    wr(devcfg, DEVCFG_CTRL, rd(devcfg, DEVCFG_CTRL) & ~PCAP_PR);
    wr(sem, SEM_CTRL, 2u);
    wr(sem, SEM_CTRL, 1u);
    return wait_ms(cfg.observe_ms, ST_OBSERVATION, ST_OBSERVATION);
}

// a controller that stopped observing: an idle one takes the monitor command "O" (a new grant does not leave idle,
// guard616); without observation after it, the ICAP granted again from the start
static int sem_resume(void)
{
    if (!(rd(sem, SEM_STATUS) & (ST_OBSERVATION | ST_INIT))) {
        wr(sem, SEM_MON_TX, 'O');
        usleep(2000);
        wr(sem, SEM_MON_TX, '\r');
        if (wait_ms(cfg.observe_ms < 10000 ? cfg.observe_ms : 10000, ST_OBSERVATION, ST_OBSERVATION)) return 1;
    }
    return sem_observe();
}

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    int ok;
    if (!f) return 0;
    ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static int firmware_ready(void);

// PROG_B held low for a second, then INIT_B (devcfg STATUS bit 4) high: the configuration logic cleared before the FPGA
// manager writes. Its own short pulse first waits for INIT_B high and fails while a CRC error holds INIT_B low (guard629
// and guard630 after the kernel's XADC probe: "Timeout waiting for PCFG_INIT")
static int prog_b_pulse(void)
{
    wr(devcfg, DEVCFG_CTRL, rd(devcfg, DEVCFG_CTRL) & ~PROG_B);
    sleep(1);
    wr(devcfg, DEVCFG_CTRL, rd(devcfg, DEVCFG_CTRL) | PROG_B);
    sleep(1);
    return (rd(devcfg, DEVCFG_STATUS) & 16u) != 0;
}

// L0-H2-03: PROG_B, the stored image again (FCLK1 first, the PL reset held while it changes), gates 0, observation
static int reload(void)
{
    char detail[240];
    uint32_t thr;
    if (!cfg.firmware[0] || !firmware_ready()) return 0;    // the image checked before the PL is cleared
    if (!prog_b_pulse()) { event("detected", "L0-H2-03", "init_b low after PROG_B"); return 0; }
    if (cfg.fclk1_ctrl) {
        wr(slcr, FPGA_RST, 0xFu);
        wr(slcr, FPGA1_CLK, cfg.fclk1_ctrl);
        thr = rd(slcr, FPGA1_THR);
        wr(slcr, FPGA1_THR, thr & ~1u);
        wr(slcr, FPGA_RST, 0u);
    }
    if (!write_text("/sys/class/fpga_manager/fpga0/flags", "0")
        || !write_text("/sys/class/fpga_manager/fpga0/firmware", cfg.firmware) || !fpga_operating())
        return 0;
    wr(slcr, FPGA_RST, 0u);
    if (gates) wr(gates, 0, 0u);              // every model gate and the engine MMCM (RUN) off
    if (rd(sem, SEM_ID) != SEM_ID_VALUE) return 0;
    if (!sem_observe()) return 0;
    snprintf(detail, sizeof(detail), "firmware=%s status=0x%02x", cfg.firmware, rd(sem, SEM_STATUS));
    event("recovered", "L0-H2-03", detail);
    return 1;
}

// one PL clock word written with that clock halted (FPGAx_THR_CNT bit 0) and running again after it
static void fclk_halted(unsigned clk, unsigned thr, uint32_t word)
{
    uint32_t t = rd(slcr, thr);
    wr(slcr, thr, t | 1u);
    wr(slcr, clk, word);
    wr(slcr, thr, t & ~1u);
}

// the file copied and hashed in one pass; 0 on any read or write error
static int copy_hash(const char *from, const char *to, char hex[65])
{
    uint8_t buf[16384], d[32];
    fa_sha256_ctx c;
    size_t n;
    int ok = 1;
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    if (!in || !out) {
        if (in) fclose(in);
        return 0;
    }
    fa_sha256_init(&c);
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fa_sha256_update(&c, buf, n);
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out)) ok = 0;
    fa_sha256_final(&c, d);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    return ok;
}

// the stored image in /lib/firmware before a reload: after an adoption with image_sha256 it is copied only now, and
// it must hash to the PL the duty keeps
static int firmware_ready(void)
{
    char path[256], hex[65] = "";
    snprintf(path, sizeof(path), "/lib/firmware/%s", cfg.firmware);
    if (!access(path, R_OK)) return 1;
    mkdir("/lib/firmware", 0755);
    if (!cfg.image[0] || !copy_hash(cfg.image, path, hex) || (kept[0] && strcmp(hex, kept))) {
        unlink(path);
        event("reload_image_failed", "L0-H2-03", hex[0] ? hex : "copy");
        return 0;
    }
    return 1;
}

// the SHA-256 of the PL partition (destination device PL in the partition attributes) of a Zynq boot image
static int boot_pl_hash(const char *path, char hex[65])
{
    uint8_t head[0xA0], buf[16384], d[32];
    uint32_t h[16], table, left = 1;
    fa_sha256_ctx c;
    int found = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(head, 1, sizeof(head), f) != sizeof(head) || memcmp(head + 0x20, "\x66\x55\x99\xAA", 4)
        || memcmp(head + 0x24, "XNLX", 4)) {
        fclose(f);
        return 0;
    }
    memcpy(&table, head + 0x9C, 4);
    for (int i = 0; i < 16 && !found; i++) {
        if (fseek(f, (long)table + 64L * i, SEEK_SET) || fread(h, 4, 16, f) != 16) break;
        if (!(h[0] | h[1] | h[2] | h[3] | h[4] | h[5] | h[6] | h[7])) break;      // the null header ends the table
        found = ((h[6] >> 4) & 3u) == 2u;
    }
    if (found && !fseek(f, (long)h[5] * 4L, SEEK_SET)) {
        fa_sha256_init(&c);
        for (left = h[1] * 4u; left;) {
            size_t n = fread(buf, 1, left < sizeof(buf) ? left : sizeof(buf), f);
            if (!n) break;
            fa_sha256_update(&c, buf, n);
            left -= (uint32_t)n;
        }
        fa_sha256_final(&c, d);
        for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    }
    fclose(f);
    return found && !left;
}

// --load with boot_bin: the PL the FSBL configured from boot_bin is the image (see the header); no PROG_B, no reset
// pulse, the controller's observation kept. 0 with the reason in why (then the load)
static int adopt(int lock, const char *hex, char *detail, size_t n)
{
    char boot[65] = "";
    uint32_t status, corrected, beats;
    int granted = 0;
    if (!fpga_operating()) { snprintf(detail, n, "fpga manager not operating"); return 0; }
    if (!boot_pl_hash(cfg.boot_bin, boot)) { snprintf(detail, n, "no PL partition in %.120s", cfg.boot_bin); return 0; }
    if (strcmp(boot, hex)) { snprintf(detail, n, "boot PL %.16s is not the image %.16s", boot, hex); return 0; }
    flock(lock, LOCK_EX);
    if (cfg.fclk1_ctrl && rd(slcr, FPGA1_CLK) != cfg.fclk1_ctrl) fclk_halted(FPGA1_CLK, FPGA1_THR, cfg.fclk1_ctrl);
    wr(slcr, FPGA1_THR, rd(slcr, FPGA1_THR) & ~1u);
    wr(slcr, FPGA0_THR, rd(slcr, FPGA0_THR) & ~1u);
    flock(lock, LOCK_UN);
    if (rd(slcr, FPGA_RST)) wr(slcr, FPGA_RST, 0u);
    if (rd(sem, SEM_ID) != SEM_ID_VALUE) { snprintf(detail, n, "no SEM window"); return 0; }
    if (gates) wr(gates, 0, 0u);              // every model gate and the engine MMCM (RUN) off
    status = rd(sem, SEM_STATUS);
    beats  = rd(sem, SEM_BEATS);
    if (!(status & ST_OBSERVATION)) {
        // a controller that ran (heartbeats) and left observation found the boot PL changed (guard630: the kernel's
        // XADC probe, DED): no adoption, the load writes the PL again. One never granted (no early start) gets it now
        if (beats || (status & ST_UNCORRECTABLE)) {
            snprintf(detail, n, "SEM left observation status=0x%02x beats=%u", status, beats);
            return 0;
        }
        if (!sem_observe()) { snprintf(detail, n, "SEM observation status=0x%02x", rd(sem, SEM_STATUS)); return 0; }
        granted = 1;
    }
    status         = rd(sem, SEM_STATUS);
    corrected      = rd(sem, SEM_CORRECTED);
    boot_uncorrect = rd(sem, SEM_UNCORRECT);
    beats          = rd(sem, SEM_BEATS);
    snprintf(detail, n, "status=0x%02x corrected=%u uncorrectable=%u beats=%u granted_here=%d", status, corrected,
             boot_uncorrect, beats, granted);
    if (corrected) {                          // the controller corrected before the monitor ran: L0-H2-01 now
        char text[96];
        snprintf(text, sizeof(text), "corrected=%u before the adoption", corrected);
        event("detected", "L0-H2-01", text);
        event("recovered", "L0-H2-01", text);
    }
    return 1;
}

// --load: the duty's PL once per power-on (see the header)
static int load_pl(int lock)
{
    char path[256], hex[65] = "", text[96], detail[512], why[200] = "";
    int adopted = 0;
    if (!cfg.image[0] || !cfg.firmware[0]) { event("duty_load_failed", "", "no image or firmware name"); return 0; }
    mkdir("/lib/firmware", 0755);
    snprintf(path, sizeof(path), "/lib/firmware/%s", cfg.firmware);
    if (cfg.boot_bin[0] && strlen(cfg.image_sha256) == 64) memcpy(hex, cfg.image_sha256, 65);
    else if (!copy_hash(cfg.image, path, hex)) { event("duty_load_failed", "", "copy"); return 0; }
    if (cfg.boot_bin[0] && !(adopted = adopt(lock, hex, why, sizeof(why)))) event("duty_adopt_refused", "", why);
    if (!adopted) {
        if (access(path, R_OK) && !copy_hash(cfg.image, path, hex)) { event("duty_load_failed", "", "copy"); return 0; }
        // a PL the boot configured and a SEM controller holds (PCAP_PR 0), or INIT_B low: the logic cleared first
        if ((!(rd(devcfg, DEVCFG_CTRL) & PCAP_PR) || !(rd(devcfg, DEVCFG_STATUS) & 16u)) && !prog_b_pulse()) {
            event("duty_load_failed", "", "init_b low after PROG_B");
            return 0;
        }
        wr(slcr, FPGA_RST, 0xFu);             // both PL clocks running before the load, the PL reset held meanwhile
        if (cfg.fclk1_ctrl) wr(slcr, FPGA1_CLK, cfg.fclk1_ctrl);
        wr(slcr, FPGA1_THR, rd(slcr, FPGA1_THR) & ~1u);
        wr(slcr, FPGA0_THR, rd(slcr, FPGA0_THR) & ~1u);
        wr(slcr, FPGA_RST, 0u);
        if (!write_text("/sys/class/fpga_manager/fpga0/flags", "0")
            || !write_text("/sys/class/fpga_manager/fpga0/firmware", cfg.firmware) || !fpga_operating()) {
            event("duty_load_failed", "", "fpga manager");
            return 0;
        }
        wr(slcr, FPGA_RST, 0u);
        if (gates) wr(gates, 0, 0u);          // every model gate and the engine MMCM (RUN) off
        if (rd(sem, SEM_ID) != SEM_ID_VALUE || !sem_observe()) {
            event("duty_load_failed", "", "SEM observation");
            return 0;
        }
    }
    if (cfg.idle_fclk0 && cfg.idle_fclk1) {
        flock(lock, LOCK_EX);
        fclk_halted(FPGA0_CLK, FPGA0_THR, cfg.idle_fclk0);
        fclk_halted(FPGA1_CLK, FPGA1_THR, cfg.idle_fclk1);
        flock(lock, LOCK_UN);
    }
    snprintf(text, sizeof(text), "keep %s\n", hex);
    if (!write_text(PL_STATE, text)) { event("duty_load_failed", "", PL_STATE); return 0; }
    snprintf(kept, sizeof(kept), "%s", hex);
    snprintf(detail, sizeof(detail), "firmware=%.120s sha256=%s fclk0=0x%08x fclk1=0x%08x%s%.200s", cfg.firmware, hex,
             rd(slcr, FPGA0_CLK), rd(slcr, FPGA1_CLK), adopted ? " " : "", adopted ? why : "");
    event(adopted ? "duty_pl_adopted" : "duty_pl_loaded", "", detail);
    return 1;
}

int main(int argc, char **argv)
{
    char detail[200];
    struct tick t, last;
    uint64_t ticks = 0, skipped = 0, next;
    uint32_t stalled = 0, lost = 0;
    int temp_alarm = 0, volt_alarm = 0, pll_alarm = 0, pending = 0, fd, lock;
    int load = argc == 3 && !strcmp(argv[2], "--load");
    if (argc != 2 && !load) { fprintf(stderr, "usage: f-astra-fm-duty CONF [--load]\n"); return 64; }
    read_config(argv[1]);
    if (mkdir(FM_DIR, 0755) && errno != EEXIST) die(FM_DIR);
    if ((fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0) die("/dev/mem");
    slcr   = map_page(fd, SLCR_BASE);
    devcfg = map_page(fd, DEVCFG_BASE);
    sem    = map_page(fd, cfg.sem_base);
    gates  = cfg.gates_addr ? map_page(fd, cfg.gates_addr) : NULL;
    close(fd);
    if ((lock = open(CLOCK_LOCK, O_RDONLY | O_CREAT | O_CLOEXEC, 0644)) < 0) die(CLOCK_LOCK);
    dlog   = fopen(cfg.log, "a");
    events = fopen(FM_DIR "/events.jsonl", "a");
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    t0 = now_ns();
    if (load && !load_pl(lock)) return 2;
    if (boot_uncorrect) {                     // counted before the adoption: the same recovery as for a new one
        pending = 1;
        snprintf(detail, sizeof(detail), "uncorrectable=%u before the adoption", boot_uncorrect);
        event("detected", "L0-H2-03", detail);
        write_text(cfg.held_file, "L0-H2-03\n");
    }
    memset(&last, 0, sizeof(last));
    if (pl_ready() && flock(lock, LOCK_SH | LOCK_NB) == 0) {
        uint32_t id = rd(sem, SEM_ID);
        last.status    = rd(sem, SEM_STATUS);
        last.corrected = rd(sem, SEM_CORRECTED);
        last.uncorrect = rd(sem, SEM_UNCORRECT);
        last.beats     = rd(sem, SEM_BEATS);
        flock(lock, LOCK_UN);
        snprintf(detail, sizeof(detail), "id=0x%08x status=0x%02x period_ms=%u", id, last.status, cfg.period_ms);
        event(id == SEM_ID_VALUE ? "duty_start" : "duty_start_no_sem", "", detail);
    } else {
        event("duty_start_pl_unavailable", "", "");
    }
    next = now_ns();
    while (!stop && access(cfg.stop_file, F_OK) != 0) {
        memset(&t, 0, sizeof(t));
        t.temp   = xadc("temp0");
        t.vccint = xadc("voltage0_vccint");
        t.locked = plls_locked(&t.pll);
        if (pl_ready() && flock(lock, LOCK_SH | LOCK_NB) == 0) {
            if (pl_ready()) {
                t.pl_read   = 1;
                t.status    = rd(sem, SEM_STATUS);
                t.corrected = rd(sem, SEM_CORRECTED);
                t.uncorrect = rd(sem, SEM_UNCORRECT);
                t.beats     = rd(sem, SEM_BEATS);
                t.gate_word = gates ? rd(gates, 0) : 0;
            }
            flock(lock, LOCK_UN);
        }
        if (!t.pl_read) skipped++;
        else if (last.beats || last.status) {
            if (t.corrected != last.corrected) {
                snprintf(detail, sizeof(detail), "corrected=%u status=0x%02x", t.corrected, t.status);
                event("detected", "L0-H2-01", detail);
                event("recovered", "L0-H2-01", detail);
            }
            if ((t.uncorrect != last.uncorrect || (t.status & ST_UNCORRECTABLE)) && !pending) {
                pending = 1;
                snprintf(detail, sizeof(detail), "uncorrectable=%u status=0x%02x", t.uncorrect, t.status);
                event("detected", "L0-H2-03", detail);
                write_text(cfg.held_file, "L0-H2-03\n");
            }
            lost    = (t.status & (ST_OBSERVATION | ST_INIT | ST_CORRECTION | ST_INJECTION)) ? 0 : lost + 1;
            stalled = t.beats == last.beats ? stalled + 1 : 0;
            if (!pending && (lost >= 2 || stalled >= cfg.stall_ticks)) {
                snprintf(detail, sizeof(detail), "status=0x%02x beats=%u lost=%u stalled=%u", t.status, t.beats, lost,
                         stalled);
                event("detected", "SEM", detail);
                if (flock(lock, LOCK_SH | LOCK_NB) == 0) {
                    int ok = pl_ready() && sem_resume();
                    flock(lock, LOCK_UN);
                    snprintf(detail, sizeof(detail), "status=0x%02x", ok ? rd(sem, SEM_STATUS) : 0u);
                    event(ok ? "recovered" : "observe_failed", "SEM", detail);
                }
                lost = stalled = 0;
            }
        }
        if (pending && !state.safehold) {
            // never while an NN session (or anything else) holds the FPGA device
            int dev = open(DEVICE_LOCK, O_RDONLY | O_CREAT | O_CLOEXEC, 0644);
            if (dev >= 0 && flock(dev, LOCK_EX | LOCK_NB) == 0) {
                flock(lock, LOCK_EX);
                if (reload()) { pending = 0; unlink(cfg.held_file); }
                else {
                    state.safehold = 1;
                    snprintf(state.cause, sizeof(state.cause), "L0-H2-03");
                    snprintf(cause, sizeof(cause), "L0-H2-03");
                    event("safehold", "L0-H2-03", cfg.firmware[0] ? "reload failed" : "no stored image");
                }
                flock(lock, LOCK_UN);
                last.beats = last.status = 0;     // the controller starts again: no comparison on the next tick
            } else if (dev >= 0) {
                event("recovery_deferred", "L0-H2-03", "FPGA device busy");
            }
            if (dev >= 0) close(dev);
        }
        if ((t.temp >= cfg.temp_limit_c) != temp_alarm) {
            temp_alarm = !temp_alarm;
            snprintf(detail, sizeof(detail), "temp_c=%.2f limit=%.1f", t.temp, cfg.temp_limit_c);
            if (temp_alarm) { state.count[FM_L0_H4_04]++; event("detected", "L0-H4-04", detail); }
            else event("recovered", "L0-H4-04", detail);
        }
        if ((t.vccint > -1e8 && (t.vccint < cfg.vccint_min || t.vccint > cfg.vccint_max)) != volt_alarm) {
            volt_alarm = !volt_alarm;
            snprintf(detail, sizeof(detail), "vccint_v=%.4f", t.vccint);
            if (volt_alarm) { state.count[FM_L0_H4_04]++; event("detected", "L0-H4-04", detail); }
            else event("recovered", "L0-H4-04", detail);
        }
        if ((!t.locked) != pll_alarm) {
            pll_alarm = !pll_alarm;
            snprintf(detail, sizeof(detail), "pll_status=0x%x", t.pll);
            if (pll_alarm) { state.count[FM_L0_H4_02]++; event("detected", "L0-H4-02", detail); }
            else event("recovered", "L0-H4-02", detail);
        }
        if (t.pl_read && !pending) last = t;
        ticks++;
        if (dlog) {
            dlog = rotated(dlog, cfg.log, cfg.log_max_bytes);
            fprintf(dlog, "{\"t_ns\":%llu,\"tick\":%llu,\"pl\":%d,\"sem_status\":%u,\"beats\":%u,\"corrected\":%u,"
                          "\"uncorrectable\":%u,\"gates\":%u,\"temp_c\":%.2f,\"vccint_v\":%.4f,\"pll\":%u}\n",
                    (unsigned long long)(now_ns() - t0), (unsigned long long)ticks, t.pl_read, t.status, t.beats,
                    t.corrected, t.uncorrect, t.gate_word, t.temp, t.vccint, t.pll);
            fflush(dlog);
        }
        health(&t, ticks, skipped);
        next += (uint64_t)cfg.period_ms * 1000000ull;
        for (uint64_t n = now_ns(); n < next && !stop; n = now_ns()) {
            struct timespec ts = {(time_t)((next - n) / 1000000000ull), (long)((next - n) % 1000000000ull)};
            nanosleep(&ts, NULL);
        }
        if (now_ns() > next + 10ull * cfg.period_ms * 1000000ull) next = now_ns();   // after a long reload
    }
    snprintf(detail, sizeof(detail), "ticks=%llu skipped=%llu", (unsigned long long)ticks, (unsigned long long)skipped);
    event("duty_end", "", detail);
    if (dlog) fclose(dlog);
    if (events) fclose(events);
    return state.safehold ? 3 : 0;
}
