/* F-ASTRA flight idle profile of the TE0715 in C (11/02: board code in C). C form of f_astra_flight_profile.sh: the
 * same knobs, steps, registers, values and status line; one /dev/mem mapping of the SLCR page instead of devmem
 * processes, the Si5338 through the I2C_SMBUS ioctl (the byte-data transfers of i2cset / i2cget), the SD card through
 * umount2 / mount; no awk, i2c-tools or shell processes and no heap allocation.
 *
 * In flight the link is CAN only, so test-bench parts go (on top of f-astra-power-profile and the PL deep idle of
 * f_astra_pl_idle.c). Measured at the 12 V input (10/21 ladder S0..SN..S0, both directions within 2 mW, PL image
 * loaded with its reset held, CAN acknowledged by the OBC side):
 *   usb     gadget unbound (the driver gates the USB0 clock)                         -0.019 W
 *   si5338  CLK2 (125 MHz MGT reference, unused) MultiSynth and driver down          -0.009 W
 *   pcap    PCAP clock 200 -> 15.9 MHz (never off: the PS-XADC temperature uses it)   -0.035 W
 *   sd      card unmounted, controller unbound, SDIO clocks gated                    -0.027 W
 *   cpu1    second core offline                                                      -0.004 W
 *   cpu     CPU 333 -> 83 MHz on the DDR PLL (cpu_1x 13.9 MHz; the USB controller   -0.034 W
 *           stops below 166 MHz; the kernel's timers slow down with it: SCALE file)
 *   armpll  after a suspend-to-RAM resume: bypassed and powered down again           -0.05 W
 * Registers: SLCR 0xF8000000 only, Si5338 over I2C 0x70. Never GPV 0xF8900000.
 *
 *   f-astra-flight-profile on KNOB... | off KNOB... (the caller gives the reverse order) | status
 * Every call first makes /run/f-astra-flight and its scale file ("1" when missing). on and off apply the knobs in the
 * given order (an unknown knob is reported and skipped), then print the status line:
 *   cpu=ARM_CLK aper=APER_CLK sdio=SDIO_CLK pcap=PCAP_CLK si33=SI5338_REG33 online=CPUS udc=UDC scale=SCALE
 * Exit 0, 2 on a usage error, 1 when /dev/mem cannot be mapped.
 * Build: cc -std=gnu99 -O2 -Wall -Wextra -Werror f_astra_flight_profile.c -o f-astra-flight-profile
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FLIGHT_DIR "/run/f-astra-flight"
#define SCALE FLIGHT_DIR "/scale"
#define GADGETS "/sys/kernel/config/usb_gadget"
#define GADGET_UDC "ci_hdrc.0"
#define SD_CTRL "e0100000.mmc"
#define SD_DRIVER "/sys/bus/platform/drivers/sdhci-arasan/"
#define SD_MNT "/media/sd-mmcblk0p1"
#define SD_DEV "/dev/mmcblk0p1"
#define SD_OPTIONS "fmask=0022,dmask=0022,codepage=437,iocharset=iso8859-1,shortname=mixed,errors=remount-ro"
#define CPUS "/sys/devices/system/cpu/online"
#define CPU1 "/sys/devices/system/cpu/cpu1/online"
#define SLCR 0xF8000000u
#define ARM_PLL 0x100u                 /* ARM_PLL_CTRL: PWRDWN bit 1, BYPASS_FORCE bit 4 */
#define ARM_CLK 0x120u                 /* ARM_CLK_CTRL: SRCSEL bits 5:4 (2 DDR PLL), DIVISOR bits 13:8 */
#define APER_CLK 0x12Cu                /* APER_CLK_CTRL: SDI0 AMBA clock bit 10 */
#define SDIO_CLK 0x150u                /* SDIO_CLK_CTRL: CLKACT0/1 bits 0..1 */
#define PCAP_CLK 0x168u
#define PCAP_SLOW 0x3F01u              /* knob on: IO PLL / 63, 15.9 MHz */
#define PCAP_RUN 0x501u                /* knob off: IO PLL / 5, 200 MHz */
#define SI5338_BUS "/dev/i2c-0"
#define SI5338_ADDR 0x70
#define SI_CLK2 33                     /* CLK2 MultiSynth / driver power-down */
#define SI_CLK2_PDN 0xe3               /* knob on: CLK2 MultiSynth and driver powered down */
#define SI_CLK2_RUN 0xc0               /* knob off: both running */

static volatile uint32_t *slcr;
static char gadget[512];               /* the first configfs gadget (see find_gadget), "" when none */

static uint32_t reg(uint32_t off) { return slcr[off / 4]; }
static void reg_write(uint32_t off, uint32_t value) { slcr[off / 4] = value; }
static void set_bits(uint32_t off, uint32_t mask, uint32_t bits) { slcr[off / 4] = (slcr[off / 4] & ~mask) | bits; }

static int map_slcr(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "f-astra-flight-profile: /dev/mem: %s\n", strerror(errno));
        return 0;
    }
    void *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, SLCR);
    int e = errno;
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "f-astra-flight-profile: map 0x%08X: %s\n", (unsigned)SLCR, strerror(e));
        return 0;
    }
    slcr = p;
    return 1;
}

static void sleep_ms(long ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR)
        ;
}

/* $(cat FILE): the text without its trailing newlines; "" when the file cannot be read */
static void read_text(const char *path, char *buf, size_t size)
{
    buf[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        n = 0;
    while (n > 0 && buf[n - 1] == '\n')
        n--;
    buf[n] = 0;
}

/* echo TEXT > FILE (the shell's open: create, truncate) */
static void write_line(const char *path, const char *text)
{
    char line[128];
    int n = snprintf(line, sizeof line, "%s\n", text);
    if (n < 0 || (size_t)n >= sizeof line)
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        fprintf(stderr, "f-astra-flight-profile: can't create %s: %s\n", path, strerror(errno));
        return;
    }
    ssize_t w = write(fd, line, (size_t)n);
    int e = errno;
    if (close(fd) || w != n)
        fprintf(stderr, "f-astra-flight-profile: write error: %s: %s\n", path, strerror(w < 0 ? e : errno));
}

/* one Si5338 byte-data register read (VALUE < 0: the value, -1 on failure) or write (0, -1), as i2cget / i2cset -y */
static int si5338(int reg_no, int value)
{
    int fd = open(SI5338_BUS, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    union i2c_smbus_data data;
    struct i2c_smbus_ioctl_data args;
    data.byte = (unsigned char)(value < 0 ? 0 : value);
    args.read_write = value < 0 ? I2C_SMBUS_READ : I2C_SMBUS_WRITE;
    args.command = (unsigned char)reg_no;
    args.size = I2C_SMBUS_BYTE_DATA;
    args.data = &data;
    int r = -1;
    if (ioctl(fd, I2C_SLAVE, SI5338_ADDR) == 0 && ioctl(fd, I2C_SMBUS, &args) == 0)
        r = value < 0 ? data.byte : 0;
    close(fd);
    if (value >= 0 && r < 0)
        fprintf(stderr, "f-astra-flight-profile: i2c 0x%02x register %d: write failed\n", SI5338_ADDR, reg_no);
    return r;
}

static int mounted(void)
{
    char buf[8192];
    int fd = open("/proc/mounts", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    size_t used = 0;
    ssize_t n;
    while (used + 1 < sizeof buf && (n = read(fd, buf + used, sizeof buf - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    buf[used] = 0;
    return strstr(buf, " " SD_MNT " ") != NULL;
}

static int is_block(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISBLK(st.st_mode);
}

/* the knob's "on": the cpu divisor and the timer scale file (DIVISOR / 4, four decimals) */
static void cpu_div(unsigned divisor)
{
    char scale[32];
    set_bits(ARM_CLK, 0x3F00u, divisor << 8);
    snprintf(scale, sizeof scale, "%u.%04u", divisor / 4, divisor % 4 * 2500);
    write_line(SCALE, scale);
}

/* one knob on (the power-saving state) or off; 1 for an unknown knob */
static int knob(const char *name, int on)
{
    if (!strcmp(name, "usb")) {
        if (gadget[0]) {
            char udc[sizeof gadget + 8];
            snprintf(udc, sizeof udc, "%s/UDC", gadget);
            write_line(udc, on ? "" : GADGET_UDC);
        }
    } else if (!strcmp(name, "si5338")) {
        si5338(SI_CLK2, on ? SI_CLK2_PDN : SI_CLK2_RUN);
    } else if (!strcmp(name, "pcap")) {
        reg_write(PCAP_CLK, on ? PCAP_SLOW : PCAP_RUN);
    } else if (!strcmp(name, "sd")) {
        if (on) {
            sync();
            umount2(SD_MNT, 0);                       /* 2>/dev/null: a card not mounted is no error */
            write_line(SD_DRIVER "unbind", SD_CTRL);
            sleep_ms(300);
            set_bits(SDIO_CLK, 0x3u, 0);
            set_bits(APER_CLK, 0x400u, 0);
        } else {
            set_bits(APER_CLK, 0x400u, 0x400u);
            set_bits(SDIO_CLK, 0x3u, 0x1u);
            write_line(SD_DRIVER "bind", SD_CTRL);
            for (int i = 0; !is_block(SD_DEV) && i < 50; i++)
                sleep_ms(100);
            if (!mounted() && mount(SD_DEV, SD_MNT, "vfat", MS_RELATIME, SD_OPTIONS))
                fprintf(stderr, "f-astra-flight-profile: mount %s: %s\n", SD_MNT, strerror(errno));
        }
    } else if (!strcmp(name, "cpu1")) {
        write_line(CPU1, on ? "0" : "1");
    } else if (!strcmp(name, "armpll")) {
        /* after a suspend-to-RAM resume the ARM PLL runs again while the CPU stays on the DDR PLL; off: nothing
         * (f-astra-power-profile stop brings it back with the CPU) */
        if (on) {
            uint32_t a = reg(ARM_PLL);
            uint32_t c = reg(ARM_CLK);
            if ((c & 0x30u) == 0x20u && !(a & 2u)) {
                reg_write(ARM_PLL, a | 0x10u);
                reg_write(ARM_PLL, a | 0x12u);
            }
        }
    } else if (!strcmp(name, "cpu")) {
        cpu_div(on ? 16 : 4);
    } else {
        fprintf(stderr, "unknown knob %s %s\n", name, on ? "on" : "off");
        return 1;
    }
    return 0;
}

static int status(void)
{
    char si33[16] = "", online[64], udc[64] = "", scale[32];
    uint32_t cpu = reg(ARM_CLK);
    uint32_t aper = reg(APER_CLK);
    uint32_t sdio = reg(SDIO_CLK);
    uint32_t pcap = reg(PCAP_CLK);
    int si = si5338(SI_CLK2, -1);
    if (si >= 0)
        snprintf(si33, sizeof si33, "0x%02x", (unsigned)si & 0xFFu);
    read_text(CPUS, online, sizeof online);
    if (gadget[0]) {
        char path[sizeof gadget + 8];
        snprintf(path, sizeof path, "%s/UDC", gadget);
        read_text(path, udc, sizeof udc);
    }
    read_text(SCALE, scale, sizeof scale);
    printf("cpu=0x%08X aper=0x%08X sdio=0x%08X pcap=0x%08X si33=%s online=%s udc=%s scale=%s\n", (unsigned)cpu,
           (unsigned)aper, (unsigned)sdio, (unsigned)pcap, si33, online, udc, scale);
    return fflush(stdout) ? 1 : 0;
}

/* G=$(ls -d GADGETS/STAR | head -1): the first entry by name, dot entries excluded */
static void find_gadget(void)
{
    char first[256] = "";
    DIR *dir = opendir(GADGETS);
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL)
        if (e->d_name[0] != '.' && (!first[0] || strcmp(e->d_name, first) < 0))
            snprintf(first, sizeof first, "%s", e->d_name);
    closedir(dir);
    if (first[0])
        snprintf(gadget, sizeof gadget, GADGETS "/%s", first);
}

int main(int argc, char **argv)
{
    struct stat st;
    if (mkdir(FLIGHT_DIR, 0777) && errno != EEXIST)
        fprintf(stderr, "f-astra-flight-profile: can't create directory '%s': %s\n", FLIGHT_DIR, strerror(errno));
    if (stat(SCALE, &st) || !S_ISREG(st.st_mode))
        write_line(SCALE, "1");
    find_gadget();
    const char *cmd = argc > 1 ? argv[1] : "";
    int on = !strcmp(cmd, "on"), off = !strcmp(cmd, "off");
    if (!on && !off && strcmp(cmd, "status")) {
        fprintf(stderr, "usage: %s on KNOB... | off KNOB... | status   (knobs: usb si5338 pcap sd cpu1 cpu armpll)\n",
                argv[0] ? argv[0] : "f-astra-flight-profile");
        return 2;
    }
    if (!map_slcr())
        return 1;
    if (on || off)
        for (int k = 2; k < argc; k++)
            knob(argv[k], on);
    int rc = status();
    munmap((void *)slcr, 4096);
    return rc;
}
