/* F-ASTRA link-off wrapper of the TE0715 in C (11/02: board code in C). C form of f_astra_link_off.sh: the same steps
 * in the same order, the same registers, values, markers, done file and exit code; register access through /dev/mem
 * mappings of the SLCR and USB0 pages (uint32_t: PORTSC1 bit 23 and any bit 31 without the sh form's signed-arithmetic
 * workaround) instead of devmem and awk processes; no heap allocation.
 *
 *   f-astra-link-off SECONDS DONE_FILE CMD...
 *
 * Runs CMD with the board's USB link off (user 10/27: the USB gadget network only while data moves; the flight link
 * is CAN): 3 s after the start (the starting SSH command returns before the link drops) the gadget is unbound (the
 * RNDIS link drops), then, when the driver left the USB0 AMBA clock on, the ULPI PHY goes into low-power suspend
 * (PORTSC1 PHCD) and the USB0 clock off, as runtime power-stages flight.sh usb-off. After CMD (or after SECONDS at the
 * latest, a backstop process, so a stuck command never leaves the board unreachable) the clock, the PHY and the gadget
 * come back. "F_ASTRA_LINK off|on UPTIME" go to the UART console (/dev/ttyPS0; board_link.LINK_MARKER_ON). DONE_FILE
 * gets CMD's exit code once the link is back; the wrapper exits with it (127 CMD not found, 126 not executable,
 * 128 + N killed by signal N). A board without the gadget (no UDC bound) runs CMD with no register access.
 * Exit 2 on a usage error (SECONDS: a busybox sleep duration) or when /dev/mem cannot be mapped (nothing changed).
 * Build: cc -std=gnu99 -O2 -Wall -Wextra -Werror f_astra_link_off.c -o f-astra-link-off
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define UDC_FILE "/sys/kernel/config/usb_gadget/f_astra_te0715_2020/UDC"
#define CONSOLE "/dev/ttyPS0"
#define SLCR 0xF8000000u
#define APER_CLK 0x12Cu                /* APER_CLK_CTRL: USB0 AMBA clock bit 2 */
#define APER_USB0 0x4u
#define USB0 0xE0002000u
#define PORTSC1 0x184u                 /* PHCD bit 23: the ULPI PHY in low-power suspend */
#define PHCD 0x00800000u
#define START_S 3.0                    /* the starting SSH command returns before the link drops */

static volatile uint32_t *slcr, *usb;

static volatile uint32_t *map_page(int fd, uint32_t base)
{
    void *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
    if (p == MAP_FAILED) {
        fprintf(stderr, "f-astra-link-off: map 0x%08X: %s\n", (unsigned)base, strerror(errno));
        return NULL;
    }
    return p;
}

static int map_registers(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "f-astra-link-off: /dev/mem: %s\n", strerror(errno));
        return 0;
    }
    slcr = map_page(fd, SLCR);
    usb = slcr ? map_page(fd, USB0) : NULL;
    close(fd);
    if (slcr && !usb)
        munmap((void *)slcr, 4096);
    return usb != NULL;
}

static void unmap_registers(void)
{
    if (usb) {
        munmap((void *)usb, 4096);
        munmap((void *)slcr, 4096);
    }
}

/* setbits / reg_set / reg_clear of the sh form: one 32-bit read-modify-write */
static void set_bits(volatile uint32_t *page, uint32_t off, uint32_t mask, uint32_t bits)
{
    page[off / 4] = (page[off / 4] & ~mask) | bits;
}

/* a busybox sleep duration: digits with an optional fraction and an optional suffix s, m, h or d */
static int parse_seconds(const char *text, double *seconds)
{
    size_t n = strspn(text, "0123456789.");
    char number[64], *end;
    if (n == 0 || n >= sizeof number)
        return 0;
    memcpy(number, text, n);
    number[n] = 0;
    errno = 0;
    double v = strtod(number, &end);
    if (errno || *end || end == number)
        return 0;
    const char *suffix = text + n;
    double scale = 1;
    if (*suffix) {
        if (suffix[1])
            return 0;
        switch (*suffix) {
        case 's': scale = 1; break;
        case 'm': scale = 60; break;
        case 'h': scale = 3600; break;
        case 'd': scale = 86400; break;
        default: return 0;
        }
    }
    *seconds = v * scale;
    return 1;
}

static void sleep_seconds(double seconds)
{
    if (seconds > 2147483647.0)                      /* the board's time_t is 32 bits */
        seconds = 2147483647.0;
    struct timespec t = {(time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9)};
    while (nanosleep(&t, &t) && errno == EINTR)
        ;
}

/* $(cat FILE): the text without its trailing newlines; "" (and a message) when the file cannot be read */
static void read_text(const char *path, char *buf, size_t size)
{
    buf[0] = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "f-astra-link-off: %s: %s\n", path, strerror(errno));
        return;
    }
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        n = 0;
    while (n > 0 && buf[n - 1] == '\n')
        n--;
    buf[n] = 0;
}

/* echo TEXT > FILE */
static void write_line(const char *path, const char *text)
{
    char line[128];
    int n = snprintf(line, sizeof line, "%s\n", text);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0 || n < 0 || (size_t)n >= sizeof line) {
        fprintf(stderr, "f-astra-link-off: %s: %s\n", path, fd < 0 ? strerror(errno) : "line too long");
        if (fd >= 0)
            close(fd);
        return;
    }
    ssize_t w = write(fd, line, (size_t)n);
    int e = errno;
    if (close(fd) || w != n)
        fprintf(stderr, "f-astra-link-off: %s: %s\n", path, strerror(w < 0 ? e : errno));
}

/* "F_ASTRA_LINK WHAT UPTIME" on the UART console (cut -d' ' -f1 /proc/uptime); silent when the console is missing */
static void mark(const char *what)
{
    char up[64] = "", line[128];
    int fd = open("/proc/uptime", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, up, sizeof up - 1);
        close(fd);
        up[n > 0 ? n : 0] = 0;
        up[strcspn(up, " \n")] = 0;
    }
    int n = snprintf(line, sizeof line, "F_ASTRA_LINK %s %s\n", what, up);
    int console = open(CONSOLE, O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (console < 0)
        return;
    if (n > 0 && (size_t)n < sizeof line && write(console, line, (size_t)n) < 0)
        n = 0;                                       /* 2>/dev/null: a failed marker is not an error */
    close(console);
}

/* the clock, the PHY and the gadget back (nothing when there was no gadget or it is bound again) */
static void link_on(const char *udc)
{
    char now[64];
    if (!udc[0])
        return;
    read_text(UDC_FILE, now, sizeof now);
    if (now[0])
        return;
    set_bits(slcr, APER_CLK, APER_USB0, APER_USB0);
    set_bits(usb, PORTSC1, PHCD, 0);                 /* PORTSC1 PHCD clear: the PHY wakes */
    sleep_seconds(0.5);
    write_line(UDC_FILE, udc);
    mark("on");
}

static int wait_status(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return 2;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 2;
}

int main(int argc, char **argv)
{
    double seconds;
    if (argc < 4 || !parse_seconds(argv[1], &seconds)) {
        fprintf(stderr, "usage: f-astra-link-off SECONDS DONE_FILE CMD...\n");
        return 2;
    }
    const char *done_file = argv[2];
    char udc[64];
    read_text(UDC_FILE, udc, sizeof udc);
    if (udc[0] && !map_registers())
        return 2;
    if (unlink(done_file) && errno != ENOENT)
        fprintf(stderr, "f-astra-link-off: %s: %s\n", done_file, strerror(errno));
    sleep_seconds(START_S);
    mark("off");
    if (udc[0]) {
        write_line(UDC_FILE, "");
        sleep_seconds(1.0);
        /* the driver may already have gated the USB0 clock (then the port registers must not be touched) */
        if (slcr[APER_CLK / 4] & APER_USB0) {
            set_bits(usb, PORTSC1, PHCD, PHCD);      /* PORTSC1 PHCD: the ULPI PHY in low-power suspend */
            sleep_seconds(0.2);
            set_bits(slcr, APER_CLK, APER_USB0, 0);
        }
    }
    fflush(NULL);
    int rc;
    pid_t backstop = fork();
    if (backstop == 0) {
        /* the backstop: after SECONDS the link comes back; a kill after that waits until link_on is complete */
        sigset_t term;
        signal(SIGTERM, SIG_DFL);
        sleep_seconds(seconds);
        sigemptyset(&term);
        sigaddset(&term, SIGTERM);
        sigprocmask(SIG_BLOCK, &term, NULL);
        link_on(udc);
        _exit(0);
    }
    if (backstop < 0) {
        /* no backstop: the command does not run with the link off unguarded */
        fprintf(stderr, "f-astra-link-off: backstop: %s\n", strerror(errno));
        rc = 2;
    } else {
        pid_t command = fork();
        if (command == 0) {
            execvp(argv[3], argv + 3);
            int e = errno;
            fprintf(stderr, "f-astra-link-off: %s: %s\n", argv[3], strerror(e));
            _exit(e == ENOENT ? 127 : 126);
        }
        if (command < 0) {
            fprintf(stderr, "f-astra-link-off: %s: %s\n", argv[3], strerror(errno));
            rc = 2;
        } else {
            rc = wait_status(command);
        }
        kill(backstop, SIGTERM);
        wait_status(backstop);
    }
    link_on(udc);
    char code[16];
    snprintf(code, sizeof code, "%d", rc);
    write_line(done_file, code);
    unmap_registers();
    return rc;
}
