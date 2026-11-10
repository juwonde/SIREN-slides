// TE0715 secure monitor, TrustZone T3d: SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104) for the FM's authenticated
// services (signed golden manifests, the ground's Safehold release). Freestanding: no C library (the monitor is built
// with -nostdlib -fno-builtin), byte loops only, so it also builds on the host for the test against Python's hashlib.
#include "fm_auth.h"

static const uint32_t k256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(struct fm_sha256 *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + k256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void fm_sha256_init(struct fm_sha256 *s)
{
    static const uint32_t h0[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    for (int i = 0; i < 8; i++) s->h[i] = h0[i];
    s->bytes = 0;
    s->fill = 0;
}

// `data` may be uncached device memory (the non-secure pool): read byte by byte through a volatile pointer
void fm_sha256_update(struct fm_sha256 *s, const volatile uint8_t *data, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        s->buf[s->fill++] = data[i];
        if (s->fill == 64) { block(s, s->buf); s->fill = 0; }
    }
    s->bytes += n;
}

void fm_sha256_final(struct fm_sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->bytes * 8u;
    uint8_t one = 0x80, zero = 0, len[8];
    fm_sha256_update(s, &one, 1);
    while (s->fill != 56) fm_sha256_update(s, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    fm_sha256_update(s, len, 8);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

// HMAC-SHA256 of the concatenation of `parts` (key of 32 bytes)
void fm_hmac_sha256(const uint8_t key[32], const struct fm_part *parts, int nparts, uint8_t out[32])
{
    struct fm_sha256 s;
    uint8_t pad[64], inner[32];
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)((i < 32 ? key[i] : 0) ^ 0x36);
    fm_sha256_init(&s);
    fm_sha256_update(&s, pad, 64);
    for (int i = 0; i < nparts; i++) fm_sha256_update(&s, parts[i].data, parts[i].bytes);
    fm_sha256_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = (uint8_t)((i < 32 ? key[i] : 0) ^ 0x5c);
    fm_sha256_init(&s);
    fm_sha256_update(&s, pad, 64);
    fm_sha256_update(&s, inner, 32);
    fm_sha256_final(&s, out);
}

// equal without an early exit (the time does not tell how many leading bytes of a forged tag were right)
int fm_equal32(const volatile uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}
