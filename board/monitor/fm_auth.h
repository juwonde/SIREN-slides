// TE0715 secure monitor, TrustZone T3d: SHA-256 and HMAC-SHA256 (fm_auth.c).
#ifndef FM_AUTH_H
#define FM_AUTH_H
#include <stdint.h>

struct fm_sha256 {
    uint32_t h[8];
    uint64_t bytes;
    uint32_t fill;
    uint8_t buf[64];
};
struct fm_part {
    const volatile uint8_t *data;
    uint32_t bytes;
};

void fm_sha256_init(struct fm_sha256 *s);
void fm_sha256_update(struct fm_sha256 *s, const volatile uint8_t *data, uint32_t n);
void fm_sha256_final(struct fm_sha256 *s, uint8_t out[32]);
void fm_hmac_sha256(const uint8_t key[32], const struct fm_part *parts, int nparts, uint8_t out[32]);
int fm_equal32(const volatile uint8_t *a, const uint8_t *b);

// The messages the ground signs (each starts with an 8-byte label):
//   golden manifest  "FAGOLD1" 0, slot (u32 LE), bytes (u32 LE), SHA-256 of the golden data
//   Safehold release "FAREL1" 0 0, the monitor's challenge (8 bytes)
//   PL windows       "FAPLWIN" 0, count (u32 LE), base and bytes per window (u32 LE)
#define FM_LABEL_GOLDEN "FAGOLD1"
#define FM_LABEL_RELEASE "FAREL1\0"
#define FM_LABEL_NONCE "FANONC1"
#define FM_LABEL_WINDOWS "FAPLWIN"
#endif
