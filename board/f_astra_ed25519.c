// F-ASTRA payload crypto: SHA-256, SHA-512 and Ed25519 verification (RFC 8032). See f_astra_ed25519.h.
// Field elements of GF(2^255 - 19) are 16 signed 64-bit limbs of 16 bits (the TweetNaCl representation); points are
// extended coordinates (X, Y, Z, T). Curve constants are derived at first use from their definitions rather than
// written out. The hash round constants were generated with exact integer roots of the first primes (FIPS 180-4).
#include "f_astra_ed25519.h"

#include <string.h>

// ---------------------------------------------------------------- SHA-256

static const uint32_t K256[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

static const uint32_t H256[8] = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, k;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void fa_sha256_init(fa_sha256_ctx *c)
{
    memcpy(c->h, H256, sizeof H256);
    c->bytes = 0;
    c->used = 0;
}

void fa_sha256_update(fa_sha256_ctx *c, const uint8_t *data, size_t len)
{
    c->bytes += len;
    while (len) {
        size_t take = 64 - c->used < len ? 64 - c->used : len;
        memcpy(c->buf + c->used, data, take);
        c->used += take;
        data += take;
        len -= take;
        if (c->used == 64) {
            sha256_block(c->h, c->buf);
            c->used = 0;
        }
    }
}

void fa_sha256_final(fa_sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->bytes * 8;
    int i;
    c->buf[c->used++] = 0x80;
    if (c->used > 56) {
        memset(c->buf + c->used, 0, 64 - c->used);
        sha256_block(c->h, c->buf);
        c->used = 0;
    }
    memset(c->buf + c->used, 0, 56 - c->used);
    for (i = 0; i < 8; i++) c->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_block(c->h, c->buf);
    for (i = 0; i < 32; i++) out[i] = (uint8_t)(c->h[i / 4] >> (24 - 8 * (i % 4)));
}

void fa_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    fa_sha256_ctx c;
    fa_sha256_init(&c);
    fa_sha256_update(&c, data, len);
    fa_sha256_final(&c, out);
}

// ---------------------------------------------------------------- SHA-512

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

static const uint64_t H512[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(uint64_t h[8], const uint8_t *p)
{
    uint64_t w[80], a, b, c, d, e, f, g, k;
    int i, j;
    for (i = 0; i < 16; i++) {
        w[i] = 0;
        for (j = 0; j < 8; j++) w[i] = w[i] << 8 | p[8 * i + j];
    }
    for (i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; k = h[7];
    for (i = 0; i < 80; i++) {
        uint64_t t1 = k + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void fa_sha512_init(fa_sha512_ctx *c)
{
    memcpy(c->h, H512, sizeof H512);
    c->bytes = 0;
    c->used = 0;
}

void fa_sha512_update(fa_sha512_ctx *c, const uint8_t *data, size_t len)
{
    c->bytes += len;
    while (len) {
        size_t take = 128 - c->used < len ? 128 - c->used : len;
        memcpy(c->buf + c->used, data, take);
        c->used += take;
        data += take;
        len -= take;
        if (c->used == 128) {
            sha512_block(c->h, c->buf);
            c->used = 0;
        }
    }
}

void fa_sha512_final(fa_sha512_ctx *c, uint8_t out[64])
{
    uint64_t bits = c->bytes * 8;    // messages here stay far below 2^61 bytes: the high 64 length bits are 0
    int i;
    c->buf[c->used++] = 0x80;
    if (c->used > 112) {
        memset(c->buf + c->used, 0, 128 - c->used);
        sha512_block(c->h, c->buf);
        c->used = 0;
    }
    memset(c->buf + c->used, 0, 120 - c->used);
    for (i = 0; i < 8; i++) c->buf[120 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha512_block(c->h, c->buf);
    for (i = 0; i < 64; i++) out[i] = (uint8_t)(c->h[i / 8] >> (56 - 8 * (i % 8)));
}

void fa_sha512(const uint8_t *data, size_t len, uint8_t out[64])
{
    fa_sha512_ctx c;
    fa_sha512_init(&c);
    fa_sha512_update(&c, data, len);
    fa_sha512_final(&c, out);
}

// ---------------------------------------------------------------- GF(2^255 - 19)

typedef int64_t gf[16];

static gf gf0, gf1, D, D2, I, BX, BY;
static int ready;

static void fe_copy(gf r, const gf a)
{
    int i;
    for (i = 0; i < 16; i++) r[i] = a[i];
}

static void fe_small(gf r, uint32_t v)
{
    memset(r, 0, sizeof(gf));
    r[0] = v & 0xffff;
    r[1] = v >> 16;
}

static void fe_carry(gf o)
{
    int i;
    for (i = 0; i < 16; i++) {
        int64_t c;
        o[i] += (int64_t)1 << 16;
        c = o[i] >> 16;                       // arithmetic shift (gcc)
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}

static void fe_select(gf p, gf q, int b)      // swap p and q when b is 1
{
    int64_t t, c = ~((int64_t)b - 1);
    int i;
    for (i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void fe_pack(uint8_t *o, const gf n)   // fully reduced little-endian encoding
{
    int i, j, b;
    gf m, t;
    fe_copy(t, n);
    fe_carry(t);
    fe_carry(t);
    fe_carry(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        fe_select(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void fe_unpack(gf o, const uint8_t *n)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static int fe_differ(const gf a, const gf b)
{
    uint8_t c[32], d[32];
    fe_pack(c, a);
    fe_pack(d, b);
    return memcmp(c, d, 32) != 0;
}

static int fe_parity(const gf a)
{
    uint8_t d[32];
    fe_pack(d, a);
    return d[0] & 1;
}

static int fe_iszero(const gf a)
{
    uint8_t d[32], z[32] = {0};
    fe_pack(d, a);
    return memcmp(d, z, 32) == 0;
}

static void fe_add(gf o, const gf a, const gf b)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void fe_sub(gf o, const gf a, const gf b)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void fe_mul(gf o, const gf a, const gf b)
{
    int64_t t[31];
    int i, j;
    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    fe_carry(o);
    fe_carry(o);
}

static void fe_sq(gf o, const gf a)
{
    fe_mul(o, a, a);
}

static void fe_pow(gf o, const gf base, const uint8_t e[32])   // base^e, e little-endian
{
    gf r;
    int i;
    fe_copy(r, gf1);
    for (i = 255; i >= 0; i--) {
        fe_sq(r, r);
        if ((e[i >> 3] >> (i & 7)) & 1) fe_mul(r, r, base);
    }
    fe_copy(o, r);
}

static void fe_inv(gf o, const gf a)           // a^(p - 2)
{
    uint8_t e[32];
    memset(e, 0xff, 32);
    e[0] = 0xeb;
    e[31] = 0x7f;
    fe_pow(o, a, e);
}

static void fe_pow2523(gf o, const gf a)       // a^((p - 5) / 8) = a^(2^252 - 3)
{
    uint8_t e[32];
    memset(e, 0xff, 32);
    e[0] = 0xfd;
    e[31] = 0x0f;
    fe_pow(o, a, e);
}

// ---------------------------------------------------------------- edwards25519 points (X, Y, Z, T)

static void pt_add(gf p[4], gf q[4])           // p = p + q
{
    gf a, b, c, d, t, e, f, g, h;
    fe_sub(a, p[1], p[0]);
    fe_sub(t, q[1], q[0]);
    fe_mul(a, a, t);
    fe_add(b, p[0], p[1]);
    fe_add(t, q[0], q[1]);
    fe_mul(b, b, t);
    fe_mul(c, p[3], q[3]);
    fe_mul(c, c, D2);
    fe_mul(d, p[2], q[2]);
    fe_add(d, d, d);
    fe_sub(e, b, a);
    fe_sub(f, d, c);
    fe_add(g, d, c);
    fe_add(h, b, a);
    fe_mul(p[0], e, f);
    fe_mul(p[1], h, g);
    fe_mul(p[2], g, f);
    fe_mul(p[3], e, h);
}

static void pt_swap(gf p[4], gf q[4], int b)
{
    int i;
    for (i = 0; i < 4; i++) fe_select(p[i], q[i], b);
}

static void pt_pack(uint8_t r[32], gf p[4])
{
    gf tx, ty, zi;
    fe_inv(zi, p[2]);
    fe_mul(tx, p[0], zi);
    fe_mul(ty, p[1], zi);
    fe_pack(r, ty);
    r[31] ^= (uint8_t)(fe_parity(tx) << 7);
}

static void pt_scalarmult(gf p[4], gf q[4], const uint8_t s[32])   // p = [s] q; q is clobbered
{
    int i;
    fe_copy(p[0], gf0);
    fe_copy(p[1], gf1);
    fe_copy(p[2], gf1);
    fe_copy(p[3], gf0);
    for (i = 255; i >= 0; i--) {
        int b = (s[i >> 3] >> (i & 7)) & 1;
        pt_swap(p, q, b);
        pt_add(q, p);
        pt_add(p, p);
        pt_swap(p, q, b);
    }
}

static void pt_scalarbase(gf p[4], const uint8_t s[32])
{
    gf q[4];
    fe_copy(q[0], BX);
    fe_copy(q[1], BY);
    fe_copy(q[2], gf1);
    fe_mul(q[3], BX, BY);
    pt_scalarmult(p, q, s);
}

// Decode a point encoding (RFC 8032 5.1.3); negate stores -P. Returns -1 for a non-canonical y, a y with no x on the
// curve, or x = 0 with the sign bit set.
static int pt_decode(gf r[4], const uint8_t s[32], int negate)
{
    uint8_t y[32], canon[32];
    gf num, den, den2, den4, den6, t, c;
    int sign = s[31] >> 7;
    memcpy(y, s, 32);
    y[31] &= 0x7f;
    fe_copy(r[2], gf1);
    fe_unpack(r[1], y);
    fe_pack(canon, r[1]);
    if (memcmp(canon, y, 32)) return -1;
    fe_sq(num, r[1]);
    fe_mul(den, num, D);
    fe_sub(num, num, r[2]);                    // u = y^2 - 1
    fe_add(den, r[2], den);                    // v = d y^2 + 1
    fe_sq(den2, den);
    fe_sq(den4, den2);
    fe_mul(den6, den4, den2);
    fe_mul(t, den6, num);
    fe_mul(t, t, den);                         // u v^7
    fe_pow2523(t, t);
    fe_mul(t, t, num);
    fe_mul(t, t, den);
    fe_mul(t, t, den);
    fe_mul(r[0], t, den);                      // x = u v^3 (u v^7)^((p - 5) / 8)
    fe_sq(c, r[0]);
    fe_mul(c, c, den);
    if (fe_differ(c, num)) fe_mul(r[0], r[0], I);
    fe_sq(c, r[0]);
    fe_mul(c, c, den);
    if (fe_differ(c, num)) return -1;
    if (sign && fe_iszero(r[0])) return -1;
    if (fe_parity(r[0]) != sign) fe_sub(r[0], gf0, r[0]);
    if (negate) fe_sub(r[0], gf0, r[0]);
    fe_mul(r[3], r[0], r[1]);
    return 0;
}

static void setup(void)
{
    gf a, b, p[4];
    uint8_t e[32], y[32];
    if (ready) return;
    fe_small(gf0, 0);
    fe_small(gf1, 1);
    fe_small(a, 121665);
    fe_small(b, 121666);
    fe_inv(b, b);
    fe_mul(D, a, b);
    fe_sub(D, gf0, D);                         // d = -121665 / 121666
    fe_add(D2, D, D);
    memset(e, 0xff, 32);                       // sqrt(-1) = 2^((p - 1) / 4) = 2^(2^253 - 5)
    e[0] = 0xfb;
    e[31] = 0x1f;
    fe_small(a, 2);
    fe_pow(I, a, e);
    fe_small(a, 4);                            // base point: y = 4 / 5, x even
    fe_small(b, 5);
    fe_inv(b, b);
    fe_mul(BY, a, b);
    fe_pack(y, BY);
    if (pt_decode(p, y, 0) == 0) fe_copy(BX, p[0]);
    ready = 1;
}

int fa_ed25519_selftest(void)
{
    gf c, m1, p[4];
    uint8_t enc[32], want[32];
    setup();
    fe_sq(c, I);
    fe_sub(m1, gf0, gf1);
    if (fe_differ(c, m1)) return -1;
    memset(want, 0x66, 32);
    want[0] = 0x58;
    fe_copy(p[0], BX);
    fe_copy(p[1], BY);
    fe_copy(p[2], gf1);
    fe_mul(p[3], BX, BY);
    pt_pack(enc, p);
    return memcmp(enc, want, 32) ? -1 : 0;
}

// ---------------------------------------------------------------- scalars mod L = 2^252 + 27742317777372353535851937790883648493

static const uint8_t L_BYTES[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
};

static void limbs(uint32_t r[9], const uint8_t b[32])
{
    int i;
    for (i = 0; i < 8; i++)
        r[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 | (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
    r[8] = 0;
}

static int geq(const uint32_t a[9], const uint32_t b[9])
{
    int i;
    for (i = 8; i >= 0; i--)
        if (a[i] != b[i]) return a[i] > b[i];
    return 1;
}

static void sub(uint32_t a[9], const uint32_t b[9])
{
    uint64_t borrow = 0;
    int i;
    for (i = 0; i < 9; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 32) & 1;
    }
}

static void reduce_l(uint8_t out[32], const uint8_t in[64])   // in (512-bit little-endian) mod L
{
    uint32_t r[9] = {0}, l[9];
    int i, k;
    limbs(l, L_BYTES);
    for (i = 511; i >= 0; i--) {
        uint32_t carry = (in[i >> 3] >> (i & 7)) & 1;
        for (k = 0; k < 9; k++) {
            uint32_t next = r[k] >> 31;
            r[k] = r[k] << 1 | carry;
            carry = next;
        }
        if (geq(r, l)) sub(r, l);
    }
    for (i = 0; i < 32; i++) out[i] = (uint8_t)(r[i / 4] >> (8 * (i % 4)));
}

static int below_l(const uint8_t s[32])
{
    uint32_t a[9], l[9];
    limbs(a, s);
    limbs(l, L_BYTES);
    return !geq(a, l);
}

// ---------------------------------------------------------------- Ed25519 verification

int fa_ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len, const uint8_t pk[32])
{
    gf p[4], q[4];
    uint8_t h[64], k[32], t[32];
    fa_sha512_ctx c;
    setup();
    if (!below_l(sig + 32)) return -1;
    if (pt_decode(q, pk, 1)) return -1;       // q = -A
    fa_sha512_init(&c);
    fa_sha512_update(&c, sig, 32);
    fa_sha512_update(&c, pk, 32);
    fa_sha512_update(&c, msg, len);
    fa_sha512_final(&c, h);
    reduce_l(k, h);                            // k = H(R || A || M) mod L
    pt_scalarmult(p, q, k);                    // p = -[k] A
    pt_scalarbase(q, sig + 32);                // q = [S] B
    pt_add(p, q);                              // [S] B - [k] A must encode as R
    pt_pack(t, p);
    return memcmp(t, sig, 32) ? -1 : 0;
}
