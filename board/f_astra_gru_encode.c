/* F-ASTRA SIREN Model 3 lossless encoder (PS side, C): the packet of gru_codec.encode, byte for byte.
 *
 * For counts q [T, C] (int32) with next-step predictions [T - 1, C] it compresses, per block of block_samples steps
 * and per channel, the counts (raw), the temporal deltas (delta, ZigZag) and the prediction residuals (gru, ZigZag)
 * with BZ2 (libbz2, BZ2_bzBuffToBuffCompress) and keeps the smallest (payload + its JSON record; ties raw -> delta ->
 * gru), then writes MAGIC, the header length, the JSON header (keys in gru_codec's order, separators "," and ":",
 * ASCII), the BZ2 auxiliary inputs aux[0 .. T - 2] and the chosen payloads. The predictions come from the GRU tail's
 * codebook indices on the board (--index and --codebooks: prediction = codebook[channel][index]) or directly
 * (--pred: for comparisons, and Comp's ModernTCN predictions, --predictor f-astra-comp-moderntcn-int8/v1, made on the
 * PS from the hybrid's outputs by f-astra-pd-zt mtcnpred). The ground decoder (gru_codec.decode or
 * directrtl_comp_mtcn.decode) runs the same integer predictor and restores q exactly.
 *
 *   f-astra-gru-encode --q Q.i32 --aux AUX.f32 (--pred PRED.i32 | --index IDX.i32 --codebooks CB.bin)
 *       --samples T --channels C --channels-json JSON --scale S --model-sha256 HEX [--block B] [--level L]
 *       --out PACKET [--metrics METRICS.json] [--segment-steps L | --window-steps L] [--predictor NAME]
 *   (two stages, the same packet: --stage pre --candidates CAND without the predictions, --out or the codec header
 *    options, in the daily window; --stage final --candidates CAND without --aux in the NN window)
 *   --threads N (1..8, default 1): the BZ2 payloads of the blocks and channels (and the auxiliary inputs) on N threads;
 *    the choice and the output order stay the one-thread ones, so every stage writes the same bytes for any N
 *   --format 2 --aux-thresholds THR.f32 (gru_codec version 2, user 11/03: the weekly Comp packets below 350 KB): the
 *    binary header (gru_codec._header_v2), the auxiliary inputs as the predictor's threshold counts (per input the
 *    first count, then the steps' differences modulo 256, BZ2; THR.f32: 6 x K float32 LE, K <= 255, the contract's
 *    aux_thresholds) and the smallest payload per block and channel (ties raw -> delta -> gru); --channels-json is
 *    not needed. Both stages take the same --format; the final stage reads the counts from the candidates.
 *   --format 3 --aux-thresholds THR.f32 (gru_codec version 3, 11/04: short packets such as one Comp run every few
 *    orbits): no BZ2; byte-aligned Golomb-Rice segments (gru_codec.rice_segment: a byte mode << 5 | k, the codes
 *    MSB-first, k 0..30 the fewest bits, ties the smaller, RICE_ESCAPE ones then 32 bits of u for a large quotient):
 *    one per auxiliary input (zigzag of its counts' first value and differences), then per block and channel the
 *    smallest of raw / delta / gru; the header is version 2's without records (payload bytes and CRC-32 instead).
 *   --format 4 --eps-n N --minute0 M --window-steps L --aux AUX.f32 --aux-thresholds THR.f32 (gru_codec version 4,
 *    11/06: near-lossless Comp, one stage): the counts as bins with |q_hat - q| <= sqrt(q) / N (N 0: lossless; the
 *    table in integers, gru_codec.bin_starts), the predictions (--pred or --index with --codebooks, made over q_hat)
 *    mapped to bins, per block of 1800 steps and channel the smallest of raw / delta / gru and the two neighbour
 *    modes (int8 coefficients, gru_codec._v4_coefs), a 17-byte fixed header + the first row's varints + the payload
 *    length + CRC-32 of the payload + CRC-32 of (auxiliary threshold counts | bin indices); no auxiliary payload (the
 *    decoder makes the inputs from minute M). --qhat-out QHAT.i32 writes q_hat (the predictor's input counts).
 *   --lattice ksem (with --format 4 and --eps-n 1..63, 11/07; --lattice none is the default): the lattice-aligned bins
 *    of gru_codec.bin_starts(eps_n, "ksem") (KSEM minute sums are multiples of 16 from 6,400 and of 64 from 25,600 on;
 *    where the step is at least the bin width each lattice value has a bin that stands for itself), the header flag
 *    0x40 and the flags byte first in the end-to-end CRC; the predictions are then made over that q_hat.
 *
 * Q.i32: T x C int32 LE; AUX.f32: T x 6 float32 LE; PRED.i32 and IDX.i32: (T - 1) x C int32 LE; CB.bin: per channel
 * a uint32 LE count and that many int32 LE values (gru_codec.codebook_bytes). JSON: the channel names as
 * json.dumps(..., separators=(",", ":"), ensure_ascii=True) writes them (gru_codec passes it verbatim).
 * Build: cc -std=gnu99 -O2 f_astra_gru_encode.c -l:libbz2.so.1 -lpthread (or -lbz2); with bzlib.h define
 * F_ASTRA_HAVE_BZLIB.
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef F_ASTRA_HAVE_BZLIB
#include <bzlib.h>
#else
/* libbz2's one-shot compressor (bzlib.h, stable since 1.0): declared here so the encoder builds where the library is
 * installed without its header (the Vitis ARM sysroot, a distribution without the -dev package) */
int BZ2_bzBuffToBuffCompress(char *dest, unsigned int *destLen, char *source, unsigned int sourceLen,
                             int blockSize100k, int verbosity, int workFactor);
#define BZ_OK 0
#endif

static const char MAGIC[8] = {'S', 'C', 'B', 'Z', '2', 1, '\r', '\n'};
static const char MAGIC2[8] = {'S', 'C', 'B', 'Z', '2', 2, '\r', '\n'};
static const char MAGIC3[8] = {'S', 'C', 'R', 'I', 'C', 3, '\r', '\n'};
#define RICE_ESCAPE 32u               /* gru_codec.RICE_ESCAPE */
static const char *PREDICTOR = "f-astra-integer-gru/v1";

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("f-astra-gru-encode: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

/* ---- growing byte buffer */
typedef struct { unsigned char *p; size_t n, cap; } buf_t;

static void put(buf_t *b, const void *data, size_t n)
{
    if (!n)            /* nothing to copy (an empty buffer has no storage yet: memcpy must not see its null pointer) */
        return;
    if (!b->p || b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n)
            cap *= 2;
        unsigned char *p = realloc(b->p, cap);
        if (!p)
            die("out of memory");
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

static void putf(buf_t *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp)
        die("format overflow");
    put(b, tmp, (size_t)n);
}

/* ---- zlib's CRC-32 (zlib.crc32) */
static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32_update(uint32_t c, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static uint32_t crc32_of(const unsigned char *p, size_t n)
{
    return crc32_update(0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
}

/* ---- SHA-256 (FIPS 180-4) */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void sha256_hex(const unsigned char *p, size_t n, char out[65])
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t full = n / 64 * 64;
    for (size_t i = 0; i < full; i += 64)
        sha256_block(h, p + i);
    unsigned char tail[128] = {0};
    size_t rest = n - full;
    memcpy(tail, p + full, rest);
    tail[rest] = 0x80;
    size_t len = rest + 1 + 8 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++)
        tail[len - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t i = 0; i < len; i += 64)
        sha256_block(h, tail + i);
    for (int i = 0; i < 8; i++)
        sprintf(out + 8 * i, "%08x", h[i]);
}

/* ---- inputs */
static unsigned char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("%s: %s", path, strerror(errno));
    buf_t b = {0};
    unsigned char tmp[65536];
    size_t got;
    while ((got = fread(tmp, 1, sizeof tmp, f)) > 0)
        put(&b, tmp, got);
    fclose(f);
    *n = b.n;
    return b.p;
}

static int32_t *read_i32(const char *path, size_t count)
{
    size_t n;
    unsigned char *p = read_file(path, &n);
    if (n != count * 4 || (count && !p))
        die("%s: %zu bytes, expected %zu", path, n, count * 4);
    int32_t *v = malloc(count * sizeof *v);
    if (!v && count)
        die("out of memory");
    for (size_t i = 0; i < count; i++)
        v[i] = (int32_t)((uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 |
                         (uint32_t)p[4 * i + 3] << 24);
    free(p);
    return v;
}

static void le32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static uint32_t zigzag(int64_t v)
{
    return (uint32_t)(((uint64_t)v << 1) ^ (uint64_t)(v >> 63));
}

static int32_t as_i32(int64_t v, const char *what)
{
    if (v < INT32_MIN || v > INT32_MAX)
        die("%s exceeds int32", what);
    return (int32_t)v;
}

/* 64 hex characters as 32 bytes (the version 2 header's hashes) */
static void hex32(const char *hex, unsigned char out[32], const char *what)
{
    if (strlen(hex) != 64)
        die("%s: 64 hex characters needed", what);
    for (int i = 0; i < 64; i++) {
        char ch = hex[i];
        int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (v < 0)
            die("%s: 64 hex characters needed", what);
        if (i % 2)
            out[i / 2] = (unsigned char)(out[i / 2] | v);
        else
            out[i / 2] = (unsigned char)(v << 4);
    }
}

static float f32_le(const unsigned char *p)
{
    uint32_t u = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* the version 2 auxiliary bytes of aux[0 .. n - 1] (T x 6 float32 LE): per input the count of its thresholds at or
 * below the value (an IEEE single comparison, gru_codec.aux_counts), first count then differences modulo 256 */
static unsigned char *aux_counts_v2(const unsigned char *aux, size_t n, const unsigned char *thr, size_t k)
{
    unsigned char *out = malloc(6 * n + 1);
    if (!out)
        die("out of memory");
    for (size_t j = 0; j < 6; j++) {
        unsigned prev = 0;
        for (size_t t = 0; t < n; t++) {
            float v = f32_le(aux + 4 * (6 * t + j));
            unsigned cnt = 0;
            for (size_t i = 0; i < k; i++)
                cnt += v >= f32_le(thr + 4 * (j * k + i));
            out[j * n + t] = (unsigned char)(t ? cnt - prev : cnt);
            prev = cnt;
        }
    }
    return out;
}

static unsigned char *bz2(const unsigned char *src, size_t n, int level, unsigned int *out_n)
{
    unsigned int cap = (unsigned int)(n + n / 100 + 600);
    unsigned char *dst = malloc(cap);
    if (!dst)
        die("out of memory");
    *out_n = cap;
    int rc = BZ2_bzBuffToBuffCompress((char *)dst, out_n, (char *)src, (unsigned int)n, level, 0, 0);
    if (rc != BZ_OK)
        die("BZ2 compression failed (%d)", rc);
    return dst;
}

/* gru_codec.rice_segment: the byte mode << 5 | k and the Golomb-Rice codes of u[0 .. n - 1], MSB-first, padded */
static unsigned char *rice(const uint32_t *u, size_t n, int mode, unsigned int *got)
{
    unsigned k = 0;
    uint64_t best = UINT64_MAX;
    for (unsigned j = 0; j <= 30; j++) {
        uint64_t bits = 0;
        for (size_t i = 0; i < n; i++) {
            uint32_t qt = u[i] >> j;
            bits += qt < RICE_ESCAPE ? (uint64_t)qt + 1 + j : 2 * RICE_ESCAPE;
        }
        if (bits < best) {
            best = bits;
            k = j;
        }
    }
    size_t bytes = 1 + (size_t)((best + 7) / 8);
    unsigned char *out = calloc(bytes, 1);
    if (!out)
        die("out of memory");
    out[0] = (unsigned char)(mode << 5 | (int)k);
    size_t pos = 8;
#define PUTBIT(b) do { if (b) out[pos >> 3] |= (unsigned char)(0x80u >> (pos & 7)); pos++; } while (0)
    for (size_t i = 0; i < n; i++) {
        uint32_t qt = u[i] >> k;
        if (qt < RICE_ESCAPE) {
            for (uint32_t r = 0; r < qt; r++)
                PUTBIT(1);
            PUTBIT(0);
            for (unsigned b = k; b-- > 0;)
                PUTBIT((u[i] >> b) & 1u);
        } else {
            for (uint32_t r = 0; r < RICE_ESCAPE; r++)
                PUTBIT(1);
            for (unsigned b = 32; b-- > 0;)
                PUTBIT((u[i] >> b) & 1u);
        }
    }
#undef PUTBIT
    *got = (unsigned int)bytes;
    return out;
}

/* version 3's auxiliary segments of aux[0 .. n - 1]: per input the zigzag of its counts' first value and differences */
static unsigned char *aux_rice_v3(const unsigned char *aux, size_t n, const unsigned char *thr, size_t k, unsigned int *got)
{
    uint32_t *u = malloc((n ? n : 1) * sizeof *u);
    buf_t all = {0};
    if (!u)
        die("out of memory");
    for (size_t j = 0; j < 6; j++) {
        int64_t prev = 0;
        for (size_t t = 0; t < n; t++) {
            float v = f32_le(aux + 4 * (6 * t + j));
            int64_t cnt = 0;
            for (size_t i = 0; i < k; i++)
                cnt += v >= f32_le(thr + 4 * (j * k + i));
            u[t] = zigzag(cnt - prev);
            prev = cnt;
        }
        unsigned int sn;
        unsigned char *seg = rice(u, n, 0, &sn);
        put(&all, seg, sn);
        free(seg);
    }
    free(u);
    *got = (unsigned int)all.n;
    return all.p;
}

/* the JSON record of one block and channel, as gru_codec writes it */
static void record_json(buf_t *b, long block, long start, long count, long ch, const char *mode, unsigned int bytes,
                        uint32_t crc)
{
    putf(b, "{\"block\":%ld,\"start\":%ld,\"count\":%ld,\"channel\":%ld,\"mode\":\"%s\",\"bytes\":%u,\"crc32\":%" PRIu32 "}",
         block, start, count, ch, mode, bytes, crc);
}

/* ---- stages: the raw and delta candidates need only the counts (a daily window, no NN), the gru candidate needs the
 * predictions (the weekly NN window). The candidates file keeps, per block and channel, the better of raw and delta
 * (ties raw), plus the compressed auxiliary inputs and the counts' SHA-256; the final stage compares the gru candidate
 * with it under the same rule (smaller payload + record wins, ties the earlier mode), so the packet is the one-stage
 * packet byte for byte. Layout (little-endian): "SCGC", u32 version 1, u32 samples, u32 channels, u32 block, u32
 * level, 64 hex chars of the counts' SHA-256, u32 aux bytes, u32 aux CRC-32, the aux payload, then per block and
 * channel u8 mode, u32 CRC-32, u32 bytes and the payload. */
static const char CAND_MAGIC[4] = {'S', 'C', 'G', 'C'};

static void put32(buf_t *b, uint32_t v)
{
    unsigned char p[4];
    le32(p, v);
    put(b, p, 4);
}

static uint32_t take32(const unsigned char *p, size_t n, size_t *off)
{
    if (*off + 4 > n)
        die("candidates: truncated");
    uint32_t v = (uint32_t)p[*off] | (uint32_t)p[*off + 1] << 8 | (uint32_t)p[*off + 2] << 16 | (uint32_t)p[*off + 3] << 24;
    *off += 4;
    return v;
}

/* one mode's column of a block and channel, BZ2-compressed (level 0: version 3, a Rice segment) */
static unsigned char *mode_payload(int m, const int32_t *q, const int32_t *pred, size_t cn, long start, long count,
                                   size_t ch, int level, unsigned char *col, unsigned int *got)
{
    if (!level) {
        uint32_t *u = (uint32_t *)(void *)col;
        for (long i = 0; i < count; i++) {
            size_t t = (size_t)(start + i);
            int64_t now = q[(t + 1) * cn + ch];
            u[i] = m == 0 ? (uint32_t)as_i32(now, "count") : m == 1 ? zigzag(as_i32(now - q[t * cn + ch], "delta"))
                                                                    : zigzag(as_i32(now - pred[t * cn + ch], "residual"));
        }
        return rice(u, (size_t)count, m, got);
    }
    for (long i = 0; i < count; i++) {
        size_t t = (size_t)(start + i);
        int64_t now = q[(t + 1) * cn + ch];
        uint32_t v;
        if (m == 0)
            v = (uint32_t)as_i32(now, "count");
        else if (m == 1)
            v = zigzag(as_i32(now - q[t * cn + ch], "delta"));
        else
            v = zigzag(as_i32(now - pred[t * cn + ch], "residual"));
        le32(col + 4 * i, v);
    }
    return bz2(col, (size_t)count * 4, level, got);
}

/* ---- the payloads of every block and channel (modes m_lo .. m_lo + nm - 1) and, as task 0, the auxiliary inputs'
 * BZ2, computed by a few threads before the stage picks from them in its one-thread order */
typedef struct { unsigned char *p; unsigned int n; uint32_t crc; } pay_t;
typedef struct {
    const int32_t *q, *pred;
    size_t cn, n, jobs;
    long block;
    int level, m_lo, nm;
    const unsigned char *aux;
    size_t aux_len;
    unsigned char *aux_c;
    unsigned int aux_n;
    pay_t *out;
    size_t next;
    pthread_mutex_t mu;
} pool_t;

static void *pool_worker(void *arg)
{
    pool_t *w = arg;
    unsigned char *col = malloc((size_t)w->block * 4);
    if (!col)
        die("out of memory");
    size_t tasks = w->jobs * (size_t)w->nm + (w->aux ? 1 : 0);
    for (;;) {
        pthread_mutex_lock(&w->mu);
        size_t i = w->next++;
        pthread_mutex_unlock(&w->mu);
        if (i >= tasks)
            break;
        if (w->aux) {
            if (i == 0) {
                w->aux_c = bz2(w->aux, w->aux_len, w->level, &w->aux_n);
                continue;
            }
            i--;
        }
        size_t job = i / (size_t)w->nm, ch = job % w->cn;
        long start = (long)(job / w->cn) * w->block;
        long count = (size_t)(start + w->block) <= w->n ? w->block : (long)w->n - start;
        pay_t *o = &w->out[i];
        o->p = mode_payload(w->m_lo + (int)(i % (size_t)w->nm), w->q, w->pred, w->cn, start, count, ch, w->level, col, &o->n);
        o->crc = crc32_of(o->p, o->n);
    }
    free(col);
    return 0;
}

static void pool_run(pool_t *w, long threads)
{
    w->out = calloc(w->jobs * (size_t)w->nm, sizeof *w->out);
    if (!w->out)
        die("out of memory");
    pthread_mutex_init(&w->mu, 0);
    pthread_t tid[8];
    long started = 0;
    for (; started < threads - 1; started++)
        if (pthread_create(&tid[started], 0, pool_worker, w))
            die("pthread_create failed");
    pool_worker(w);
    for (long k = 0; k < started; k++)
        pthread_join(tid[k], 0);
    pthread_mutex_destroy(&w->mu);
}

/* the predictions [n, cn]: a file of them, or the codebook value of the GRU tail's index */
static int32_t *load_predictions(const char *pred_path, const char *idx_path, const char *cb_path, size_t n, size_t cn)
{
    if (pred_path)
        return read_i32(pred_path, n * cn);
    int32_t *index = read_i32(idx_path, n * cn);
    size_t cb_n;
    unsigned char *cb = read_file(cb_path, &cb_n);
    int32_t **books = calloc(cn, sizeof *books);
    uint32_t *sizes = calloc(cn, sizeof *sizes);
    if (!books || !sizes)
        die("out of memory");
    size_t off = 0;
    for (size_t ch = 0; ch < cn; ch++) {
        if (off + 4 > cb_n)
            die("codebooks: truncated");
        sizes[ch] = (uint32_t)cb[off] | (uint32_t)cb[off + 1] << 8 | (uint32_t)cb[off + 2] << 16 | (uint32_t)cb[off + 3] << 24;
        off += 4;
        if (off + (size_t)sizes[ch] * 4 > cb_n)
            die("codebooks: truncated");
        books[ch] = malloc((size_t)sizes[ch] * sizeof **books + 1);
        if (!books[ch])
            die("out of memory");
        for (uint32_t k = 0; k < sizes[ch]; k++, off += 4)
            books[ch][k] = (int32_t)((uint32_t)cb[off] | (uint32_t)cb[off + 1] << 8 | (uint32_t)cb[off + 2] << 16 |
                                     (uint32_t)cb[off + 3] << 24);
    }
    if (off != cb_n)
        die("codebooks: extra bytes");
    int32_t *pred = malloc(n * cn * sizeof *pred + 1);
    if (!pred)
        die("out of memory");
    for (size_t t = 0; t < n; t++)
        for (size_t ch = 0; ch < cn; ch++) {
            int32_t k = index[t * cn + ch];
            if (k < 0 || (uint32_t)k >= sizes[ch])
                die("index %" PRId32 " out of channel %zu's codebook (%" PRIu32 ")", k, ch, sizes[ch]);
            pred[t * cn + ch] = books[ch][k];
        }
    for (size_t ch = 0; ch < cn; ch++)
        free(books[ch]);
    free(books);
    free(sizes);
    free(cb);
    free(index);
    return pred;
}

/* ---- version 4 (gru_codec.encode_v4, 11/06): near-lossless bins, no auxiliary payload, neighbour modes */
#define V4_TOP (1u << 24)
#define V4_BLOCK 1800
typedef struct { uint32_t *start, *value; size_t n; unsigned eps_n; } bins_t;

/* floor(sqrt(x)), integers only */
static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x)
        bit >>= 2;
    while (bit) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* gru_codec.V4_LATTICES: (from q, step) of an instrument lattice, q0 and steps ascending, each q0 a multiple of its own
 * and the previous step */
typedef struct { uint32_t from, step; } lattice_t;
static const lattice_t LATTICE_KSEM[] = {{6400u, 16u}, {25600u, 64u}};
#define V4_LATTICE_FLAG 0x40u

static void bins_push(bins_t *b, size_t *cap, uint32_t start, uint32_t value)
{
    if (b->n == *cap) {
        *cap *= 2;
        uint32_t *s = realloc(b->start, *cap * sizeof *s);
        if (!s)
            die("out of memory");
        b->start = s;
        uint32_t *v = realloc(b->value, *cap * sizeof *v);
        if (!v)
            die("out of memory");
        b->value = v;
    }
    b->start[b->n] = start;
    b->value[b->n] = value;
    b->n++;
}

/* gru_codec.bin_starts: the bin at lo is 2 * (isqrt(lo) / eps_n) + 1 wide and stands for lo + isqrt(lo) / eps_n.
 * With a lattice (nlat > 0), h(v) = isqrt(v) / eps_n: where lo lies in a region of step s and s >= 2 h(lo) + 1, the
 * next lattice value p >= lo gets a bin that stands for p, from max(p - h(p - h(p)), lo) to p + h(p - h(p)); the
 * integers before it get ordinary bins, the last one cut short (width w: stands for start + (w - 1) / 2). An ordinary
 * bin is also cut at the start of a region where the rule holds. */
static void bins_build(bins_t *b, unsigned eps_n, const lattice_t *lat, size_t nlat)
{
    size_t cap = 1u << 16;
    b->eps_n = eps_n;
    b->n = 0;
    b->start = b->value = 0;
    if (!eps_n)
        return;
    b->start = malloc(cap * sizeof *b->start);
    b->value = malloc(cap * sizeof *b->value);
    if (!b->start || !b->value)
        die("out of memory");
    for (uint32_t lo = 0; lo < V4_TOP;) {
        uint32_t half = isqrt32(lo) / eps_n, step = 0, cut = 0;
        for (size_t r = 0; r < nlat; r++) {
            if (lo >= lat[r].from)
                step = lat[r].step;
            else if (!cut && lat[r].step >= 2 * (isqrt32(lat[r].from) / eps_n) + 1)
                cut = lat[r].from;          /* the next region start where the rule holds */
        }
        if (step && step >= 2 * half + 1) {
            uint32_t p = (lo + step - 1) / step * step;
            uint32_t hp = isqrt32(p - isqrt32(p) / eps_n) / eps_n;
            uint32_t a = p - hp > lo ? p - hp : lo;
            while (lo < a) {
                uint32_t w = 2 * (isqrt32(lo) / eps_n) + 1;
                if (w > a - lo)
                    w = a - lo;
                bins_push(b, &cap, lo, lo + (w - 1) / 2);
                lo += w;
            }
            bins_push(b, &cap, lo, p);
            lo = p + hp + 1;
        } else {
            uint32_t w = 2 * half + 1;
            if (cut && w > cut - lo)
                w = cut - lo;
            bins_push(b, &cap, lo, lo + (w - 1) / 2);
            lo += w;
        }
    }
}

static int64_t bin_of(const bins_t *b, int64_t x)
{
    if (x < 0 || x >= (int64_t)V4_TOP)
        die("version 4: value %" PRId64 " outside 0 .. 2^24 - 1", x);
    if (!b->eps_n)
        return x;
    size_t lo = 0, hi = b->n;            /* the last start <= x: start[lo] <= x < start[hi] */
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (b->start[mid] <= (uint64_t)x)
            lo = mid;
        else
            hi = mid;
    }
    return (int64_t)lo;
}

static int64_t bin_value(const bins_t *b, int64_t i)
{
    return b->eps_n ? (int64_t)b->value[i] : i;
}

/* round half to even without libm (Python's round of a float) */
static long round_even(double v)
{
    long t = (long)v;
    if ((double)t > v)
        t -= 1;
    double f = v - (double)t;
    if (f > 0.5 || (f == 0.5 && (t & 1)))
        t += 1;
    return t;
}

/* gru_codec._v4_coefs: int64 sums, a float64 solve in the same order (volatile: no fused multiply-add), 16 x slope
 * rounded half to even, clipped to int8 */
static void v4_coefs(const int64_t *x0, const int64_t *x1, int k, const int64_t *y, size_t n, int8_t out[2])
{
    int64_t s00 = 0, s01 = 0, s11 = 0, t0 = 0, t1 = 0;
    for (size_t i = 0; i < n; i++) {
        s00 += x0[i] * x0[i];
        t0 += x0[i] * y[i];
        if (k == 2) {
            s01 += x0[i] * x1[i];
            s11 += x1[i] * x1[i];
            t1 += x1[i] * y[i];
        }
    }
    double beta[2] = {0.0, 0.0};
    if (k == 1) {
        if (s00)
            beta[0] = (double)t0 / (double)s00;
    } else {
        volatile double p = (double)s00 * (double)s11, r = (double)s01 * (double)s01;
        double det = p - r;
        if (det != 0.0) {
            volatile double u1 = (double)s11 * (double)t0, u2 = (double)s01 * (double)t1;
            volatile double v1 = (double)s00 * (double)t1, v2 = (double)s01 * (double)t0;
            beta[0] = (u1 - u2) / det;
            beta[1] = (v1 - v2) / det;
        }
    }
    for (int j = 0; j < k; j++) {
        double v = 16.0 * beta[j];
        long r = v > 1e9 ? 127 : v < -1e9 ? -128 : round_even(v);
        out[j] = (int8_t)(r > 127 ? 127 : r < -128 ? -128 : r);
    }
}

static void put_varint(buf_t *b, uint64_t v)
{
    unsigned char c;
    while (v >= 0x80) {
        c = (unsigned char)((v & 0x7F) | 0x80);
        put(b, &c, 1);
        v >>= 7;
    }
    c = (unsigned char)v;
    put(b, &c, 1);
}

/* one segment: the byte mode << 5 | k, the neighbour coefficients, then the Rice codes of zigzag (or raw) values */
static unsigned char *v4_segment(const int64_t *s, size_t n, int mode, const int8_t *coef, int nc, uint32_t *u,
                                 unsigned int *got)
{
    for (size_t i = 0; i < n; i++)
        u[i] = mode == 0 ? (uint32_t)as_i32(s[i], "bin index") : zigzag(as_i32(s[i], "version 4 residual"));
    unsigned int rn;
    unsigned char *seg = rice(u, n, mode, &rn);
    unsigned char *out = malloc(rn + (size_t)nc);
    if (!out)
        die("out of memory");
    out[0] = seg[0];
    for (int j = 0; j < nc; j++)
        out[1 + j] = (unsigned char)coef[j];
    memcpy(out + 1 + nc, seg + 1, rn - 1);
    free(seg);
    *got = rn + (unsigned int)nc;
    return out;
}

static int encode_v4(const int32_t *q, size_t t_len, size_t cn, const unsigned char *aux, const unsigned char *thr,
                     size_t k_thr, const int32_t *pred, const char *model, unsigned eps_n, int lattice,
                     uint32_t minute0, unsigned window, const char *out_path, const char *metrics_path,
                     const char *qhat_path)
{
    size_t n = t_len - 1, half = cn / 2;
    bins_t b;
    bins_build(&b, eps_n, lattice ? LATTICE_KSEM : 0, lattice ? sizeof LATTICE_KSEM / sizeof *LATTICE_KSEM : 0);
    int64_t *ii = malloc(t_len * cn * sizeof *ii), *ip = malloc(n * cn * sizeof *ip);
    if (!ii || !ip)
        die("out of memory");
    for (size_t i = 0; i < t_len * cn; i++)
        ii[i] = bin_of(&b, q[i]);
    for (size_t i = 0; i < n * cn; i++)
        ip[i] = bin_of(&b, pred[i]);
    int64_t *streams = malloc(3 * cn * V4_BLOCK * sizeof *streams), *res = malloc(V4_BLOCK * sizeof *res);
    int64_t **signed_of = calloc(cn, sizeof *signed_of);
    uint32_t *u = malloc(V4_BLOCK * sizeof *u);
    int *chosen = calloc(cn, sizeof *chosen);
    if (!streams || !res || !signed_of || !u || !chosen)
        die("out of memory");
    buf_t payload = {0};
    long modes[5] = {0, 0, 0, 0, 0};
    for (size_t start = 0; start < n; start += V4_BLOCK) {
        size_t count = start + V4_BLOCK <= n ? V4_BLOCK : n - start;
        for (size_t j = 0; j < cn; j++) {
            int64_t *raw = streams + (3 * j) * V4_BLOCK, *dl = raw + V4_BLOCK, *gr = dl + V4_BLOCK;
            for (size_t i = 0; i < count; i++) {
                size_t t = start + i;
                raw[i] = ii[(t + 1) * cn + j];
                dl[i] = raw[i] - ii[t * cn + j];
                gr[i] = raw[i] - ip[t * cn + j];
            }
            unsigned char *best = 0;
            unsigned int best_n = 0;
            int best_mode = 0;
            for (int m = 0; m < 3; m++) {
                unsigned int got;
                unsigned char *seg = v4_segment(m == 0 ? raw : m == 1 ? dl : gr, count, m, 0, 0, u, &got);
                if (!best || got < best_n) {
                    free(best);
                    best = seg; best_n = got; best_mode = m;
                } else {
                    free(seg);
                }
            }
            /* neighbours: the previous channel of the same half, the first half's channel at the same position */
            size_t nb[2];
            int nn = 0;
            if (half && j % half && chosen[j - 1] != 0)
                nb[nn++] = j - 1;
            if (half && j >= half && (j % half == 0 || j - half != j - 1) && chosen[j - half] != 0)
                nb[nn++] = j - half;
            if (nn) {
                for (int base = 1; base <= 2; base++) {
                    const int64_t *y = base == 1 ? dl : gr;
                    int8_t coef[2];
                    v4_coefs(signed_of[nb[0]], nn == 2 ? signed_of[nb[1]] : 0, nn, y, count, coef);
                    for (size_t i = 0; i < count; i++) {
                        int64_t acc = 8;
                        for (int a = 0; a < nn; a++)
                            acc += signed_of[nb[a]][i] * (int64_t)coef[a];
                        res[i] = y[i] - (acc >> 4);
                    }
                    unsigned int got;
                    unsigned char *seg = v4_segment(res, count, base + 2, coef, nn, u, &got);
                    if (got < best_n) {
                        free(best);
                        best = seg; best_n = got; best_mode = base + 2;
                    } else {
                        free(seg);
                    }
                }
            }
            chosen[j] = best_mode;
            signed_of[j] = (best_mode == 1 || best_mode == 3 || best_mode == 0) ? dl : gr;
            modes[best_mode]++;
            put(&payload, best, best_n);
            free(best);
        }
    }
    /* end-to-end CRC: (with a lattice the flags byte,) the auxiliary threshold counts [T - 1, 6] then the bin indices
     * int32 LE [T, C] */
    unsigned char flags = (unsigned char)(eps_n | 0x80u | (lattice ? V4_LATTICE_FLAG : 0u));
    uint32_t e2e = 0xFFFFFFFFu;
    if (lattice)
        e2e = crc32_update(e2e, &flags, 1);
    for (size_t t = 0; t < n; t++)
        for (size_t j = 0; j < 6; j++) {
            float v = f32_le(aux + 4 * (6 * t + j));
            unsigned char cnt = 0;
            for (size_t i = 0; i < k_thr; i++)
                cnt = (unsigned char)(cnt + (v >= f32_le(thr + 4 * (j * k_thr + i))));
            e2e = crc32_update(e2e, &cnt, 1);
        }
    for (size_t i = 0; i < t_len * cn; i++) {
        unsigned char le[4];
        le32(le, (uint32_t)as_i32(ii[i], "bin index"));
        e2e = crc32_update(e2e, le, 4);
    }
    e2e ^= 0xFFFFFFFFu;
    unsigned char model_b[32];
    hex32(model, model_b, "--model-sha256");
    buf_t h = {0};
    unsigned char head[9];
    head[0] = 4;
    head[1] = flags;
    head[2] = (unsigned char)t_len;
    head[3] = (unsigned char)(t_len >> 8);
    le32(head + 4, minute0);
    head[8] = (unsigned char)window;
    put(&h, head, 9);
    put(&h, model_b, 8);
    for (size_t j = 0; j < cn; j++)
        put_varint(&h, (uint64_t)ii[j]);
    put_varint(&h, payload.n);
    put32(&h, crc32_of(payload.p, payload.n));
    put32(&h, e2e);
    FILE *f = fopen(out_path, "wb");
    if (!f || fwrite(h.p, 1, h.n, f) != h.n || fwrite(payload.p, 1, payload.n, f) != payload.n || fclose(f))
        die("%s: write failed", out_path);
    int64_t max_err = 0;
    for (size_t i = 0; i < t_len * cn; i++) {
        int64_t e = bin_value(&b, ii[i]) - q[i];
        if (e < 0)
            e = -e;
        if (e > max_err)
            max_err = e;
    }
    if (qhat_path) {
        FILE *g = fopen(qhat_path, "wb");
        if (!g)
            die("%s: %s", qhat_path, strerror(errno));
        for (size_t i = 0; i < t_len * cn; i++) {
            unsigned char le[4];
            le32(le, (uint32_t)bin_value(&b, ii[i]));
            if (fwrite(le, 1, 4, g) != 4)
                die("%s: write failed", qhat_path);
        }
        if (fclose(g))
            die("%s: write failed", qhat_path);
    }
    char metrics[1024];
    snprintf(metrics, sizeof metrics,
             "{\"count_raw_bytes\":%zu,\"packet_bytes\":%zu,\"header_bytes\":%zu,\"selected_count_payload_bytes\":%zu,"
             "\"mode_blocks\":{\"raw\":%ld,\"delta\":%ld,\"gru\":%ld,\"delta+x\":%ld,\"gru+x\":%ld},\"samples\":%zu,"
             "\"channels\":%zu,\"eps_n\":%u,\"minute0\":%" PRIu32 ",\"max_abs_error\":%" PRId64 ",\"version\":4%s}\n",
             t_len * cn * 4, h.n + payload.n, h.n, payload.n, modes[0], modes[1], modes[2], modes[3], modes[4], t_len,
             cn, eps_n, minute0, max_err, lattice ? ",\"lattice\":\"ksem\"" : "");
    if (metrics_path) {
        FILE *m = fopen(metrics_path, "w");
        if (!m || fputs(metrics, m) < 0 || fclose(m))
            die("%s: write failed", metrics_path);
    }
    fputs(metrics, stdout);
    free(h.p);
    free(payload.p);
    free(streams);
    free(res);
    free(signed_of);
    free(u);
    free(chosen);
    free(ii);
    free(ip);
    free(b.start);
    free(b.value);
    return 0;
}

int main(int argc, char **argv)
{
    const char *q_path = 0, *aux_path = 0, *pred_path = 0, *idx_path = 0, *cb_path = 0, *out_path = 0, *metrics_path = 0;
    const char *channels_json = 0, *model = 0, *cand_path = 0, *stage_name = "all", *predictor = PREDICTOR;
    const char *thr_path = 0, *qhat_path = 0, *lattice_name = 0;
    long t_len = 0, c = 0, scale = -1, block = 1800, level = 5, segment = 0, window = 0, threads = 1, format = 1;
    long eps_n = -1;
    long long minute0 = -1;
    for (int i = 1; i < argc; i++) {
        const char *k = argv[i];
        if (i + 1 >= argc)
            die("%s needs a value", k);
        const char *v = argv[++i];
        if (!strcmp(k, "--q")) q_path = v;
        else if (!strcmp(k, "--aux")) aux_path = v;
        else if (!strcmp(k, "--pred")) pred_path = v;
        else if (!strcmp(k, "--index")) idx_path = v;
        else if (!strcmp(k, "--codebooks")) cb_path = v;
        else if (!strcmp(k, "--out")) out_path = v;
        else if (!strcmp(k, "--metrics")) metrics_path = v;
        else if (!strcmp(k, "--channels-json")) channels_json = v;
        else if (!strcmp(k, "--model-sha256")) model = v;
        else if (!strcmp(k, "--samples")) t_len = strtol(v, 0, 10);
        else if (!strcmp(k, "--channels")) c = strtol(v, 0, 10);
        else if (!strcmp(k, "--scale")) scale = strtol(v, 0, 10);
        else if (!strcmp(k, "--block")) block = strtol(v, 0, 10);
        else if (!strcmp(k, "--level")) level = strtol(v, 0, 10);
        else if (!strcmp(k, "--segment-steps")) segment = strtol(v, 0, 10);
        else if (!strcmp(k, "--window-steps")) window = strtol(v, 0, 10);
        else if (!strcmp(k, "--stage")) stage_name = v;
        else if (!strcmp(k, "--candidates")) cand_path = v;
        else if (!strcmp(k, "--predictor")) predictor = v;
        else if (!strcmp(k, "--threads")) threads = strtol(v, 0, 10);
        else if (!strcmp(k, "--format")) format = strtol(v, 0, 10);
        else if (!strcmp(k, "--aux-thresholds")) thr_path = v;
        else if (!strcmp(k, "--eps-n")) eps_n = strtol(v, 0, 10);
        else if (!strcmp(k, "--minute0")) minute0 = strtoll(v, 0, 10);
        else if (!strcmp(k, "--qhat-out")) qhat_path = v;
        else if (!strcmp(k, "--lattice")) lattice_name = v;
        else die("unknown option %s", k);
    }
    if (format == 4) {
        /* one stage: the counts, the auxiliary inputs and their thresholds (the end-to-end CRC), the predictions over
         * q_hat, the bound 1/eps_n sigma, the start minute and the window */
        if (!q_path || !aux_path || !thr_path || !out_path || !model || (!pred_path == !(idx_path && cb_path)))
            die("--format 4 needs --q, --aux, --aux-thresholds, --out, --model-sha256 and --pred or --index with --codebooks");
        if (t_len < 2 || t_len > 0xFFFF || c < 1 || eps_n < 0 || eps_n > 63 || minute0 < 0 || minute0 > 0xFFFFFFFFLL ||
            window < 1 || window > 255)
            die("--format 4: --samples 2..65535, --channels, --eps-n 0..63, --minute0 u32, --window-steps 1..255");
        int lattice = lattice_name && strcmp(lattice_name, "none");
        if (lattice && strcmp(lattice_name, "ksem"))
            die("--lattice ksem or none");
        if (lattice && eps_n < 1)
            die("--lattice ksem needs --eps-n 1..63");
        crc_init();
        size_t cn4 = (size_t)c, tl4 = (size_t)t_len;
        int32_t *q4 = read_i32(q_path, tl4 * cn4);
        size_t aux_n, thr_n;
        unsigned char *aux4 = read_file(aux_path, &aux_n), *thr4 = read_file(thr_path, &thr_n);
        if (aux_n != tl4 * 24)
            die("%s: %zu bytes, expected %zu", aux_path, aux_n, tl4 * 24);
        if (thr_n % 24 || thr_n / 24 < 1 || thr_n / 24 > 255)
            die("%s: 6 x K float32 thresholds, K 1..255", thr_path);
        int32_t *pred4 = load_predictions(pred_path, idx_path, cb_path, tl4 - 1, cn4);
        int rc = encode_v4(q4, tl4, cn4, aux4, thr4, thr_n / 24, pred4, model, (unsigned)eps_n, lattice,
                           (uint32_t)minute0, (unsigned)window, out_path, metrics_path, qhat_path);
        free(q4);
        free(aux4);
        free(thr4);
        free(pred4);
        return rc;
    }
    if (format < 1 || format > 3)
        die("--format 1, 2, 3 or 4");
    if (lattice_name)
        die("--lattice needs --format 4");
    if (format >= 2 && strcmp(stage_name, "final") && !thr_path)
        die("--format 2 or 3 needs --aux-thresholds where the auxiliary inputs are compressed");
    if (format >= 2 && !channels_json)
        channels_json = "";      /* not in the version 2 header */
    int stage = !strcmp(stage_name, "all") ? 0 : !strcmp(stage_name, "pre") ? 1 : !strcmp(stage_name, "final") ? 2 : -1;
    if (stage < 0)
        die("--stage all, pre or final");
    if (!q_path || (stage != 2 && !aux_path) || (stage != 0 && !cand_path))
        die("--q, --aux (not in the final stage) and --candidates (pre and final stages) needed");
    if (stage != 1 && (!out_path || !channels_json || !model || (!pred_path == !(idx_path && cb_path))))
        die("--out, --channels-json, --model-sha256 and either --pred or --index with --codebooks needed");
    if (t_len < 2 || c < 1 || (stage != 1 && scale < 0) || block < 1 || level < 1 || level > 9 || segment < 0)
        die("invalid --samples, --channels, --scale, --block or --level");
    /* gru_codec.WINDOW_FLAG: windows (gru_predictor.windowed) in the version 2 segment field, never with segments */
    if (window < 0 || window > 0xFFFF || segment >= 0x10000 || (window && segment))
        die("--segment-steps below 65536 or --window-steps 1..65535, not both");
    if (threads < 1 || threads > 8)
        die("--threads 1..8");
    if (!*predictor)
        die("--predictor needs a name");
    for (const char *s = predictor; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e || *s == '"' || *s == '\\')
            die("--predictor must be printable ASCII without quotes or backslashes");
    if (channels_json)
        for (const char *s = channels_json; *s; s++)
            if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e)
                die("--channels-json must be printable ASCII (json.dumps with ensure_ascii)");
    crc_init();
    size_t n = (size_t)t_len - 1, cn = (size_t)c;
    int32_t *q = read_i32(q_path, (size_t)t_len * cn);
    for (size_t i = 0; i < (size_t)t_len * cn; i++)
        if (q[i] < 0)
            die("counts must not be negative");
    /* q as int32 little-endian bytes for its SHA-256 */
    unsigned char *q_le = malloc((size_t)t_len * cn * 4);
    if (!q_le)
        die("out of memory");
    for (size_t i = 0; i < (size_t)t_len * cn; i++)
        le32(q_le + 4 * i, (uint32_t)q[i]);
    char q_hash[65];
    sha256_hex(q_le, (size_t)t_len * cn * 4, q_hash);
    free(q_le);
    /* the compressed auxiliary inputs: from the candidates in the final stage */
    unsigned char *aux_c = 0, *cand = 0, *aux_raw = 0;
    size_t aux_len = 0;
    unsigned int aux_c_n = 0;
    uint32_t aux_crc = 0;
    size_t cand_n = 0, coff = 0;
    if (stage == 2) {
        cand = read_file(cand_path, &cand_n);
        if (cand_n < 4 + 20 + 64 || memcmp(cand, CAND_MAGIC, 4))
            die("%s: not a candidates file", cand_path);
        coff = 4;
        uint32_t version = take32(cand, cand_n, &coff), cs = take32(cand, cand_n, &coff), cc = take32(cand, cand_n, &coff),
                 cb = take32(cand, cand_n, &coff), cl = take32(cand, cand_n, &coff);
        if (version != (uint32_t)format || cs != (uint32_t)t_len || cc != (uint32_t)c || cb != (uint32_t)block ||
            cl != (uint32_t)level)
            die("candidates: made for another format, samples, channels, block or level");
        if (memcmp(cand + coff, q_hash, 64))
            die("candidates: made from other counts");
        coff += 64;
        aux_c_n = take32(cand, cand_n, &coff);
        aux_crc = take32(cand, cand_n, &coff);
        if (coff + aux_c_n > cand_n)
            die("candidates: truncated");
        aux_c = cand + coff;
        coff += aux_c_n;
        if (crc32_of(aux_c, aux_c_n) != aux_crc)
            die("candidates: auxiliary payload CRC");
    } else {
        size_t aux_n;
        unsigned char *aux = read_file(aux_path, &aux_n);
        if (aux_n != (size_t)t_len * 24)
            die("%s: %zu bytes, expected %zu", aux_path, aux_n, (size_t)t_len * 24);
        for (size_t i = 0; i < (size_t)t_len * 6; i++) {
            uint32_t u = (uint32_t)aux[4 * i] | (uint32_t)aux[4 * i + 1] << 8 | (uint32_t)aux[4 * i + 2] << 16 |
                         (uint32_t)aux[4 * i + 3] << 24;
            if ((u & 0x7F800000u) == 0x7F800000u)
                die("auxiliary inputs must be finite");
        }
        if (format >= 2) {
            size_t thr_n;
            unsigned char *thr = read_file(thr_path, &thr_n);
            size_t k = thr_n / 24;
            if (thr_n % 24 || k < 1 || k > 255)
                die("%s: 6 x K float32 thresholds, K 1..255", thr_path);
            for (size_t i = 0; i < 6 * k; i++) {
                uint32_t u = (uint32_t)thr[4 * i] | (uint32_t)thr[4 * i + 1] << 8 | (uint32_t)thr[4 * i + 2] << 16 |
                             (uint32_t)thr[4 * i + 3] << 24;
                if ((u & 0x7F800000u) == 0x7F800000u)
                    die("auxiliary thresholds must be finite");
            }
            if (format == 3) {
                aux_c = aux_rice_v3(aux, n, thr, k, &aux_c_n);    /* the segments themselves: no pool task */
                aux_crc = crc32_of(aux_c, aux_c_n);
            } else {
                aux_raw = aux_counts_v2(aux, n, thr, k);
                aux_len = 6 * n;
            }
            free(thr);
            free(aux);
        } else {
            aux_raw = aux;      /* compressed with the payloads below */
            aux_len = n * 24;
        }
    }
    /* the predictions: given, or the codebook value of the GRU tail's index (none before the NN window) */
    int32_t *pred = stage == 1 ? 0 : load_predictions(pred_path, idx_path, cb_path, n, cn);
    /* blocks and channels: the smallest of raw / delta / gru (payload bytes + record JSON bytes), ties the earlier */
    buf_t records = {0}, payloads = {0}, rec = {0}, pre = {0};
    long modes[3] = {0, 0, 0};
    static const char *NAMES[3] = {"raw", "delta", "gru"};
    pool_t pool = {.q = q, .pred = pred, .cn = cn, .n = n, .jobs = ((n + (size_t)block - 1) / (size_t)block) * cn,
                   .block = block, .level = format == 3 ? 0 : (int)level, .m_lo = stage == 2 ? 2 : 0, .aux = aux_raw,
                   .aux_len = aux_len};
    pool.nm = (stage == 1 ? 2 : 3) - pool.m_lo;
    pool_run(&pool, threads);
    if (aux_raw) {
        aux_c = pool.aux_c;
        aux_c_n = pool.aux_n;
        aux_crc = crc32_of(aux_c, aux_c_n);
        free(aux_raw);
    }
    int first = 1;
    if (stage == 1) {
        put(&pre, CAND_MAGIC, 4);
        put32(&pre, (uint32_t)format); put32(&pre, (uint32_t)t_len); put32(&pre, (uint32_t)c); put32(&pre, (uint32_t)block);
        put32(&pre, (uint32_t)level);
        put(&pre, q_hash, 64);
        put32(&pre, aux_c_n); put32(&pre, aux_crc);
        put(&pre, aux_c, aux_c_n);
    }
    for (long start = 0, b = 0; (size_t)start < n; start += block, b++) {
        long count = (size_t)(start + block) <= n ? block : (long)n - start;
        for (size_t ch = 0; ch < cn; ch++) {
            unsigned char *best = 0;
            unsigned int best_n = 0;
            size_t best_total = 0;
            int best_mode = 0, owned = 1;
            uint32_t best_crc = 0;
            if (stage == 2) {
                /* the day's better of raw and delta, from the candidates */
                if (coff + 9 > cand_n)
                    die("candidates: truncated");
                best_mode = cand[coff++];
                best_crc = take32(cand, cand_n, &coff);
                best_n = take32(cand, cand_n, &coff);
                if (best_mode > 1 || coff + best_n > cand_n)
                    die("candidates: bad record");
                best = cand + coff;
                coff += best_n;
                owned = 0;
                if (crc32_of(best, best_n) != best_crc)
                    die("candidates: payload CRC");
                rec.n = 0;
                if (format == 1)
                    record_json(&rec, b, start, count, (long)ch, NAMES[best_mode], best_n, best_crc);
                best_total = best_n + rec.n;
            }
            for (int m = pool.m_lo; m < pool.m_lo + pool.nm; m++) {
                pay_t *pp = &pool.out[((size_t)b * cn + ch) * (size_t)pool.nm + (size_t)(m - pool.m_lo)];
                unsigned int got = pp->n;
                unsigned char *payload = pp->p;
                uint32_t crc = pp->crc;
                rec.n = 0;
                if (format == 1)      /* version 2 records are all 9 bytes, version 3 has none: the payload decides */
                    record_json(&rec, b, start, count, (long)ch, NAMES[m], got, crc);
                size_t total = got + rec.n;
                if (!best || total < best_total) {
                    if (owned)
                        free(best);
                    best = payload; best_n = got; best_total = total; best_mode = m; best_crc = crc; owned = 1;
                } else {
                    free(payload);
                }
            }
            if (stage == 1) {
                unsigned char mode = (unsigned char)best_mode;
                put(&pre, &mode, 1);
                put32(&pre, best_crc);
                put32(&pre, best_n);
                put(&pre, best, best_n);
            } else if (format == 3) {
                put(&payloads, best, best_n);
            } else if (format == 2) {
                unsigned char mode = (unsigned char)best_mode;
                put(&records, &mode, 1);
                put32(&records, best_n);
                put32(&records, best_crc);
                put(&payloads, best, best_n);
            } else {
                rec.n = 0;
                record_json(&rec, b, start, count, (long)ch, NAMES[best_mode], best_n, best_crc);
                if (!first)
                    put(&records, ",", 1);
                first = 0;
                put(&records, rec.p, rec.n);
                put(&payloads, best, best_n);
            }
            modes[best_mode]++;
            if (owned)
                free(best);
        }
    }
    if (stage == 1) {
        FILE *f = fopen(cand_path, "wb");
        if (!f || fwrite(pre.p, 1, pre.n, f) != pre.n || fclose(f))
            die("%s: write failed", cand_path);
        printf("{\"stage\":\"pre\",\"candidate_bytes\":%zu,\"mode_blocks\":{\"raw\":%ld,\"delta\":%ld},\"samples\":%ld,"
               "\"channels\":%ld,\"block_samples\":%ld}\n", pre.n, modes[0], modes[1], t_len, c, block);
        free(pre.p);
        free(rec.p);
        free(pool.out);
        free(aux_c);
        free(q);
        return 0;
    }
    if (stage == 2 && coff != cand_n)
        die("candidates: extra bytes");
    buf_t h = {0};
    if (format == 3) {
        /* gru_codec._header_v3: version 2's fields without records, the payload's bytes and CRC-32 before the hash */
        unsigned char model_b[32], q_b[32], head[20];
        size_t plen = strlen(predictor);
        if (cn > 0xFFFF || plen > 255)
            die("version 3: channels or predictor name too large");
        hex32(model, model_b, "--model-sha256");
        hex32(q_hash, q_b, "counts hash");
        head[0] = 3; head[1] = (unsigned char)level; head[2] = (unsigned char)cn; head[3] = (unsigned char)(cn >> 8);
        le32(head + 4, (uint32_t)t_len); le32(head + 8, (uint32_t)scale); le32(head + 12, (uint32_t)block);
        le32(head + 16, window ? 0x10000u | (uint32_t)window : (uint32_t)segment);
        put(&h, head, 20);
        put(&h, model_b, 32);
        unsigned char pl = (unsigned char)plen;
        put(&h, &pl, 1);
        put(&h, predictor, plen);
        for (size_t ch = 0; ch < cn; ch++)
            put32(&h, (uint32_t)q[ch]);
        /* the payload = the auxiliary segments, then the chosen segments */
        uint32_t crc = crc32_update(crc32_update(0xFFFFFFFFu, aux_c, aux_c_n), payloads.p, payloads.n) ^ 0xFFFFFFFFu;
        put32(&h, (uint32_t)(aux_c_n + payloads.n));
        put32(&h, crc);
        put(&h, q_b, 32);
    } else if (format == 2) {
        /* gru_codec._header_v2: <BBHIIII version level channels samples scale block segment, the model's 32 bytes,
         * u8 predictor length and name, int32 initial counts, u32 aux bytes and CRC, the records, the counts' hash */
        unsigned char model_b[32], q_b[32], head[20];
        size_t plen = strlen(predictor);
        if (cn > 0xFFFF || plen > 255)
            die("version 2: channels or predictor name too large");
        hex32(model, model_b, "--model-sha256");
        hex32(q_hash, q_b, "counts hash");
        head[0] = 2; head[1] = (unsigned char)level; head[2] = (unsigned char)cn; head[3] = (unsigned char)(cn >> 8);
        le32(head + 4, (uint32_t)t_len); le32(head + 8, (uint32_t)scale); le32(head + 12, (uint32_t)block);
        le32(head + 16, window ? 0x10000u | (uint32_t)window : (uint32_t)segment);
        put(&h, head, 20);
        put(&h, model_b, 32);
        unsigned char pl = (unsigned char)plen;
        put(&h, &pl, 1);
        put(&h, predictor, plen);
        for (size_t ch = 0; ch < cn; ch++)
            put32(&h, (uint32_t)q[ch]);
        put32(&h, aux_c_n);
        put32(&h, aux_crc);
        put(&h, records.p, records.n);
        put(&h, q_b, 32);
    } else {
    putf(&h, "{\"version\":1,\"codec\":\"bz2\",\"level\":%ld,\"dtype\":\"int32-le\",\"samples\":%ld,\"channels\":", level, t_len);
    put(&h, channels_json, strlen(channels_json));
    putf(&h, ",\"scale\":%ld,\"model_sha256\":\"", scale);
    put(&h, model, strlen(model));
    putf(&h, "\",\"predictor\":\"%s\",\"block_samples\":%ld,", predictor, block);
    if (segment)       /* the predictor's state was zero again every that many steps (gru_predictor.segmented) */
        putf(&h, "\"segment_steps\":%ld,", segment);
    if (window)        /* every step's prediction from its own window (gru_predictor.windowed) */
        putf(&h, "\"window_steps\":%ld,", window);
    putf(&h, "\"initial_q\":[");
    for (size_t ch = 0; ch < cn; ch++)
        putf(&h, ch ? ",%" PRId32 : "%" PRId32, q[ch]);
    putf(&h, "],\"aux_bytes\":%u,\"aux_crc32\":%" PRIu32 ",\"records\":[", aux_c_n, aux_crc);
    put(&h, records.p, records.n);
    putf(&h, "],\"q_sha256\":\"%s\"}", q_hash);
    }
    unsigned char len[4];
    le32(len, (uint32_t)h.n);
    FILE *f = fopen(out_path, "wb");
    if (!f)
        die("%s: %s", out_path, strerror(errno));
    size_t packet = sizeof MAGIC + 4 + h.n + aux_c_n + payloads.n;
    if (fwrite(format == 3 ? MAGIC3 : format == 2 ? MAGIC2 : MAGIC, 1, sizeof MAGIC, f) != sizeof MAGIC ||
        fwrite(len, 1, 4, f) != 4 ||
        fwrite(h.p, 1, h.n, f) != h.n ||
        fwrite(aux_c, 1, aux_c_n, f) != aux_c_n || fwrite(payloads.p, 1, payloads.n, f) != payloads.n || fclose(f))
        die("%s: write failed", out_path);
    char metrics[1024];
    snprintf(metrics, sizeof metrics,
             "{\"count_raw_bytes\":%zu,\"packet_bytes\":%zu,\"auxiliary_payload_bytes\":%u,"
             "\"selected_count_payload_bytes\":%zu,\"header_bytes\":%zu,\"mode_blocks\":{\"raw\":%ld,\"delta\":%ld,"
             "\"gru\":%ld},\"samples\":%ld,\"channels\":%ld,\"block_samples\":%ld}\n",
             (size_t)t_len * cn * 4, packet, aux_c_n, payloads.n, sizeof MAGIC + 4 + h.n, modes[0], modes[1], modes[2],
             t_len, c, block);
    if (metrics_path) {
        FILE *m = fopen(metrics_path, "w");
        if (!m || fputs(metrics, m) < 0 || fclose(m))
            die("%s: write failed", metrics_path);
    }
    fputs(metrics, stdout);
    /* every allocation back before the exit (the final stage's auxiliary payload lies inside the candidates) */
    free(h.p);
    free(records.p);
    free(payloads.p);
    free(rec.p);
    free(pool.out);
    if (stage == 2)
        free(cand);
    else
        free(aux_c);
    free(pred);
    free(q);
    return 0;
}
