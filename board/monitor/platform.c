// F-ASTRA TE0715 secure monitor, TrustZone stage T2: the secure world's platform setup before the kernel runs in the
// non-secure world, and the secure services that kernel uses (SMC).
// - Setup: the PS watchdog armed and never fed (a stopped boot resets the board); DDR, OCM, the PS peripherals and the
//   IOU slaves non-secure; the peripheral clocks the kernel's fixed clocks need; GIC group 1, SCU access; the core's
//   prefetch and power bits (monitor.S); the L2 written back and off (the kernel enables it through the monitor).
// - Services: SLCR read and write (TrustZone registers read-only), devcfg and XADC interface read and write, PL310
//   secure register write, PL register read and write (M_AXI_GP0; a slave error comes back as a result), PS
//   watchdog restart and status (mode and timeout stay as the monitor set them), PSCI CPU_ON for CPU1.
// - The SLCR, devcfg, DDR controller, OCM controller, TTC0, SWDT, CoreSight and the PL310 control registers are
//   secure-only in hardware, and so are the PL registers behind M_AXI_GP0/1 (their security registers cannot be
//   reached, see below) (dry runs 2026-10-17/18).
#include <stdint.h>
#include "fm_auth.h"

uint32_t kernel_entry, dtb_address;
#define SECURE_SECTION 13u                // DDR section of this image (monitor.ld 0x34000000), secure from T3a

static void w(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
static uint32_t r(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }

// both PS UARTs, only when U-Boot enabled the transmitter (CR TXEN set, TXDIS clear)
static void putc_uart(uint32_t base, char c)
{
    if ((r(base) & 0x30u) != 0x10u) return;
    while (r(base + 0x2C) & 0x10u) {}            // SR TXFULL
    w(base + 0x30, (uint8_t)c);
}

static void puts_uart(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') { putc_uart(0xE0000000u, '\r'); putc_uart(0xE0001000u, '\r'); }
        putc_uart(0xE0000000u, *s);
        putc_uart(0xE0001000u, *s);
    }
}

static void puthex(uint32_t v)
{
    char text[11] = "0x";
    for (int i = 0; i < 8; i++) text[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 0xF];
    text[10] = 0;
    puts_uart(text);
}

static uint32_t hex(const char *s)
{
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (; *s; s++) {
        uint32_t d = *s >= '0' && *s <= '9' ? (uint32_t)(*s - '0') : *s >= 'a' && *s <= 'f' ? (uint32_t)(*s - 'a' + 10)
                   : *s >= 'A' && *s <= 'F' ? (uint32_t)(*s - 'A' + 10) : 16u;
        if (d > 15) break;
        v = v * 16 + d;
    }
    return v;
}

// SiP SMC services for the non-secure kernel (Linux syscon regmap of the SLCR, drivers/mfd/syscon.c with
// tools/te0715-monitor/kernel/apply-t2b.py): SLCR read and write. The TrustZone registers (0x400-0x4FF) are read-only
// for the non-secure world, so it cannot reopen what the monitor closed. Results: r0 = 0 or NOT_SUPPORTED (-1), r1.
#define SMC_SLCR_READ 0x82000F01u
#define SMC_SLCR_WRITE 0x82000F02u
#define SMC_DEVCFG_READ 0x82000F03u      // devcfg registers and the XADC interface (0xF8007000, 0x000-0x11C)
#define SMC_DEVCFG_WRITE 0x82000F04u
#define SMC_L2C_WRITE_SEC 0x82000F05u    // PL310 secure registers (Linux outer_cache.write_sec)
// 0x82000F06 is retired (M_AXI_GP port security, unreachable on this board)
#define SMC_PL_READ 0x82000F07u          // PL registers behind M_AXI_GP0 (0x40000000-0x7FFFFFFF, secure-only here)
#define SMC_PL_WRITE 0x82000F08u
#define SMC_BUS_ERROR 0xFFFFFFFEu        // the PL slave answered with an error; r1 = DFSR with bit 31 set
#define SMC_PL_NOT_READY 0xFFFFFFFDu     // the PL is not configured, or its level shifters or resets block GP0
#define SMC_SWDT_FEED 0x82000F09u        // PS watchdog restart (the monitor armed it; the non-secure world feeds it)
#define SMC_SWDT_READ 0x82000F0Au        // PS watchdog MODE, CONTROL, RESTART, STATUS (offset 0x0-0xC)
#define SMC_OCMC_READ 0x82000F0Bu        // OCM controller: parity control, error address, interrupt status (0x0-0xC)
#define SMC_L2C_READ 0x82000F0Cu         // PL310 registers (0x000-0xFFC), for the FM parity check
#define SMC_SWDT_TIMEOUT 0x82000F0Du     // PS watchdog CONTROL (key, CRV >= 1, prescaler 4096), then a restart

// TrustZone T3b: the FM's secure services. The non-secure FM service calls them through /dev/f-astra-sec (ioctl).
// - Safehold latch: from then on until a reset, every PL and devcfg write is refused (the non-secure world can no
//   longer drive or reload the PL); reads and the watchdog stay.
// - Golden copies: per slot (a model), the first load of the boot copies the golden expected output from non-secure
//   DDR (the FM service's pool, uncached) into DDR section 13; a later load must bring the same bytes. A compare
//   checks a PL output in non-secure DDR against the secure copy, so a corrupted reference in the non-secure world
//   cannot pass a golden check.
#define SMC_FM_STATE 0x82000F10u           // r1 = flags (bit 0 Safehold), r2 = loaded golden slots (bit per slot)
#define SMC_FM_SAFEHOLD 0x82000F11u        // latch Safehold
#define SMC_FM_GOLDEN_LOAD 0x82000F12u     // r1 = slot, r2 = address, r3 = bytes: r0 = 0 copied, 1 the same as loaded,
                                           // GOLDEN_CHANGED otherwise; r1 = the copy's CRC-32
#define SMC_FM_GOLDEN_COMPARE 0x82000F13u  // r1 = slot, r2 = address, r3 = bytes: r1 = 0 equal, else first difference + 1
// T3c: r1 = the FM guard's base (its ID read first; a later call moves the watch to another model's guard). From then
// on until a reset the watchdog restarts (feed, and a timeout change) only after a write to the watched guard's
// HEARTBEAT register (the FM service writes it with every heartbeat, through the PL service here) since the last
// restart, or in Safehold: a hung FM service resets the board even when another process keeps feeding.
#define SMC_FM_WATCH 0x82000F14u
#define FM_GUARD_ID 0x464D4731u            // FMG_ID 'FMG1'
#define FM_GUARD_HEARTBEAT 0x20u
#define SMC_DENIED 0xFFFFFFFBu             // refused in Safehold
#define SMC_GOLDEN_CHANGED 0xFFFFFFFAu
#define FM_GOLDEN_BASE 0x34100000u         // section 13 after this image
#define FM_GOLDEN_SLOT_BYTES 0x00F00000u
#define FM_GOLDEN_SLOTS 4u
static volatile uint32_t fm_flags, golden_bytes[FM_GOLDEN_SLOTS], golden_crc[FM_GOLDEN_SLOTS];

// T3d: the ground's key (32 bytes, fm_key.c that the build writes from a key file into its ignored output folder; no
// key file: fm_hmac_key_set 0 and the T3b behaviour). With a key:
// - a golden slot loads only with the ground's manifest (FM_LABEL_GOLDEN, slot, bytes, SHA-256 of the data, signed):
//   GOLDEN_SIGNED copies the data into the slot first and checks the copy, so the non-secure world cannot change it
//   between the check and the copy; the unsigned load (first load of the boot trusted) is refused.
// - the Safehold latch can be released without a reset by the ground: RELEASE_CHALLENGE (only while latched) gives a
//   fresh 8-byte challenge, RELEASE takes the ground's tag over it (one try per challenge).
// The key sits in this image: secure at run time (section 13), but in the clear in the image file on the SD card.
extern const uint8_t fm_hmac_key[32];
extern const uint32_t fm_hmac_key_set;
#define SMC_FM_GOLDEN_SIGNED 0x82000F15u     // r1 = slot, r2 = address of [SHA-256][tag][data], r3 = data bytes: r0 as
                                             // GOLDEN_LOAD (or AUTH_FAILED), r1 = the copy's CRC-32
#define SMC_FM_RELEASE_CHALLENGE 0x82000F16u // r1, r2 = the challenge (bytes 0-3, 4-7, little-endian words)
#define SMC_FM_RELEASE 0x82000F17u           // r1 = address of the ground's 32-byte tag
#define SMC_AUTH_FAILED 0xFFFFFFF9u          // a tag or digest that does not hold (or no armed challenge)
#define SMC_AUTH_REQUIRED 0xFFFFFFF8u        // an unsigned golden load with a key, or a signed service without one
#define SMC_NOT_LATCHED 0xFFFFFFF7u          // a release challenge outside Safehold
#define FM_STATE_KEY 2u                      // FM_STATE r1 bit 1: the monitor has a key
static uint8_t golden_sha[FM_GOLDEN_SLOTS][32], release_nonce[8];
static volatile uint32_t release_armed, release_challenges;
// T3d: the PL register windows of the loaded bitstream (the ground's signed manifest, from the harness address map).
// With a key, a PL access (read, write, watch target) needs a window registered since the PL's last PROG_B pulse (a new
// configuration clears them); anything else is refused before it reaches the bus, where an undecoded address hangs.
#define SMC_PL_WINDOWS 0x82000F18u          // r1 = address of [count][count x (base, bytes)][tag]: r0 = 0, AUTH_*, -1
#define PL_WINDOWS_MAX 32u
static uint32_t pl_window_base[PL_WINDOWS_MAX], pl_window_bytes[PL_WINDOWS_MAX];
static volatile uint32_t pl_windows;

static int pl_window_ok(uint32_t address)
{
    if (!fm_hmac_key_set) return 1;                  // no key: the whole GP0 window (T3b)
    for (uint32_t i = 0; i < pl_windows; i++)
        if (address - pl_window_base[i] < pl_window_bytes[i] && pl_window_bytes[i] - (address - pl_window_base[i]) >= 4u)
            return 1;
    return 0;
}
static volatile uint32_t watch_heartbeat, heartbeat_seen;   // the watched HEARTBEAT address (0: no watch)
static uint32_t crc32_range(uint32_t start, uint32_t bytes);

// a range of non-secure DDR: inside the 1 GB, outside this section
static int ns_ddr(uint32_t address, uint32_t bytes)
{
    return bytes && address < 0x40000000u && bytes <= 0x40000000u - address
           && (address + bytes <= 0x34000000u || address >= 0x38000000u);
}

// PSCI 0.1 (the device tree's /psci node gives the function id): CPU_ON for CPU1, results as PSCI return codes
#define PSCI_CPU_ON 0x84000003u
#define PSCI_INVALID_PARAMETERS 0xFFFFFFFEu
#define PSCI_ALREADY_ON 0xFFFFFFFCu

volatile uint32_t pl_abort_status[2];   // per CPU, written by the secure data abort handler (monitor.S)
static volatile uint32_t secondary_entry, secondary_context, secondary_on;
void secondary_start(void);

static uint32_t cpu_index(void)
{
    uint32_t mpidr;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr & 1u;
}
uint32_t pl_read32(uint32_t address);
void pl_write32(uint32_t address, uint32_t value);

// A GP0 access gets no bus response (the core stops in here) while the PL is unconfigured, its level shifters are off
// or its resets are held, and also under a design that does not decode the address (the boot's PL design, dry runs
// 21 and 22): the PL services check first. Only a PL configured after a PROG_B pulse through the devcfg service counts
// (the kernel's FPGA manager pulses PROG_B on every full load, the FM service before an SEM reload): PCFG_DONE (devcfg
// INT_STS bit 2) follows the PL's DONE, which a PROG_B pulse drops until the new configuration completes. The boot's
// design, configured before this monitor ran, is never reached.
static volatile uint32_t pl_reset_seen;

static int pl_ready(void)
{
    return pl_reset_seen && (r(0xF800700Cu) & 0x4u) && (r(0xF8000900u) & 0xFu) == 0xFu && !(r(0xF8000240u) & 0xFu);
}

// the SLCR registers that change the PL's state: its clocks (FPGA0-3 clock and throttle control 0x170-0x1AC), its
// resets (FPGA_RST_CTRL 0x240) and its level shifters (LVL_SHFTR_EN 0x900); refused in Safehold (a failed PL reload
// in Safehold had left the PL in reset, dry run 23)
static int pl_state_register(uint32_t offset)
{
    return (offset >= 0x170u && offset < 0x1B0u) || offset == 0x240u || offset == 0x900u;
}

static int slcr_allowed(uint32_t offset, int write)
{
    if ((offset & 3u) || offset >= 0x1000u) return 0;
    if (offset >= 0x400u && offset < 0x500u) return !write;
    return 1;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// T3d: a golden load with the ground's manifest at `address` (SHA-256 of the data, tag), the data after it. The
// manifest is read once; a new slot gets the data copied in and checked there (the slot stays empty when it fails); a
// loaded slot answers whether the manifest names the same data (1) or other data (GOLDEN_CHANGED).
static uint32_t golden_signed(uint32_t slot, uint32_t address, uint32_t bytes)
{
    uint8_t manifest[64], head[16], tag[32], digest[32];
    uint32_t copy = FM_GOLDEN_BASE + slot * FM_GOLDEN_SLOT_BYTES;
    struct fm_part parts[2] = {{head, 16}, {manifest, 32}};
    struct fm_sha256 s;
    if (!fm_hmac_key_set) return SMC_AUTH_REQUIRED;
    for (int i = 0; i < 64; i++) manifest[i] = *(volatile uint8_t *)(uintptr_t)(address + (uint32_t)i);
    for (int i = 0; i < 8; i++) head[i] = (uint8_t)FM_LABEL_GOLDEN[i];
    put_le32(head + 8, slot);
    put_le32(head + 12, bytes);
    fm_hmac_sha256(fm_hmac_key, parts, 2, tag);
    if (!fm_equal32(manifest + 32, tag)) return SMC_AUTH_FAILED;
    if (golden_bytes[slot])
        return bytes == golden_bytes[slot] && fm_equal32(manifest, golden_sha[slot]) ? 1u : SMC_GOLDEN_CHANGED;
    for (uint32_t i = 0; i < bytes; i++)
        *(volatile uint8_t *)(uintptr_t)(copy + i) = *(volatile uint8_t *)(uintptr_t)(address + 64u + i);
    fm_sha256_init(&s);
    fm_sha256_update(&s, (const volatile uint8_t *)(uintptr_t)copy, bytes);
    fm_sha256_final(&s, digest);
    if (!fm_equal32(manifest, digest)) return SMC_AUTH_FAILED;
    for (int i = 0; i < 32; i++) golden_sha[slot][i] = digest[i];
    golden_crc[slot] = crc32_range(copy, bytes);
    golden_bytes[slot] = bytes;
    return 0;
}

// T3d: a fresh challenge for the ground's Safehold release: HMAC(key, FM_LABEL_NONCE, the whole 64-bit global timer,
// count), 8 bytes. A recorded tag fits again only if the timer value repeats to the cycle at the same count.
static void release_challenge(uint32_t *regs)
{
    uint8_t message[20], mac[32];
    struct fm_part part = {message, 20};
    uint32_t hi, lo;
    do { hi = r(0xF8F00204u); lo = r(0xF8F00200u); } while (hi != r(0xF8F00204u));
    for (int i = 0; i < 8; i++) message[i] = (uint8_t)FM_LABEL_NONCE[i];
    put_le32(message + 8, lo);
    put_le32(message + 12, hi);
    put_le32(message + 16, ++release_challenges);
    fm_hmac_sha256(fm_hmac_key, &part, 1, mac);
    for (int i = 0; i < 8; i++) release_nonce[i] = mac[i];
    release_armed = 1;
    regs[1] = (uint32_t)mac[0] | (uint32_t)mac[1] << 8 | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24;
    regs[2] = (uint32_t)mac[4] | (uint32_t)mac[5] << 8 | (uint32_t)mac[6] << 16 | (uint32_t)mac[7] << 24;
}

// T3d: the ground's tag over the armed challenge (HMAC(key, FM_LABEL_RELEASE, challenge)) clears the Safehold latch
static uint32_t release(uint32_t address)
{
    uint8_t message[16], mac[32], tag[32];
    struct fm_part part = {message, 16};
    if (!release_armed) return SMC_AUTH_FAILED;
    release_armed = 0;                                   // one try per challenge
    for (int i = 0; i < 32; i++) tag[i] = *(volatile uint8_t *)(uintptr_t)(address + (uint32_t)i);
    for (int i = 0; i < 8; i++) message[i] = (uint8_t)FM_LABEL_RELEASE[i];
    for (int i = 0; i < 8; i++) message[8 + i] = release_nonce[i];
    fm_hmac_sha256(fm_hmac_key, &part, 1, mac);
    if (!fm_equal32(tag, mac)) return SMC_AUTH_FAILED;
    fm_flags &= ~1u;
    puts_uart("f-astra monitor: safehold released by the ground\n");
    return 0;
}

// T3d: the ground's window manifest, read once into secure memory: count (1-32), then base and bytes per window (each
// inside the GP0 window, word-aligned), then HMAC(key, "FAPLWIN", count, the windows)
static uint32_t pl_windows_load(uint32_t address)
{
    uint8_t m[4 + 8 * PL_WINDOWS_MAX + 32], mac[32];
    uint32_t count, bytes, word[2 * PL_WINDOWS_MAX];
    struct fm_part parts[2] = {{(const uint8_t *)FM_LABEL_WINDOWS, 8}, {m, 0}};
    for (uint32_t i = 0; i < 4; i++) m[i] = *(volatile uint8_t *)(uintptr_t)(address + i);
    count = (uint32_t)m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 24;
    if (count == 0 || count > PL_WINDOWS_MAX) return 0xFFFFFFFFu;
    bytes = 4u + 8u * count;
    for (uint32_t i = 4; i < bytes + 32u; i++) m[i] = *(volatile uint8_t *)(uintptr_t)(address + i);
    parts[1].bytes = bytes;
    fm_hmac_sha256(fm_hmac_key, parts, 2, mac);
    if (!fm_equal32(m + bytes, mac)) return SMC_AUTH_FAILED;
    for (uint32_t i = 0; i < 2 * count; i++)
        word[i] = (uint32_t)m[4 + 4 * i] | (uint32_t)m[5 + 4 * i] << 8 | (uint32_t)m[6 + 4 * i] << 16
                  | (uint32_t)m[7 + 4 * i] << 24;
    for (uint32_t i = 0; i < count; i++)
        if (word[2 * i] < 0x40000000u || word[2 * i] >= 0x80000000u || (word[2 * i] & 3u) || word[2 * i + 1] < 4u
            || (word[2 * i + 1] & 3u) || word[2 * i + 1] > 0x80000000u - word[2 * i])
            return 0xFFFFFFFFu;
    pl_windows = 0;
    for (uint32_t i = 0; i < count; i++) {
        pl_window_base[i] = word[2 * i];
        pl_window_bytes[i] = word[2 * i + 1];
    }
    pl_windows = count;
    return 0;
}

void smc_handler(uint32_t *regs)
{
    if ((fm_flags & 1u) && ((regs[0] == SMC_DEVCFG_WRITE && regs[1] < 0x100u) || regs[0] == SMC_PL_WRITE
                            || (regs[0] == SMC_SLCR_WRITE && pl_state_register(regs[1])))) {
        // Safehold: the PL is neither driven, reloaded, reset, unclocked nor cut off; the XADC interface (devcfg
        // 0x100-0x11C) stays usable (temperature and supplies, dry run 24)
        regs[0] = SMC_DENIED;
    } else if (regs[0] == SMC_SLCR_READ && slcr_allowed(regs[1], 0)) {
        regs[1] = r(0xF8000000u + regs[1]);
        regs[0] = 0;
    } else if (regs[0] == SMC_SLCR_WRITE && slcr_allowed(regs[1], 1)) {
        w(0xF8000000u + regs[1], regs[2]);
        regs[0] = 0;
    } else if ((regs[0] == SMC_DEVCFG_READ || regs[0] == SMC_DEVCFG_WRITE) && !(regs[1] & 3u) && regs[1] < 0x120u) {
        if (regs[0] == SMC_DEVCFG_READ) regs[1] = r(0xF8007000u + regs[1]);
        else w(0xF8007000u + regs[1], regs[2]);
        // PROG_B low (CTRL bit 30) clears the PL: from here on only the configuration that follows is reached
        if (regs[0] == SMC_DEVCFG_WRITE && regs[1] == 0u && !(regs[2] & (1u << 30))) {
            w(0xF800700Cu, 0x4u);                        // PCFG_DONE status cleared as well (in case it is sticky)
            pl_reset_seen = 1;
            pl_windows = 0;                              // T3d: the next configuration brings its own windows
        }
        regs[0] = 0;
    } else if (regs[0] == SMC_L2C_WRITE_SEC && (regs[1] == 0x100u || regs[1] == 0x104u || regs[1] == 0x108u
                                                 || regs[1] == 0x10Cu || regs[1] == 0xF40u || regs[1] == 0xF60u
                                                 || regs[1] == 0xF80u || regs[1] == 0x220u)) {   // 0x220: INTCLR
        // AUX keeps non-secure lockdown (26) and interrupt access (27): the kernel unlocks the ways and clears the
        // L2 interrupts (parity) from the non-secure world
        w(0xF8F02000u + regs[1], regs[1] == 0x104u ? regs[2] | (1u << 26) | (1u << 27) : regs[2]);
        regs[0] = 0;
    } else if ((regs[0] == SMC_PL_READ || regs[0] == SMC_PL_WRITE) && !(regs[1] & 3u)
               && regs[1] >= 0x40000000u && regs[1] < 0x80000000u && !pl_ready()) {
        regs[0] = SMC_PL_NOT_READY;
    } else if ((regs[0] == SMC_PL_READ || regs[0] == SMC_PL_WRITE) && !(regs[1] & 3u)
               && regs[1] >= 0x40000000u && regs[1] < 0x80000000u && !pl_window_ok(regs[1])) {
        regs[0] = SMC_DENIED;                            // T3d: outside the bitstream's registered windows
    } else if ((regs[0] == SMC_PL_READ || regs[0] == SMC_PL_WRITE) && !(regs[1] & 3u)
               && regs[1] >= 0x40000000u && regs[1] < 0x80000000u) {
        uint32_t value = 0, cpu = cpu_index();
        pl_abort_status[cpu] = 0;
        if (regs[0] == SMC_PL_READ) value = pl_read32(regs[1]);
        else pl_write32(regs[1], regs[2]);
        if (regs[0] == SMC_PL_WRITE && regs[1] == watch_heartbeat && !pl_abort_status[cpu]) heartbeat_seen = 1;
        regs[0] = pl_abort_status[cpu] ? SMC_BUS_ERROR : 0;
        regs[1] = pl_abort_status[cpu] ? pl_abort_status[cpu] : value;
    } else if (regs[0] == PSCI_CPU_ON) {
        // r1 = target MPIDR (CPU1 only), r2 = its non-secure entry, r3 = context id. CPU1 waits in the BootROM loop
        // for a start address at 0xFFFFFFF0 (OCM, mapped high) and an event.
        if ((regs[1] & 0xFFFFFFu) != 1u) {
            regs[0] = PSCI_INVALID_PARAMETERS;
        } else if (secondary_on) {
            regs[0] = PSCI_ALREADY_ON;
        } else {
            secondary_entry = regs[2];
            secondary_context = regs[3];
            secondary_on = 1;
            w(0xFFFFFFF0u, (uint32_t)(uintptr_t)secondary_start);
            __asm__ volatile("dsb\n\tsev" ::: "memory");
            regs[0] = 0;
        }
    } else if ((regs[0] == SMC_SWDT_FEED || regs[0] == SMC_SWDT_TIMEOUT) && watch_heartbeat && !heartbeat_seen
               && !(fm_flags & 1u)) {
        regs[0] = SMC_DENIED;                            // T3c: no FM heartbeat since the last restart
    } else if (regs[0] == SMC_FM_WATCH && regs[1] >= 0x40000000u && regs[1] < 0x80000000u && pl_window_ok(regs[1])
               && pl_window_ok(regs[1] + FM_GUARD_HEARTBEAT)
               && !(regs[1] & 0xFFFu) && pl_ready()) {
        uint32_t cpu = cpu_index(), id;
        pl_abort_status[cpu] = 0;
        id = pl_read32(regs[1]);
        if (!pl_abort_status[cpu] && id == FM_GUARD_ID) {
            // the watch moves to this guard (another model's FM); no restart is granted by it
            if (!watch_heartbeat) puts_uart("f-astra monitor: watchdog follows the FM heartbeat\n");
            watch_heartbeat = regs[1] + FM_GUARD_HEARTBEAT;
            regs[0] = 0;
        } else {
            regs[0] = 0xFFFFFFFFu;
        }
    } else if (regs[0] == SMC_SWDT_FEED) {
        w(0xF8005008u, 0x1999u);                         // restart key: reload the counter (mode and timeout stay)
        heartbeat_seen = 0;
        regs[0] = 0;
    } else if (regs[0] == SMC_SWDT_READ && !(regs[1] & 3u) && regs[1] < 0x10u) {
        regs[1] = r(0xF8005000u + regs[1]);
        regs[0] = 0;
    } else if (regs[0] == SMC_SWDT_TIMEOUT && (regs[1] >> 14) == 0x248u && (regs[1] & 3u) == 3u
               && ((regs[1] >> 2) & 0xFFFu) >= 1u) {
        w(0xF8005004u, regs[1]);                         // the mode (reset, enabled) stays as the monitor set it
        w(0xF8005008u, 0x1999u);
        heartbeat_seen = 0;
        regs[0] = 0;
    } else if (regs[0] == SMC_OCMC_READ && !(regs[1] & 3u) && regs[1] < 0x10u) {
        regs[1] = r(0xF800C000u + regs[1]);
        regs[0] = 0;
    } else if (regs[0] == SMC_FM_STATE) {
        uint32_t loaded = 0;
        for (uint32_t i = 0; i < FM_GOLDEN_SLOTS; i++) loaded |= golden_bytes[i] ? 1u << i : 0u;
        regs[1] = fm_flags | (fm_hmac_key_set ? FM_STATE_KEY : 0u);
        regs[2] = loaded;
        regs[3] = watch_heartbeat;
        regs[0] = 0;
    } else if (regs[0] == SMC_FM_SAFEHOLD) {
        if (!(fm_flags & 1u)) puts_uart("f-astra monitor: safehold latched\n");
        fm_flags |= 1u;
        regs[0] = 0;
    } else if (regs[0] == SMC_FM_GOLDEN_LOAD && fm_hmac_key_set) {
        regs[0] = SMC_AUTH_REQUIRED;                     // T3d: with a key, golden data comes with a manifest only
    } else if (regs[0] == SMC_FM_GOLDEN_SIGNED && regs[1] < FM_GOLDEN_SLOTS && regs[3] <= FM_GOLDEN_SLOT_BYTES
               && ns_ddr(regs[2], regs[3] + 64u)) {
        uint32_t slot = regs[1];
        regs[0] = golden_signed(slot, regs[2], regs[3]);
        regs[1] = golden_crc[slot];
    } else if (regs[0] == SMC_FM_RELEASE_CHALLENGE) {
        regs[0] = !fm_hmac_key_set ? SMC_AUTH_REQUIRED : !(fm_flags & 1u) ? SMC_NOT_LATCHED : 0u;
        if (regs[0] == 0) release_challenge(regs);
    } else if (regs[0] == SMC_PL_WINDOWS && ns_ddr(regs[1], 4u + 8u * PL_WINDOWS_MAX + 32u)) {
        regs[0] = !fm_hmac_key_set ? SMC_AUTH_REQUIRED : pl_windows_load(regs[1]);
    } else if (regs[0] == SMC_FM_RELEASE && ns_ddr(regs[1], 32u)) {
        regs[0] = !fm_hmac_key_set ? SMC_AUTH_REQUIRED : release(regs[1]);
    } else if (regs[0] == SMC_FM_GOLDEN_LOAD && regs[1] < FM_GOLDEN_SLOTS && ns_ddr(regs[2], regs[3])
               && regs[3] <= FM_GOLDEN_SLOT_BYTES) {
        uint32_t slot = regs[1], copy = FM_GOLDEN_BASE + slot * FM_GOLDEN_SLOT_BYTES, crc = crc32_range(regs[2], regs[3]);
        if (golden_bytes[slot]) {                        // loaded earlier in this boot: the same bytes only
            regs[0] = crc == golden_crc[slot] && regs[3] == golden_bytes[slot] ? 1u : SMC_GOLDEN_CHANGED;
        } else {
            for (uint32_t i = 0; i < regs[3]; i++)
                *(volatile uint8_t *)(uintptr_t)(copy + i) = *(volatile uint8_t *)(uintptr_t)(regs[2] + i);
            golden_crc[slot] = crc32_range(copy, regs[3]);
            golden_bytes[slot] = regs[3];
            regs[0] = golden_crc[slot] == crc ? 0u : SMC_GOLDEN_CHANGED;   // the source changed while copying
        }
        regs[1] = golden_crc[slot];
    } else if (regs[0] == SMC_FM_GOLDEN_COMPARE && regs[1] < FM_GOLDEN_SLOTS && golden_bytes[regs[1]]
               && regs[3] == golden_bytes[regs[1]] && ns_ddr(regs[2], regs[3])) {
        uint32_t copy = FM_GOLDEN_BASE + regs[1] * FM_GOLDEN_SLOT_BYTES, i;
        for (i = 0; i < regs[3]; i++)
            if (*(volatile uint8_t *)(uintptr_t)(copy + i) != *(volatile uint8_t *)(uintptr_t)(regs[2] + i)) break;
        regs[1] = i < regs[3] ? i + 1u : 0u;
        regs[0] = 0;
    } else if (regs[0] == SMC_L2C_READ && !(regs[1] & 3u) && regs[1] < 0x1000u) {
        regs[1] = r(0xF8F02000u + regs[1]);
        regs[0] = 0;
    } else {
        regs[0] = 0xFFFFFFFFu;
    }
}

// CPU1's part of the secure setup (monitor.S secondary_start): its banked GIC registers (every SGI and PPI in group
// 1, the CPU interface on for both groups, all priorities); returns the non-secure entry (r0) and context id (r1)
uint64_t secondary_setup(void)
{
    w(0xF8F01080u, 0xFFFFFFFFu);
    w(0xF8F00100u, 0x3u);
    w(0xF8F00104u, 0xFFu);
    puts_uart("f-astra monitor: cpu1 entry ");
    puthex(secondary_entry);
    puts_uart("\n");
    return ((uint64_t)secondary_context << 32) | secondary_entry;
}

// secure data abort during a PL register service (monitor.S sec_data_abort): the slave answered SLVERR or DECERR.
// Records DFSR (bit 31 set) for the service; returns 1 for an asynchronous abort (DFSR FS = 0b10110).
int pl_abort_record(uint32_t dfsr, uint32_t dfar)
{
    pl_abort_status[cpu_index()] = dfsr | 0x80000000u;
    puts_uart("f-astra monitor: pl bus error dfsr ");
    puthex(dfsr);
    puts_uart(" dfar ");
    puthex(dfar);
    puts_uart("\n");
    return ((((dfsr >> 6) & 0x10u) | (dfsr & 0xFu)) == 0x16u) ? 1 : 0;
}

// any other secure exception: report the mode (CPSR) and the return address, then the caller stops
void secure_exception_report(uint32_t cpsr, uint32_t lr)
{
    puts_uart("f-astra monitor: unexpected secure exception cpsr ");
    puthex(cpsr);
    puts_uart(" lr ");
    puthex(lr);
    puts_uart("\n");
}

// CRC-32 (zlib) of a memory range read with the caches off: what the non-secure kernel will find in DDR
static uint32_t crc32_range(uint32_t start, uint32_t bytes)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < bytes; i++) {
        crc ^= *(volatile uint8_t *)(uintptr_t)(start + i);
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void monitor_main(int argc, char *const argv[])
{
    // U-Boot `go ADDR KERNEL DTB` passes argv[0] = ADDR
    kernel_entry = argc > 1 ? hex(argv[1]) : 0;
    dtb_address = argc > 2 ? hex(argv[2]) : 0;
    puts_uart("\nf-astra monitor T2: kernel");
    puthex(kernel_entry);
    puts_uart(" dtb ");
    puthex(dtb_address);
    puts_uart("\n");
    if (!kernel_entry || !dtb_address) { puts_uart("f-astra monitor: usage go ADDR KERNEL DTB\n"); for (;;) {} }

    // PS watchdog (SWDT, secure): CPU_1x / 4096, 600 s (the longest), reset; never fed by the monitor yet
    w(0xF8005004, 0x00920000u | (3975u << 2) | 3u);      // CCR: CKEY, CRV, prescaler 4096
    w(0xF8005000, 0x00ABC000u | 0x30u | 0x2u | 0x1u);    // ZMR: ZKEY, RSTLEN 16, RSTEN, WDEN
    w(0xF8005008, 0x1999u);                              // restart: load the counter
    puts_uart("f-astra monitor: swdt armed\n");

    w(0xF8000008, 0xDF0Du);                              // SLCR unlock
    // the non-secure kernel's clocks are fixed rates (no gating): the PS peripherals it uses stay clocked
    // (APER_CLK_CTRL: DMA, USB0, SDIO0, CAN0, I2C0, UART0, GPIO)
    w(0xF800012C, r(0xF800012C) | 0x00550405u);
    // TrustZone: DDR sections, OCM pages, PS DMA controller, GEM, SDIO and USB non-secure, except DDR section 13
    // (0x34000000-0x37FFFFFF, T3a): this monitor and the secure FM data; the device tree reserves it no-map, and a
    // non-secure access (the kernel or a PL master) gets DECERR
    w(0xF8000430, 0xFFFFu & ~(1u << SECURE_SECTION));
    w(0xF8000400, 0xFFFFFFFFu); w(0xF8000404, 0xFFFFFFFFu); w(0xF8000408, 0xFFFFFFFFu);
    w(0xF8000440, 1u); w(0xF8000444, 0xFFFFu); w(0xF8000448, 0xFu);
    w(0xF8000450, 0xFFFFFFFFu); w(0xF8000454, 0xFFFFFFFFu); w(0xF8000458, 0xFFFFFFFFu);
    // the IOU slaves (APB: UART, CAN, GPIO, I2C, SPI; SDIO; QSPI) non-secure
    puts_uart("f-astra monitor: slcr tz\n");
    w(0xE0200018, 0xFFFFFFFFu);
    puts_uart("f-astra monitor: iou apb\n");
    w(0xE0200008, 0xFFFFFFFFu);
    puts_uart("f-astra monitor: iou sdio0\n");
    w(0xE020000C, 0xFFFFFFFFu);
    puts_uart("f-astra monitor: iou sdio1\n");
    w(0xE0200010, 0xFFFFFFFFu);
    puts_uart("f-astra monitor: iou qspi\n");
    // M_AXI_GP0/1 stay as reset left them, always secure: their security registers (security_gp0/1_axi, 0xF890001C,
    // 0xF8900020) sit in the top interconnect GPV (0xF8900000), and on this board every GPV access, read or write,
    // gets no bus response: from U-Boot, from here and from the secure Linux, with the PL configured or not
    // (2026-10-17/18; the NIC-301 ID registers at 0xF8900FE0 too). The non-secure world reaches the PL registers
    // only through the secure world.

    // OCM parity (FM L0-H4-01): what the kernel's Zynq OCM driver writes in the secure boot (OCM_PARITY_CTRL 0x1E:
    // parity check and its interrupts); the OCM controller is secure-only, so here
    w(0xF800C000, 0x1Eu);

    // GIC: every interrupt in group 1 (non-secure), both groups on; CPU interface: both groups, all priorities
    for (uint32_t i = 0; i < 3; i++) w(0xF8F01080u + 4 * i, 0xFFFFFFFFu);
    w(0xF8F01000, 0x3u);
    w(0xF8F00100, 0x3u);
    w(0xF8F00104, 0xFFu);
    // SCU: secure and non-secure access to the SCU, private timers and global timer
    w(0xF8F00050, 0xFu);
    w(0xF8F00054, 0xFFFu);
    puts_uart("f-astra monitor: gic scu\n");

    // PL310 L2: the kernel must start with every data cache off (its decompressor knows nothing of the outer cache:
    // with the L2 on, the decompressed kernel stayed dirty in it and the kernel read stale DDR, dry runs 3 and 4).
    // The secure firmware's lines are written back and the L2 stays off; the kernel enables it itself through the
    // L2C write SMC.
    if (r(0xF8F02100) & 1u) {
        w(0xF8F027FC, 0xFFu);                            // clean and invalidate all ways
        while (r(0xF8F027FC) & 0xFFu) {}
        w(0xF8F02730, 0u);                               // cache sync
        while (r(0xF8F02730) & 1u) {}
        w(0xF8F02100, 0u);                               // disable
        puts_uart("f-astra monitor: l2 written back and off\n");
    }
    if (argc > 4) {          // optional: the initrd (start, bytes) to check in DDR before the kernel reads it
        puts_uart("f-astra monitor: initrd crc ");
        puthex(crc32_range(hex(argv[3]), hex(argv[4])));
        puts_uart("\n");
    }
    puts_uart("f-astra monitor: secure setup done, entering the non-secure kernel\n");
}
