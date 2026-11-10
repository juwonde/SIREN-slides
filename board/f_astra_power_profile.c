/* F-ASTRA power profile of the TE0715 payload in C (11/01: board code in C; the boot's rcS S42 ran the sh form,
 * tools/petalinux/te0715-2020.2/f-astra-power-profile, about 50 devmem programs, 0.42 s of every boot). Same steps,
 * same order, same log lines; no heap allocation.
 *
 *   f-astra-power-profile start   the DDR controller's automatic power-down (DDRC ctrl bit 1), the Ethernet PHY in
 *                                 power-down (BMCR bit 11 of the first PHY that answers on the GEM0 MDIO port), the
 *                                 powersave governor with the policy minimum at 333 MHz, the CPU onto the DDR PLL and
 *                                 the ARM PLL bypassed and powered down (only with the same VCO), FCLK1..3 at their
 *                                 slowest, the trace clock (DBG_CLK_CTRL bit 0) and the PL330 DMA clock (APER bit 0) off
 *   f-astra-power-profile stop    all back: PHY powered, DDR power-down off, the ARM PLL locked and the CPU on it, the
 *                                 performance governor with the table's lowest minimum, FCLK1..3 at their boot value,
 *                                 trace and DMA clocks on
 *   f-astra-power-profile status  the registers and the CPU
 *   start keep-fclk1, stop keep-fclk1   FCLK1 left as it is (11/10 boot stage: with the SD's duty configuration the
 *                                 FSBL configured the duty PL and its 1x domain runs on FCLK1 from U-Boot on; only the
 *                                 FM duty monitor changes that clock, halted while it does). FCLK2..3 as above
 * Build: cc -std=gnu99 -O2 -Wall -Wextra -Werror f_astra_power_profile.c -o f-astra-power-profile
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SLCR 0xF8000000u
#define DDRC 0xF8006000u               /* ctrl 0x00 (bit 0 out of soft reset, bit 1 powerdown_en), param3 0x20 */
#define GEM0 0xE000B000u               /* net_ctrl 0x00 (bit 4 MDIO enable), net_cfg 0x04, net_status 0x08, phy_maint 0x34 */
#define ARM_PLL 0x100u                 /* RESET bit 0, PWRDWN bit 1, BYPASS_FORCE bit 4, FDIV bits 18:12 */
#define DDR_PLL 0x104u
#define PLL_STATUS 0x10Cu              /* ARM_PLL_LOCK bit 0 */
#define ARM_CLK 0x120u                 /* SRCSEL bits 5:4 (0 ARM PLL, 2 DDR PLL) */
#define APER_CLK 0x12Cu                /* DMA bit 0, GEM0 bit 6 */
#define DBG_CLK 0x164u                 /* CLKACT_TRC bit 0 */
#define FCLK_SLOW 0x03F03F00u
#define FCLK_BOOT 0x00101800u
#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"

static const uint32_t FCLK_UNUSED[3] = {0x180u, 0x190u, 0x1A0u};
static volatile uint32_t *slcr, *ddrc, *gem;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("f-astra-power-profile: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static volatile uint32_t *map_page(int fd, uint32_t base)
{
    void *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (p == MAP_FAILED)
        die("map 0x%08X: %s", (unsigned)base, strerror(errno));
    return p;
}

static uint32_t rd(volatile uint32_t *page, uint32_t off) { return page[off / 4]; }
static void wr(volatile uint32_t *page, uint32_t off, uint32_t v) { page[off / 4] = v; }

/* whole-file text (NUL-terminated, newline cut); 0 when the file is missing */
static int read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return 0;
    buf[n] = 0;
    buf[strcspn(buf, "\n")] = 0;
    return 1;
}

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return 0;
    size_t n = strlen(text);
    ssize_t w = write(fd, text, n);
    return close(fd) == 0 && w == (ssize_t)n;
}

/* the CPU onto the DDR PLL when both PLLs have the same feedback divider, then the ARM PLL bypassed and powered down */
static void arm_pll_off(void)
{
    uint32_t a = rd(slcr, ARM_PLL), d = rd(slcr, DDR_PLL), c = rd(slcr, ARM_CLK);
    if (((a >> 12) & 0x7F) != ((d >> 12) & 0x7F)) {
        puts("arm pll: the DDR PLL runs at another VCO, kept");
        return;
    }
    if (c & 0x30) {
        puts("arm pll: the CPU is not on it, kept");
        return;
    }
    wr(slcr, ARM_CLK, (c & ~0x30u) | 0x20u);
    wr(slcr, ARM_PLL, a | 0x10u);
    wr(slcr, ARM_PLL, a | 0x12u);
    printf("arm pll off: cpu clock 0x%08X pll 0x%08X\n", (unsigned)rd(slcr, ARM_CLK), (unsigned)rd(slcr, ARM_PLL));
}

/* powered, reset, locked, bypass off, the CPU back on the ARM PLL */
static int arm_pll_on(void)
{
    uint32_t a = rd(slcr, ARM_PLL);
    if (a & 2) {
        wr(slcr, ARM_PLL, (a & ~2u) | 0x10u);
        wr(slcr, ARM_PLL, (a & ~2u) | 0x11u);
        wr(slcr, ARM_PLL, (a & ~3u) | 0x10u);
        int i = 0;
        while (!(rd(slcr, PLL_STATUS) & 1))
            if (++i >= 5000000) {
                puts("arm pll: no lock");
                return 1;
            }
        wr(slcr, ARM_PLL, a & ~0x13u);
    }
    wr(slcr, ARM_CLK, rd(slcr, ARM_CLK) & ~0x30u);
    printf("arm pll on: cpu clock 0x%08X pll 0x%08X\n", (unsigned)rd(slcr, ARM_CLK), (unsigned)rd(slcr, ARM_PLL));
    return 0;
}

static void fclk_set(uint32_t v, int keep_fclk1)
{
    for (int k = keep_fclk1 ? 1 : 0; k < 3; k++)
        wr(slcr, FCLK_UNUSED[k], v);
}

static void clock_bit(uint32_t off, uint32_t bit, int on)
{
    wr(slcr, off, (rd(slcr, off) & ~bit) | (on ? bit : 0));
}

static void cpu_governor(const char *governor)
{
    if (access(CPUFREQ "scaling_governor", W_OK)) {
        puts("cpufreq: none");
        return;
    }
    /* the policy minimum: 333 MHz while running (the OPPs below are for receive-only windows), the table's lowest
     * when stopped */
    if (!access(CPUFREQ "scaling_min_freq", W_OK)) {
        char min[32] = "";
        if (!strcmp(governor, "powersave"))
            strcpy(min, "333334");
        else
            read_text(CPUFREQ "cpuinfo_min_freq", min, sizeof min);
        if (min[0])
            write_text(CPUFREQ "scaling_min_freq", min);
    }
    write_text(CPUFREQ "scaling_governor", governor);
    char now[32] = "", cur[32] = "";
    read_text(CPUFREQ "scaling_governor", now, sizeof now);
    read_text(CPUFREQ "cpuinfo_cur_freq", cur, sizeof cur);
    printf("cpufreq %s %s kHz\n", now, cur);
}

/* the GEM0 MDIO port: its AMBA clock, MDC divider (bits 20:18 = 7) and management port on; restored after */
static uint32_t aper_saved, ctl_saved;

static void mdio_on(void)
{
    aper_saved = rd(slcr, APER_CLK);
    wr(slcr, APER_CLK, aper_saved | 0x40u);
    wr(gem, 0x04, (rd(gem, 0x04) & ~0x1C0000u) | 0x1C0000u);
    ctl_saved = rd(gem, 0x00);
    wr(gem, 0x00, ctl_saved | 0x10u);
}

static void mdio_off(void)
{
    wr(gem, 0x00, ctl_saved);
    wr(slcr, APER_CLK, aper_saved);
}

static int mdio_idle(void)
{
    for (int i = 0; i < 2000; i++)
        if (rd(gem, 0x08) & 4)
            return 0;
    return -1;
}

/* a clause-22 read (value, or -1 when the port stays busy) or write (0 or -1) */
static int mdio_rd(uint32_t addr, uint32_t reg)
{
    if (mdio_idle())
        return -1;
    wr(gem, 0x34, 0x60020000u | (addr << 23) | (reg << 18));
    if (mdio_idle())
        return -1;
    return (int)(rd(gem, 0x34) & 0xFFFF);
}

static int mdio_wr(uint32_t addr, uint32_t reg, uint32_t v)
{
    if (mdio_idle())
        return -1;
    wr(gem, 0x34, 0x50020000u | (addr << 23) | (reg << 18) | (v & 0xFFFF));
    return mdio_idle();
}

/* the first PHY address that answers: page 0 (register 22), then BMCR power-down (bit 11) set (down) or cleared */
static int phy(int down)
{
    mdio_on();
    for (uint32_t a = 0; a < 32; a++) {
        int id1 = mdio_rd(a, 2);
        if (id1 < 0)
            break;
        if (id1 != 0xFFFF && id1 != 0) {
            mdio_wr(a, 22, 0);
            int v = mdio_rd(a, 0);
            if (v < 0)
                break;
            mdio_wr(a, 0, down ? ((uint32_t)v | 0x800u) : ((uint32_t)v & ~0x800u));
            printf("phy %u bmcr %d -> %d\n", (unsigned)a, v, mdio_rd(a, 0));
            mdio_off();
            return 0;
        }
    }
    mdio_off();
    puts("no phy");
    return 1;
}

static void status(void)
{
    char gov[32] = "", cur[32] = "";
    printf("ddrc 0x%08X param3 0x%08X\n", (unsigned)rd(ddrc, 0x00), (unsigned)rd(ddrc, 0x20));
    if (read_text(CPUFREQ "scaling_governor", gov, sizeof gov) && read_text(CPUFREQ "cpuinfo_cur_freq", cur, sizeof cur))
        printf("cpufreq %s %s kHz\n", gov, cur);
    printf("arm pll 0x%08X cpu clock 0x%08X fclk1..3 0x%08X 0x%08X 0x%08X\n", (unsigned)rd(slcr, ARM_PLL),
           (unsigned)rd(slcr, ARM_CLK), (unsigned)rd(slcr, FCLK_UNUSED[0]), (unsigned)rd(slcr, FCLK_UNUSED[1]),
           (unsigned)rd(slcr, FCLK_UNUSED[2]));
    printf("dbg clock 0x%08X aper 0x%08X\n", (unsigned)rd(slcr, DBG_CLK), (unsigned)rd(slcr, APER_CLK));
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    int start = !strcmp(cmd, "start"), stop = !strcmp(cmd, "stop");
    int keep  = argc == 3 && (start || stop) && !strcmp(argv[2], "keep-fclk1");
    if ((!start && !stop && strcmp(cmd, "status")) || argc > 2 + keep) {
        printf("usage: %s start|stop [keep-fclk1] | status\n", argv[0]);
        return 1;
    }
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        die("/dev/mem: %s", strerror(errno));
    slcr = map_page(fd, SLCR);
    ddrc = map_page(fd, DDRC);
    gem = map_page(fd, GEM0);
    close(fd);
    int rc = 0;
    if (start || stop) {
        uint32_t value = rd(ddrc, 0x00);
        /* the controller must be out of its soft reset (bit 0) before a dynamic bit changes */
        if (!(value & 1)) {
            printf("ddrc 0x%08X: not running\n", (unsigned)value);
            rc = 1;
        } else {
            if (start) {
                wr(ddrc, 0x00, value | 2u);
                phy(1);
            } else {
                wr(ddrc, 0x00, value & ~2u);
                phy(0);
            }
            printf("ddrc 0x%08X\n", (unsigned)rd(ddrc, 0x00));
            if (start) {
                cpu_governor("powersave");
                arm_pll_off();
                fclk_set(FCLK_SLOW, keep);
                clock_bit(DBG_CLK, 1, 0);
                clock_bit(APER_CLK, 1, 0);
            } else {
                arm_pll_on();
                cpu_governor("performance");
                fclk_set(FCLK_BOOT, keep);
                clock_bit(DBG_CLK, 1, 1);
                clock_bit(APER_CLK, 1, 1);
            }
        }
    } else {
        status();
    }
    fflush(stdout);
    munmap((void *)slcr, 4096);
    munmap((void *)ddrc, 4096);
    munmap((void *)gem, 4096);
    return rc;
}
