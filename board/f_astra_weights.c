// F-ASTRA payload weight-update tool (docs/weight-update-design.md). Banks under ROOT (an SD folder):
//   bank-a/, bank-b/   package.fawp (base.fawp for a partial package) and state ("writing N", "installed N",
//                      "verified N" or "bad N", with a check line); every file is written in place (write_file)
//   active.1, active.2 the active bank (two copies, each with a SHA-256 check line)
//   accepted.1, .2     the highest accepted package sequence (replays are refused)
// Commands (one JSON line on stdout; exit 0 ok, 3 incomplete upload, 4 refused or failed check, 5 no usable bank, 2 setup):
//   install INCOMING ROOT   INCOMING holds head.bin (header, manifest, signature) and chunk-NNNNN.bin (64 KB payload
//                           chunks). Checks the signature (compiled-in trusted keys), the sequence and every chunk
//                           (missing or damaged chunks are listed for a resend), then the whole package, and writes it
//                           to the inactive bank.
//   activate ROOT           points the active copies at the newest installed bank
//   stage ROOT SESSION      checks the active bank again (signature, hashes, the session's bitstream and run lines) and
//                           writes its files into the session folder (the layout: weight_package.LAYOUT_FIELDS); a bank that fails is marked bad and the other
//                           bank is tried; with no usable bank nothing is written (the bundle's own weights stay)
//   mark ROOT verified|bad  the golden result of the active bank; bad also switches back to the other bank
//   status ROOT             pointers, banks and the accepted sequence
//   verify PACKAGE          every check of a package file except the target
// Partial packages (P2 S3): a manifest line "base SEQUENCE MANIFEST_SHA256" names the full package the partial one
// applies on top of. install takes it only when the active bank holds exactly that package (its manifest hash and
// sequence) and copies that package into the new bank as base.fawp; stage writes the base's files first and the
// partial package's files over them, so the models the partial package does not carry keep their base weights.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32                      // host unit tests only (the payload build is Linux)
#include <direct.h>
#include <io.h>
#define fsync _commit
#define mkdir(path, mode) _mkdir(path)
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

#include "f_astra_ed25519.h"
#include "f_astra_weights_keys.h"   // TRUSTED_KEYS[][32], TRUSTED_KEY_COUNT (generated per bundle)

#define HEADER_BYTES 32
#define SIG_BYTES 64
#define CHUNK_BYTES 65536
#define MAX_FILES 48
#define MAX_ASSEMBLE 8
#define MAX_PARTS 4
#define MAX_CHUNKS 512
#define MAX_PACKAGE (24u << 20)

typedef struct { char name[72]; char kind; uint32_t offset, length; char sha[65]; } entry_t;
typedef struct { char name[72]; uint32_t length; char sha[65]; int parts; char part[MAX_PARTS][72]; uint32_t at[MAX_PARTS]; } assemble_t;
typedef struct {
    uint64_t sequence, base_sequence;
    int has_base;
    char base_manifest[65];
    char key_id[17];
    char target_bitstream[65], target_layout[65];
    int files, assembles, chunks;
    entry_t file[MAX_FILES];
    assemble_t assemble[MAX_ASSEMBLE];
    char chunk[MAX_CHUNKS][65];
} manifest_t;

static manifest_t man;

static void out_json(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void out_json(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void hexs(const uint8_t *d, size_t n, char *out)
{
    static const char h[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[2 * n] = 0;
}

static void sha256_hex(const uint8_t *d, size_t n, char out[65])
{
    uint8_t h[32];
    fa_sha256(d, n, h);
    hexs(h, 32, out);
}

static int valid_name(const char *s)
{
    size_t i, n = strlen(s);
    if (n == 0 || n > 64) return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (i && (c == '.' || c == '_' || c == '-'));
        if (!ok) return 0;
    }
    return 1;
}

static uint8_t *read_file(const char *path, size_t *len, size_t cap)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || (size_t)n > cap || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void sync_dir(const char *path)
{
    char dir[512];
    char *slash;
    int fd;
    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '/');
    if (slash) *slash = 0; else snprintf(dir, sizeof dir, ".");
    fd = open(dir, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
}

// write path in place: open (created if missing), write, cut to the new length, fsync. No temporary file and no
// rename: on the SD card's FAT32 partition, temporary files renamed over their targets left two names sharing one 8.3
// short name (2026-10-14, the boot partition; F-ASTRA 6.73.0). A write cut short leaves a torn file, so every reader
// checks: the state files carry a check line (and the pointers two copies), a bank is marked "writing" while its
// package is written, and a package is checked in full (signature, hashes) before it is used.
static int write_file(const char *path, const uint8_t *data, size_t len)
{
    size_t done = 0;
    struct stat st;
    int created = stat(path, &st) != 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_BINARY, 0644);
    if (fd < 0) return -1;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n <= 0) { close(fd); return -1; }
        done += (size_t)n;
    }
    if (ftruncate(fd, (off_t)len) || fsync(fd)) { close(fd); return -1; }
    if (close(fd)) return -1;
    if (created) sync_dir(path);    // a new directory entry
    return 0;
}

static uint32_t u32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// header fields of package bytes: manifest, signature and payload lengths
static int header(const uint8_t *d, size_t n, uint32_t *m, uint32_t *s, uint32_t *p)
{
    if (n < HEADER_BYTES || memcmp(d, "FAWP", 4) || u32le(d + 4) != 1) return -1;
    *m = u32le(d + 8); *s = u32le(d + 12); *p = u32le(d + 16);
    if (*s != SIG_BYTES || *m > (1u << 20)) return -1;
    return 0;
}

static int parse_manifest(const uint8_t *text, size_t n)
{
    char *copy = malloc(n + 1), *line, *save = NULL;
    int first = 1, ended = 0;
    if (!copy) return -1;
    memcpy(copy, text, n);
    copy[n] = 0;
    memset(&man, 0, sizeof man);
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char a[80], b[80], c[80], d[80], e[80];
        if (ended) goto bad;
        if (first) { if (strcmp(line, "FAWP-MANIFEST 1")) goto bad; first = 0; continue; }
        if (!strcmp(line, "end")) { ended = 1; continue; }
        if (sscanf(line, "sequence %llu", (unsigned long long *)&man.sequence) == 1) continue;
        if (sscanf(line, "key_id %16s", man.key_id) == 1) continue;
        if (!strncmp(line, "created ", 8)) continue;
        if (sscanf(line, "chunk_size %79s", a) == 1) { if (atoi(a) != CHUNK_BYTES) goto bad; continue; }
        if (!strncmp(line, "base ", 5)) {
            unsigned long long bs;
            if (man.has_base || sscanf(line, "base %llu %79s", &bs, a) != 2 || strlen(a) != 64 || !bs) goto bad;
            man.has_base = 1;
            man.base_sequence = bs;
            strcpy(man.base_manifest, a);
            continue;
        }
        if (sscanf(line, "target %79s %79s", a, b) == 2) {
            if (strlen(b) != 64) goto bad;
            if (!strcmp(a, "bitstream")) strcpy(man.target_bitstream, b);
            else if (!strcmp(a, "layout")) strcpy(man.target_layout, b);
            continue;
        }
        if ((line[0] == 'f' || line[0] == 'p') && sscanf(line, "%79s %79s %79s %79s %79s", d, a, b, c, e) == 5) {
            entry_t *f;
            if (man.files == MAX_FILES || !valid_name(a) || strlen(e) != 64) goto bad;
            if (strcmp(d, "file") && strcmp(d, "part")) goto bad;
            f = &man.file[man.files++];
            strcpy(f->name, a);
            f->kind = d[0];
            f->offset = (uint32_t)strtoul(b, NULL, 10);
            f->length = (uint32_t)strtoul(c, NULL, 10);
            strcpy(f->sha, e);
            continue;
        }
        if (!strncmp(line, "assemble ", 9)) {
            assemble_t *x;
            char *tok, *s2 = NULL;
            if (man.assembles == MAX_ASSEMBLE) goto bad;
            x = &man.assemble[man.assembles++];
            tok = strtok_r(line + 9, " ", &s2);
            if (!tok || !valid_name(tok)) goto bad;
            strcpy(x->name, tok);
            tok = strtok_r(NULL, " ", &s2);
            if (!tok) goto bad;
            x->length = (uint32_t)strtoul(tok, NULL, 10);
            tok = strtok_r(NULL, " ", &s2);
            if (!tok || strlen(tok) != 64) goto bad;
            strcpy(x->sha, tok);
            while ((tok = strtok_r(NULL, " ", &s2))) {
                char *at = strchr(tok, '@');
                if (!at || x->parts == MAX_PARTS) goto bad;
                *at = 0;
                if (!valid_name(tok)) goto bad;
                strcpy(x->part[x->parts], tok);
                x->at[x->parts++] = (uint32_t)strtoul(at + 1, NULL, 10);
            }
            continue;
        }
        if (sscanf(line, "chunk %79s %79s", a, b) == 2) {
            if (man.chunks == MAX_CHUNKS || atoi(a) != man.chunks || strlen(b) != 64) goto bad;
            strcpy(man.chunk[man.chunks++], b);
            continue;
        }
        goto bad;
    }
    free(copy);
    return (ended && man.sequence && strlen(man.key_id) == 16 && man.chunks) ? 0 : -1;
bad:
    free(copy);
    return -1;
}

// signature over the manifest by a compiled-in trusted key whose id is the manifest's key_id
static int check_signature(const uint8_t *manifest, uint32_t m, const uint8_t *sig)
{
    int k;
    for (k = 0; k < TRUSTED_KEY_COUNT; k++) {
        char id[65];
        sha256_hex(TRUSTED_KEYS[k], 32, id);
        id[16] = 0;
        if (!strcmp(id, man.key_id)) return fa_ed25519_verify(sig, manifest, m, TRUSTED_KEYS[k]);
    }
    return -1;
}

static const entry_t *find_file(const char *name)
{
    int i;
    for (i = 0; i < man.files; i++)
        if (!strcmp(man.file[i].name, name)) return &man.file[i];
    return NULL;
}

// chunk, file and assembled-image hashes of a payload; the manifest must already be parsed
static const char *check_payload(const uint8_t *payload, uint32_t p)
{
    char h[65];
    uint32_t end = 0;
    int i, j;
    if ((uint32_t)man.chunks != (p + CHUNK_BYTES - 1) / CHUNK_BYTES) return "chunk count";
    for (i = 0; i < man.chunks; i++) {
        uint32_t o = (uint32_t)i * CHUNK_BYTES, n = p - o < CHUNK_BYTES ? p - o : CHUNK_BYTES;
        sha256_hex(payload + o, n, h);
        if (strcmp(h, man.chunk[i])) return "chunk hash";
    }
    for (i = 0; i < man.files; i++) {
        const entry_t *e = &man.file[i];
        if (e->offset > p || e->length > p - e->offset) return "file outside the payload";
        sha256_hex(payload + e->offset, e->length, h);
        if (strcmp(h, e->sha)) return "file hash";
        if (e->offset + e->length > end) end = e->offset + e->length;
    }
    if (end != p) return "payload length";
    for (i = 0; i < man.assembles; i++) {
        const assemble_t *x = &man.assemble[i];
        uint8_t *img = calloc(1, x->length ? x->length : 1);
        if (!img) return "memory";
        for (j = 0; j < x->parts; j++) {
            const entry_t *e = find_file(x->part[j]);
            if (!e || e->kind != 'p' || x->at[j] > x->length || e->length > x->length - x->at[j]) { free(img); return "assembled part"; }
            memcpy(img + x->at[j], payload + e->offset, e->length);
        }
        sha256_hex(img, x->length, h);
        free(img);
        if (strcmp(h, x->sha)) return "assembled image hash";
    }
    return NULL;
}

// whole package bytes: header, signature, manifest, payload; NULL when every check holds
static const char *check_package(const uint8_t *d, size_t n)
{
    uint32_t m, s, p;
    if (header(d, n, &m, &s, &p) || n != (size_t)HEADER_BYTES + m + s + p) return "header";
    if (parse_manifest(d + HEADER_BYTES, m)) return "manifest";
    if (check_signature(d + HEADER_BYTES, m, d + HEADER_BYTES + m)) return "signature";
    return check_payload(d + HEADER_BYTES + m + s, p);
}

// SHA-256 of the manifest bytes of package bytes (what a partial package's base line names)
static int manifest_hash(const uint8_t *d, size_t n, char out[65])
{
    uint32_t m, s, p;
    if (header(d, n, &m, &s, &p) || n < (size_t)HEADER_BYTES + m) return -1;
    sha256_hex(d + HEADER_BYTES, m, out);
    return 0;
}

static manifest_t partial_man;   // a partial package's manifest while its base is checked or staged

// the base of the partial package in `man`: every package check, a full package, the sequence and manifest hash the
// base line names and the same target; `man` is the partial package's manifest again afterwards
static const char *check_base(const uint8_t *base, size_t n)
{
    char h[65];
    const char *why;
    partial_man = man;
    why = check_package(base, n);
    if (!why && man.has_base) why = "base is partial";
    if (!why && (manifest_hash(base, n, h) || strcmp(h, partial_man.base_manifest) || man.sequence != partial_man.base_sequence))
        why = "base";
    if (!why && (strcmp(man.target_bitstream, partial_man.target_bitstream) || strcmp(man.target_layout, partial_man.target_layout)))
        why = "base target";
    man = partial_man;
    return why;
}

// ---------------------------------------------------------------- small state files (two copies with a check line)

static int read_record(const char *path, char *value, size_t cap)
{
    char buf[256], body[200], check[65], want[65];
    FILE *f = fopen(path, "r");
    size_t n;
    char *nl;
    if (!f) return -1;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    nl = strstr(buf, "\ncheck ");
    if (!nl || (size_t)(nl - buf) + 1 >= sizeof body) return -1;
    memcpy(body, buf, (size_t)(nl - buf) + 1);
    body[nl - buf + 1] = 0;
    if (sscanf(nl + 7, "%64s", check) != 1) return -1;
    sha256_hex((const uint8_t *)body, strlen(body), want);
    want[16] = 0;
    if (strcmp(want, check) || strlen(body) >= cap) return -1;
    strcpy(value, body);
    return 0;
}

static int write_record(const char *path, const char *body)
{
    char buf[256], check[65];
    sha256_hex((const uint8_t *)body, strlen(body), check);
    check[16] = 0;
    snprintf(buf, sizeof buf, "%scheck %s\n", body, check);
    return write_file(path, (const uint8_t *)buf, strlen(buf));
}

static int write_both(const char *root, const char *name, const char *body)
{
    char p1[512], p2[512];
    snprintf(p1, sizeof p1, "%s/%s.1", root, name);
    snprintf(p2, sizeof p2, "%s/%s.2", root, name);
    return write_record(p1, body) || write_record(p2, body);
}

// the valid copy (the newer sequence when both are valid and differ); body "bank X\nsequence N\n" or "sequence N\n"
static int read_both(const char *root, const char *name, char bank[8], uint64_t *seq)
{
    char p[512], body[200];
    int i, found = 0;
    *seq = 0;
    bank[0] = 0;
    for (i = 1; i <= 2; i++) {
        char b[8] = "";
        unsigned long long s = 0;
        snprintf(p, sizeof p, "%s/%s.%d", root, name, i);
        if (read_record(p, body, sizeof body)) continue;
        if (!strncmp(body, "bank ", 5) ? sscanf(body, "bank %7s\nsequence %llu", b, &s) != 2 : sscanf(body, "sequence %llu", &s) != 1)
            continue;
        if (!found || s > *seq) { *seq = s; snprintf(bank, 8, "%s", b); }
        found = 1;
    }
    return found ? 0 : -1;
}

// a bank's state: "<state> <sequence>\n" and a check line (writing, installed, verified or bad). A state file written
// before 6.98.0 has no check line; it is taken only when the whole file is exactly that line. A torn file is no state.
static int bank_state(const char *root, const char *bank, char state[16], uint64_t *seq)
{
    char p[512], body[200], again[200];
    unsigned long long s = 0;
    snprintf(p, sizeof p, "%s/bank-%s/state", root, bank);
    state[0] = 0;
    *seq = 0;
    if (read_record(p, body, sizeof body)) {
        FILE *f = fopen(p, "r");
        size_t n;
        if (!f) return -1;
        n = fread(body, 1, sizeof body - 1, f);
        fclose(f);
        body[n] = 0;
    }
    if (sscanf(body, "%15s %llu", state, &s) != 2) { state[0] = 0; return -1; }
    snprintf(again, sizeof again, "%s %llu\n", state, s);
    if (strcmp(again, body) || (strcmp(state, "writing") && strcmp(state, "installed") && strcmp(state, "verified")
                                && strcmp(state, "bad"))) {
        state[0] = 0;
        return -1;
    }
    *seq = s;
    return 0;
}

static int set_bank_state(const char *root, const char *bank, const char *state, uint64_t seq)
{
    char p[512], body[64];
    snprintf(p, sizeof p, "%s/bank-%s/state", root, bank);
    snprintf(body, sizeof body, "%s %llu\n", state, (unsigned long long)seq);
    return write_record(p, body);
}

// a bank can be staged: its package was written in full and was not found bad
static int usable(const char *state) { return !strcmp(state, "installed") || !strcmp(state, "verified"); }

static int set_active(const char *root, const char *bank, uint64_t seq)
{
    char body[64];
    snprintf(body, sizeof body, "bank %s\nsequence %llu\n", bank, (unsigned long long)seq);
    return write_both(root, "active", body);
}

static const char *other(const char *bank) { return bank[0] == 'a' ? "b" : "a"; }

// ---------------------------------------------------------------- commands

static int cmd_install(const char *incoming, const char *root)
{
    char path[600], missing[4096] = "", damaged[4096] = "", active[8] = "", h[65];
    uint8_t *head, *pkg, *base = NULL;
    size_t hn, total, bn = 0;
    uint32_t m, s, p;
    uint64_t accepted, aseq;
    const char *why, *bank;
    int i, gaps = 0;
    snprintf(path, sizeof path, "%s/head.bin", incoming);
    head = read_file(path, &hn, (1u << 20) + 4096);
    if (!head || header(head, hn, &m, &s, &p) || hn != (size_t)HEADER_BYTES + m + s || p > MAX_PACKAGE) {
        out_json("{\"status\":\"refused\",\"reason\":\"head\"}");
        return 4;
    }
    if (parse_manifest(head + HEADER_BYTES, m)) { out_json("{\"status\":\"refused\",\"reason\":\"manifest\"}"); return 4; }
    if (check_signature(head + HEADER_BYTES, m, head + HEADER_BYTES + m)) {
        out_json("{\"status\":\"refused\",\"reason\":\"signature\"}");
        return 4;
    }
    read_both(root, "accepted", active, &accepted);
    if (man.sequence <= accepted) {
        out_json("{\"status\":\"refused\",\"reason\":\"sequence\",\"sequence\":%llu,\"accepted\":%llu}",
                 (unsigned long long)man.sequence, (unsigned long long)accepted);
        return 4;
    }
    // the chunk count before the chunks are placed (check_payload repeats it): a signed manifest with more chunks than
    // the payload needs would otherwise copy past the end of pkg
    if ((uint32_t)man.chunks != (p + CHUNK_BYTES - 1) / CHUNK_BYTES) {
        out_json("{\"status\":\"refused\",\"reason\":\"chunk count\"}");
        return 4;
    }
    total = hn + p;
    pkg = malloc(total ? total : 1);
    if (!pkg) return 2;
    memcpy(pkg, head, hn);
    for (i = 0; i < man.chunks; i++) {
        uint32_t want = p - (uint32_t)i * CHUNK_BYTES < CHUNK_BYTES ? p - (uint32_t)i * CHUNK_BYTES : CHUNK_BYTES;
        size_t cn;
        uint8_t *chunk;
        snprintf(path, sizeof path, "%s/chunk-%05d.bin", incoming, i);
        chunk = read_file(path, &cn, CHUNK_BYTES);
        if (!chunk) {
            if (strlen(missing) < sizeof missing - 16) snprintf(missing + strlen(missing), sizeof missing - strlen(missing), "%s%d", gaps ? "," : "", i);
            gaps++;
            continue;
        }
        sha256_hex(chunk, cn, h);
        if (cn != want || strcmp(h, man.chunk[i])) {
            if (strlen(damaged) < sizeof damaged - 16) snprintf(damaged + strlen(damaged), sizeof damaged - strlen(damaged), "%s%d", damaged[0] ? "," : "", i);
            gaps++;
        } else {
            memcpy(pkg + hn + (size_t)i * CHUNK_BYTES, chunk, cn);
        }
        free(chunk);
    }
    if (gaps) {
        out_json("{\"status\":\"incomplete\",\"sequence\":%llu,\"chunks\":%d,\"missing\":[%s],\"damaged\":[%s]}",
                 (unsigned long long)man.sequence, man.chunks, missing, damaged);
        return 3;
    }
    if ((why = check_package(pkg, total))) { out_json("{\"status\":\"refused\",\"reason\":\"%s\"}", why); return 4; }
    bank = read_both(root, "active", active, &aseq) == 0 && active[0] ? other(active) : "a";
    if (man.has_base) {
        // a partial package: the active bank must hold exactly its base, which goes into the new bank with it
        char state[16];
        uint64_t sseq;
        if (!active[0] || bank_state(root, active, state, &sseq) || !usable(state)) {
            out_json("{\"status\":\"refused\",\"reason\":\"base: no usable active bank\"}");
            return 4;
        }
        snprintf(path, sizeof path, "%s/bank-%s/package.fawp", root, active);
        base = read_file(path, &bn, MAX_PACKAGE + (2u << 20));
        if (!base || (why = check_base(base, bn))) {
            out_json("{\"status\":\"refused\",\"reason\":\"%s\"}", base ? why : "base missing");
            return 4;
        }
    }
    snprintf(path, sizeof path, "%s/bank-%s", root, bank);
    mkdir(root, 0755);
    mkdir(path, 0755);
    // the bank's files are written in place: until the new package is written and checked, the bank is not usable
    // (an install cut short leaves "writing"; stage and activate skip it, the active bank is untouched)
    if (set_bank_state(root, bank, "writing", man.sequence)) { out_json("{\"status\":\"error\",\"reason\":\"state\"}"); return 2; }
    snprintf(path, sizeof path, "%s/bank-%s/base.fawp", root, bank);
    if (base) {
        if (write_file(path, base, bn)) { out_json("{\"status\":\"error\",\"reason\":\"write base\"}"); return 2; }
        free(base);
        base = read_file(path, &bn, MAX_PACKAGE + (2u << 20));
        if (!base || (why = check_base(base, bn))) {
            out_json("{\"status\":\"error\",\"reason\":\"written base: %s\"}", base ? why : "read");
            return 4;
        }
        free(base);
    } else {
        unlink(path);                                     // a full package: no base (a stale one of an older partial goes)
    }
    snprintf(path, sizeof path, "%s/bank-%s/package.fawp", root, bank);
    if (write_file(path, pkg, total)) { out_json("{\"status\":\"error\",\"reason\":\"write\"}"); return 2; }
    free(pkg);
    pkg = read_file(path, &total, MAX_PACKAGE + (2u << 20));
    if (!pkg || (why = check_package(pkg, total))) {
        out_json("{\"status\":\"error\",\"reason\":\"written package: %s\"}", pkg ? why : "read");
        return 4;
    }
    if (set_bank_state(root, bank, "installed", man.sequence)) return 2;
    {
        char body[64];
        snprintf(body, sizeof body, "sequence %llu\n", (unsigned long long)man.sequence);
        if (write_both(root, "accepted", body)) return 2;
    }
    out_json("{\"status\":\"installed\",\"bank\":\"%s\",\"sequence\":%llu,\"bytes\":%lu}", bank, (unsigned long long)man.sequence, (unsigned long)total);
    return 0;
}

static int cmd_activate(const char *root)
{
    char state[16], best[8] = "";
    uint64_t seq, top = 0;
    const char *banks[2] = {"a", "b"};
    int i;
    for (i = 0; i < 2; i++)
        if (bank_state(root, banks[i], state, &seq) == 0 && !strcmp(state, "installed") && seq > top) {
            top = seq;
            snprintf(best, sizeof best, "%s", banks[i]);
        }
    if (!best[0]) { out_json("{\"status\":\"refused\",\"reason\":\"no installed bank\"}"); return 5; }
    if (set_active(root, best, top)) return 2;
    out_json("{\"status\":\"active\",\"bank\":\"%s\",\"sequence\":%llu}", best, (unsigned long long)top);
    return 0;
}

static int file_sha(const char *path, char out[65])
{
    size_t n;
    uint8_t *d = read_file(path, &n, 64u << 20);
    if (!d) return -1;
    sha256_hex(d, n, out);
    free(d);
    return 0;
}

// SHA-256 of the layout fields of the session's run lines (weight_package.LAYOUT_FIELDS): the leading fields of each
// line that fix addresses and buffer layout, joined by one space, one line each
static int layout_sha(const char *session, char out[65])
{
    static const char *names[3] = {"models.txt", "engines.txt", "hybrids.txt"};
    static const int counts[3] = {6, 11, 14};
    fa_sha256_ctx c;
    uint8_t h[32];
    int i;
    fa_sha256_init(&c);
    for (i = 0; i < 3; i++) {
        char p[600], *line, *save = NULL;
        size_t n;
        uint8_t *d;
        snprintf(p, sizeof p, "%s/%s", session, names[i]);
        d = read_file(p, &n, 1u << 20);
        if (!d) continue;
        d[n] = 0;
        for (line = strtok_r((char *)d, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *tok, *s2 = NULL;
            int k = 0;
            for (tok = strtok_r(line, " \t\r", &s2); tok && k < counts[i]; tok = strtok_r(NULL, " \t\r", &s2), k++) {
                if (k) fa_sha256_update(&c, (const uint8_t *)" ", 1);
                fa_sha256_update(&c, (const uint8_t *)tok, strlen(tok));
            }
            if (k) fa_sha256_update(&c, (const uint8_t *)"\n", 1);
        }
        free(d);
    }
    fa_sha256_final(&c, h);
    hexs(h, 32, out);
    return 0;
}

// check one bank against the session; on success write its files into the session folder
// the files and assembled images of checked package bytes (the manifest in `man`) into the session folder
static const char *write_files(const uint8_t *pkg, size_t n, const char *session, int *files)
{
    char path[600];
    uint32_t m, s, p;
    int i, j;
    header(pkg, n, &m, &s, &p);
    const uint8_t *payload = pkg + HEADER_BYTES + m + s;
    for (i = 0; i < man.files; i++) {
        const entry_t *e = &man.file[i];
        if (e->kind != 'f') continue;
        snprintf(path, sizeof path, "%s/%s", session, e->name);
        if (write_file(path, payload + e->offset, e->length)) return "write";
        (*files)++;
    }
    for (i = 0; i < man.assembles; i++) {
        const assemble_t *x = &man.assemble[i];
        uint8_t *img = calloc(1, x->length ? x->length : 1);
        if (!img) return "memory";
        for (j = 0; j < x->parts; j++) {
            const entry_t *e = find_file(x->part[j]);
            memcpy(img + x->at[j], payload + e->offset, e->length);
        }
        snprintf(path, sizeof path, "%s/%s", session, x->name);
        if (write_file(path, img, x->length)) { free(img); return "write"; }
        free(img);
        (*files)++;
    }
    return NULL;
}

static const char *stage_bank(const char *root, const char *bank, const char *session, int *files)
{
    char path[600], h[65];
    uint8_t *pkg, *base = NULL;
    size_t n, bn = 0;
    const char *why;
    snprintf(path, sizeof path, "%s/bank-%s/package.fawp", root, bank);
    pkg = read_file(path, &n, MAX_PACKAGE + (2u << 20));
    if (!pkg) return "package missing";
    if ((why = check_package(pkg, n))) { free(pkg); return why; }
    snprintf(path, sizeof path, "%s/target.bit.bin", session);
    if (file_sha(path, h) || strcmp(h, man.target_bitstream)) { free(pkg); return "target bitstream"; }
    layout_sha(session, h);
    if (strcmp(h, man.target_layout)) { free(pkg); return "target layout"; }
    *files = 0;
    if (man.has_base) {
        // a partial package: its base's files first (checked again), then the partial package's over them
        snprintf(path, sizeof path, "%s/bank-%s/base.fawp", root, bank);
        base = read_file(path, &bn, MAX_PACKAGE + (2u << 20));
        if (!base) { free(pkg); return "base missing"; }
        if ((why = check_base(base, bn))) { free(base); free(pkg); return why; }
        partial_man = man;
        check_package(base, bn);                          // the base's manifest in `man` (checked just above)
        why = write_files(base, bn, session, files);
        man = partial_man;
        free(base);
        if (why) { free(pkg); return why; }
    }
    why = write_files(pkg, n, session, files);
    free(pkg);
    return why;
}

static int cmd_stage(const char *root, const char *session)
{
    char active[8], state[16], record[600];
    uint64_t aseq, seq;
    const char *why = NULL, *first_why = NULL, *bank;
    int files = 0, rolled = 0, attempt;
    if (read_both(root, "active", active, &aseq) || !active[0]) {
        out_json("{\"status\":\"factory\",\"reason\":\"no active bank\"}");
        return 5;
    }
    bank = active;
    for (attempt = 0; attempt < 2; attempt++) {
        if (bank_state(root, bank, state, &seq) == 0 && usable(state) && !(why = stage_bank(root, bank, session, &files))) break;
        if (!why) why = !strcmp(state, "writing") ? "bank install interrupted" : "bank not usable";
        if (!first_why) first_why = why;
        if (bank_state(root, bank, state, &seq) == 0 && usable(state)) set_bank_state(root, bank, "bad", seq);
        bank = other(bank);
        rolled = 1;
        why = NULL;
        if (bank_state(root, bank, state, &seq) || !usable(state)) { attempt = 2; break; }
    }
    if (attempt >= 2) {
        out_json("{\"status\":\"factory\",\"reason\":\"%s\"}", first_why ? first_why : "no usable bank");
        return 5;
    }
    if (rolled) set_active(root, bank, man.sequence);
    snprintf(record, sizeof record,
             "{\"status\":\"staged\",\"bank\":\"%s\",\"sequence\":%llu,\"files\":%d,\"rolled_back\":%s,\"state\":\"%s\"%s%s%s}",
             bank, (unsigned long long)man.sequence, files, rolled ? "true" : "false", state,
             first_why ? ",\"rollback_reason\":\"" : "", first_why ? first_why : "", first_why ? "\"" : "");
    out_json("%s", record);
    {
        char p[600];
        snprintf(p, sizeof p, "%s/weights-stage.json", session);
        write_file(p, (const uint8_t *)record, strlen(record));
    }
    return 0;
}

static int cmd_mark(const char *root, const char *what)
{
    char active[8], state[16], ostate[16];
    uint64_t aseq, seq, oseq;
    if (read_both(root, "active", active, &aseq) || !active[0] || bank_state(root, active, state, &seq)) {
        out_json("{\"status\":\"refused\",\"reason\":\"no active bank\"}");
        return 5;
    }
    if (!strcmp(what, "verified")) {
        if (set_bank_state(root, active, "verified", seq)) return 2;
        out_json("{\"status\":\"verified\",\"bank\":\"%s\",\"sequence\":%llu}", active, (unsigned long long)seq);
        return 0;
    }
    if (strcmp(what, "bad")) return 2;
    if (set_bank_state(root, active, "bad", seq)) return 2;
    if (bank_state(root, other(active), ostate, &oseq) == 0 && usable(ostate)) {
        if (set_active(root, other(active), oseq)) return 2;
        out_json("{\"status\":\"rolled_back\",\"bad\":\"%s\",\"bank\":\"%s\",\"sequence\":%llu}", active, other(active),
                 (unsigned long long)oseq);
        return 0;
    }
    out_json("{\"status\":\"bad\",\"bank\":\"%s\",\"fallback\":\"factory\"}", active);
    return 0;
}

static int cmd_status(const char *root)
{
    char active[8], sa[16] = "", sb[16] = "", dummy[8];
    uint64_t aseq = 0, qa = 0, qb = 0, accepted = 0;
    int ok = read_both(root, "active", active, &aseq) == 0;
    read_both(root, "accepted", dummy, &accepted);
    bank_state(root, "a", sa, &qa);
    bank_state(root, "b", sb, &qb);
    out_json("{\"status\":\"ok\",\"active\":\"%s\",\"active_sequence\":%llu,\"accepted\":%llu,"
             "\"banks\":{\"a\":{\"state\":\"%s\",\"sequence\":%llu},\"b\":{\"state\":\"%s\",\"sequence\":%llu}},\"trusted_keys\":%d}",
             ok ? active : "", (unsigned long long)aseq, (unsigned long long)accepted, sa, (unsigned long long)qa, sb,
             (unsigned long long)qb, TRUSTED_KEY_COUNT);
    return 0;
}

static int cmd_verify(const char *path)
{
    size_t n;
    uint8_t *d = read_file(path, &n, MAX_PACKAGE + (2u << 20));
    const char *why;
    if (!d) { out_json("{\"status\":\"error\",\"reason\":\"read\"}"); return 2; }
    why = check_package(d, n);
    if (why) { out_json("{\"status\":\"refused\",\"reason\":\"%s\"}", why); return 4; }
    if (man.has_base)
        out_json("{\"status\":\"ok\",\"sequence\":%llu,\"files\":%d,\"assembled\":%d,\"chunks\":%d,\"base_sequence\":%llu,"
                 "\"base_manifest\":\"%s\"}", (unsigned long long)man.sequence, man.files, man.assembles, man.chunks,
                 (unsigned long long)man.base_sequence, man.base_manifest);
    else
        out_json("{\"status\":\"ok\",\"sequence\":%llu,\"files\":%d,\"assembled\":%d,\"chunks\":%d}",
                 (unsigned long long)man.sequence, man.files, man.assembles, man.chunks);
    return 0;
}

int main(int argc, char **argv)
{
    if (fa_ed25519_selftest()) { out_json("{\"status\":\"error\",\"reason\":\"crypto self-test\"}"); return 2; }
    if (argc == 4 && !strcmp(argv[1], "install")) return cmd_install(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "activate")) return cmd_activate(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "stage")) return cmd_stage(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "mark")) return cmd_mark(argv[2], argv[3]);
    if (argc == 3 && !strcmp(argv[1], "status")) return cmd_status(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "verify")) return cmd_verify(argv[2]);
    fprintf(stderr, "usage: %s install INCOMING ROOT | activate ROOT | stage ROOT SESSION | mark ROOT verified|bad | "
                    "status ROOT | verify PACKAGE\n", argv[0]);
    return 64;
}
