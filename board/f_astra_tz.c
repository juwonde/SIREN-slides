// F-ASTRA TrustZone T1 session helper (TE0715, secure Linux): opens exactly the models pool's 64 MB DDR sections to the
// PL's non-secure masters (SLCR TZ_DDR_RAM, f_astra_fm_core.h fm_tz_ddr_mask) and puts the value it found back at the
// end. The bitstream marks every PL access non-secure (f_astra_axi_ns), so a PL access outside the open sections ends
// in DECERR. The SLCR lock is left as found (Linux keeps it unlocked; locking it breaks the reboot path).
//   f-astra-tz open POOL_DEVICE SAVE_FILE   save TZ_DDR_RAM, write the pool's mask
//   f-astra-tz restore SAVE_FILE            write the saved value back
//   f-astra-tz read                         print TZ_DDR_RAM
//   f-astra-tz set VALUE                    write VALUE (the section mapping check of T1)
//   f-astra-tz pl-windows POOL_DEVICE MANIFEST
//                                           T3d, non-secure Linux with the TE0715 monitor: register the loaded
//                                           bitstream's PL register windows (the ground's signed manifest,
//                                           tools/te0715-monitor/ground_auth.py pl-windows), staged in the pool's
//                                           last page for the monitor; exit 0 when the monitor took it
// Prints one JSON line {"schema":"f-astra/tz/v1",...}; exit 0 when the read-back is the value written, 2 otherwise.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "f_astra_fm_core.h"

struct pool_info { uint64_t dma_address; uint32_t bytes; uint32_t output_offset; };
#define POOL_INFO _IOR('F', 1, struct pool_info)
#define SLCR_BASE 0xF8000000u
enum { SLCR_LOCK = 0x4, SLCR_UNLOCK = 0x8, SLCR_LOCKSTA = 0xC };

static volatile uint32_t *slcr;

// bytes of Linux memory (top-level "System RAM" lines of /proc/iomem) inside the sections of `mask`: 0 when the open
// sections hold the pool's reserved region only (T1b), the CMA pool's neighbours otherwise (T1a)
static unsigned long long linux_ram_in(uint32_t mask)
{
    char line[160];
    unsigned long long total = 0;
    FILE *f = fopen("/proc/iomem", "r");
    if (!f) return ~0ull;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long lo, hi;
        if (line[0] == ' ' || !strstr(line, "System RAM") || sscanf(line, "%llx-%llx", &lo, &hi) != 2) continue;
        for (uint32_t s = 0; s < FM_TZ_SECTIONS; s++) {
            unsigned long long a = (unsigned long long)s * FM_TZ_SECTION_BYTES, b = a + FM_TZ_SECTION_BYTES - 1;
            if ((mask >> s) & 1u && hi >= a && lo <= b) total += (hi < b ? hi : b) - (lo > a ? lo : a) + 1;
        }
    }
    fclose(f);
    return total;
}

static void fail(const char *what)
{
    fprintf(stderr, "F_ASTRA_TZ_FAILED %s errno=%d\n", what, errno);
    exit(2);
}

static uint32_t tz_read(void) { return slcr[(FM_SLCR_TZ_DDR_RAM - SLCR_BASE) / 4]; }

static uint32_t tz_write(uint32_t value)
{
    uint32_t locked = slcr[SLCR_LOCKSTA / 4] & 1u;
    if (locked) slcr[SLCR_UNLOCK / 4] = 0xDF0Du;
    slcr[(FM_SLCR_TZ_DDR_RAM - SLCR_BASE) / 4] = value;
    __sync_synchronize();
    if (locked) slcr[SLCR_LOCK / 4] = 0x767Bu;
    return tz_read();
}

struct fastra_sec_call { uint32_t fid, a1, a2, a3, r0, r1, r2, r3; };
#define FASTRA_SEC_CALL _IOWR('F', 0x40, struct fastra_sec_call)
#define SEC_PL_WINDOWS 0x82000F18u

// T3d: through /dev/f-astra-sec (the non-secure world cannot reach the SLCR, so this runs before the SLCR mapping)
static int pl_windows(const char *pool_path, const char *manifest_path)
{
    struct pool_info info;
    struct fastra_sec_call c;
    uint8_t manifest[4096], *mem;
    size_t bytes;
    FILE *f;
    int sec, rc, pool = open(pool_path, O_RDWR);
    if (pool < 0 || ioctl(pool, POOL_INFO, &info) || info.bytes < sizeof(manifest)) fail(pool_path);
    if (!(f = fopen(manifest_path, "rb"))) fail(manifest_path);
    bytes = fread(manifest, 1, sizeof(manifest), f);
    fclose(f);
    mem = mmap(NULL, info.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pool, 0);
    if (mem == MAP_FAILED) fail("mmap pool");
    memset(mem + info.bytes - sizeof(manifest), 0, sizeof(manifest));
    memcpy(mem + info.bytes - sizeof(manifest), manifest, bytes);
    __sync_synchronize();
    memset(&c, 0, sizeof(c));
    c.fid = SEC_PL_WINDOWS;
    c.a1 = (uint32_t)(info.dma_address + info.bytes - sizeof(manifest));
    sec = open("/dev/f-astra-sec", O_RDWR);
    rc = sec < 0 ? -1 : ioctl(sec, FASTRA_SEC_CALL, &c);
    printf("{\"schema\":\"f-astra/tz/v1\",\"mode\":\"pl-windows\",\"manifest_bytes\":%zu,\"r0\":\"0x%08x\",\"errno\":%d}\n",
           bytes, c.r0, rc ? errno : 0);
    return rc == 0 && c.r0 == 0 ? 0 : 2;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "pl-windows") && argc == 4) return pl_windows(argv[2], argv[3]);
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    uint32_t before, value, after;
    if (fd < 0) fail("/dev/mem");
    slcr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SLCR_BASE);
    if (slcr == MAP_FAILED) fail("mmap SLCR");
    before = tz_read();
    if (!strcmp(mode, "open") && argc == 4) {
        struct pool_info info;
        FILE *f;
        int pool = open(argv[2], O_RDWR);
        if (pool < 0 || ioctl(pool, POOL_INFO, &info)) fail(argv[2]);
        close(pool);
        if (!(f = fopen(argv[3], "w")) || fprintf(f, "0x%08x\n", before) < 0 || fclose(f)) fail(argv[3]);
        value = fm_tz_ddr_mask(info.dma_address, info.bytes);
        after = tz_write(value);
        printf("{\"schema\":\"f-astra/tz/v1\",\"mode\":\"open\",\"pool\":\"0x%08llx\",\"bytes\":%u,\"before\":\"0x%08x\","
               "\"written\":\"0x%08x\",\"after\":\"0x%08x\",\"linux_ram_in_open_sections\":%llu}\n",
               (unsigned long long)info.dma_address, info.bytes, before, value, after, linux_ram_in(value));
    } else if (!strcmp(mode, "restore") && argc == 3) {
        FILE *f = fopen(argv[2], "r");
        unsigned saved;
        if (!f || fscanf(f, "%x", &saved) != 1) fail(argv[2]);
        fclose(f);
        value = saved;
        after = tz_write(value);
        printf("{\"schema\":\"f-astra/tz/v1\",\"mode\":\"restore\",\"before\":\"0x%08x\",\"written\":\"0x%08x\","
               "\"after\":\"0x%08x\"}\n", before, value, after);
    } else if (!strcmp(mode, "set") && argc == 3) {
        value = (uint32_t)strtoul(argv[2], NULL, 0);
        after = tz_write(value);
        printf("{\"schema\":\"f-astra/tz/v1\",\"mode\":\"set\",\"before\":\"0x%08x\",\"written\":\"0x%08x\","
               "\"after\":\"0x%08x\"}\n", before, value, after);
    } else if (!strcmp(mode, "read") && argc == 2) {
        printf("{\"schema\":\"f-astra/tz/v1\",\"mode\":\"read\",\"value\":\"0x%08x\"}\n", before);
        return 0;
    } else {
        fprintf(stderr, "usage: %s open POOL_DEVICE SAVE_FILE | restore SAVE_FILE | read | set VALUE"
                " | pl-windows POOL_DEVICE MANIFEST\n", argv[0]);
        return 64;
    }
    return after == value ? 0 : 2;
}
