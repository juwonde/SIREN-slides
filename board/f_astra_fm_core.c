// F-ASTRA board FM service core; see f_astra_fm_core.h.
#include "f_astra_fm_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32  /* host unit test only */
#include <io.h>
#define fsync _commit
#endif

const char *const fm_names[FM_COUNT] = {"L0-H1-01", "L0-H1-02", "L0-H1-03", "L0-H3-02", "L0-H4-05",
                                        "L1-S2-01", "L1-S2-02", "L1-S3-01", "L1-S3-02", "L0-H4-02", "L0-H4-04",
                                        "L2-G1-02", "L0-H4-01", "L0-H4-03", "L1-S1-01", "L1-S1-02"};
const char *const fm_action_names[ACT_COUNT] = {"discard", "safe_output", "block", "interface_init", "pl_ip_reset",
                                                "reverify", "retry", "safehold", "reload", "restart"};

// Recovery sequences as written in the v2 design (Appendix I, L0/L1).
static const enum fm_action plan_range[] = {ACT_SAFE_OUTPUT, ACT_PL_IP_RESET, ACT_REVERIFY};
static const enum fm_action plan_format[] = {ACT_DISCARD, ACT_INTERFACE_INIT, ACT_PL_IP_RESET};
static const enum fm_action plan_timing[] = {ACT_BLOCK, ACT_PL_IP_RESET};
static const enum fm_action plan_pl_wdt[] = {ACT_PL_IP_RESET, ACT_INTERFACE_INIT, ACT_RETRY};
static const enum fm_action plan_ps_pl[] = {ACT_BLOCK, ACT_PL_IP_RESET, ACT_INTERFACE_INIT, ACT_RETRY};
static const enum fm_action plan_io[] = {ACT_INTERFACE_INIT, ACT_PL_IP_RESET, ACT_RETRY};
static const enum fm_action plan_input[] = {ACT_DISCARD, ACT_INTERFACE_INIT};
static const enum fm_action plan_late[] = {ACT_DISCARD, ACT_INTERFACE_INIT, ACT_PL_IP_RESET, ACT_RETRY};
static const enum fm_action plan_result[] = {ACT_SAFE_OUTPUT, ACT_INTERFACE_INIT, ACT_RETRY};
static const enum fm_action plan_clock[] = {ACT_PL_IP_RESET, ACT_INTERFACE_INIT, ACT_RETRY};
static const enum fm_action plan_environment[] = {ACT_SAFEHOLD};
static const enum fm_action plan_memory[] = {ACT_SAFE_OUTPUT, ACT_RELOAD, ACT_REVERIFY};
static const enum fm_action plan_protection[] = {ACT_DISCARD, ACT_INTERFACE_INIT, ACT_RETRY};
// L1-S1-01/02: the supervisor restarts the worker; repeated failures escalate through the recovery limit (L2-G1-01)
static const enum fm_action plan_supervision[] = {ACT_RESTART};

#define PLAN(p) do { *count = (int)(sizeof(p) / sizeof((p)[0])); return (p); } while (0)
const enum fm_action *fm_plan(enum fm_id id, int *count)
{
    switch (id) {
    case FM_L0_H1_01: PLAN(plan_range);
    case FM_L0_H1_02: PLAN(plan_format);
    case FM_L0_H1_03: PLAN(plan_timing);
    case FM_L0_H3_02: PLAN(plan_pl_wdt);
    case FM_L0_H4_05: PLAN(plan_ps_pl);
    case FM_L1_S2_01: PLAN(plan_io);
    case FM_L1_S2_02: PLAN(plan_input);
    case FM_L1_S3_01: PLAN(plan_late);
    case FM_L0_H4_02: PLAN(plan_clock);
    case FM_L0_H4_04: case FM_L2_G1_02: PLAN(plan_environment);
    case FM_L0_H4_01: PLAN(plan_memory);
    case FM_L0_H4_03: PLAN(plan_protection);
    case FM_L1_S1_01: case FM_L1_S1_02: PLAN(plan_supervision);
    default: PLAN(plan_result);
    }
}

int fm_classify(const struct fm_obs *o, enum fm_id *ids, int max)
{
    int n = 0;
#define ADD(cond, id) do { if ((cond) && n < max) ids[n++] = (id); } while (0)
    ADD(o->environment_alarm, FM_L0_H4_04);
    ADD(o->memory_corrupt || o->parity_errors, FM_L0_H4_01);
    ADD(o->protection_violation, FM_L0_H4_03);
    ADD(o->pll_unlocked, FM_L0_H4_02);
    ADD(o->input_bad, FM_L1_S2_02);
    ADD(o->guard_faults & FM_GUARD_WDT, FM_L0_H3_02);
    ADD(o->dma_status_errors || (o->guard_faults & FM_GUARD_STALL), FM_L0_H4_05);
    ADD(o->guard_faults & (FM_GUARD_DEADLINE | FM_GUARD_STALE), FM_L0_H1_03);
    ADD(o->guard_faults & FM_GUARD_RANGE, FM_L0_H1_01);
    ADD(o->guard_faults & FM_GUARD_FORMAT, FM_L0_H1_02);
    ADD(o->transfer_timeout, FM_L1_S2_01);
    ADD(o->inference_late, FM_L1_S3_01);
    ADD(o->result_bad, FM_L1_S3_02);
#undef ADD
    return n;
}

// The state file holds two slots written in place: an SD FAT file keeps its size and clusters, and no tmp+rename can
// leave it missing after a power loss. A slot is the line "FMS2 gen=G crc=C" and the key=value lines, padded with
// newlines to FM_STATE_SLOT bytes (C: CRC-32 of everything after the first line). A save writes the slot that does not
// hold the newest valid record, with the next generation, and syncs it; a load takes the newest slot whose CRC holds,
// so a write cut short leaves the record before it. A file in the earlier text form still loads.
#define FM_STATE_SLOT 512
#define FM_STATE_HEAD 33            // "FMS2 gen=%010u crc=%08x\n"

static void state_parse(const char *text, struct fm_state *s)
{
    char line[96], key[48], value[48];
    while (*text) {
        size_t n = strcspn(text, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) - 1 ? n : sizeof(line) - 1), text);
        text += n + (text[n] == '\n');
        if (sscanf(line, "%47[^=]=%47s", key, value) < 2) continue;
        if (!strcmp(key, "safehold")) s->safehold = (uint32_t)strtoul(value, NULL, 10);
        else if (!strcmp(key, "boot_pending")) s->boot_pending = (uint32_t)strtoul(value, NULL, 10);
        else if (!strcmp(key, "last_sequence")) s->last_sequence = strtoull(value, NULL, 10);
        else if (!strcmp(key, "cause")) snprintf(s->cause, sizeof(s->cause), "%.15s", strcmp(value, "-") ? value : "");
        else for (int i = 0; i < FM_COUNT; i++)
            if (!strcmp(key, fm_names[i])) s->count[i] = (uint32_t)strtoul(value, NULL, 10);
    }
}

// slot k of a two-slot file: 1 when its CRC holds (*gen its generation)
static int state_slot(const char *file, size_t bytes, int k, uint32_t *gen)
{
    const char *slot = file + (size_t)k * FM_STATE_SLOT;
    unsigned g, crc;
    if (bytes < (size_t)(k + 1) * FM_STATE_SLOT || memcmp(slot, "FMS2 gen=", 9) || slot[FM_STATE_HEAD - 1] != '\n'
        || sscanf(slot, "FMS2 gen=%10u crc=%8x", &g, &crc) != 2)
        return 0;
    *gen = g;
    return fm_crc32((const uint8_t *)slot + FM_STATE_HEAD, FM_STATE_SLOT - FM_STATE_HEAD) == crc;
}

// the newest valid slot of a two-slot file, or -1
static int state_newest(const char *file, size_t bytes, uint32_t *gen)
{
    uint32_t g[2];
    int ok0 = state_slot(file, bytes, 0, &g[0]), ok1 = state_slot(file, bytes, 1, &g[1]);
    int k = ok0 && ok1 ? (g[1] > g[0]) : ok1 ? 1 : ok0 ? 0 : -1;
    if (k >= 0) *gen = g[k];
    return k;
}

// 1 when the text starts with "KEY=" for a key of the state record
static int state_text_key(const char *text)
{
    static const char *const other[] = {"safehold", "cause", "boot_pending", "last_sequence"};
    size_t n = strcspn(text, "=\n");
    if (text[n] != '=') return 0;
    for (int i = 0; i < FM_COUNT; i++)
        if (strlen(fm_names[i]) == n && !strncmp(text, fm_names[i], n)) return 1;
    for (int i = 0; i < 4; i++)
        if (strlen(other[i]) == n && !strncmp(text, other[i], n)) return 1;
    return 0;
}

int fm_state_load(const char *path, struct fm_state *s)
{
    char file[2 * FM_STATE_SLOT + 1], body[FM_STATE_SLOT - FM_STATE_HEAD + 1];
    size_t bytes;
    uint32_t gen;
    int k;
    FILE *f = fopen(path, "rb");
    memset(s, 0, sizeof(*s));
    if (!f) return 0;
    bytes = fread(file, 1, sizeof(file) - 1, f);
    fclose(f);
    file[bytes] = 0;
    if (!bytes) return 0;                              // created, never written: no record yet
    if ((k = state_newest(file, bytes, &gen)) >= 0) {  // a slot whose CRC holds, whatever the other one holds
        memcpy(body, file + (size_t)k * FM_STATE_SLOT + FM_STATE_HEAD, sizeof(body) - 1);
        body[sizeof(body) - 1] = 0;
        state_parse(body, s);
        return 0;
    }
    // the earlier text form starts with one of its keys; anything else is a two-slot file with both slots damaged (a
    // damaged first header must not pass for text: it would read as a fresh state)
    if (state_text_key(file)) {
        state_parse(file, s);
        return 0;
    }
    return -1;
}

int fm_state_save(const char *path, const struct fm_state *s)
{
    char file[2 * FM_STATE_SLOT], slot[FM_STATE_SLOT + 1], text[1024], *body = slot + FM_STATE_HEAD;
    size_t bytes = 0, room = FM_STATE_SLOT - FM_STATE_HEAD, len = 0;
    uint32_t gen = 0;
    int k = -1, target, ok;
    FILE *f;
    for (int i = 0; i < FM_COUNT; i++)          // 16 items of at most 20 bytes: well inside the buffer
        len += (size_t)snprintf(text + len, sizeof(text) - len, "%s=%u\n", fm_names[i], s->count[i]);
    len += (size_t)snprintf(text + len, sizeof(text) - len, "safehold=%u\ncause=%s\nboot_pending=%u\nlast_sequence=%llu\n",
                            s->safehold, s->cause[0] ? s->cause : "-", s->boot_pending, (unsigned long long)s->last_sequence);
    if (len >= room) return -1;
    memcpy(body, text, len);
    memset(body + len, '\n', room - len);
    if ((f = fopen(path, "r+b"))) bytes = fread(file, 1, sizeof(file), f);
    else if (!(f = fopen(path, "w+b"))) return -1;
    k = state_newest(file, bytes, &gen);   // only a slot whose CRC holds counts (a damaged slot 0 keeps slot 1's)
    target = k == 0;                   // the other slot than the newest record's (a new or damaged file: slot 0)
    gen = k >= 0 ? gen + 1 : 1;
    snprintf(slot, FM_STATE_HEAD + 1, "FMS2 gen=%010u crc=%08x", gen, fm_crc32((const uint8_t *)body, room));
    slot[FM_STATE_HEAD - 1] = '\n';
    ok = !fseek(f, (long)target * FM_STATE_SLOT, SEEK_SET) && fwrite(slot, 1, FM_STATE_SLOT, f) == FM_STATE_SLOT;
    if (ok && k < 0)                   // a new file: both slots, so the file has its final size from the start
        ok = fwrite(slot, 1, FM_STATE_SLOT, f) == FM_STATE_SLOT;
    ok = ok && !fflush(f) && !fsync(fileno(f));
    return fclose(f) || !ok ? -1 : 0;
}

int fm_count_recovery(struct fm_state *s, enum fm_id id, uint32_t limit)
{
    s->count[id]++;
    if (s->count[id] > limit) {
        s->safehold = 1;
        snprintf(s->cause, sizeof(s->cause), "%s", fm_names[id]);
        return 1;
    }
    return 0;
}

// slicing by 4 (four bytes a step, the word assembled byte by byte so any alignment and byte order work): the weight
// scrub and a hybrid model's window check read megabytes between batches and at every start
uint32_t fm_crc32_add(uint32_t crc, const uint8_t *data, size_t bytes)
{
    static uint32_t table[4][256];
    static int ready;
    if (!ready) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            table[0][n] = c;
        }
        for (uint32_t n = 0; n < 256; n++)
            for (int t = 1; t < 4; t++) table[t][n] = (table[t - 1][n] >> 8) ^ table[0][table[t - 1][n] & 0xFFu];
        ready = 1;
    }
    crc = ~crc;
    for (; bytes >= 4; data += 4, bytes -= 4) {
        crc ^= (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
        crc = table[3][crc & 0xFFu] ^ table[2][(crc >> 8) & 0xFFu] ^ table[1][(crc >> 16) & 0xFFu] ^ table[0][crc >> 24];
    }
    while (bytes--) crc = (crc >> 8) ^ table[0][(crc ^ *data++) & 0xFFu];
    return ~crc;
}

uint32_t fm_crc32(const uint8_t *data, size_t bytes)
{
    return fm_crc32_add(0, data, bytes);
}

const char *fm_check_batch(const struct fm_batch_header *h, const uint8_t *payload, size_t available,
                           uint32_t frame_bytes, uint32_t max_frames, uint64_t last_sequence)
{
    if (h->magic != FM_BATCH_MAGIC || h->version != 1) return "header";
    if (h->frames == 0 || h->frames > max_frames) return "frames";
    if ((uint64_t)h->frames * frame_bytes != h->payload_bytes || h->payload_bytes > available) return "length";
    if (h->sequence <= last_sequence) return "stale";
    if (fm_crc32(payload, h->payload_bytes) != h->crc32) return "crc";
    return 0;
}

static int64_t element(const uint8_t *p, uint32_t elem_bytes, uint32_t bits, int is_signed, int *extension_ok)
{
    uint64_t raw = 0;
    for (uint32_t i = 0; i < elem_bytes; i++) raw |= (uint64_t)p[i] << (8 * i);
    uint32_t container = 8 * elem_bytes;
    uint64_t value = bits < 64 ? raw & ((1ull << bits) - 1) : raw;
    int negative = is_signed && ((value >> (bits - 1)) & 1u);
    uint64_t upper = container > bits ? raw >> bits : 0, all = container > bits ? (1ull << (container - bits)) - 1 : 0;
    *extension_ok = upper == (negative ? all : 0);
    return negative ? (int64_t)(value | ~((1ull << bits) - 1)) : (int64_t)value;
}

int fm_check_result(const uint8_t *out, size_t bytes, uint32_t elements, uint32_t elem_bytes, uint32_t elem_bits,
                    int is_signed, int64_t lo, int64_t hi, uint32_t *bad_index)
{
    if ((uint64_t)elements * elem_bytes != bytes) { *bad_index = 0; return 1; }
    for (uint32_t i = 0; i < elements; i++) {
        int ok;
        int64_t v = element(out + (size_t)i * elem_bytes, elem_bytes, elem_bits, is_signed, &ok);
        if (!ok || v < lo || v > hi) { *bad_index = i; return 1; }
    }
    return 0;
}

void fm_fill_safe(uint8_t *out, uint32_t elements, uint32_t elem_bytes, int64_t safe)
{
    for (uint32_t i = 0; i < elements; i++)
        for (uint32_t b = 0; b < elem_bytes; b++) out[(size_t)i * elem_bytes + b] = (uint8_t)((uint64_t)safe >> (8 * b));
}

void fm_parity_read(fm_read32 read, void *ctx, struct fm_parity *p)
{
    p->l2_aux = read(ctx, FM_L2C_BASE + FM_L2C_AUX_CTRL);
    p->l2_raw = read(ctx, FM_L2C_BASE + FM_L2C_RAW_INTR);
    p->ocm_ctrl = read(ctx, FM_OCM_BASE + FM_OCM_PARITY_CTRL);
    p->ocm_status = read(ctx, FM_OCM_BASE + FM_OCM_IRQ_STS);
    p->ocm_address = read(ctx, FM_OCM_BASE + FM_OCM_ERR_ADDRESS);
}

unsigned fm_parity_enabled(const struct fm_parity *p)
{
    return ((p->l2_aux & FM_L2C_AUX_PARITY) ? FM_PARITY_L2 : 0u)
         | ((p->ocm_ctrl & 0x1Fu) == FM_OCM_PARITY_ENABLE ? FM_PARITY_OCM : 0u);
}

unsigned fm_parity_errors(const struct fm_parity *now, const struct fm_parity *last)
{
    unsigned errors = (now->l2_raw & (FM_L2C_PARRT | FM_L2C_PARRD)) ? FM_PARITY_L2 : 0u;
    uint32_t status = now->ocm_status & FM_OCM_IRQ_ERRORS;
    if (now->ocm_irqs > last->ocm_irqs
        || (status && (status != (last->ocm_status & FM_OCM_IRQ_ERRORS) || now->ocm_address != last->ocm_address)))
        errors |= FM_PARITY_OCM;
    return errors;
}

int fm_parity_text(char *buf, size_t size, const struct fm_parity *p)
{
    return snprintf(buf, size, "l2_aux=0x%08x l2_raw=0x%08x ocm_ctrl=0x%08x ocm_status=0x%08x ocm_address=0x%08x "
                    "ocm_irqs=%llu", p->l2_aux, p->l2_raw, p->ocm_ctrl, p->ocm_status, p->ocm_address,
                    (unsigned long long)p->ocm_irqs);
}

uint64_t fm_irq_count(const char *interrupts, const char *name)
{
    size_t length = strlen(name);
    for (const char *line = interrupts; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : 0) {
        const char *end = strchr(line, '\n'), *p;
        size_t n = end ? (size_t)(end - line) : strlen(line);
        while (n && (line[n - 1] == ' ' || line[n - 1] == '\r')) n--;
        if (n < length || memcmp(line + n - length, name, length) || (n > length && line[n - length - 1] != ' '))
            continue;
        p = strchr(line, ':');
        if (!p || p > line + n) return 0;
        uint64_t total = 0;
        for (p++;;) {
            char *next;
            while (*p == ' ') p++;
            if (*p < '0' || *p > '9') break;
            total += strtoull(p, &next, 10);
            p = next;
        }
        return total;
    }
    return 0;
}

int fm_event_json(char *buf, size_t size, uint64_t t_ns, long batch, const char *event, const char *item,
                  const char *detail, const struct fm_state *s)
{
    int n = snprintf(buf, size, "{\"t_ns\":%llu,\"batch\":%ld,\"event\":\"%s\",\"item\":\"%s\",\"detail\":\"%s\",\"counts\":{",
                     (unsigned long long)t_ns, batch, event, item ? item : "", detail ? detail : "");
    for (int i = 0; i < FM_COUNT && n > 0 && (size_t)n < size; i++)
        n += snprintf(buf + n, size - (size_t)n, "%s\"%s\":%u", i ? "," : "", fm_names[i], s->count[i]);
    if (n > 0 && (size_t)n < size)
        n += snprintf(buf + n, size - (size_t)n, "},\"safehold\":%u}", s->safehold);
    return n;
}

uint32_t fm_tz_ddr_mask(uint64_t address, uint64_t bytes)
{
    uint32_t mask = 0;
    if (!bytes) return 0;
    for (uint64_t s = address / FM_TZ_SECTION_BYTES; s <= (address + bytes - 1) / FM_TZ_SECTION_BYTES && s < FM_TZ_SECTIONS; s++)
        mask |= 1u << s;
    return mask;
}
