// F-ASTRA AXI-Lite loader: applies run-time parameters (FINN run-time weights, LayerNorm parameters) to the harness's
// AXI-Lite windows through /dev/mem before a model runs. One operation per line of the load file:
//   C ADDR MASK EXPECT   read ADDR and require (value & MASK) == EXPECT (e.g. the accelerator is out of reset)
//   P ADDR MASK EXPECT   read ADDR until (value & MASK) == EXPECT, for at most POLL_MS (e.g. a core held between frames)
//   W ADDR VALUE         write VALUE
//   V ADDR VALUE         write VALUE, then read it back and require VALUE
// ADDR is an absolute, 4-byte aligned physical address (the bundle resolves ports and offsets with the harness record).
// Prints a JSON summary; exit 0 when every operation held, 4 on a failed check or readback, 2 on a setup error.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define APERTURE 0x10000u
#define MAX_APERTURES 64
#define POLL_MS 1000

static int mem_fd = -1;
static uint32_t bases[MAX_APERTURES];
static volatile uint32_t *maps[MAX_APERTURES];
static int mapped;

static void fail(const char *what)
{
    fprintf(stderr, "F_ASTRA_AXILITE_LOAD_FAILED %s errno=%d\n", what, errno);
    exit(2);
}

// the register at `addr`, through a cached 64 KB mapping of its aperture
static volatile uint32_t *reg(uint32_t addr)
{
    uint32_t base = addr & ~(APERTURE - 1);
    int i;
    for (i = 0; i < mapped; i++)
        if (bases[i] == base) return maps[i] + (addr - base) / 4;
    if (mapped == MAX_APERTURES) { errno = ENOMEM; fail("too many apertures"); }
    maps[mapped] = mmap(NULL, APERTURE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, base);
    if (maps[mapped] == MAP_FAILED) fail("mmap aperture");
    bases[mapped] = base;
    return maps[mapped++] + (addr - base) / 4;
}

int main(int argc, char **argv)
{
    FILE *f;
    char kind;
    unsigned long line = 0, writes = 0, verified = 0, checks = 0, polls = 0, failed = 0;
    unsigned int addr, a, b;
    if (argc != 2) {
        fprintf(stderr, "usage: %s LOAD_FILE\n", argv[0]);
        return 64;
    }
    f = fopen(argv[1], "r");
    if (!f) fail(argv[1]);
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) fail("/dev/mem");
    while (fscanf(f, " %c %x %x", &kind, &addr, &a) == 3) {
        volatile uint32_t *r;
        line++;
        if (addr & 3) { errno = EINVAL; fail("unaligned address"); }
        r = reg(addr);
        if (kind == 'C' || kind == 'P') {
            uint32_t value;
            int tries = 0;
            if (fscanf(f, " %x", &b) != 1) { errno = EINVAL; fail("check line"); }
            value = *r;
            // a poll reads again every millisecond until the condition holds or POLL_MS passed
            while (kind == 'P' && (value & a) != b && tries++ < POLL_MS) {
                struct timespec rest = {0, 1000000};
                nanosleep(&rest, NULL);
                value = *r;
            }
            if (kind == 'P') polls++; else checks++;
            if ((value & a) != b) {
                fprintf(stderr, "F_ASTRA_AXILITE_CHECK_FAILED line=%lu kind=%c addr=0x%08x value=0x%08x mask=0x%08x expect=0x%08x\n",
                        line, kind, addr, value, a, b);
                failed++;
                break;          // a failed condition (e.g. a model held in reset): write nothing after it
            }
        } else if (kind == 'W' || kind == 'V') {
            *r = a;
            writes++;
            if (kind == 'V') {
                uint32_t back;
                __sync_synchronize();
                back = *r;
                verified++;
                if (back != a) {
                    fprintf(stderr, "F_ASTRA_AXILITE_READBACK_FAILED line=%lu addr=0x%08x wrote=0x%08x read=0x%08x\n", line, addr,
                            a, back);
                    failed++;
                }
            }
        } else {
            errno = EINVAL;
            fail("operation kind");
        }
    }
    if (!feof(f) && !failed) { errno = EINVAL; fail("load file syntax"); }
    fclose(f);
    printf("{\"schema\":\"f-astra/axilite-load/v1\",\"lines\":%lu,\"checks\":%lu,\"polls\":%lu,\"writes\":%lu,\"verified\":%lu,"
           "\"failed\":%lu,\"apertures\":%d}\n", line, checks, polls, writes, verified, failed, mapped);
    return failed ? 4 : 0;
}
