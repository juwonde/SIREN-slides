// F-ASTRA payload crypto for weight-update packages: SHA-256 (payload and chunk hashes), SHA-512 and Ed25519
// signature verification (RFC 8032, pure Ed25519). Self-contained (no OpenSSL) so the payload loader carries its own
// verifier. Verification handles only public data, so it is not written to be constant-time.
#ifndef F_ASTRA_ED25519_H
#define F_ASTRA_ED25519_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t buf[64];
    size_t used;
} fa_sha256_ctx;

typedef struct {
    uint64_t h[8];
    uint64_t bytes;
    uint8_t buf[128];
    size_t used;
} fa_sha512_ctx;

void fa_sha256_init(fa_sha256_ctx *c);
void fa_sha256_update(fa_sha256_ctx *c, const uint8_t *data, size_t len);
void fa_sha256_final(fa_sha256_ctx *c, uint8_t out[32]);
void fa_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

void fa_sha512_init(fa_sha512_ctx *c);
void fa_sha512_update(fa_sha512_ctx *c, const uint8_t *data, size_t len);
void fa_sha512_final(fa_sha512_ctx *c, uint8_t out[64]);
void fa_sha512(const uint8_t *data, size_t len, uint8_t out[64]);

// 0 when sig (R || S) is a valid Ed25519 signature of msg under the 32-byte public key pk, -1 otherwise. Rejects a
// non-canonical S (S >= L), a non-canonical or off-curve public key and a public key encoding x = 0 with the sign bit.
int fa_ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len, const uint8_t pk[32]);

// Internal consistency of the curve constants (base point encoding, sqrt(-1)); 0 when they hold.
int fa_ed25519_selftest(void);

#endif
