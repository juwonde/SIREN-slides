// PL register access of the F-ASTRA runners (rd, wr, map_registers).
// - Secure Linux (the installed image): a /dev/mem mapping of each register window.
// - Non-secure Linux (TrustZone T2b): M_AXI_GP0 stays secure on the TE0715 (its port security register cannot be
//   reached), so the secure monitor reads and writes each word for the kernel's /dev/f-astra-sec (file offset =
//   physical address). A "mapping" is then the window's physical base, used only through rd and wr; a slave error
//   or a refused address stops the runner through its fail(). The offsets are 64-bit (pread64, pwrite64): the PS
//   registers sit above 2^31, where a 32-bit off_t is negative.
// The device exists only under the non-secure kernel, so the same binary runs in both worlds.
#ifndef F_ASTRA_PL_IO_H
#define F_ASTRA_PL_IO_H

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef APERTURE
#define APERTURE 0x10000u
#endif

static void fail(const char *what);

static int pl_io_fd = -2;              // -2: not probed, -1: /dev/mem mappings, else /dev/f-astra-sec

static int pl_io_secure_monitor(void)
{
    if (pl_io_fd == -2) {
        pl_io_fd = open("/dev/f-astra-sec", O_RDWR | O_CLOEXEC);
        if (pl_io_fd < 0) pl_io_fd = -1;
    }
    return pl_io_fd >= 0;
}

static uint32_t rd(volatile uint32_t *base, unsigned off)
{
    uint32_t v;
    if (pl_io_fd < 0) return base[off / 4];
    if (pread64(pl_io_fd, &v, 4, (off64_t)((uintptr_t)base + off)) != 4) fail("/dev/f-astra-sec read");
    return v;
}

static void wr(volatile uint32_t *base, unsigned off, uint32_t v)
{
    if (pl_io_fd < 0) {
        base[off / 4] = v;
        return;
    }
    if (pwrite64(pl_io_fd, &v, 4, (off64_t)((uintptr_t)base + off)) != 4) fail("/dev/f-astra-sec write");
}

static void *map_registers(int fd, uint32_t base)
{
    void *p;
    if (pl_io_secure_monitor()) return (void *)(uintptr_t)base;
    p = mmap(NULL, APERTURE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (p == MAP_FAILED) fail("mmap registers");
    return p;
}

#endif
