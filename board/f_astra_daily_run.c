/*
 * f-astra-daily: one daily power-on's work on the board (F-ASTRA 6.196.0; user 11/02: "C로 가는게 이득인 경우
 * (Redundancy or reliability or Speed or Power) 에 C 로 가자"). The C form of the board script that ran the measured
 * daily windows (the flight path of runtime daily-floor.sh, 10/31-11/02):
 *   1. CSP: the CAN CMD service stopped, can0 at 500 kbit/s, the CSP v1 node (node 12) receiving the day's object
 *   2. [idle], the floor profile entered (f-astra-floor enter KHZ), [idle]
 *   3. the download: until the node closes the received file (inotify IN_CLOSE_WRITE: the node fsyncs and closes it
 *      when the last byte arrived) or the timeout; the node stopped and the file moved to the object's name
 *   4. the floor left in the background (the SD card comes back while the next steps run)
 *   5. bzip2 objects unpacked with libbz2 under the performance governor
 *   6. the day's window (./f-astra-daily-window FOLDER NAME)
 *   7. the products (products-NAME.sha256) copied to the SD card and synced
 *   8. always, also after a failure: the floor left, the CSP node stopped, the CAN CMD service started again
 * Gains over the script: the CAN CMD service and the floor are restored on every exit path (the script exited with
 * CAN CMD stopped when the window failed: no CAN commands until a reboot); the download ends on the close event
 * instead of a one-second size poll and a one-second wait; no shell, sed, awk, cp or bunzip2 processes; a CSP node
 * that does not end on SIGTERM gets SIGKILL after 2 s (it never holds the run, and so the USB link, forever).
 * The timeline (FOLDER/timeline-lo.txt) has the script's lines: "UPTIME begin|end PHASE NAME"; with --console DEV
 * each line and each message also goes there as "F_ASTRA_DAILY ..." (the UART console: the run is seen while the
 * USB link is off).
 *
 * usage: f-astra-daily FOLDER FLOOR DL IDLE STORE NAME [KHZ] [COMP] [CSP] [--floor-tool PATH] [--node PATH]
 *                      [--can-cmd PATH] [--ip PATH] [--governor PATH] [--store-dir DIR] [--rx-dir DIR] [--console DEV]
 *   FLOOR 0 none | 3 the floor tool; DL the receive timeout in s; IDLE the idle phases in s (0: none); STORE 1 copy
 *   the products; KHZ the floor's CPU clock (83334); COMP 0 raw | 2 bzip2 (raw.bin.bz2); CSP 1 receive over CSP v1
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef F_ASTRA_HAVE_BZLIB
#include <bzlib.h>
#else
/* libbz2's reader (bzlib.h, stable since 1.0), declared here for a sysroot without the header (the Vitis one) */
typedef void BZFILE;
BZFILE *BZ2_bzReadOpen(int *bzerror, FILE *f, int verbosity, int small, void *unused, int nUnused);
int BZ2_bzRead(int *bzerror, BZFILE *b, void *buf, int len);
void BZ2_bzReadClose(int *bzerror, BZFILE *b);
#define BZ_OK 0
#define BZ_STREAM_END 4
#endif

#define PATH_LEN 512

static const char *folder, *name;
static char timeline[PATH_LEN];
static const char *floor_tool = "/usr/bin/f-astra-floor", *node_tool = "/usr/bin/f-astra-csp-v1-node",
                  *can_cmd = "/etc/init.d/f-astra-can-cmd", *ip_tool = "ip",
                  *governor = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                  *store_dir = "/media/sd-mmcblk0p1/f-astra-daily-test", *rx_dir = "/run", *console;
static pid_t node_pid, leave_pid;
static int can_stopped, floor_entered;

/* one line to the --console device; non-blocking, so a stalled console never holds the run */
static void to_console(const char *line)
{
    if (!console)
        return;
    int fd = open(console, O_WRONLY | O_NOCTTY | O_NONBLOCK | O_APPEND);
    if (fd < 0)
        return;
    ssize_t n = write(fd, line, strlen(line));      /* a lost console line is not an error of the run */
    (void)n;
    close(fd);
}

static void say(const char *fmt, ...)
{
    char text[256], line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    fprintf(stderr, "f-astra-daily: %s\n", text);
    snprintf(line, sizeof line, "F_ASTRA_DAILY %s\n", text);
    to_console(line);
}

static int path(char *out, const char *dir, const char *file)
{
    int n = snprintf(out, PATH_LEN, "%s/%s", dir, file);
    return n > 0 && n < PATH_LEN ? 0 : -1;
}

static void mark(const char *edge, const char *phase, const char *who)
{
    char up[64] = "0";
    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%63s", up) != 1)
            strcpy(up, "0");
        fclose(f);
    }
    char line[PATH_LEN];
    snprintf(line, sizeof line, "F_ASTRA_DAILY %s %s %s %s\n", edge, phase, who, up);
    to_console(line);
    f = fopen(timeline, "a");
    if (!f)
        return;
    fprintf(f, "%s %s %s %s\n", up, edge, phase, who);
    fclose(f);
}

static void pause_s(double s)
{
    struct timespec t = {(time_t)s, (long)((s - (double)(time_t)s) * 1e9)};
    while (nanosleep(&t, &t) && errno == EINTR)
        ;
}

/* a program with its output to FOLDER/OUT (NULL: inherited), without waiting; env: one NAME=VALUE or NULL */
static pid_t spawn(char *const argv[], const char *out, const char *env)
{
    char p[PATH_LEN];
    if (out && path(p, folder, out))
        return -1;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) {
        dup2(null, 0);
        close(null);
    }
    if (out) {
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            _exit(127);
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
    }
    if (env && putenv((char *)env))
        _exit(127);
    execvp(argv[0], argv);
    _exit(127);
}

static int finish(pid_t pid)
{
    int st;
    if (pid <= 0)
        return -1;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static int run(char *const argv[], const char *out)
{
    return finish(spawn(argv, out, NULL));
}

/* the day's raw records file (FOLDER/raw-*.bin: the daily folder has exactly one) */
static int raw_name(char *out)
{
    DIR *d = opendir(folder);
    if (!d)
        return -1;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 8 && !strncmp(e->d_name, "raw-", 4) && !strcmp(e->d_name + n - 4, ".bin")) {
            if (found++ || path(out, folder, e->d_name))
                found = 2;
        }
    }
    closedir(d);
    return found == 1 ? 0 : -1;
}

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) ? -1 : (long)st.st_size;
}

/* the watch on FILE's folder for its close after writing; set before the CSP node starts, so that a transfer that
 * ends at once is not missed (an inotify watch sees only later events) */
static int watch_closed(const char *file)
{
    char dir[PATH_LEN];
    const char *base = strrchr(file, '/');
    if (!base || (size_t)(base - file) >= sizeof dir)
        return -1;
    memcpy(dir, file, (size_t)(base - file));
    dir[base - file] = 0;
    int fd = inotify_init1(IN_CLOEXEC);
    if (fd >= 0 && inotify_add_watch(fd, dir, IN_CLOSE_WRITE) < 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

/* until FILE is closed after writing (watch FD of watch_closed, closed here), or TIMEOUT s; 0: closed, 1: timed
 * out, -1: error */
static int wait_closed(int fd, const char *file, double timeout)
{
    const char *base = strrchr(file, '/');
    if (fd < 0 || !base) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    base++;
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        double left = timeout - ((double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9);
        if (left <= 0) {
            close(fd);
            return 1;
        }
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval tv = {(time_t)left, (long)((left - (double)(time_t)left) * 1e6)};
        int r = select(fd + 1, &set, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            close(fd);
            return -1;
        }
        if (r == 0)
            continue;
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *e = (struct inotify_event *)p;
            if (e->len && !strcmp(e->name, base)) {
                close(fd);
                return 0;
            }
            p += sizeof *e + e->len;
        }
    }
}

/* the CSP node ended: SIGTERM, and SIGKILL when it is still there after 2 s */
static void node_stop(void)
{
    if (node_pid <= 0)
        return;
    kill(node_pid, SIGTERM);
    for (int i = 0; i < 40; i++) {
        pid_t r = waitpid(node_pid, NULL, WNOHANG);
        if (r == node_pid || (r < 0 && errno != EINTR)) {
            node_pid = 0;
            return;
        }
        pause_s(0.05);
    }
    say("the CSP node did not end on SIGTERM: SIGKILL");
    kill(node_pid, SIGKILL);
    finish(node_pid);
    node_pid = 0;
}

static int bunzip(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    int rc = -1, err = BZ_OK;
    BZFILE *b = out ? BZ2_bzReadOpen(&err, in, 0, 0, NULL, 0) : NULL;
    char *buf = b && err == BZ_OK ? malloc(1 << 16) : NULL;
    if (buf) {
        for (;;) {
            int n = BZ2_bzRead(&err, b, buf, 1 << 16);
            if ((err == BZ_OK || err == BZ_STREAM_END) && n > 0 && fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
                err = -100;
                break;
            }
            if (err != BZ_OK)
                break;
        }
        rc = err == BZ_STREAM_END ? 0 : -1;
    }
    free(buf);
    if (b) {
        int e2;
        BZ2_bzReadClose(&e2, b);
    }
    if (out && fclose(out))
        rc = -1;
    if (in)
        fclose(in);
    return rc;
}

static int write_text(const char *p, const char *text)
{
    int fd = open(p, O_WRONLY | O_TRUNC);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    return close(fd) || n != (ssize_t)strlen(text) ? -1 : 0;
}

static int copy_synced(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY), out = in >= 0 ? open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    int rc = in >= 0 && out >= 0 ? 0 : -1;
    char buf[1 << 15];
    for (ssize_t n; !rc && (n = read(in, buf, sizeof buf)) != 0;) {
        if (n < 0 || write(out, buf, (size_t)n) != n)
            rc = -1;
    }
    if (out >= 0 && (fsync(out) || close(out)))
        rc = -1;
    if (in >= 0)
        close(in);
    return rc;
}

/* the products of products-NAME.sha256 ("SHA256  FILE" lines) to the store directory */
static int store_products(void)
{
    char list[PATH_LEN], line[PATH_LEN], src[PATH_LEN], dst[PATH_LEN], file[PATH_LEN];
    char listname[PATH_LEN];
    if (snprintf(listname, sizeof listname, "products-%s.sha256", name) >= (int)sizeof listname || path(list, folder, listname))
        return -1;
    if (mkdir(store_dir, 0755) && errno != EEXIST)
        return -1;
    FILE *f = fopen(list, "r");
    if (!f)
        return -1;
    int rc = 0, count = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%*64s %511s", file) != 1 || strchr(file, '/'))
            continue;
        if (path(src, folder, file) || path(dst, store_dir, file) || copy_synced(src, dst))
            rc = -1;
        count++;
    }
    fclose(f);
    sync();
    return rc || !count ? -1 : 0;
}

/* everything the run changed, back: on every exit path */
static void restore(void)
{
    node_stop();
    if (floor_entered && !leave_pid) {
        char *const argv[] = {(char *)floor_tool, "leave", NULL};
        leave_pid = spawn(argv, "floor-leave.out", NULL);
    }
    if (leave_pid > 0) {
        finish(leave_pid);
        leave_pid = -1;
    }
    if (can_stopped) {
        char *const argv[] = {(char *)can_cmd, "start", NULL};
        if (run(argv, "csp-cmd-start.out"))
            say("CAN CMD service did not start again");
        can_stopped = 0;
    }
}

int main(int argc, char **argv)
{
    int pos = 0;
    const char *a[9] = {0};
    for (int i = 1; i < argc; i++) {
        const char **opt = !strcmp(argv[i], "--floor-tool") ? &floor_tool : !strcmp(argv[i], "--node") ? &node_tool
                         : !strcmp(argv[i], "--can-cmd") ? &can_cmd : !strcmp(argv[i], "--ip") ? &ip_tool
                         : !strcmp(argv[i], "--governor") ? &governor : !strcmp(argv[i], "--store-dir") ? &store_dir
                         : !strcmp(argv[i], "--rx-dir") ? &rx_dir : !strcmp(argv[i], "--console") ? &console : NULL;
        if (opt) {
            if (++i >= argc)
                return 2;
            *opt = argv[i];
        } else if (pos < 9) {
            a[pos++] = argv[i];
        } else {
            return 2;
        }
    }
    if (pos < 6) {
        fprintf(stderr, "usage: f-astra-daily FOLDER FLOOR DL IDLE STORE NAME [KHZ] [COMP] [CSP] [--floor-tool PATH] "
                        "[--node PATH] [--can-cmd PATH] [--ip PATH] [--governor PATH] [--store-dir DIR] [--rx-dir DIR] "
                        "[--console DEV]\n");
        return 2;
    }
    folder = a[0];
    name = a[5];
    int floor = atoi(a[1]), store = atoi(a[4]), comp = a[7] ? atoi(a[7]) : 0, csp = a[8] ? atoi(a[8]) : 0;
    double dl = atof(a[2]), idle = atof(a[3]);
    const char *khz = a[6] ? a[6] : "83334";
    if ((floor != 0 && floor != 3) || (comp != 0 && comp != 2) || csp != 1 || !*name || strchr(name, '/')) {
        fprintf(stderr, "f-astra-daily: FLOOR 0|3, COMP 0|2 and CSP 1 only (the script's other modes were tests)\n");
        return 2;
    }
    char raw[PATH_LEN], obj[PATH_LEN], rx[PATH_LEN];
    if (path(timeline, folder, "timeline-lo.txt") || raw_name(raw)) {
        say("FOLDER needs exactly one raw-*.bin");
        return 2;
    }
    if (comp == 2 ? path(obj, folder, "raw.bin.bz2") : (snprintf(obj, sizeof obj, "%s", raw) >= (int)sizeof obj))
        return 2;
    if (snprintf(rx, sizeof rx, "%s/f-astra-csp-daily-%s.bin", rx_dir, name) >= (int)sizeof rx)
        return 2;
    long expected = file_size(obj);         /* the test bed uploads the object too: its size is the expected size */
    if (comp)
        unlink(raw);
    unlink(obj);
    unlink(rx);
    int rc = 0;

    /* 1. CSP receive */
    char *const stop[] = {(char *)can_cmd, "stop", NULL};
    can_stopped = 1;
    run(stop, "csp-cmd-stop.out");
    char *const bitrate[] = {(char *)ip_tool, "link", "set", "can0", "type", "can", "bitrate", "500000", "sample-point",
                             "0.875", "loopback", "off", "berr-reporting", "on", NULL};
    char *const up[] = {(char *)ip_tool, "link", "set", "can0", "up", NULL};
    char *const node[] = {(char *)node_tool, "can0", "12", "--receive", rx, NULL};
    int watch = -1;
    if (run(bitrate, NULL) || run(up, NULL) || (watch = watch_closed(rx)) < 0 ||
        (node_pid = spawn(node, "csp-rx.out", "F_ASTRA_CSP_CAN_TX_GAP_US=500")) <= 0) {
        say("CAN, the receive watch or the CSP node failed");
        node_pid = 0;
        if (watch >= 0)
            close(watch);
        restore();
        return 3;
    }
    /* 2. idle, floor, idle */
    if (idle > 0) {
        mark("begin", "idle", name);
        pause_s(idle);
        mark("end", "idle", name);
    }
    if (floor == 3) {
        mark("begin", "floor", name);
        char *const enter[] = {(char *)floor_tool, "enter", (char *)khz, NULL};
        floor_entered = 1;
        if (run(enter, "floor-enter.out"))
            say("floor enter failed (the run goes on at the normal clock)");
        mark("end", "floor", name);
        if (idle > 0) {
            mark("begin", "floorid", name);
            pause_s(idle);
            mark("end", "floorid", name);
        }
    }
    /* 3. download */
    mark("begin", "download", name);
    int w = wait_closed(watch, rx, dl);
    mark("end", "download", name);
    node_stop();
    long got = file_size(rx);
    char note[96];
    snprintf(note, sizeof note, "received %ld of %ld\n", got, expected);
    char rxout[PATH_LEN];
    if (!path(rxout, folder, "csp-rx.out")) {
        FILE *f = fopen(rxout, "a");
        if (f) {
            fputs(note, f);
            fclose(f);
        }
    }
    if (w != 0 || got <= 0 || (expected >= 0 && got != expected)) {
        say("download incomplete (%s)", w == 1 ? "timeout" : w < 0 ? "watch error" : "size");
        restore();
        return 4;
    }
    /* the received file to the object's name (a copy when the folder is on another file system) */
    if (rename(rx, obj) && (errno != EXDEV || copy_synced(rx, obj) || unlink(rx))) {
        say("received file not moved to the folder");
        restore();
        return 4;
    }
    /* 4. the floor left while the next steps run */
    if (floor_entered) {
        char *const leave[] = {(char *)floor_tool, "leave", NULL};
        leave_pid = spawn(leave, "floor-leave.out", NULL);
        pause_s(0.2);
    }
    /* 5. unpack */
    if (comp == 2) {
        char g0[64] = "";
        FILE *g = fopen(governor, "r");
        if (g) {
            if (!fgets(g0, sizeof g0, g))
                g0[0] = 0;
            fclose(g);
        }
        write_text(governor, "performance");
        mark("begin", "decomp", name);
        int b = bunzip(obj, raw);
        mark("end", "decomp", name);
        if (*g0)
            write_text(governor, g0);
        if (b) {
            say("bzip2 object did not unpack");
            restore();
            return 5;
        }
    }
    /* 6. the day's window */
    char window[PATH_LEN], out[PATH_LEN];
    if (path(window, folder, "f-astra-daily-window") || snprintf(out, sizeof out, "window-%s.out", name) >= (int)sizeof out) {
        restore();
        return 2;
    }
    char *const win[] = {window, (char *)folder, (char *)name, NULL};
    rc = run(win, out);
    if (rc) {
        say("the day's window failed (%d)", rc);
        restore();
        return rc;
    }
    /* 7. products to the SD card (after the floor's leave: the card is back) */
    if (store) {
        mark("begin", "store", name);
        if (leave_pid > 0) {
            finish(leave_pid);
            leave_pid = -1;
        }
        int s = store_products();
        mark("end", "store", name);
        if (s) {
            say("storing the products failed");
            restore();
            return 6;
        }
    }
    /* 8. back to the normal state */
    restore();
    if (idle > 0) {
        mark("begin", "idle", "tail");
        pause_s(idle);
        mark("end", "idle", "tail");
    }
    return 0;
}
