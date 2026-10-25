/* F-ASTRA SIREN Model 3 lossless encoder (PS side, C): the packet of gru_codec.encode, byte for byte.
 *
 * For counts q [T, C] (int32) with next-step predictions [T - 1, C] it compresses, per block of block_samples steps
 * and per channel, the counts (raw), the temporal deltas (delta, ZigZag) and the prediction residuals (gru, ZigZag)
 * with BZ2 (libbz2, BZ2_bzBuffToBuffCompress) and keeps the smallest (payload + its JSON record; ties raw -> delta ->
 * gru), then writes MAGIC, the header length, the JSON header (keys in gru_codec's order, separators "," and ":",
 * ASCII), the BZ2 auxiliary inputs aux[0 .. T - 2] and the chosen payloads. The predictions come from the GRU tail's
 * codebook indices on the board (--index and --codebooks: prediction = codebook[channel][index]) or directly
 * (--pred, for comparisons). The ground decoder (gru_codec.decode) runs the same integer predictor and restores q
 * exactly.
 *
 *   f-astra-gru-encode --q Q.i32 --aux AUX.f32 (--pred PRED.i32 | --index IDX.i32 --codebooks CB.bin)
 *       --samples T --channels C --channels-json JSON --scale S --model-sha256 HEX [--block B] [--level L]
 *       --out PACKET [--metrics METRICS.json] [--segment-steps L]
 *   (two stages, the same packet: --stage pre --candidates CAND without the predictions, --out or the codec header
 *    options, in the daily window; --stage final --candidates CAND without --aux in the NN window)
 *
 * Q.i32: T x C int32 LE; AUX.f32: T x 6 float32 LE; PRED.i32 and IDX.i32: (T - 1) x C int32 LE; CB.bin: per channel
 * a uint32 LE count and that many int32 LE values (gru_codec.codebook_bytes). JSON: the channel names as
 * json.dumps(..., separators=(",", ":"), ensure_ascii=True) writes them (gru_codec passes it verbatim).
 * Build: cc -std=gnu99 -O2 f_astra_gru_encode.c -l:libbz2.so.1 (or -lbz2); with bzlib.h define F_ASTRA_HAVE_BZLIB.
 */
#include <errno.h>
#include <inttypes.h>
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
    if (b->n + n > b->cap) {
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

static uint32_t crc32_of(const unsigned char *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
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
    if (n != count * 4)
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

/* one mode's column of a block and channel, BZ2-compressed */
static unsigned char *mode_payload(int m, const int32_t *q, const int32_t *pred, size_t cn, long start, long count,
                                   size_t ch, int level, unsigned char *col, unsigned int *got)
{
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

int main(int argc, char **argv)
{
    const char *q_path = 0, *aux_path = 0, *pred_path = 0, *idx_path = 0, *cb_path = 0, *out_path = 0, *metrics_path = 0;
    const char *channels_json = 0, *model = 0, *cand_path = 0, *stage_name = "all";
    long t_len = 0, c = 0, scale = -1, block = 1800, level = 5, segment = 0;
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
        else if (!strcmp(k, "--stage")) stage_name = v;
        else if (!strcmp(k, "--candidates")) cand_path = v;
        else die("unknown option %s", k);
    }
    int stage = !strcmp(stage_name, "all") ? 0 : !strcmp(stage_name, "pre") ? 1 : !strcmp(stage_name, "final") ? 2 : -1;
    if (stage < 0)
        die("--stage all, pre or final");
    if (!q_path || (stage != 2 && !aux_path) || (stage != 0 && !cand_path))
        die("--q, --aux (not in the final stage) and --candidates (pre and final stages) needed");
    if (stage != 1 && (!out_path || !channels_json || !model || (!pred_path == !(idx_path && cb_path))))
        die("--out, --channels-json, --model-sha256 and either --pred or --index with --codebooks needed");
    if (t_len < 2 || c < 1 || (stage != 1 && scale < 0) || block < 1 || level < 1 || level > 9 || segment < 0)
        die("invalid --samples, --channels, --scale, --block or --level");
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
    for (size_t i = 0; i < (size_t)t_len * cn; i++)
        le32(q_le + 4 * i, (uint32_t)q[i]);
    char q_hash[65];
    sha256_hex(q_le, (size_t)t_len * cn * 4, q_hash);
    free(q_le);
    /* the compressed auxiliary inputs: from the candidates in the final stage */
    unsigned char *aux_c = 0, *cand = 0;
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
        if (version != 1 || cs != (uint32_t)t_len || cc != (uint32_t)c || cb != (uint32_t)block || cl != (uint32_t)level)
            die("candidates: made for other samples, channels, block or level");
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
        aux_c = bz2(aux, n * 24, (int)level, &aux_c_n);
        aux_crc = crc32_of(aux_c, aux_c_n);
        free(aux);
    }
    /* the predictions: given, or the codebook value of the GRU tail's index */
    int32_t *pred = 0;
    if (stage == 1) {
        /* no predictions before the NN window */
    } else if (pred_path) {
        pred = read_i32(pred_path, n * cn);
    } else {
        int32_t *index = read_i32(idx_path, n * cn);
        size_t cb_n;
        unsigned char *cb = read_file(cb_path, &cb_n);
        int32_t **books = calloc(cn, sizeof *books);
        uint32_t *sizes = calloc(cn, sizeof *sizes);
        size_t off = 0;
        for (size_t ch = 0; ch < cn; ch++) {
            if (off + 4 > cb_n)
                die("codebooks: truncated");
            sizes[ch] = (uint32_t)cb[off] | (uint32_t)cb[off + 1] << 8 | (uint32_t)cb[off + 2] << 16 | (uint32_t)cb[off + 3] << 24;
            off += 4;
            if (off + (size_t)sizes[ch] * 4 > cb_n)
                die("codebooks: truncated");
            books[ch] = malloc((size_t)sizes[ch] * sizeof **books + 1);
            for (uint32_t k = 0; k < sizes[ch]; k++, off += 4)
                books[ch][k] = (int32_t)((uint32_t)cb[off] | (uint32_t)cb[off + 1] << 8 | (uint32_t)cb[off + 2] << 16 |
                                         (uint32_t)cb[off + 3] << 24);
        }
        if (off != cb_n)
            die("codebooks: extra bytes");
        pred = malloc(n * cn * sizeof *pred);
        for (size_t t = 0; t < n; t++)
            for (size_t ch = 0; ch < cn; ch++) {
                int32_t k = index[t * cn + ch];
                if (k < 0 || (uint32_t)k >= sizes[ch])
                    die("index %" PRId32 " out of channel %zu's codebook (%" PRIu32 ")", k, ch, sizes[ch]);
                pred[t * cn + ch] = books[ch][k];
            }
    }
    /* blocks and channels: the smallest of raw / delta / gru (payload bytes + record JSON bytes), ties the earlier */
    buf_t records = {0}, payloads = {0}, rec = {0}, pre = {0};
    long modes[3] = {0, 0, 0};
    static const char *NAMES[3] = {"raw", "delta", "gru"};
    unsigned char *col = malloc((size_t)block * 4);
    int first = 1;
    if (stage == 1) {
        put(&pre, CAND_MAGIC, 4);
        put32(&pre, 1); put32(&pre, (uint32_t)t_len); put32(&pre, (uint32_t)c); put32(&pre, (uint32_t)block);
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
                record_json(&rec, b, start, count, (long)ch, NAMES[best_mode], best_n, best_crc);
                best_total = best_n + rec.n;
            }
            for (int m = stage == 2 ? 2 : 0; m < (stage == 1 ? 2 : 3); m++) {
                unsigned int got;
                unsigned char *payload = mode_payload(m, q, pred, cn, start, count, ch, (int)level, col, &got);
                uint32_t crc = crc32_of(payload, got);
                rec.n = 0;
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
        return 0;
    }
    if (stage == 2 && coff != cand_n)
        die("candidates: extra bytes");
    buf_t h = {0};
    putf(&h, "{\"version\":1,\"codec\":\"bz2\",\"level\":%ld,\"dtype\":\"int32-le\",\"samples\":%ld,\"channels\":", level, t_len);
    put(&h, channels_json, strlen(channels_json));
    putf(&h, ",\"scale\":%ld,\"model_sha256\":\"", scale);
    put(&h, model, strlen(model));
    putf(&h, "\",\"predictor\":\"%s\",\"block_samples\":%ld,", PREDICTOR, block);
    if (segment)       /* the predictor's state was zero again every that many steps (gru_predictor.segmented) */
        putf(&h, "\"segment_steps\":%ld,", segment);
    putf(&h, "\"initial_q\":[");
    for (size_t ch = 0; ch < cn; ch++)
        putf(&h, ch ? ",%" PRId32 : "%" PRId32, q[ch]);
    putf(&h, "],\"aux_bytes\":%u,\"aux_crc32\":%" PRIu32 ",\"records\":[", aux_c_n, aux_crc);
    put(&h, records.p, records.n);
    putf(&h, "],\"q_sha256\":\"%s\"}", q_hash);
    unsigned char len[4];
    le32(len, (uint32_t)h.n);
    FILE *f = fopen(out_path, "wb");
    if (!f)
        die("%s: %s", out_path, strerror(errno));
    size_t packet = sizeof MAGIC + 4 + h.n + aux_c_n + payloads.n;
    if (fwrite(MAGIC, 1, sizeof MAGIC, f) != sizeof MAGIC || fwrite(len, 1, 4, f) != 4 || fwrite(h.p, 1, h.n, f) != h.n ||
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
    return 0;
}
