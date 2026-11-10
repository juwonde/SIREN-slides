/* F-ASTRA boot-path init programs of the TE0715 payload in C (11/02, user: the board's shell layer in C). One program,
 * /usr/bin/f-astra-boot, called through the init links under the name of the sh form it replaces (rc runs
 * /etc/rcS.d/S40f-astra-time-keep start: the name after the rc prefix picks the program) or as
 * f-astra-boot NAME ACTION. The sh forms stay in tools/petalinux/te0715-2020.2 (and tools/te0715-j1-can-runtime.sh) as
 * the reference: same checks in the same order, same files, same console lines; tests/test_boot_c.py compares them.
 *
 *   f-astra-time-keep {start|stop|save|status}      the SD card's time file: the clock moved forward to it at boot
 *                                                    (and the RTC written), the time written to it at shutdown;
 *                                                    both only forward
 *   f-astra-boot-watchdog {start|status|stop}        the SWDT reset-on-timeout overlay (hash checked), the driver
 *                                                    rebound, the watchdog started on the primary slot, a log line
 *   f-astra-j1-can {start|stop|status}               CAN0 on J1: the overlay with the runtime checks of
 *                                                    te0715-j1-can-runtime.sh, bit timing, left down
 *   f-astra-can-cmd {start|stop|status}              CAN0 up and the CSP v1 CMD node (node 12, OBC node 1)
 *   f-astra-boot-health {start|status|stop}          the Initial check and the Healthy Bit, the watchdog stopped
 *   f-astra-j12-rndis {start|stop|release|status}    the J12 USB RNDIS gadget, its addresses and the one-lease DHCP
 *   f-astra-j12-linklocal {start|status}             the J12 link-local address
 *
 * The system's ip (bit timing, addresses, status listings) and udhcpd are run as programs; everything else (configfs,
 * sysfs, the watchdog, the clock and RTC, register reads through /dev/mem, SHA-256, the log files) is done here.
 * F_ASTRA_BOOT_ROOT (tests only) prefixes the file paths of the SD card, /proc/mounts and /run.
 * Build: cc -std=gnu99 -O2 -Wall -Wextra -Werror f_astra_boot.c f_astra_ed25519.c -lz -o f-astra-boot
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <ifaddrs.h>
#include <linux/rtc.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "f_astra_ed25519.h"

#ifdef F_ASTRA_HAVE_ZLIB
#include <zlib.h>
#else
/* zlib's gz reader (zlib.h, stable since 1.0): declared here so the program builds where the library is installed
 * without its header (the Vitis ARM sysroot; the board image has libz.so.1) */
typedef struct gzFile_s *gzFile;
gzFile gzopen(const char *path, const char *mode);
char *gzgets(gzFile file, char *buf, int len);
int gzclose(gzFile file);
#endif

#define SD "/media/sd-mmcblk0p1"
#define STATE SD "/f-astra-boot.txt"
#define HEALTH_LOG SD "/f-astra-fm/boot-health.log"
#define INJECT SD "/f-astra-fm/inject-boot-unhealthy"
#define TIME_FILE SD "/f-astra-time.txt"
#define TIME_MIN 1704067200L           /* 2024-01-01 */
#define TIME_MAX 2147483647L           /* the 32-bit time_t of this image */
#define WDT "/proc/device-tree/amba/watchdog@f8005000"
#define SWDT_DTBO "/usr/share/f-astra/swdt-reset.dtbo"
#define SWDT_OVERLAY "/sys/kernel/config/device-tree/overlays/f_astra_swdt_reset"
#define CAN_DTBO "/usr/share/f-astra/j1-can0.dtbo"
#define CAN_OVERLAY "/sys/kernel/config/device-tree/overlays/f_astra_j1_can0"
#define CAN_NODE "/usr/bin/f-astra-csp-v1-node"
#define CAN_PIDFILE "/run/f-astra-can-cmd.pid"
#define GADGET "/sys/kernel/config/usb_gadget/f_astra_te0715_2020"
#define DHCP_CONF "/run/f-astra-j12-udhcpd.conf"
#define DHCP_PID "/run/f-astra-j12-udhcpd.pid"
#define RNDIS_STATUS "/run/f-astra-j12-rndis.status"
#define HOSTNAME "te0715-f-astra"
#define KERNEL_GLOB "5.4.*xilinx*v2020.2*f-astra-te0715*"
#define LOG_LINES 199                  /* the health log keeps its last 199 lines and the new one */

static const char *prog = "f-astra-boot";

static void die(int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(code);
}

/* a file path under F_ASTRA_BOOT_ROOT (tests) or as is */
static const char *at(const char *path, char *buf, size_t size)
{
    const char *root = getenv("F_ASTRA_BOOT_ROOT");
    if (!root || !*root)
        return path;
    if ((size_t)snprintf(buf, size, "%s%s", root, path) >= size)
        die(1, "path too long: %s", path);
    return buf;
}

static int exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

/* src into out[size], cut to fit */
static void copy_str(char *out, size_t size, const char *src)
{
    size_t k = strnlen(src, size - 1);
    memcpy(out, src, k);
    out[k] = 0;
}

/* the whole file into buf (NUL-terminated, NULs dropped as tr -d '\000'); its length, -1 when missing */
static long slurp(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t n = 0;
    for (;;) {
        ssize_t r = read(fd, buf + n, size - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n == size - 1)
            break;
    }
    close(fd);
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (buf[i])
            buf[w++] = buf[i];
    buf[w] = 0;
    return (long)w;
}

/* the first line of a file, cut at the newline; 0 when missing */
static int first_line(const char *path, char *buf, size_t size)
{
    if (slurp(path, buf, size) < 0)
        return 0;
    buf[strcspn(buf, "\n")] = 0;
    return 1;
}

/* write text over the file in place (no rename on the FAT partition), then sync; 0 on success */
static int put_in_place(const char *path, const char *text, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t r = write(fd, text + done, len - done);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        done += (size_t)r;
    }
    int rc = fsync(fd);
    if (close(fd) || rc)
        return -1;
    sync();
    return 0;
}

/* a sysfs or configfs attribute (printf '%s' or echo): one write; 0 on success */
static int put(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    size_t len = strlen(value);
    ssize_t r = write(fd, value, len);
    int c = close(fd);
    return r == (ssize_t)len && c == 0 ? 0 : -1;
}

/* the whole binary file written in one write (configfs dtbo); 0 on success */
static int copy_file(const char *from, const char *to)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return -1;
    struct stat st;
    if (fstat(in, &st) || st.st_size <= 0 || st.st_size > (1 << 20)) {
        close(in);
        return -1;
    }
    char *buf = malloc((size_t)st.st_size);
    if (!buf) {
        close(in);
        return -1;
    }
    ssize_t n = read(in, buf, (size_t)st.st_size);
    close(in);
    int rc = -1;
    if (n == st.st_size) {
        int out = open(to, O_WRONLY);
        if (out >= 0) {
            ssize_t w = write(out, buf, (size_t)n);
            rc = (close(out) == 0 && w == n) ? 0 : -1;
        }
    }
    free(buf);
    return rc;
}

/* SHA-256 of a file as lower-case hex; 0 on success */
static int file_sha256(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fa_sha256_ctx c;
    uint8_t buf[4096], d[32];
    fa_sha256_init(&c);
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;)
        fa_sha256_update(&c, buf, n);
    int bad = ferror(f);
    fclose(f);
    if (bad)
        return -1;
    fa_sha256_final(&c, d);
    for (int i = 0; i < 32; i++)
        sprintf(hex + 2 * i, "%02x", d[i]);
    return 0;
}

/* the first field of a sha256sum line */
static int hash_file_field(const char *path, char hex[65])
{
    char line[256];
    if (!first_line(path, line, sizeof line))
        return -1;
    size_t n = strcspn(line, " \t");
    if (n != 64)
        return -1;
    memcpy(hex, line, 64);
    hex[64] = 0;
    return 0;
}

static int mounted(const char *dir)
{
    char p[512], text[16384], pattern[256];
    if (slurp(at("/proc/mounts", p, sizeof p), text, sizeof text) < 0)
        return 0;
    snprintf(pattern, sizeof pattern, " %s ", dir);
    return strstr(text, pattern) != NULL;
}

static void wait_sd_mounted(void)
{
    for (int i = 0; !mounted(SD) && i < 10; i++)
        sleep(1);
}

/* the value of KEY in f-astra-boot.txt (as sed -n "s/^KEY=//p" | head -n 1, \r dropped); "" when absent */
static void state_value(const char *key, char *out, size_t size)
{
    char p[512], text[4096];
    out[0] = 0;
    if (slurp(at(STATE, p, sizeof p), text, sizeof text) < 0)
        return;
    size_t k = strlen(key);
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        char *w = line, *r = line;           /* tr -d '\r' anywhere in the line */
        for (; *r; r++)
            if (*r != '\r')
                *w++ = *r;
        *w = 0;
        if (!strncmp(line, key, k) && line[k] == '=') {
            copy_str(out, size, line + k + 1);
            return;
        }
        line = next;
    }
}

/* boot.scr boots the primary only for fa_slot=primary (or a missing value); anything else is the fallback */
static const char *slot(void)
{
    char s[64];
    state_value("fa_slot", s, sizeof s);
    return (!*s || !strcmp(s, "primary")) ? "primary" : "fallback";
}

static void uptime(char *out, size_t size)
{
    char p[512], line[128];
    if (!first_line(at("/proc/uptime", p, sizeof p), line, sizeof line))
        snprintf(line, sizeof line, "?");
    line[strcspn(line, " ")] = 0;
    copy_str(out, size, line);
}

/* the health log: its last 199 lines and LINE written back in place (no rename on the FAT partition), then sync */
static void log_line(const char *line)
{
    char p[512], d[512];
    if (!mounted(SD))
        return;
    const char *log = at(HEALTH_LOG, p, sizeof p);
    copy_str(d, sizeof d, log);
    *strrchr(d, '/') = 0;
    mkdir(d, 0755);
    char *old = NULL;
    size_t n = 0;
    FILE *f = fopen(log, "rb");
    if (f) {
        size_t cap = 0;
        char buf[4096];
        for (size_t r; (r = fread(buf, 1, sizeof buf, f)) > 0;) {
            if (n + r + 1 > cap) {
                size_t want = (n + r + 1) * 2;
                char *grown = realloc(old, want);
                if (!grown) {
                    free(old);
                    fclose(f);
                    return;
                }
                old = grown;
                cap = want;
            }
            memcpy(old + n, buf, r);
            n += r;
        }
        fclose(f);
    }
    /* tail -n 199: past the final newline, the 199th newline from the end starts the kept lines */
    size_t start = 0, newlines = 0, i = (n && old[n - 1] == '\n') ? n - 1 : n;
    for (; i > 0; i--)
        if (old[i - 1] == '\n' && ++newlines == LOG_LINES) {
            start = i;
            break;
        }
    size_t keep = n - start, len = strlen(line);
    char *text = malloc(keep + len + 2);
    if (!text) {
        free(old);
        return;
    }
    if (keep)
        memcpy(text, old + start, keep);
    if (keep && text[keep - 1] != '\n')
        text[keep++] = '\n';
    memcpy(text + keep, line, len);
    text[keep + len] = '\n';
    put_in_place(log, text, keep + len + 1);
    free(text);
    free(old);
}

/* a register read through /dev/mem as busybox devmem prints it */
static uint32_t devmem(uint32_t addr)
{
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0)
        die(1, "/dev/mem: %s", strerror(errno));
    uint32_t page = addr & ~4095u;
    volatile uint32_t *p = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, page);
    close(fd);
    if (p == MAP_FAILED)
        die(1, "map 0x%08X: %s", (unsigned)addr, strerror(errno));
    uint32_t v = p[(addr - page) / 4];
    munmap((void *)p, 4096);
    return v;
}

/* run a system program (ip, udhcpd) and wait; its exit status, -1 when it could not run */
static int run(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* pidof: a process whose name (comm, or argv[0]'s base name past comm's 15 characters) is NAME */
static int pidof(const char *name)
{
    char p[64], text[512];
    for (int pid = 1; pid < 32768; pid++) {
        snprintf(p, sizeof p, "/proc/%d/comm", pid);
        if (!first_line(p, text, sizeof text))
            continue;
        if (!strcmp(text, name))
            return pid;
        if (strlen(text) == 15 && !strncmp(text, name, 15)) {
            snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
            int fd = open(p, O_RDONLY);
            if (fd < 0)
                continue;
            ssize_t n = read(fd, text, sizeof text - 1);
            close(fd);
            if (n <= 0)
                continue;
            text[n] = 0;
            const char *base = strrchr(text, '/');
            if (!strcmp(base ? base + 1 : text, name))
                return pid;
        }
    }
    return 0;
}

static int hostname_is_board(void)
{
    char h[256] = "";
    gethostname(h, sizeof h - 1);
    return !strcmp(h, HOSTNAME);
}

static int kernel_is_board(void)
{
    struct utsname u;
    return uname(&u) == 0 && fnmatch(KERNEL_GLOB, u.release, 0) == 0;
}

/* the base name of the device tree node behind a network device ("readlink -f .../device/of_node") */
static int of_node_is(const char *ifname, const char *node)
{
    char p[256], real[PATH_MAX];
    snprintf(p, sizeof p, "/sys/class/net/%s/device/of_node", ifname);
    if (!realpath(p, real))
        return 0;
    const char *base = strrchr(real, '/');
    return !strcmp(base ? base + 1 : real, node);
}

static int link_flags(const char *ifname, int set_up, int set_down)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    struct ifreq r;
    memset(&r, 0, sizeof r);
    snprintf(r.ifr_name, IFNAMSIZ, "%s", ifname);
    int rc = ioctl(s, SIOCGIFFLAGS, &r);
    if (rc == 0 && (set_up || set_down)) {
        if (set_up)
            r.ifr_flags |= IFF_UP;
        else
            r.ifr_flags &= ~IFF_UP;
        rc = ioctl(s, SIOCSIFFLAGS, &r);
    }
    close(s);
    return rc == 0 ? r.ifr_flags : -1;
}

static int can_bit_timing(void)
{
    char *const ip[] = {"ip", "link", "set", "can0", "type", "can", "bitrate", "500000", "sample-point", "0.875",
                        "loopback", "off", "berr-reporting", "on", NULL};
    return run(ip);
}

/* ---- f-astra-time-keep ---- */

/* the saved seconds, or 0 when the file is missing or not a plausible time (first line, digits only) */
static long saved_time(void)
{
    char p[512], line[256], digits[256];
    if (!first_line(at(TIME_FILE, p, sizeof p), line, sizeof line))
        return 0;
    size_t n = 0;
    for (char *c = line; *c && n < sizeof digits - 1; c++)
        if (isdigit((unsigned char)*c))
            digits[n++] = *c;
    digits[n] = 0;
    if (!n || n > 10)
        return 0;
    long s = strtol(digits, NULL, 10);
    return (s >= TIME_MIN && s <= TIME_MAX) ? s : 0;
}

static int time_keep(const char *action)
{
    char p[512];
    if (!strcmp(action, "start")) {
        wait_sd_mounted();
        long last = saved_time(), now = (long)time(NULL);
        if (last && last > now) {
            struct timespec ts = {last, 0};
            if (clock_settime(CLOCK_REALTIME, &ts) == 0) {          /* date -u -s @last && hwclock -w -u */
                int rtc = open("/dev/rtc0", O_WRONLY);
                if (rtc < 0)
                    rtc = open("/dev/rtc", O_WRONLY);
                time_t t = last;
                struct tm tm;
                gmtime_r(&t, &tm);
                if (rtc < 0 || ioctl(rtc, RTC_SET_TIME, (struct rtc_time *)&tm) < 0)
                    fprintf(stderr, "%s: RTC not written: %s\n", prog, strerror(errno));
                if (rtc >= 0)
                    close(rtc);
            } else
                fprintf(stderr, "%s: clock not set: %s\n", prog, strerror(errno));
            printf("F_ASTRA_TIME_KEEP_RESTORED from=%ld to=%ld\n", now, last);
        } else if (last)
            printf("F_ASTRA_TIME_KEEP_UNCHANGED now=%ld saved=%ld\n", now, last);
        else
            printf("F_ASTRA_TIME_KEEP_UNCHANGED now=%ld saved=none\n", now);
        return 0;
    }
    if (!strcmp(action, "stop") || !strcmp(action, "save")) {
        if (!mounted(SD))
            return 0;
        long last = saved_time(), now = (long)time(NULL);
        if (last && last > now) {
            printf("F_ASTRA_TIME_KEEP_NOT_SAVED now=%ld saved=%ld\n", now, last);
            return 0;
        }
        char text[32];
        int len = snprintf(text, sizeof text, "%ld\n", now);
        return put_in_place(at(TIME_FILE, p, sizeof p), text, (size_t)len) ? 1 : 0;
    }
    if (!strcmp(action, "status")) {
        long last = saved_time();
        if (last)
            printf("saved=%ld now=%ld\n", last, (long)time(NULL));
        else
            printf("saved= now=%ld\n", (long)time(NULL));
        return 0;
    }
    fprintf(stderr, "usage: f-astra-time-keep {start|stop|save|status}\n");
    return 2;
}

/* ---- f-astra-boot-watchdog ---- */

static int boot_watchdog(const char *action)
{
    if (!strcmp(action, "start")) {
        wait_sd_mounted();
        int ready = 0;
        char compat[128], want[65], got[65];
        if (slurp(WDT "/compatible", compat, sizeof compat) >= 0 && !strcmp(compat, "cdns,wdt-r1p2") && exists(SWDT_DTBO) &&
            file_sha256(SWDT_DTBO, got) == 0 && hash_file_field(SWDT_DTBO ".sha256", want) == 0 && !strcmp(got, want)) {
            if (!mounted("/sys/kernel/config"))
                mount("none", "/sys/kernel/config", "configfs", 0, NULL);
            if (!exists(WDT "/reset-on-timeout") && mkdir(SWDT_OVERLAY, 0755) == 0)
                copy_file(SWDT_DTBO, SWDT_OVERLAY "/dtbo");
            if (exists(WDT "/reset-on-timeout")) {
                put("/sys/bus/platform/drivers/cdns-wdt/unbind", "f8005000.watchdog\n");
                put("/sys/bus/platform/drivers/cdns-wdt/bind", "f8005000.watchdog\n");
                struct stat st;
                if (stat("/dev/watchdog", &st) == 0 && S_ISCHR(st.st_mode))
                    ready = 1;
            }
        }
        const char *s = slot();
        int armed = 0;
        /* open and close without 'V': the watchdog runs on (CONFIG_WATCHDOG_NOWAYOUT is off, so 'V' stops it later) */
        if (!strcmp(s, "primary") && ready) {
            int fd = open("/dev/watchdog", O_WRONLY);
            if (fd >= 0) {
                close(fd);
                armed = 1;
            }
        }
        char tries[64], up[64], line[512];
        state_value("fa_tries", tries, sizeof tries);
        uptime(up, sizeof up);
        snprintf(line, sizeof line, "%ld watchdog uptime=%s slot=%s tries=%s swdt_reset=%d armed=%d reboot_status=0x%08X",
                 (long)time(NULL), up, s, tries, ready, armed, (unsigned)devmem(0xF8000258u));
        log_line(line);
        printf("F_ASTRA_BOOT_WATCHDOG slot=%s swdt_reset=%d armed=%d\n", s, ready, armed);
        return 0;
    }
    if (!strcmp(action, "status")) {
        char tries[64];
        state_value("fa_tries", tries, sizeof tries);
        printf("slot=%s tries=%s swdt_reset=%d\n", slot(), tries, exists(WDT "/reset-on-timeout"));
        return 0;
    }
    if (!strcmp(action, "stop"))
        return 0;
    fprintf(stderr, "usage: f-astra-boot-watchdog {start|status}\n");
    return 2;
}

/* ---- f-astra-j1-can (with te0715-j1-can-runtime.sh) ---- */

static int dt_status_disabled(const char *node)
{
    char p[256], s[64];
    snprintf(p, sizeof p, "/proc/device-tree/amba/%s/status", node);
    return slurp(p, s, sizeof s) >= 0 && !strcmp(s, "disabled");
}

/* CONFIG_CAN_XILINXCAN=y in /proc/config.gz (zcat | grep -qx) */
static int kernel_has_xilinx_can(void)
{
    gzFile z = gzopen("/proc/config.gz", "rb");
    if (!z)
        return 0;
    char line[512];
    int found = 0;
    while (!found && gzgets(z, line, sizeof line)) {
        line[strcspn(line, "\n")] = 0;
        found = !strcmp(line, "CONFIG_CAN_XILINXCAN=y");
    }
    gzclose(z);
    return found;
}

/* apply the source-bound J1 CAN overlay without reboot or PL programming; CAN stays down */
static int j1_can_runtime(const char *dtbo, const char *hash)
{
    char got[65], boot[64], boot2[64];
    if (!hostname_is_board())
        die(2, "wrong target");
    if (!kernel_is_board())
        die(2, "wrong kernel");
    if (strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64 || !exists(dtbo))
        exit(2);
    if (file_sha256(dtbo, got) || strcmp(got, hash))
        die(2, "changed overlay");
    if (!kernel_has_xilinx_can())
        exit(1);
    if (!dt_status_disabled("i2c@e0004000") || !dt_status_disabled("can@e0008000"))
        exit(1);
    if (!exists("/sys/bus/platform/devices/700.pinctrl"))
        exit(1);
    if (if_nametoindex("can0"))
        die(2, "CAN already present; inspect instead of replay");
    mkdir("/run/lock", 0755);
    int lock = open("/run/lock/f-astra-fpga-device.lock", O_WRONLY | O_CREAT, 0644);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB))
        die(2, "board owned by another task");
    if (exists(CAN_OVERLAY))
        die(2, "attempt already exists");
    if (!first_line("/proc/sys/kernel/random/boot_id", boot, sizeof boot))
        exit(1);
    uint32_t fclk = devmem(0xF8000170u), pll = devmem(0xF8000108u);
    char status[64];
    if (mkdir(CAN_OVERLAY, 0755) || copy_file(dtbo, CAN_OVERLAY "/dtbo") ||
        !first_line(CAN_OVERLAY "/status", status, sizeof status) || strcmp(status, "applied"))
        exit(1);
    unsigned idx = if_nametoindex("can0");
    if (!idx)
        exit(1);
    printf("can0: ifindex %u\n", idx);
    if (!first_line("/proc/sys/kernel/random/boot_id", boot2, sizeof boot2) || strcmp(boot, boot2) ||
        devmem(0xF8000170u) != fclk || devmem(0xF8000108u) != pll)
        exit(1);
    printf("CAN0_J1_REGISTERED_DOWN_NO_PL_REPROGRAM\n");
    fflush(stdout);
    close(lock);
    return 0;
}

static int j1_can(const char *action)
{
    if (!strcmp(action, "start")) {
        if (!exists("/sys/class/net/can0")) {
            char hash[65];
            if (hash_file_field("/usr/share/f-astra/j1-can0.sha256", hash))
                exit(1);
            j1_can_runtime(CAN_DTBO, hash);
            if (can_bit_timing())
                exit(1);
        }
        if (!of_node_is("can0", "can@e0008000"))
            exit(1);
        printf("CAN0_J1_READY_EXTERNAL_COMMUNICATION_NOT_TESTED\n");
        return 0;
    }
    if (!strcmp(action, "stop"))
        return link_flags("can0", 0, 1) < 0;
    if (!strcmp(action, "status")) {
        char *const ip[] = {"ip", "-details", "-statistics", "link", "show", "can0", NULL};
        return run(ip);
    }
    fprintf(stderr, "usage: f-astra-j1-can {start|stop|status}\n");
    return 2;
}

/* ---- f-astra-can-cmd ---- */

static int can_cmd_pid(void)
{
    char p[512], line[64];
    if (!first_line(at(CAN_PIDFILE, p, sizeof p), line, sizeof line))
        return 0;
    int pid = atoi(line);
    return pid > 0 && kill(pid, 0) == 0 ? pid : 0;
}

static int can_cmd(const char *action)
{
    char p[512];
    if (!strcmp(action, "start")) {
        if (!of_node_is("can0", "can@e0008000"))
            exit(1);
        if (can_cmd_pid()) {
            printf("F_ASTRA_CAN_CMD_ALREADY_RUNNING\n");
            return 0;
        }
        int flags = link_flags("can0", 0, 0);
        if (flags < 0)
            exit(1);
        if (!(flags & IFF_UP)) {
            if (can_bit_timing() || link_flags("can0", 1, 0) < 0)
                exit(1);
        }
        /* every time the OBC sets is also kept on the SD card (f-astra-time-keep restores it after a power cycle) */
        setenv("F_ASTRA_TIME_FILE", TIME_FILE, 1);
        fflush(stdout);
        pid_t pid = fork();                 /* start-stop-daemon -S -b -m: in the background, the pid file made here */
        if (pid < 0)
            exit(1);
        if (pid == 0) {
            setsid();
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) {
                dup2(null, 0);
                dup2(null, 1);
                dup2(null, 2);
                if (null > 2)
                    close(null);
            }
            execl(CAN_NODE, CAN_NODE, "can0", "12", "--cmd", "1", (char *)NULL);
            _exit(127);
        }
        char text[32];
        int len = snprintf(text, sizeof text, "%d\n", (int)pid);
        int fd = open(at(CAN_PIDFILE, p, sizeof p), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || write(fd, text, (size_t)len) != len || close(fd))
            exit(1);
        printf("F_ASTRA_CAN_CMD_STARTED\n");
        return 0;
    }
    if (!strcmp(action, "stop")) {
        int pid = can_cmd_pid();
        if (pid) {                          /* start-stop-daemon -K -p PIDFILE -x NODE: only that program */
            char exe[64], real[PATH_MAX];
            snprintf(exe, sizeof exe, "/proc/%d/exe", pid);
            ssize_t n = readlink(exe, real, sizeof real - 1);
            if (n > 0) {
                real[n] = 0;
                if (!strcmp(real, CAN_NODE))
                    kill(pid, SIGTERM);
            }
        }
        unlink(at(CAN_PIDFILE, p, sizeof p));
        return link_flags("can0", 0, 1) < 0;
    }
    if (!strcmp(action, "status")) {
        int pid = can_cmd_pid();
        if (pid)
            printf("running %d\n", pid);
        else
            printf("stopped\n");
        fflush(stdout);
        char *const ip[] = {"ip", "-details", "-statistics", "link", "show", "can0", NULL};
        return run(ip);
    }
    fprintf(stderr, "usage: f-astra-can-cmd {start|stop|status}\n");
    return 2;
}

/* ---- f-astra-boot-health ---- */

static void temperature(char *out, size_t size)
{
    char r[64], o[64], s[64];
    out[0] = 0;
    if (!first_line("/sys/bus/iio/devices/iio:device0/in_temp0_raw", r, sizeof r) ||
        !first_line("/sys/bus/iio/devices/iio:device0/in_temp0_offset", o, sizeof o) ||
        !first_line("/sys/bus/iio/devices/iio:device0/in_temp0_scale", s, sizeof s))
        return;
    snprintf(out, size, "%.1f", (strtod(r, NULL) + strtod(o, NULL)) * strtod(s, NULL) / 1000.0);
}

static int has_ipv4(const char *addr)
{
    struct ifaddrs *list, *i;
    struct in_addr want;
    int found = 0;
    if (inet_pton(AF_INET, addr, &want) != 1 || getifaddrs(&list))
        return 0;
    for (i = list; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
            ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == want.s_addr)
            found = 1;
    freeifaddrs(list);
    return found;
}

static int boot_health(const char *action)
{
    char p[512];
    if (!strcmp(action, "start")) {
        const char *s = slot();
        char tries[64], why[128] = "", inject[64], up[64], temp[32], common[256], line[512];
        state_value("fa_tries", tries, sizeof tries);
        char rw[600];
        snprintf(rw, sizeof rw, "%s/.f-astra-boot-rw", at(SD, p, sizeof p));
        if (!mounted(SD) || put_in_place(rw, "ok\n", 3) || unlink(rw))
            strcat(why, "sd,");
        if (!hostname_is_board())
            strcat(why, "hostname,");
        if (!pidof("sshd"))
            strcat(why, "sshd,");
        if (!access("/etc/init.d/f-astra-can-cmd", X_OK) && !pidof("f-astra-csp-v1-node"))
            strcat(why, "can_cmd,");
        if (!has_ipv4("192.168.71.2"))
            strcat(why, "j12,");
        if (slurp(at(INJECT, p, sizeof p), inject, sizeof inject) >= 0) {
            size_t w = 0;                   /* tr -cd a-z */
            for (size_t r = 0; inject[r]; r++)
                if (inject[r] >= 'a' && inject[r] <= 'z')
                    inject[w++] = inject[r];
            inject[w] = 0;
            if (!strcmp(inject, s))
                strcat(why, "injected,");
        }
        long now = (long)time(NULL);
        uptime(up, sizeof up);
        temperature(temp, sizeof temp);
        snprintf(common, sizeof common, "uptime=%s slot=%s tries=%s temp_c=%s", up, s, *tries ? tries : "none", temp);
        if (!*why) {
            int stopped = 0;
            int fd = open("/dev/watchdog", O_WRONLY);
            if (fd >= 0) {
                stopped = write(fd, "V", 1) == 1;
                if (close(fd))
                    stopped = 0;
            }
            if (!strcmp(s, "primary")) {
                /* in place (no rename on FAT); boot.scr reads an empty file as the primary slot with no tries */
                char text[96];
                int len = snprintf(text, sizeof text, "fa_slot=primary\nfa_tries=0\nfa_healthy=%ld\n", now);
                put_in_place(at(STATE, p, sizeof p), text, (size_t)len);
            }
            snprintf(line, sizeof line, "%ld healthy %s watchdog_stopped=%d", now, common, stopped);
            log_line(line);
            printf("F_ASTRA_BOOT_HEALTHY slot=%s watchdog_stopped=%d\n", s, stopped);
        } else {
            why[strlen(why) - 1] = 0;
            snprintf(line, sizeof line, "%ld unhealthy %s why=%s", now, common, why);
            log_line(line);
            printf("F_ASTRA_BOOT_UNHEALTHY slot=%s why=%s\n", s, why);
        }
        return 0;
    }
    if (!strcmp(action, "status")) {
        char tries[64], healthy[64];
        state_value("fa_tries", tries, sizeof tries);
        state_value("fa_healthy", healthy, sizeof healthy);
        printf("slot=%s tries=%s healthy=%s\n", slot(), tries, healthy);
        return 0;
    }
    if (!strcmp(action, "stop"))
        return 0;
    fprintf(stderr, "usage: f-astra-boot-health {start|status}\n");
    return 2;
}

/* ---- f-astra-j12-rndis ---- */

static void note(const char *text)
{
    char p[512], line[256];
    int len = snprintf(line, sizeof line, "%s\n", text);
    int fd = open(at(RNDIS_STATUS, p, sizeof p), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, line, (size_t)len) != len)
            fprintf(stderr, "%s: status not written\n", prog);
        close(fd);
    }
    openlog("f-astra-j12-rndis", 0, LOG_USER);
    syslog(LOG_NOTICE, "%s", text);
    closelog();
}

static void check_profile(void)
{
    if (!hostname_is_board())
        die(2, "wrong board profile");
    if (!kernel_is_board())
        die(2, "wrong kernel generation");
}

/* the DHCP server of this script only (its pid file, its configuration on the command line) */
static int stop_dhcp(void)
{
    char p[512], line[64], path[64], cmd[1024];
    const char *pidfile = at(DHCP_PID, p, sizeof p);
    if (access(pidfile, R_OK))
        return 0;
    if (!first_line(pidfile, line, sizeof line) || !*line || strspn(line, "0123456789") != strlen(line))
        return 1;
    int pid = atoi(line);
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, cmd, sizeof cmd - 1);
        close(fd);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++)
                if (!cmd[i])
                    cmd[i] = ' ';
            cmd[n] = 0;
            if (strstr(cmd, DHCP_CONF))
                kill(pid, SIGTERM);
        }
    }
    unlink(pidfile);
    return 0;
}

static int gadget_put(const char *rel, const char *value)
{
    char path[512];
    snprintf(path, sizeof path, GADGET "/%s", rel);
    if (put(path, value))
        die(1, "%s: %s", path, strerror(errno));
    return 0;
}

static void gadget_mkdir(const char *rel)
{
    char path[512];
    snprintf(path, sizeof path, GADGET "%s%s", *rel ? "/" : "", rel);
    if (mkdir(path, 0755) && errno != EEXIST)
        die(1, "%s: %s", path, strerror(errno));
}

static void read_udc(char *out, size_t size)
{
    if (!first_line(GADGET "/UDC", out, size))
        out[0] = 0;
}

static void release_gadget(void)
{
    check_profile();
    stop_dhcp();
    if (exists(GADGET)) {
        char bound[64];
        read_udc(bound, sizeof bound);
        if (*bound && strcmp(bound, "ci_hdrc.0")) {
            note("failed:unexpected-udc");
            exit(1);
        }
        put(GADGET "/UDC", "");
        /* UDC disconnect keeps the RNDIS instance and its registered usb0; remove it before the caller can unbind its
         * controller, otherwise usb0 keeps a freed parent device on this 5.4 kernel */
        struct stat st;
        if (!lstat(GADGET "/os_desc/c.1", &st) && S_ISLNK(st.st_mode))
            unlink(GADGET "/os_desc/c.1");
        if (!lstat(GADGET "/configs/c.1/rndis.usb0", &st) && S_ISLNK(st.st_mode))
            unlink(GADGET "/configs/c.1/rndis.usb0");
        rmdir(GADGET "/functions/rndis.usb0");
        rmdir(GADGET "/configs/c.1/strings/0x409");
        rmdir(GADGET "/configs/c.1");
        rmdir(GADGET "/strings/0x409");
        if (rmdir(GADGET))
            die(1, "%s: %s", GADGET, strerror(errno));
    }
    if (exists("/sys/class/net/usb0")) {
        note("failed:netdev-still-present");
        exit(1);
    }
    note("released:configfs-and-netdev-removed");
}

static int address_replace(const char *cidr)
{
    char a[32];
    copy_str(a, sizeof a, cidr);
    char *const ip[] = {"ip", "address", "replace", a, "dev", "usb0", NULL};
    return run(ip);
}

static int j12_rndis(const char *action)
{
    char p[512];
    if (!strcmp(action, "stop")) {
        stop_dhcp();
        if (exists(GADGET))
            put(GADGET "/UDC", "");
        note("stopped");
        return 0;
    }
    if (!strcmp(action, "release")) {
        release_gadget();
        return 0;
    }
    if (!strcmp(action, "status")) {
        char text[512];
        if (slurp(at(RNDIS_STATUS, p, sizeof p), text, sizeof text) >= 0)
            fputs(text, stdout);
        if (first_line("/sys/class/udc/ci_hdrc.0/state", text, sizeof text))
            printf("%s\n", text);
        if (first_line("/sys/class/udc/ci_hdrc.0/current_speed", text, sizeof text))
            printf("%s\n", text);
        fflush(stdout);
        char *const ip[] = {"ip", "-brief", "address", "show", "usb0", NULL};
        run(ip);
        return 0;
    }
    if (strcmp(action, "start")) {
        fprintf(stderr, "usage: f-astra-j12-rndis {start|stop|release|status}\n");
        return 2;
    }
    check_profile();
    note("starting");
    if (!mounted("/sys/kernel/config"))
        mount("configfs", "/sys/kernel/config", "configfs", 0, NULL);
    for (int remaining = 60; !exists("/sys/class/udc/ci_hdrc.0") && remaining > 0; remaining--)
        sleep(1);
    if (!exists("/sys/class/udc/ci_hdrc.0")) {
        note("failed:no-native-udc");
        return 1;
    }
    /* never steal a UDC from another gadget, or unload/rebind the controller */
    DIR *gadgets = opendir("/sys/kernel/config/usb_gadget");
    if (gadgets) {
        struct dirent *e;
        char other[512], owner[64];
        while ((e = readdir(gadgets))) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "f_astra_te0715_2020"))
                continue;
            snprintf(other, sizeof other, "/sys/kernel/config/usb_gadget/%s/UDC", e->d_name);
            if (first_line(other, owner, sizeof owner) && *owner) {
                closedir(gadgets);
                note("failed:udc-owned-by-other-gadget");
                return 1;
            }
        }
        closedir(gadgets);
    }
    if (!exists(GADGET)) {
        gadget_mkdir("");
        gadget_put("idVendor", "0x1d6b");
        gadget_put("idProduct", "0x0104");
        gadget_put("bcdDevice", "0x0200");
        gadget_put("bcdUSB", "0x0200");
        gadget_put("bDeviceClass", "0xef");
        gadget_put("bDeviceSubClass", "0x04");
        gadget_put("bDeviceProtocol", "0x01");
        gadget_mkdir("strings/0x409");
        gadget_mkdir("configs/c.1");
        gadget_mkdir("configs/c.1/strings/0x409");
        gadget_put("strings/0x409/serialnumber", "F-ASTRA-TE0715-J12-2020R1");
        gadget_put("strings/0x409/manufacturer", "F-ASTRA");
        gadget_put("strings/0x409/product", "TE0715 J12 RNDIS 2020.2");
        gadget_put("configs/c.1/strings/0x409/configuration", "RNDIS only");
        gadget_put("configs/c.1/bmAttributes", "0xc0");
        gadget_put("configs/c.1/MaxPower", "2");
        gadget_mkdir("functions/rndis.usb0");
        gadget_put("functions/rndis.usb0/dev_addr", "02:71:15:00:00:02");
        gadget_put("functions/rndis.usb0/host_addr", "02:71:15:00:00:01");
        gadget_put("functions/rndis.usb0/class", "0xef");
        gadget_put("functions/rndis.usb0/subclass", "0x04");
        gadget_put("functions/rndis.usb0/protocol", "0x01");
        gadget_put("os_desc/use", "1");
        gadget_put("os_desc/b_vendor_code", "0xcd");
        gadget_put("os_desc/qw_sign", "MSFT100");
        gadget_put("functions/rndis.usb0/os_desc/interface.rndis/compatible_id", "RNDIS");
        gadget_put("functions/rndis.usb0/os_desc/interface.rndis/sub_compatible_id", "5162001");
        if (symlink(GADGET "/functions/rndis.usb0", GADGET "/configs/c.1/rndis.usb0") ||
            symlink(GADGET "/configs/c.1", GADGET "/os_desc/c.1"))
            die(1, "gadget links: %s", strerror(errno));
    }
    char bound[64];
    read_udc(bound, sizeof bound);
    if (*bound && strcmp(bound, "ci_hdrc.0")) {
        note("failed:unexpected-udc");
        return 1;
    }
    if (!*bound)
        gadget_put("UDC", "ci_hdrc.0");
    char iface[64];
    if (!first_line(GADGET "/functions/rndis.usb0/ifname", iface, sizeof iface) || strcmp(iface, "usb0")) {
        note("failed:unexpected-netdev");
        return 1;
    }
    if (address_replace("192.168.71.2/24"))
        return 1;
    /* Windows may retain an APIPA address with DHCP disabled after reconnecting: a second, explicit point-to-point
     * address across every cold boot */
    if (address_replace("169.254.25.253/16") || link_flags("usb0", 1, 0) < 0)
        return 1;
    stop_dhcp();
    /* one USB lease only, no router/DNS option and no default-route changes */
    char conf[512];
    int len = snprintf(conf, sizeof conf, "start 192.168.71.1\nend 192.168.71.1\ninterface usb0\nmax_leases 1\noption subnet "
                       "255.255.255.0\noption lease 3600\npidfile %s\nlease_file /run/f-astra-j12-udhcpd.leases\nauto_time 0\n",
                       at(DHCP_PID, p, sizeof p));
    int fd = open(at(DHCP_CONF, p, sizeof p), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, conf, (size_t)len) != len || close(fd))
        return 1;
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        return 1;
    if (pid == 0) {
        setsid();
        int log = open("/run/f-astra-j12-udhcpd.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0)
            dup2(null, 0);
        if (log >= 0) {
            dup2(log, 1);
            dup2(log, 2);
        }
        execlp("udhcpd", "udhcpd", "-f", DHCP_CONF, (char *)NULL);
        _exit(127);
    }
    sleep(1);
    if (kill(pid, 0) || waitpid(pid, NULL, WNOHANG) == pid) {
        note("failed:dhcp-process-exited");
        return 1;
    }
    note("configured:usb0 board=192.168.71.2,169.254.25.253; transport-not-yet-tested");
    return 0;
}

/* ---- f-astra-j12-linklocal ---- */

static int j12_linklocal(const char *action)
{
    if (!hostname_is_board())
        exit(2);
    char *const show[] = {"ip", "-brief", "address", "show", "usb0", NULL};
    if (!strcmp(action, "start")) {
        /* do not rebind USB, reset the PHY, change PLLs, or change any PC route */
        char mac[64];
        if (!first_line("/sys/class/net/usb0/address", mac, sizeof mac) || strcmp(mac, "02:71:15:00:00:02"))
            exit(1);
        if (address_replace("169.254.25.253/16") || link_flags("usb0", 1, 0) < 0)
            exit(1);
        fflush(stdout);
        run(show);
        printf("J12_LINKLOCAL_PERSISTENCE_READY\n");
        return 0;
    }
    if (!strcmp(action, "status"))
        return run(show);
    fprintf(stderr, "usage: f-astra-j12-linklocal {start|status}\n");
    return 2;
}

/* ---- f-astra-sysinit, f-astra-shutdown (busybox init's sysinit and shutdown entries; the Yocto image r21) ---- */

static void mount_fs(const char *source, const char *target, const char *type, unsigned long flags, const char *data)
{
    mkdir(target, 0755);
    if (!mounted(target) && mount(source, target, type, flags, data))
        fprintf(stderr, "%s: mount %s: %s\n", prog, target, strerror(errno));
}

/* a step as its own process (an exit or a failed check ends that step only, as one rc script did) */
static int step(int (*fn)(const char *), const char *name, const char *action)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        prog = name;
        exit(fn(action));
    }
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int start_program(char *const argv[])
{
    fflush(stdout);
    return run(argv);
}

static void loopback_up(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return;
    struct ifreq r;
    memset(&r, 0, sizeof r);
    snprintf(r.ifr_name, IFNAMSIZ, "lo");
    struct sockaddr_in *a = (struct sockaddr_in *)&r.ifr_addr;
    a->sin_family = AF_INET;
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ioctl(s, SIOCSIFADDR, &r))
        fprintf(stderr, "%s: lo address: %s\n", prog, strerror(errno));
    close(s);
    link_flags("lo", 1, 0);
}

/* the SD card's boot partition at its mount point, once the MMC driver has made the device (10 s at most), with the
   options image r20's udev mounted it with (f_astra_floor.c and f_astra_flight_profile.c remount it the same way) */
static void mount_sd(void)
{
    struct stat st;
    for (int i = 0; i < 1000 && stat("/dev/mmcblk0p1", &st); i++)
        usleep(10000);
    mount_fs("/dev/mmcblk0p1", SD, "vfat", MS_RELATIME,
             "fmask=0022,dmask=0022,codepage=437,iocharset=iso8859-1,shortname=mixed,errors=remount-ro");
}

/* FM always on during duty (user 11/09: Fault Management runs the whole duty and the PL is never switched off then):
   when the SD card carries the duty configuration (f-astra-nn/duty.conf: the flight bundle's PL image and its
   clocks), the FM duty monitor loads that PL once and stays until the poweroff; its own process group, not waited for */
#define DUTY_CONF SD "/f-astra-nn/duty.conf"
static void duty_start(void)
{
    if (access(DUTY_CONF, R_OK) || access("/usr/bin/f-astra-fm-duty", X_OK))
        return;
    mkdir("/run/f-astra-fm", 0755);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int out = open("/run/f-astra-fm/duty.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out >= 0) {
            dup2(out, 1);
            dup2(out, 2);
            close(out);
        }
        execl("/usr/bin/f-astra-fm-duty", "f-astra-fm-duty", DUTY_CONF, "--load", (char *)NULL);
        _exit(127);
    }
    printf("F_ASTRA_FM_DUTY_STARTED %d\n", (int)pid);
}

static int sysinit(const char *action)
{
    (void)action;
    mount_fs("proc", "/proc", "proc", 0, NULL);
    mount_fs("sysfs", "/sys", "sysfs", 0, NULL);
    mount_fs("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount_fs("devpts", "/dev/pts", "devpts", 0, "mode=0620,gid=5");
    mount_fs("tmpfs", "/run", "tmpfs", MS_NODEV | MS_NOSUID, "mode=0755");
    /* no tmpfs on the volatile directory: the initramfs root is already in RAM; the image carries its log and tmp */
    mkdir("/run/lock", 0755);
    mkdir("/run/sshd", 0755);
    mount_fs("none", "/sys/kernel/config", "configfs", 0, NULL);
    char host[64];
    if (first_line("/etc/hostname", host, sizeof host) && *host && sethostname(host, strlen(host)))
        fprintf(stderr, "%s: hostname: %s\n", prog, strerror(errno));
    loopback_up();
    mount_sd();
    /* the image r20 rc order: rcS S40 time keep, S41 boot watchdog, S42 power profile, S79 J1 CAN; rc5 S09 haveged,
     * S20 syslog, S21 CAN CMD, S25 J12 RNDIS, S26 J12 link-local, S97 sshd, S98 boot health */
    step(time_keep, "f-astra-time-keep", "start");
    step(boot_watchdog, "f-astra-boot-watchdog", "start");
    /* with the duty configuration the duty PL may already run on FCLK1 (BOOT.BIN's FSBL, the U-Boot early start, the
       device tree's fclk-enable keeping FCLK1 on): the profile leaves FCLK1 to the FM duty monitor */
    char *const profile[] = {"/usr/bin/f-astra-power-profile", "start",
                             access(DUTY_CONF, R_OK) ? NULL : "keep-fclk1", NULL};
    start_program(profile);
    duty_start();                     /* after the profile: it sets FCLK2..3 (and FCLK1 without duty) to their slowest */
    step(j1_can, "f-astra-j1-can", "start");
    char *const haveged[] = {"/usr/sbin/haveged", "-w", "1024", "-v", "0", NULL};
    start_program(haveged);
    char *const syslogd[] = {"/sbin/syslogd", NULL}, *const klogd[] = {"/sbin/klogd", NULL};
    start_program(syslogd);
    start_program(klogd);
    step(can_cmd, "f-astra-can-cmd", "start");
    step(j12_rndis, "f-astra-j12-rndis", "start");
    step(j12_linklocal, "f-astra-j12-linklocal", "start");
    char *const sshd[] = {"/usr/sbin/sshd", NULL};
    start_program(sshd);
    /* the console timeline's mark for the boot step that sshd's start takes (its parent exits once it listens) */
    printf("F_ASTRA_SSHD_STARTED\n");
    step(boot_health, "f-astra-boot-health", "start");
    return 0;
}

/* the image r20 K links' order: K05 time keep (saved), K10 CAN CMD, K25 J12 RNDIS */
static int shutdown_programs(const char *action)
{
    (void)action;
    step(time_keep, "f-astra-time-keep", "stop");
    step(can_cmd, "f-astra-can-cmd", "stop");
    step(j12_rndis, "f-astra-j12-rndis", "stop");
    sync();
    return 0;
}

/* ---- dispatch ---- */

static const struct {
    const char *name;
    int (*fn)(const char *);
} PROGRAMS[] = {
    {"f-astra-time-keep", time_keep},     {"f-astra-boot-watchdog", boot_watchdog}, {"f-astra-j1-can", j1_can},
    {"f-astra-can-cmd", can_cmd},         {"f-astra-boot-health", boot_health},     {"f-astra-j12-rndis", j12_rndis},
    {"f-astra-j12-linklocal", j12_linklocal}, {"f-astra-sysinit", sysinit},             {"f-astra-shutdown", shutdown_programs},
};

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    /* an rc link: S40f-astra-time-keep, K20f-astra-can-cmd */
    if ((name[0] == 'S' || name[0] == 'K') && isdigit((unsigned char)name[1]) && isdigit((unsigned char)name[2]))
        name += 3;
    int first = 1;
    if (!strcmp(name, "f-astra-boot")) {
        if (argc < 2)
            die(2, "usage: f-astra-boot NAME ACTION");
        name = argv[1];
        first = 2;
    }
    for (size_t i = 0; i < sizeof PROGRAMS / sizeof PROGRAMS[0]; i++)
        if (!strcmp(name, PROGRAMS[i].name)) {
            prog = PROGRAMS[i].name;
            return PROGRAMS[i].fn(argc > first ? argv[first] : "");
        }
    die(2, "unknown program: %s", name);
    return 2;
}
