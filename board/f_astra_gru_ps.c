// F-ASTRA SIREN Model 3 integer GRU predictor on the PS (TE0715 Cortex-A9, plain C99): gru_predictor.Integer.step
// byte for byte, the whole predictor without the PL. Per step over the vector [x codes (NX), h codes (H)]: the 6H gate
// accumulators (int8 gate matrix, int32), the requantization A = rne((acc * multiplier + offset) / 2^shift) (int64,
// round half to even, floor shifts), the sigmoid gate codes r, z (count of thresholds <= A_x + A_h), the candidate
// n (tanh thresholds on A_nx + rne(r * A_nh / 2^gate_bits)), h' = clip(n + rne(z * (h - n) / 2^gate_bits)), the
// output accumulators (int8 output layer, int32) and the codebook indices (count of index thresholds <= accumulator).
// The parameters come from gru_ps.export_params (f_astra/gru_ps.py documents the file; load_params reads it).
//
//   f-astra-gru-ps PARAMS XCODES STEPS OUT_PREFIX [EXPECTED] [SEGMENT_STEPS]
//
// XCODES: int16 LE STEPS x NX with NX = C + 6 (the x codes of every step; a delta contract's D delta inputs are then
// derived here as Integer.inputs does: x(t) - x(t-1) of the first D codes, 0 at the first step, not reset with the
// hidden state) or STEPS x (C + 6 + D) prepared input codes taken verbatim. A context contract (header flag 1, 11/03:
// the D extra inputs are the day's moving averages, gru_predictor.context_codes) takes prepared codes only. The run starts from the zero hidden state;
// with SEGMENT_STEPS > 0 the hidden state is zero again at every multiple of it (gru_predictor.segmented). Writes
// OUT_PREFIX-output.bin (int16 LE [STEPS][H + C]: h' then the indices, the FINN tail's and the sequence runner's
// format) and OUT_PREFIX-index.i32 (int32 LE [STEPS][C]). EXPECTED ("-" for none): the same [STEPS][H + C] as int16
// (the sequence runner's expected file) or as int32 (the G4 corpus's expected-grug4.i32); the run goes on through a
// mismatch. Prints one JSON line (steps, seconds of the step loop by CLOCK_MONOTONIC_RAW, microseconds per step,
// mismatched steps). Exit 0 all steps equal (or nothing to compare), 2 setup failure, 4 mismatch.
//
// Integer semantics replicated from the Python reference: numpy's >> on int64 is a floor shift, so the requantization
// uses the arithmetic right shift (checked at start) and the remainder as the low bits; the round-half-to-even test
// is r > half or (r == half and the floored quotient odd); np.searchsorted(side="right") on the sorted tables is the
// count of thresholds <= value (a binary search here); np.clip bounds h' to [-2^(h_bits-1), 2^(h_bits-1) - 1].
// Every accumulator fits int32 and every requantized product int64 for codes in the declared ranges (the export
// checks the bounds, this tool rechecks them and the input codes' ranges). The gate products run on NEON (int16 x
// int16 -> int32, vmlal) when __ARM_NEON is defined and F_ASTRA_GRU_PS_PLAIN is not; the plain loop is the reference
// and the NEON path is checked against it on random vectors at start.
// Build: cc -std=gnu99 -O2 -Wall -Wextra -Werror f_astra_gru_ps.c (host test); the board binary comes from
// directrtl_board._compile_c (arm-linux-gnueabihf-gcc -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard).
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__ARM_NEON) && !defined(F_ASTRA_GRU_PS_PLAIN)
#include <arm_neon.h>
#define GRU_PS_NEON 1
#else
#define GRU_PS_NEON 0
#endif

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#define TIMER_NAME "CLOCK_MONOTONIC"
#else
#define TIMER_NAME "CLOCK_MONOTONIC_RAW"
#endif

#define MAGIC "FAGRUPS1"
#define HEADER_BYTES 88
#define AUXILIARY 6
#define BLOCK 8                                  /* columns per product block (one NEON int16x8 load) */
#define SELF_CHECK_VECTORS 8

struct params {
    uint32_t c, hd, nx, nd, x_bits, h_bits, gate_bits, fixed_bits, n_sig, n_tanh, n_index, flags;
    uint8_t sha[32];
    uint32_t k, kp, hp;                          /* gate columns NX + H, padded to BLOCK; H padded to BLOCK */
    int16_t *gates;                              /* [6H][kp], the int8 rows widened, zero padded */
    uint32_t *first, *blocks;                    /* per gate row: first block and block count holding nonzero weights */
    int32_t *mul, *shift;
    int64_t *add, *sig, *tanh_thr, *index_thr;
    uint32_t *index_count, *index_first;
    int16_t *out_w;                              /* [C][hp], widened, zero padded */
};

struct blob { const uint8_t *p; size_t n, at; };

static void fail(const char *what)
{
    fprintf(stderr, "F_ASTRA_GRU_PS_FAILED %s errno=%d\n", what, errno);
    exit(2);
}

static void *xmalloc(size_t bytes)
{
    void *p = calloc(bytes ? bytes : 1, 1);
    if (!p) fail("out of memory");
    return p;
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *data;
    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) fail(path);
    data = xmalloc((size_t)n);
    if (n && fread(data, 1, (size_t)n, f) != (size_t)n) fail(path);
    fclose(f);
    *size = (size_t)n;
    return data;
}

static const uint8_t *take(struct blob *b, size_t bytes, const char *what)
{
    const uint8_t *r = b->p + b->at;
    if (bytes > b->n - b->at) { errno = EINVAL; fail(what); }
    b->at += bytes;
    return r;
}

static uint32_t u32_at(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void *copy_of(struct blob *b, size_t bytes, const char *what)
{
    void *out = xmalloc(bytes);
    memcpy(out, take(b, bytes, what), bytes);
    return out;
}

/* ---- integer helpers (the reference's semantics) */

/* Round half to even of v / 2^s (s >= 1): numpy's floor shift, remainder and parity test. */
static inline int64_t rne_shift(int64_t v, int s)
{
    int64_t q = v >> s;
    uint64_t r = (uint64_t)v & ((1ull << s) - 1), half = 1ull << (s - 1);
    return q + (r > half || (r == half && (q & 1)));
}

/* Count of thresholds <= key in a sorted table: np.searchsorted(table, key, side="right"). */
static inline uint32_t upper_bound(const int64_t *a, uint32_t n, int64_t key)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (a[mid] <= key) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* ---- products: int16 weights (widened int8) x int16 codes -> int32, over whole blocks of BLOCK columns */

static int32_t dot_plain(const int16_t *w, const int16_t *x, uint32_t blocks)
{
    uint32_t i, n = blocks * BLOCK;
    int32_t s = 0;
    for (i = 0; i < n; i++) s += (int32_t)w[i] * (int32_t)x[i];
    return s;
}

#if GRU_PS_NEON
static int32_t dot_neon(const int16_t *w, const int16_t *x, uint32_t blocks)
{
    int32x4_t s0 = vdupq_n_s32(0), s1 = vdupq_n_s32(0);
    int32x2_t t;
    uint32_t i;
    for (i = 0; i + 1 < blocks; i += 2) {
        int16x8_t a0 = vld1q_s16(w + BLOCK * i), b0 = vld1q_s16(x + BLOCK * i);
        int16x8_t a1 = vld1q_s16(w + BLOCK * (i + 1)), b1 = vld1q_s16(x + BLOCK * (i + 1));
        s0 = vmlal_s16(s0, vget_low_s16(a0), vget_low_s16(b0));
        s1 = vmlal_s16(s1, vget_low_s16(a1), vget_low_s16(b1));
        s0 = vmlal_s16(s0, vget_high_s16(a0), vget_high_s16(b0));
        s1 = vmlal_s16(s1, vget_high_s16(a1), vget_high_s16(b1));
    }
    if (i < blocks) {
        int16x8_t a0 = vld1q_s16(w + BLOCK * i), b0 = vld1q_s16(x + BLOCK * i);
        s0 = vmlal_s16(s0, vget_low_s16(a0), vget_low_s16(b0));
        s0 = vmlal_s16(s0, vget_high_s16(a0), vget_high_s16(b0));
    }
    s0 = vaddq_s32(s0, s1);
    t = vpadd_s32(vget_low_s32(s0), vget_high_s32(s0));
    return vget_lane_s32(t, 0) + vget_lane_s32(t, 1);
}
#define dot_fast dot_neon
#define PATH_NAME "neon"
#else
#define dot_fast dot_plain
#define PATH_NAME "plain"
#endif

static void gate_products(const struct params *p, const int16_t *v, int32_t *acc, int fast)
{
    uint32_t r, rows = 6 * p->hd;
    for (r = 0; r < rows; r++) {
        const int16_t *w = p->gates + (size_t)r * p->kp + (size_t)p->first[r] * BLOCK;
        const int16_t *x = v + (size_t)p->first[r] * BLOCK;
        acc[r] = fast ? dot_fast(w, x, p->blocks[r]) : dot_plain(w, x, p->blocks[r]);
    }
}

/* ---- the parameter file */

static void load_params(const char *path, struct params *p)
{
    size_t size;
    uint8_t *data = load(path, &size);
    struct blob b = { data, size, 0 };
    const uint8_t *head = take(&b, HEADER_BYTES, "parameter header");
    uint32_t rows, r, i, limit;
    int8_t *gates8, *out8;
    if (memcmp(head, MAGIC, 8)) { errno = EINVAL; fail("parameter magic"); }
    p->c = u32_at(head + 8); p->hd = u32_at(head + 12); p->nx = u32_at(head + 16); p->nd = u32_at(head + 20);
    p->x_bits = u32_at(head + 24); p->h_bits = u32_at(head + 28); p->gate_bits = u32_at(head + 32);
    p->fixed_bits = u32_at(head + 36); p->n_sig = u32_at(head + 40); p->n_tanh = u32_at(head + 44);
    p->n_index = u32_at(head + 48);
    p->flags = u32_at(head + 52);                /* bit 0: the extra inputs are context codes (prepared only) */
    memcpy(p->sha, head + 56, 32);
    if (p->flags > 1 || (p->flags && !p->nd)) { errno = EINVAL; fail("parameter header flags"); }
    if (!p->c || !p->hd || p->c > 4096 || p->hd > 4096 || p->nx != p->c + AUXILIARY + p->nd || p->nd > p->c + AUXILIARY
        || p->x_bits < 1 || p->x_bits > 8 || p->h_bits < 8 || p->h_bits > 16 || p->gate_bits < 6 || p->gate_bits > 15
        || p->n_sig >= (1u << p->gate_bits) || p->n_tanh >= (1u << p->h_bits) || p->n_sig == 0 || p->n_tanh == 0) {
        errno = EINVAL;
        fail("parameter header values");
    }
    rows = 6 * p->hd;
    p->k = p->nx + p->hd;
    p->kp = (p->k + BLOCK - 1) / BLOCK * BLOCK;
    p->hp = (p->hd + BLOCK - 1) / BLOCK * BLOCK;
    gates8 = copy_of(&b, (size_t)rows * p->k, "gate weights");
    p->mul = copy_of(&b, (size_t)rows * 4, "multipliers");
    p->add = copy_of(&b, (size_t)rows * 8, "offsets");
    p->shift = copy_of(&b, (size_t)rows * 4, "shifts");
    p->sig = copy_of(&b, (size_t)p->n_sig * 8, "sigmoid thresholds");
    p->tanh_thr = copy_of(&b, (size_t)p->n_tanh * 8, "tanh thresholds");
    out8 = copy_of(&b, (size_t)p->c * p->hd, "output weights");
    p->index_count = copy_of(&b, (size_t)p->c * 4, "index counts");
    p->index_thr = copy_of(&b, (size_t)p->n_index * 8, "index thresholds");
    if (b.at != b.n) { errno = EINVAL; fail("parameter file length"); }
    p->index_first = xmalloc((size_t)p->c * 4);
    for (i = 0, limit = 0; i < p->c; i++) {
        p->index_first[i] = limit;
        if (p->index_count[i] > p->n_index - limit) { errno = EINVAL; fail("index counts"); }
        limit += p->index_count[i];
    }
    if (limit != p->n_index) { errno = EINVAL; fail("index counts"); }
    /* widened rows, their nonzero block ranges, the sortedness of the tables and the int32 / int64 bounds */
    p->gates = xmalloc((size_t)rows * p->kp * 2);
    p->first = xmalloc((size_t)rows * 4);
    p->blocks = xmalloc((size_t)rows * 4);
    for (r = 0; r < rows; r++) {
        uint32_t lo = p->k, hi = 0;
        int64_t bound = 0, requant;
        for (i = 0; i < p->k; i++) {
            int16_t w = gates8[(size_t)r * p->k + i];
            int64_t code_max = i < p->nx - p->nd ? (int64_t)1 << (p->x_bits - 1)
                               : i < p->nx ? ((int64_t)1 << p->x_bits) - 1 : (int64_t)1 << (p->h_bits - 1);
            p->gates[(size_t)r * p->kp + i] = w;
            if (w) { if (i < lo) lo = i; hi = i + 1; }
            bound += (w < 0 ? -(int64_t)w : (int64_t)w) * code_max;
        }
        p->first[r] = lo < hi ? lo / BLOCK : 0;
        p->blocks[r] = lo < hi ? (hi + BLOCK - 1) / BLOCK - p->first[r] : 0;
        if (p->shift[r] < 1 || p->shift[r] > 62 || p->mul[r] < 1) { errno = EINVAL; fail("requant constants"); }
        requant = (bound * p->mul[r] + (p->add[r] < 0 ? -p->add[r] : p->add[r]));
        if (bound >= ((int64_t)1 << 31) || requant >= ((int64_t)1 << 62)
            || ((requant >> p->shift[r]) + 1) * ((int64_t)p->n_sig + 1) >= ((int64_t)1 << 62)) {
            errno = ERANGE;
            fail("gate accumulator bounds");
        }
    }
    for (i = 1; i < p->n_sig; i++) if (p->sig[i] < p->sig[i - 1]) { errno = EINVAL; fail("sigmoid table order"); }
    for (i = 1; i < p->n_tanh; i++) if (p->tanh_thr[i] < p->tanh_thr[i - 1]) { errno = EINVAL; fail("tanh table order"); }
    p->out_w = xmalloc((size_t)p->c * p->hp * 2);
    for (r = 0; r < p->c; r++) {
        int64_t bound = 0;
        for (i = 0; i < p->hd; i++) {
            int16_t w = out8[(size_t)r * p->hd + i];
            p->out_w[(size_t)r * p->hp + i] = w;
            bound += (w < 0 ? -(int64_t)w : (int64_t)w) << (p->h_bits - 1);
        }
        if (bound >= ((int64_t)1 << 31)) { errno = ERANGE; fail("output accumulator bounds"); }
        for (i = 1; i < p->index_count[r]; i++)
            if (p->index_thr[p->index_first[r] + i] < p->index_thr[p->index_first[r] + i - 1]) { errno = EINVAL; fail("index table order"); }
    }
    free(gates8);
    free(out8);
    free(data);
}

/* ---- one step: v = [x codes, h codes] (kp, zero padded) -> h' (hd) and the indices (c) */

static void step(const struct params *p, const int16_t *v, const int16_t *h, int32_t *acc, int64_t *a, int16_t *hv,
                 int32_t *index, int fast)
{
    uint32_t hd = p->hd, r, j;
    int64_t hh = (int64_t)1 << (p->h_bits - 1);
    gate_products(p, v, acc, fast);
    for (r = 0; r < 6 * hd; r++) a[r] = rne_shift((int64_t)acc[r] * p->mul[r] + p->add[r], p->shift[r]);
    for (j = 0; j < hd; j++) {
        int64_t g_r = upper_bound(p->sig, p->n_sig, a[j] + a[3 * hd + j]);
        int64_t g_z = upper_bound(p->sig, p->n_sig, a[hd + j] + a[4 * hd + j]);
        int64_t pre = a[2 * hd + j] + rne_shift(g_r * a[5 * hd + j], (int)p->gate_bits);
        int64_t n = (int64_t)upper_bound(p->tanh_thr, p->n_tanh, pre) - hh;
        int64_t next = n + rne_shift(g_z * ((int64_t)h[j] - n), (int)p->gate_bits);
        if (next < -hh) next = -hh;
        if (next > hh - 1) next = hh - 1;
        hv[j] = (int16_t)next;
    }
    for (r = 0; r < p->c; r++) {
        int32_t acc_o = fast ? dot_fast(p->out_w + (size_t)r * p->hp, hv, p->hp / BLOCK)
                             : dot_plain(p->out_w + (size_t)r * p->hp, hv, p->hp / BLOCK);
        index[r] = (int32_t)upper_bound(p->index_thr + p->index_first[r], p->index_count[r], acc_o);
    }
}

/* The fast products against the plain loop on random vectors within the code ranges (identical by construction:
 * integer sums of the same products in another order, no overflow by the bounds). */
static void self_check(const struct params *p)
{
    uint32_t rows = 6 * p->hd, i, r, n;
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    int16_t *v = xmalloc((size_t)p->kp * 2);
    int32_t *fast = xmalloc((size_t)rows * 4), *plain = xmalloc((size_t)rows * 4);
    for (n = 0; n < SELF_CHECK_VECTORS; n++) {
        for (i = 0; i < p->k; i++) {
            int64_t span = i < p->nx - p->nd ? (int64_t)1 << p->x_bits : i < p->nx ? ((int64_t)2 << p->x_bits) - 1 : (int64_t)1 << p->h_bits;
            int64_t low = i < p->nx - p->nd ? -((int64_t)1 << (p->x_bits - 1)) : i < p->nx ? 1 - ((int64_t)1 << p->x_bits)
                          : -((int64_t)1 << (p->h_bits - 1));
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            v[i] = (int16_t)(low + (int64_t)((seed >> 33) % (uint64_t)span));
        }
        gate_products(p, v, fast, 1);
        gate_products(p, v, plain, 0);
        for (r = 0; r < rows; r++)
            if (fast[r] != plain[r]) { errno = EIO; fail("fast products differ from the plain loop"); }
    }
    free(v);
    free(fast);
    free(plain);
}

int main(int argc, char **argv)
{
    struct params p;
    const char *params_path, *x_path, *prefix, *expected_path = NULL;
    unsigned long steps, segment = 0, t, mismatched = 0;
    long first_mismatch = -1;
    size_t x_bytes, expected_bytes = 0, k;
    uint8_t *xcodes, *expected = NULL;
    int expected_wide = 0, derive_delta, fast = GRU_PS_NEON;
    uint32_t columns, out_n, i;
    int16_t *v, *h, *hv, *out;
    int32_t *acc, *index;
    int64_t *a, x_lo, x_hi, d_lim;
    uint64_t start, elapsed;
    FILE *output_file, *index_file;
    char name[1024];
    volatile int64_t minus8 = -8, minus1 = -1;
    union { uint32_t u; uint8_t b[4]; } endian = { 1 };

    if (argc < 5 || argc > 7) {
        fprintf(stderr, "usage: %s PARAMS XCODES STEPS OUT_PREFIX [EXPECTED|-] [SEGMENT_STEPS]\n", argv[0]);
        return 64;
    }
    params_path = argv[1]; x_path = argv[2]; prefix = argv[4];
    steps = strtoul(argv[3], NULL, 10);
    if (argc >= 6 && strcmp(argv[5], "-")) expected_path = argv[5];
    if (argc == 7) segment = strtoul(argv[6], NULL, 10);
    if (!steps || endian.b[0] != 1 || (minus8 >> 1) != -4 || (minus1 >> 3) != -1 || sizeof(long) < 4) {
        errno = EINVAL;
        fail("steps, little-endian host and arithmetic right shift required");
    }
    memset(&p, 0, sizeof(p));
    load_params(params_path, &p);
    out_n = p.hd + p.c;
    xcodes = load(x_path, &x_bytes);
    if (x_bytes == (size_t)steps * p.nx * 2) {
        columns = p.nx;
        derive_delta = 0;
    } else if (p.nd && !p.flags && x_bytes == (size_t)steps * (p.nx - p.nd) * 2) {
        columns = p.nx - p.nd;
        derive_delta = 1;
    } else {
        errno = EINVAL;
        fail("x codes size: STEPS x (C + 6) or STEPS x (C + 6 + D) int16");
    }
    if (expected_path) {
        expected = load(expected_path, &expected_bytes);
        if (expected_bytes == (size_t)steps * out_n * 4) expected_wide = 1;
        else if (expected_bytes != (size_t)steps * out_n * 2) { errno = EINVAL; fail("expected size: STEPS x (H + C) int16 or int32"); }
    }
    x_lo = -((int64_t)1 << (p.x_bits - 1));
    x_hi = ((int64_t)1 << (p.x_bits - 1)) - 1;
    d_lim = ((int64_t)1 << p.x_bits) - 1;
    for (t = 0; t < steps; t++) {
        const int16_t *x = (const int16_t *)(xcodes + (size_t)t * columns * 2);
        for (i = 0; i < columns; i++) {
            int64_t code = x[i];
            if (i < p.nx - p.nd ? (code < x_lo || code > x_hi) : (code < -d_lim || code > d_lim)) {
                fprintf(stderr, "F_ASTRA_GRU_PS_FAILED x code out of range step=%lu column=%u value=%lld errno=0\n",
                        t, (unsigned)i, (long long)code);
                return 2;
            }
        }
    }
    if (fast) self_check(&p);

    v = xmalloc((size_t)p.kp * 2);
    h = xmalloc((size_t)p.hd * 2);
    hv = xmalloc((size_t)p.hp * 2);
    out = xmalloc((size_t)out_n * 2);
    acc = xmalloc((size_t)6 * p.hd * 4);
    a = xmalloc((size_t)6 * p.hd * 8);
    index = xmalloc((size_t)p.c * 4);
    snprintf(name, sizeof(name), "%s-output.bin", prefix);
    output_file = fopen(name, "wb");
    snprintf(name, sizeof(name), "%s-index.i32", prefix);
    index_file = fopen(name, "wb");
    if (!output_file || !index_file) fail("output files");

    start = now_ns();
    for (t = 0; t < steps; t++) {
        const int16_t *x = (const int16_t *)(xcodes + (size_t)t * columns * 2);
        if (segment && t % segment == 0) memset(h, 0, (size_t)p.hd * 2);     /* the zero state */
        memcpy(v, x, (size_t)columns * 2);
        if (derive_delta) {
            const int16_t *prev = t ? (const int16_t *)(xcodes + (size_t)(t - 1) * columns * 2) : NULL;
            for (i = 0; i < p.nd; i++) v[columns + i] = (int16_t)(prev ? x[i] - prev[i] : 0);
        }
        memcpy(v + p.nx, h, (size_t)p.hd * 2);
        step(&p, v, h, acc, a, hv, index, fast);
        memcpy(h, hv, (size_t)p.hd * 2);                                    /* fed back into the next step */
        memcpy(out, hv, (size_t)p.hd * 2);
        for (k = 0; k < p.c; k++) out[p.hd + k] = (int16_t)index[k];
        if (expected) {
            int differs = 0;
            if (expected_wide) {
                const int32_t *e = (const int32_t *)(expected + (size_t)t * out_n * 4);
                for (k = 0; k < out_n && !differs; k++) differs = e[k] != (int32_t)out[k];
            } else {
                differs = memcmp(out, expected + (size_t)t * out_n * 2, (size_t)out_n * 2) != 0;
            }
            if (differs) {
                if (first_mismatch < 0) first_mismatch = (long)t;
                mismatched++;
            }
        }
        if (fwrite(out, 2, out_n, output_file) != out_n) fail("output write");
        if (fwrite(index, 4, p.c, index_file) != p.c) fail("index write");
    }
    elapsed = now_ns() - start;
    if (fclose(output_file) || fclose(index_file)) fail("output close");

    printf("{\"schema\":\"f-astra/gru-ps-run/v1\",\"steps\":%lu,\"segment_steps\":%lu,\"channels\":%u,\"hidden\":%u,"
           "\"inputs\":%u,\"delta_inputs\":%u,\"context_inputs\":%u,\"derived_delta\":%d,\"path\":\"%s\",\"seconds\":%.6f,"
           "\"us_per_step\":%.3f,\"compared\":%d,\"mismatched_steps\":%lu,\"first_mismatch\":%ld,\"contract_sha256\":\"",
           steps, segment, (unsigned)p.c, (unsigned)p.hd, (unsigned)p.nx, p.flags ? 0u : (unsigned)p.nd,
           p.flags ? (unsigned)p.nd : 0u, derive_delta, PATH_NAME,
           (double)elapsed / 1e9, (double)elapsed / 1e3 / (double)steps, expected != NULL, mismatched, first_mismatch);
    for (i = 0; i < 32; i++) printf("%02x", p.sha[i]);
    printf("\",\"timer\":\"%s over the step loop (file writes included)\"}\n", TIMER_NAME);
    return mismatched ? 4 : 0;
}
