/* F-ASTRA floor profile and fast poweroff of the TE0715 payload (user 10/31: preprocessing and baseline power; board
 * code in C). C form of f_astra_floor.sh and f_astra_fast_poweroff.sh, same steps and order; no heap allocation.
 *
 *   f-astra-floor enter [CPU_KHZ]   receive-only floor: PCAP clock 200 -> 15.9 MHz (never off: the PS-XADC runs on
 *                                   it), CPU1 offline, the SD card unmounted and its controller unbound, the CPU at
 *                                   CPU_KHZ (default 83334) through cpufreq when that OPP exists (image r15), the
 *                                   Si5338's CLK2 (MultiSynth and driver) and PLL off (no PS or PL user), and with
 *                                   the PL parked the IO PLL off (its users moved onto the DDR PLL)
 *   f-astra-floor enter CPU_KHZ FCLK0 FCLK1   the same with the PL running (FM always on): FCLK0/FCLK1 take the two
 *                                   words (or F_ASTRA_FLOOR_FCLK="FCLK0 FCLK1"); both on the DDR PLL let the IO PLL
 *                                   go off as well
 *   f-astra-floor leave            everything back in reverse; waits until the SD card is mounted again
 *   f-astra-floor status
 *   f-astra-floor poweroff          the time keep saved (forward only), sync, the SD card remounted read-only, then
 *                                   power off without init; the normal poweroff when the remount fails
 *
 * Measured under CAN reception (12 V input, floor ladders L1/L2, 10/31): PCAP -0.047 W, CPU 83 MHz -0.048 W, SD
 * unbound -0.024 W, CPU1 offline -0.005 W, Si5338 CLK2 -0.009 W and PLL -0.034 W (ladder L3); poweroff 9.3 s / 27.9 J
 * (rc0) -> 2.4 s / 7.3 J.
 * Build: cc -std=gnu99 -O2 -Wall -Wextra -Werror f_astra_floor.c -o f-astra-floor
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SD "/media/sd-mmcblk0p1"
#define SD_DEV "/dev/mmcblk0p1"
#define SD_CTRL "e0100000.mmc"
#define SD_DRIVER "/sys/bus/platform/drivers/sdhci-arasan/"
#define SD_OPTIONS "fmask=0022,dmask=0022,codepage=437,iocharset=iso8859-1,shortname=mixed,errors=remount-ro"
#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"
#define CPU1 "/sys/devices/system/cpu/cpu1/online"
#define STATE "/run/f-astra-floor.state"
#define TIME_FILE SD "/f-astra-time.txt"
#define SLCR 0xF8000000u
#define APER_CLK 0x12Cu                /* APER_CLK_CTRL: USB0 bit 2, SDI0 bit 10 */
#define APER_USB0 0x4u                 /* on while the USB gadget link is up; f-astra-link-off gates it */
#define SDIO_CLK 0x150u                /* SDIO_CLK_CTRL: CLKACT0/1 bits 0..1 */
#define PCAP_CLK 0x168u                /* PCAP_CLK_CTRL */
#define PCAP_SLOW 0x3F01u              /* IO PLL / 63: 15.9 MHz */
#define SI5338_BUS "/dev/i2c-0"
#define SI5338_ADDR 0x70
#define SI_CLK2 33                     /* CLK2 MultiSynth / driver power-down bits 1..0 */
#define SI_PLL 50                      /* PLL_EN bits 7..6 (the clk-si5338 driver's AWE_PLL_EN) */
#define IO_PLL 0x108u                  /* IO_PLL_CTRL: RESET bit 0, PWRDWN bit 1, BYPASS_FORCE bit 4 */
#define PLL_STATUS 0x10Cu              /* IO_PLL_LOCK bit 2 */
#define UART_CLK 0x154u
#define CAN_CLK 0x15Cu
#define FPGA_RST 0x240u                /* 0xF: the PL held in reset (parked) */
#define UART0 0xE0000000u              /* BAUDGEN 0x18, BAUDDIV 0x34 */
#define CAN0 0xE0008000u               /* SRR 0x00 (CEN bit 1), BRPR 0x08, BTR 0x0C */
/* With the PL parked every IO PLL user moves onto the DDR PLL (1333.33 MHz; peripheral SRCSEL 3) and the IO PLL is
 * bypassed and powered down (-0.071 W under reception, ladder L4): UART 1333.33/13 = 102.56 MHz with CD 89 and BDIV 9
 * (115,240 baud), CAN 1333.33/37/4 = 9.009 MHz with BRPR 0 and 18 tq (TS1 15, TS2 2, SJW 2: 500.5 kbit/s, sample
 * point 88.9 %; the controller in configuration mode for the change, the driver and its sockets stay up), PCAP / 63,
 * FCLK0..3 at their slowest. */
#define DDR_SRC (3u << 4)
#define UART_DDR ((13u << 8) | DDR_SRC | 1u)
#define UART_CD 89u
#define UART_BDIV 9u
#define CAN_DDR ((4u << 20) | (37u << 8) | DDR_SRC | 1u)
#define CAN_BTR (14u | (1u << 4) | (1u << 7))
#define PCAP_DDR ((63u << 8) | DDR_SRC | 1u)
#define FCLK_DDR ((63u << 20) | (63u << 8) | DDR_SRC)
#define TIME_MIN 1704067200L           /* 2024-01-01, as f-astra-time-keep */
#define TIME_MAX 2147483647L
/* The PL running through the reception (user 11/09: FM always on during duty, the PL never off then; per-block clock 0
 * allowed): `enter CPU_KHZ FCLK0 FCLK1` gives the two PL clocks' words for the floor (SEM controller on FCLK0, the 1x
 * infrastructure with the SEM converter and the gate register on FCLK1). Both on the DDR PLL (SRCSEL 3) let the IO PLL
 * go off as with the PL parked; any other source keeps it on. Each word changes with its clock halted (FPGAx_THR_CNT
 * bit 0, the Linux clock gate) under an exclusive lock on CLOCK_LOCK, the lock the FM duty monitor holds shared around
 * its PL reads; the caller has every model gate and the engine MMCM (RUN) off first. Measured guard616 (11/09, r36v3m,
 * USB on): the SEM keeps observing and scanning at 10 and 1 MHz (heartbeats in proportion), AXI reads at FCLK1 1 MHz. */
#define CLOCK_LOCK "/run/lock/f-astra-pl-clock.lock"

static const uint32_t FCLK[4] = {0x170u, 0x180u, 0x190u, 0x1A0u};
static const uint32_t THR[4]  = {0x178u, 0x188u, 0x198u, 0x1A8u};
static volatile uint32_t *slcr, *uart, *can;

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("f-astra-floor: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

/* one 4 KB register page of /dev/mem */
static volatile uint32_t *map_page(int fd, uint32_t base)
{
    void *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (p == MAP_FAILED)
        die("map 0x%08X: %s", (unsigned)base, strerror(errno));
    return p;
}

static void map_slcr(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        die("/dev/mem: %s", strerror(errno));
    slcr = map_page(fd, SLCR);
    uart = map_page(fd, UART0);
    can = map_page(fd, CAN0);
    close(fd);
}

static void unmap_all(void)
{
    munmap((void *)slcr, 4096);
    munmap((void *)uart, 4096);
    munmap((void *)can, 4096);
}

static uint32_t reg(uint32_t off) { return slcr[off / 4]; }
static void reg_set(uint32_t off, uint32_t mask, uint32_t bits) { slcr[off / 4] = (slcr[off / 4] & ~mask) | bits; }

/* one FCLK word written with that clock halted, its gate bit as it was before (a running PL sees no short pulse) */
static void fclk_write(int k, uint32_t word)
{
    uint32_t thr = reg(THR[k]);
    slcr[THR[k] / 4]  = thr | 1u;
    slcr[FCLK[k] / 4] = word;
    slcr[THR[k] / 4]  = thr;
}

/* the exclusive clock lock (-1 when the lock file cannot be opened: the change goes ahead, nothing else may be
 * reading the PL then) */
static int clock_lock(void)
{
    int fd = open(CLOCK_LOCK, O_RDONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0 && flock(fd, LOCK_EX)) {
        close(fd);
        fd = -1;
    }
    return fd;
}

static void clock_unlock(int fd)
{
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

/* what io_pll_off changes: UART clock, BAUDGEN, BAUDDIV, CAN clock, BRPR, BTR, FCLK0..3, IO_PLL_CTRL (11 words) */
static void io_pll_save(uint32_t saved[11])
{
    saved[0] = reg(UART_CLK); saved[1] = uart[0x18 / 4]; saved[2] = uart[0x34 / 4];
    saved[3] = reg(CAN_CLK); saved[4] = can[0x08 / 4]; saved[5] = can[0x0C / 4];
    for (int k = 0; k < 4; k++)
        saved[6 + k] = reg(FCLK[k]);
    saved[10] = reg(IO_PLL);
}

/* IO PLL users onto the DDR PLL (FCLK0..3 as `fclk`), the IO PLL bypassed and powered down */
static void io_pll_off(const uint32_t fclk[4])
{
    can[0x00 / 4] = 0;                               /* CEN 0: configuration mode */
    reg_set(CAN_CLK, 0xFFFFFFFFu, CAN_DDR);
    can[0x08 / 4] = 0;
    can[0x0C / 4] = CAN_BTR;
    can[0x00 / 4] = 2;                               /* CEN 1 */
    reg_set(UART_CLK, 0xFFFFFFFFu, UART_DDR);
    uart[0x18 / 4] = UART_CD;
    uart[0x34 / 4] = UART_BDIV;
    reg_set(PCAP_CLK, 0xFFFFFFFFu, PCAP_DDR);
    for (int k = 0; k < 4; k++)
        fclk_write(k, fclk[k]);
    reg_set(IO_PLL, 0x10, 0x10);                     /* bypass, then power down */
    reg_set(IO_PLL, 0x12, 0x12);
}

/* the IO PLL powered, reset, locked and back in the path; its users back as saved (PCAP: the caller) */
static void io_pll_on(const uint32_t saved[11])
{
    uint32_t v = saved[10];
    slcr[IO_PLL / 4] = (v & ~0x13u) | 0x10u | 0x1u;
    slcr[IO_PLL / 4] = (v & ~0x13u) | 0x10u;
    for (int i = 0; i < 100000 && !(reg(PLL_STATUS) & 4); i++)
        ;
    if (!(reg(PLL_STATUS) & 4))
        die("IO PLL: no lock");
    slcr[IO_PLL / 4] = v;
    for (int k = 0; k < 4; k++)
        fclk_write(k, saved[6 + k]);
    reg_set(UART_CLK, 0xFFFFFFFFu, saved[0]);
    uart[0x18 / 4] = saved[1];
    uart[0x34 / 4] = saved[2];
    can[0x00 / 4] = 0;
    reg_set(CAN_CLK, 0xFFFFFFFFu, saved[3]);
    can[0x08 / 4] = saved[4];
    can[0x0C / 4] = saved[5];
    can[0x00 / 4] = 2;
}

/* whole-file text read into buf (NUL-terminated); 0 when the file is missing */
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
    return 1;
}

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return 0;
    size_t n = strlen(text);
    ssize_t w = write(fd, text, n);
    int ok = close(fd) == 0 && w == (ssize_t)n;
    return ok;
}

/* one Si5338 register read (VALUE < 0: the value, -1 on failure) or write (0 on success) on the PS I2C bus */
static int si5338(int reg_no, int value)
{
    int fd = open(SI5338_BUS, O_RDWR);
    if (fd < 0)
        return -1;
    unsigned char b[2] = {(unsigned char)reg_no, (unsigned char)value};
    int r = -1;
    if (ioctl(fd, I2C_SLAVE, SI5338_ADDR) == 0) {
        if (value < 0)
            r = write(fd, b, 1) == 1 && read(fd, b + 1, 1) == 1 ? b[1] : -1;
        else
            r = write(fd, b, 2) == 2 ? 0 : -1;
    }
    close(fd);
    return r;
}

static int mounted(void)
{
    char buf[8192];
    return read_text("/proc/mounts", buf, sizeof buf) && strstr(buf, " " SD " ") != NULL;
}

/* whether KHZ is one of the cpufreq table's frequencies */
static int has_opp(const char *khz)
{
    char buf[512];
    if (!read_text(CPUFREQ "scaling_available_frequencies", buf, sizeof buf))
        return 0;
    size_t n = strlen(khz);
    for (char *p = buf; (p = strstr(p, khz)) != NULL; p += n)
        if ((p == buf || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\n' || p[n] == 0))
            return 1;
    return 0;
}

static void status(void)
{
    char online[32] = "", cur[32] = "", min[32] = "";
    read_text("/sys/devices/system/cpu/online", online, sizeof online);
    read_text(CPUFREQ "scaling_cur_freq", cur, sizeof cur);
    read_text(CPUFREQ "scaling_min_freq", min, sizeof min);
    online[strcspn(online, "\n")] = cur[strcspn(cur, "\n")] = min[strcspn(min, "\n")] = 0;
    printf("pcap 0x%08X sdio 0x%08X aper 0x%08X online %s cpu %s kHz min %s sd %d si33 %d si50 %d fclk0 0x%08X "
           "fclk1 0x%08X iopll 0x%08X\n",
           (unsigned)reg(PCAP_CLK), (unsigned)reg(SDIO_CLK), (unsigned)reg(APER_CLK), online, cur, min, mounted(),
           si5338(SI_CLK2, -1), si5338(SI_PLL, -1), (unsigned)reg(FCLK[0]), (unsigned)reg(FCLK[1]), (unsigned)reg(IO_PLL));
}

static void enter(const char *khz, uint32_t f0, uint32_t f1)
{
    if (access(STATE, F_OK) == 0)
        die("already entered");
    /* a floor entered with the USB link up dropped the RNDIS link for good (guard275, 11/01): only under link-off */
    if (reg(APER_CLK) & APER_USB0)
        die("the USB link is up (USB0 clock on): enter the floor under f-astra-link-off");
    char min[32] = "";
    read_text(CPUFREQ "scaling_min_freq", min, sizeof min);
    min[strcspn(min, "\n")] = 0;
    int sd = 0;
    sync();
    if (mounted() && umount2(SD, 0) == 0) {
        sd = write_text(SD_DRIVER "unbind", SD_CTRL);
        usleep(300000);
        reg_set(SDIO_CLK, 0x3, 0);
        reg_set(APER_CLK, 0x400, 0);
    }
    int clk2 = si5338(SI_CLK2, -1), pll = si5338(SI_PLL, -1);
    int parked = reg(FPGA_RST) == 0xFu;              /* FCLK0..3 come from the IO PLL: only with the PL held */
    /* the PL running (FM always on): FCLK0 and FCLK1 as given, the IO PLL off when both are on the DDR PLL */
    int pl_on = !parked && f0 && f1;
    int iooff = parked || (pl_on && (f0 & 0x30u) == DDR_SRC && (f1 & 0x30u) == DDR_SRC);
    uint32_t fclk[4] = {FCLK_DDR, FCLK_DDR, FCLK_DDR, FCLK_DDR};
    if (pl_on) {
        fclk[0] = f0;
        fclk[1] = f1;
        fclk[2] = reg(FCLK[2]);
        fclk[3] = reg(FCLK[3]);
    }
    uint32_t io[11] = {0};
    if (iooff || pl_on)
        io_pll_save(io);
    char state[512];
    int n = snprintf(state, sizeof state, "%u %s %d %d %d %d %d", (unsigned)reg(PCAP_CLK), min[0] ? min : "-", sd, clk2,
                     pll, iooff, pl_on);
    for (int k = 0; k < 11; k++)
        n += snprintf(state + n, sizeof state - (size_t)n, " %u", (unsigned)io[k]);
    snprintf(state + n, sizeof state - (size_t)n, "\n");
    int fd = open(STATE, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0 || write(fd, state, strlen(state)) != (ssize_t)strlen(state) || close(fd))
        die(STATE ": %s", strerror(errno));
    reg_set(PCAP_CLK, 0xFFFFFFFFu, PCAP_SLOW);
    write_text(CPU1, "0");
    if (has_opp(khz))
        write_text(CPUFREQ "scaling_min_freq", khz);
    if (clk2 >= 0 && pll >= 0) {
        si5338(SI_CLK2, clk2 | 0x3);
        si5338(SI_PLL, pll & 0x3F);
    }
    int lock = pl_on ? clock_lock() : -1;
    if (iooff)
        io_pll_off(fclk);
    else if (pl_on) {                                /* the PL clocks only, the IO PLL stays on */
        fclk_write(0, f0);
        fclk_write(1, f1);
    }
    clock_unlock(lock);
    status();
}

static void leave(void)
{
    char state[512];
    unsigned pcap;
    char min[32];
    int sd, clk2, pll, iooff, pl_on, used;
    uint32_t io[11];
    if (!read_text(STATE, state, sizeof state) ||
        sscanf(state, "%u %31s %d %d %d %d %d%n", &pcap, min, &sd, &clk2, &pll, &iooff, &pl_on, &used) != 7)
        die("not entered");
    for (int k = 0, at = used, m; k < 11; k++, at += m) {
        unsigned v;
        if (sscanf(state + at, " %u%n", &v, &m) != 1)
            die("state: IO PLL words");
        io[k] = v;
    }
    int lock = pl_on ? clock_lock() : -1;
    if (iooff)
        io_pll_on(io);
    else if (pl_on) {
        fclk_write(0, io[6]);
        fclk_write(1, io[7]);
    }
    clock_unlock(lock);
    if (clk2 >= 0 && pll >= 0) {
        si5338(SI_PLL, pll);
        si5338(SI_CLK2, clk2);
    }
    if (strcmp(min, "-"))
        write_text(CPUFREQ "scaling_min_freq", min);
    write_text(CPU1, "1");
    reg_set(PCAP_CLK, 0xFFFFFFFFu, pcap);
    if (sd) {
        reg_set(APER_CLK, 0x400, 0x400);
        reg_set(SDIO_CLK, 0x3, 0x1);
        write_text(SD_DRIVER "bind", SD_CTRL);
        /* the card re-initializes; image r20's udev mounts the partition, without a device manager (the Yocto image
           r21) this program mounts it as soon as its node is there (guard410: 20 s waited for a udev that is not there) */
        int udev = access("/sbin/udevd", X_OK) == 0;
        for (int i = 0; i < (udev ? 250 : 2500) && !mounted(); i++) {
            if (!udev && access(SD_DEV, F_OK) == 0 && mount(SD_DEV, SD, "vfat", MS_RELATIME, SD_OPTIONS) == 0)
                break;
            usleep(udev ? 100000 : 10000);
        }
        if (!mounted() && mount(SD_DEV, SD, "vfat", MS_RELATIME, SD_OPTIONS))
            die("SD mount: %s", strerror(errno));
    }
    if (unlink(STATE))
        die(STATE ": %s", strerror(errno));
    status();
}

/* f-astra-time-keep save: the clock written to the SD card's time file, forward only */
static void time_keep_save(void)
{
    if (!mounted())
        return;
    char buf[64];
    long now = (long)time(NULL), saved = 0;
    if (read_text(TIME_FILE, buf, sizeof buf)) {
        char digits[16];
        size_t k = 0;
        for (char *p = buf; *p && *p != '\n' && k + 1 < sizeof digits; p++)
            if (*p >= '0' && *p <= '9')
                digits[k++] = *p;
        digits[k] = 0;
        if (k && k <= 10) {
            long v = strtol(digits, 0, 10);
            if (v >= TIME_MIN && v <= TIME_MAX)
                saved = v;
        }
    }
    if (saved > now) {
        printf("F_ASTRA_TIME_KEEP_NOT_SAVED now=%ld saved=%ld\n", now, saved);
        return;
    }
    /* in place (no rename on the FAT partition) */
    int fd = open(TIME_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    int n = snprintf(buf, sizeof buf, "%ld\n", now);
    if (write(fd, buf, (size_t)n) != n)
        fprintf(stderr, "f-astra-floor: time keep write failed\n");
    fsync(fd);
    close(fd);
}

static void fast_poweroff(void)
{
    time_keep_save();
    sync();
    int console = open("/dev/ttyPS0", O_WRONLY | O_NOCTTY);
    if (mounted() && mount(NULL, SD, NULL, MS_REMOUNT | MS_RDONLY, NULL)) {
        if (console >= 0) {
            if (write(console, "F_ASTRA_FAST_POWEROFF fallback\n", 31) < 0)
                fprintf(stderr, "f-astra-floor: console write failed\n");
            close(console);
        }
        execl("/sbin/poweroff", "poweroff", (char *)NULL);
        die("poweroff: %s", strerror(errno));
    }
    if (console >= 0) {
        if (write(console, "F_ASTRA_FAST_POWEROFF\n", 22) < 0)
            fprintf(stderr, "f-astra-floor: console write failed\n");
        close(console);
    }
    sync();
    reboot(RB_POWER_OFF);
    die("reboot: %s", strerror(errno));
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "", *base = strrchr(argv[0], '/');
    /* the image's /usr/bin/f-astra-fast-poweroff (formerly f_astra_fast_poweroff.sh) is a link to this program */
    if (!strcmp(base ? base + 1 : argv[0], "f-astra-fast-poweroff") || !strcmp(cmd, "poweroff")) {
        fast_poweroff();
        return 0;
    }
    map_slcr();
    if (!strcmp(cmd, "enter")) {
        /* the PL clock words from the command line or, for a caller that passes only CPU_KHZ (f-astra-daily),
           from F_ASTRA_FLOOR_FCLK="FCLK0 FCLK1" */
        unsigned long f0 = 0, f1 = 0;
        const char *env = getenv("F_ASTRA_FLOOR_FCLK");
        if (argc > 4) {
            f0 = strtoul(argv[3], NULL, 0);
            f1 = strtoul(argv[4], NULL, 0);
        } else if (env && *env) {
            char *end;
            f0 = strtoul(env, &end, 0);
            f1 = strtoul(end, &end, 0);
            if (!f0 || !f1)
                die("F_ASTRA_FLOOR_FCLK: two words");
        }
        enter(argc > 2 ? argv[2] : "83334", (uint32_t)f0, (uint32_t)f1);
    }
    else if (!strcmp(cmd, "leave"))
        leave();
    else if (!strcmp(cmd, "status"))
        status();
    else {
        fprintf(stderr, "usage: f-astra-floor enter [CPU_KHZ [FCLK0 FCLK1]] | leave | status | poweroff\n");
        unmap_all();
        return 2;
    }
    unmap_all();
    return 0;
}
