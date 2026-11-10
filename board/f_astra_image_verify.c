// F-ASTRA boot image verifier (FM L2-G2-02 on the TE0715 SD boot). A recovery or update image.ub is installed only
// after this check passes: the manifest's Ed25519 signature verifies under a compiled-in trusted key
// (f_astra_weights_keys.h, the weight-update keys), the image matches the manifest's SHA-256 and size, the manifest
// names this board's target (its hostname), and its sequence is above the last accepted image sequence (ACCEPTED, a
// decimal in a file; a missing file is 0), so an older signed image cannot be replayed.
// usage: f-astra-image-verify MANIFEST SIGNATURE IMAGE ACCEPTED
// The manifest is text, signed byte for byte, one key=value per line: schema=f-astra/boot-image/v1, target,
// image_sha256, image_bytes, sequence, key_id. Prints one JSON line; exit 0 accepted, 1 refused, 2 usage or I/O.
// The ACCEPTED file is written by the ground after the new image passed its Boot Health, never here.
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "f_astra_ed25519.h"
#include "f_astra_weights_keys.h"   // TRUSTED_KEYS[][32], TRUSTED_KEY_COUNT (generated per build)

#define MANIFEST_MAX 4096
#define SCHEMA "f-astra/boot-image/v1"

static const char *value(const char *text, const char *key, char *out, size_t size)
{
    char pattern[40];
    const char *at;
    size_t n = 0;
    snprintf(pattern, sizeof(pattern), "%s=", key);
    for (at = text; (at = strstr(at, pattern)); at++)
        if (at == text || at[-1] == '\n') break;
    if (!at) return NULL;
    for (at += strlen(pattern); *at && *at != '\n' && n + 1 < size; at++) out[n++] = *at;
    out[n] = 0;
    return out;
}

static void hex(const uint8_t *d, size_t n, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[2 * n] = 0;
}

static int refuse(const char *reason)
{
    printf("{\"status\":\"refused\",\"reason\":\"%s\"}\n", reason);
    return 1;
}

int main(int argc, char **argv)
{
    char manifest[MANIFEST_MAX + 1], field[128], id[65], kid[17] = "", digest_hex[65], host[64] = "";
    uint8_t sig[64], digest[32], buf[65536];
    unsigned long long sequence, accepted = 0, bytes, size = 0;
    size_t m;
    int k, trusted = -1;
    FILE *f;
    fa_sha256_ctx ctx;
    if (argc != 5) { fprintf(stderr, "usage: f-astra-image-verify MANIFEST SIGNATURE IMAGE ACCEPTED\n"); return 2; }
    if (fa_ed25519_selftest()) { printf("{\"status\":\"error\",\"reason\":\"crypto self-test\"}\n"); return 2; }
    if (!(f = fopen(argv[1], "rb"))) return 2;
    m = fread(manifest, 1, MANIFEST_MAX + 1, f);
    fclose(f);
    if (m == 0 || m > MANIFEST_MAX) return refuse("manifest size");
    manifest[m] = 0;
    if (!(f = fopen(argv[2], "rb"))) return 2;
    k = (int)fread(sig, 1, sizeof(sig), f) == (int)sizeof(sig) && fgetc(f) == EOF;
    fclose(f);
    if (!k) return refuse("signature size");
    if (!value(manifest, "schema", field, sizeof(field)) || strcmp(field, SCHEMA)) return refuse("schema");
    if (!value(manifest, "key_id", field, sizeof(field))) return refuse("key_id");
    for (k = 0; k < TRUSTED_KEY_COUNT; k++) {
        uint8_t h[32];
        fa_sha256(TRUSTED_KEYS[k], 32, h);
        hex(h, 32, id);
        id[16] = 0;
        if (!strcmp(id, field)) { trusted = k; memcpy(kid, id, sizeof(kid)); }
    }
    if (trusted < 0) return refuse("untrusted key");
    if (fa_ed25519_verify(sig, (const uint8_t *)manifest, m, TRUSTED_KEYS[trusted])) return refuse("bad signature");
    // only signed fields from here on
    if (gethostname(host, sizeof(host) - 1) || !value(manifest, "target", field, sizeof(field)) || strcmp(field, host))
        return refuse("target");
    if (!value(manifest, "sequence", field, sizeof(field))) return refuse("sequence");
    sequence = strtoull(field, NULL, 10);
    if ((f = fopen(argv[4], "r"))) {
        if (fscanf(f, "%llu", &accepted) != 1) accepted = 0;
        fclose(f);
    }
    if (sequence <= accepted) {
        printf("{\"status\":\"refused\",\"reason\":\"stale sequence\",\"sequence\":%llu,\"accepted\":%llu}\n", sequence,
               accepted);
        return 1;
    }
    if (!value(manifest, "image_bytes", field, sizeof(field))) return refuse("image_bytes");
    bytes = strtoull(field, NULL, 10);
    if (!(f = fopen(argv[3], "rb"))) return 2;
    fa_sha256_init(&ctx);
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0; size += n) fa_sha256_update(&ctx, buf, n);
    fclose(f);
    fa_sha256_final(&ctx, digest);
    hex(digest, 32, digest_hex);
    if (size != bytes || !value(manifest, "image_sha256", field, sizeof(field)) || strcmp(field, digest_hex))
        return refuse("image differs from the manifest");
    printf("{\"status\":\"accepted\",\"key_id\":\"%s\",\"sequence\":%llu,\"accepted_before\":%llu,\"image_sha256\":\"%s\","
           "\"image_bytes\":%llu}\n", kid, sequence, accepted, digest_hex, size);
    return 0;
}
