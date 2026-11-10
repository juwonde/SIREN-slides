// F-ASTRA board FM service core (no hardware access): batch input checks, result rules, fault
// classification, the v2 recovery ladder per FM item, persistent recovery counts and telemetry lines.
// docs/f-astra-fault-management.md describes the items; f_astra/fault_management.py holds the catalog.
#ifndef F_ASTRA_FM_CORE_H
#define F_ASTRA_FM_CORE_H
#include <stddef.h>
#include <stdint.h>

enum fm_id { FM_L0_H1_01, FM_L0_H1_02, FM_L0_H1_03, FM_L0_H3_02, FM_L0_H4_05, FM_L1_S2_01, FM_L1_S2_02,
             FM_L1_S3_01, FM_L1_S3_02, FM_L0_H4_02, FM_L0_H4_04, FM_L2_G1_02, FM_L0_H4_01, FM_L0_H4_03,
             FM_L1_S1_01, FM_L1_S1_02, FM_COUNT };
enum fm_action { ACT_DISCARD, ACT_SAFE_OUTPUT, ACT_BLOCK, ACT_INTERFACE_INIT, ACT_PL_IP_RESET, ACT_REVERIFY,
                 ACT_RETRY, ACT_SAFEHOLD, ACT_RELOAD, ACT_RESTART, ACT_COUNT };

extern const char *const fm_names[FM_COUNT];
extern const char *const fm_action_names[ACT_COUNT];

// Guard FAULTS bits (f_astra_fm_guard.v).
#define FM_GUARD_RANGE    (1u << 0)
#define FM_GUARD_FORMAT   (1u << 1)
#define FM_GUARD_DEADLINE (1u << 2)
#define FM_GUARD_STALE    (1u << 3)
#define FM_GUARD_WDT      (1u << 4)
#define FM_GUARD_STALL    (1u << 5)
// AXI DMA status error bits (DMAIntErr, DMASlvErr, DMADecErr).
#define FM_DMA_ERRORS 0x70u

struct fm_obs {
    int transfer_timeout;       // DMA completion not seen within the batch timeout
    uint32_t dma_status_errors; // MM2S|S2MM status & FM_DMA_ERRORS
    uint32_t guard_faults;      // guard FAULTS register
    int input_bad;              // batch header/CRC/length/time check failed
    int inference_late;         // completed but beyond the inference deadline
    int result_bad;             // PS result rule failed
    int pll_unlocked;           // ARM/DDR/IO PLL not locked (SLCR PLL_STATUS)
    int environment_alarm;      // XADC temperature or supply outside the configured range
    int memory_corrupt;         // CRC scrub of the protected FM data failed (DDR without ECC on this board)
    int protection_violation;   // a write to MMU-protected FM data was blocked (TrustZone is not used here)
    unsigned parity_errors;     // new L2 or OCM parity errors (FM_PARITY_L2 | FM_PARITY_OCM)
};

// L0-H4-01 parity (R3). PL310 L2 cache: AUX_CTRL bit 21 enables parity (base DT arm,parity-enable from image r10);
// raw interrupt status PARRT (tag RAM) and PARRD (data RAM), L2C-310 TRM interrupt bit assignments. Linux masks the
// L2 interrupts here, so the service polls the raw status each batch and clears it through INTCLR. OCM: the kernel's
// Zynq OCM driver writes OCM_PARITY_CTRL = 0x1E and owns the parity interrupt (IRQ_STS error bits, the address in
// PARITY_ERRADDRESS) without clearing the status, so a new OCM error is a higher ocmc interrupt count or another status
// or address than at the last check.
#define FM_L2C_BASE 0xF8F02000u
#define FM_L2C_AUX_CTRL 0x104u
#define FM_L2C_RAW_INTR 0x21Cu
#define FM_L2C_INTR_CLEAR 0x220u
#define FM_L2C_AUX_PARITY (1u << 21)
#define FM_L2C_PARRT (1u << 1)
#define FM_L2C_PARRD (1u << 2)
#define FM_OCM_BASE 0xF800C000u
#define FM_OCM_PARITY_CTRL 0x0u
#define FM_OCM_ERR_ADDRESS 0x4u
#define FM_OCM_IRQ_STS 0x8u
#define FM_OCM_PARITY_ENABLE 0x1Eu
#define FM_OCM_IRQ_ERRORS 0x7u
#define FM_PARITY_L2 1u
#define FM_PARITY_OCM 2u
struct fm_parity {
    uint32_t l2_aux, l2_raw, ocm_ctrl, ocm_status, ocm_address;
    uint64_t ocm_irqs;          // the ocmc line of /proc/interrupts, all CPUs
};
typedef uint32_t (*fm_read32)(void *ctx, uint32_t address);
// Read the five registers through `read` (physical addresses); ocm_irqs is left to the caller.
void fm_parity_read(fm_read32 read, void *ctx, struct fm_parity *p);
// FM_PARITY_L2 | FM_PARITY_OCM for each memory whose parity is on.
unsigned fm_parity_enabled(const struct fm_parity *p);
// New errors of `now` since `last`: L2 raw parity bits (cleared after each check), OCM interrupts or status change.
unsigned fm_parity_errors(const struct fm_parity *now, const struct fm_parity *last);
// "l2_aux=0x... l2_raw=0x... ocm_ctrl=0x... ocm_status=0x... ocm_address=0x... ocm_irqs=N" into buf.
int fm_parity_text(char *buf, size_t size, const struct fm_parity *p);
// The summed per-CPU count of the /proc/interrupts line whose name ends with `name`; 0 when absent.
uint64_t fm_irq_count(const char *interrupts, const char *name);

// TrustZone T1 (PL masters non-secure): SLCR TZ_DDR_RAM opens 64 MB DDR sections to non-secure masters, bit n for
// [n x 64 MB, (n + 1) x 64 MB) of the 1 GB DDR. The bits of every section that [address, address + bytes) touches.
#define FM_SLCR_TZ_DDR_RAM 0xF8000430u
#define FM_TZ_SECTION_BYTES (64u << 20)
#define FM_TZ_SECTIONS 16u
uint32_t fm_tz_ddr_mask(uint64_t address, uint64_t bytes);

// Faults of one batch in recovery priority order; returns their number.
int fm_classify(const struct fm_obs *o, enum fm_id *ids, int max);
// The v2 recovery sequence of an item.
const enum fm_action *fm_plan(enum fm_id id, int *count);

struct fm_state {
    uint32_t count[FM_COUNT];   // recoveries per item since the last ground reset
    uint32_t safehold;          // 1 after the recovery limit was exceeded
    char cause[16];             // item that caused the safehold
    uint32_t boot_pending;      // Initial runs started without reaching the Healthy Bit (L2-G1-02)
    uint64_t last_sequence;     // the newest batch sequence taken (always-on service: a replay across restarts is stale)
};
int fm_state_load(const char *path, struct fm_state *s);   // missing file = fresh state; -1: both slots damaged
int fm_state_save(const char *path, const struct fm_state *s);
// Count one recovery of `id`; returns 1 when that exceeds `limit` (L2-G1-01) and marks the safehold.
int fm_count_recovery(struct fm_state *s, enum fm_id id, uint32_t limit);

#define FM_BATCH_MAGIC 0x4E494146u   // "FAIN"
#define FM_KIND_GRU_STEPS 3           // payload: every step's x(t) codes; the service keeps h (GRU step loop)
#define FM_KIND_ENGINE_ROWS 2         // payload: every frame's engine row, then every frame's input (engine-fed serve)
struct fm_batch_header {
    uint32_t magic;
    uint16_t version;
    uint16_t kind;
    uint32_t frames;
    uint32_t payload_bytes;
    uint64_t sequence;           // strictly increasing per mission: a repeat is stale data
    uint32_t crc32;              // of the payload
    uint32_t reserved;
};
uint32_t fm_crc32(const uint8_t *data, size_t bytes);
// the CRC of `crc`'s data followed by `data` (crc 0: none before); fm_crc32(d, n) == fm_crc32_add(0, d, n)
uint32_t fm_crc32_add(uint32_t crc, const uint8_t *data, size_t bytes);
// L1-S2-02: 0 when the header describes `payload` (frames x frame_bytes, CRC, newer sequence); else a reason.
const char *fm_check_batch(const struct fm_batch_header *h, const uint8_t *payload, size_t available,
                           uint32_t frame_bytes, uint32_t max_frames, uint64_t last_sequence);

// L1-S3-02: element count and every element within [lo, hi] at its declared width; little-endian
// containers of elem_bytes (1, 2, 4). Returns 0, or 1 with the first bad element index.
int fm_check_result(const uint8_t *out, size_t bytes, uint32_t elements, uint32_t elem_bytes, uint32_t elem_bits,
                    int is_signed, int64_t lo, int64_t hi, uint32_t *bad_index);
void fm_fill_safe(uint8_t *out, uint32_t elements, uint32_t elem_bytes, int64_t safe);

// One JSON telemetry line (no trailing newline) into buf; returns its length.
int fm_event_json(char *buf, size_t size, uint64_t t_ns, long batch, const char *event, const char *item,
                  const char *detail, const struct fm_state *s);
#endif
