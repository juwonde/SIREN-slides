/* F-ASTRA daily window on the board PS in C (11/02: board code in C). C form of f_astra_daily_window.sh: the same
 * steps in the same order, the same files, timeline lines and output lines, the same exit status as its set -e; no
 * sh, cat or sha256sum processes: the day's programs are started directly (fork, exec), the products are hashed
 * in-process (SHA-256 of f_astra_ed25519.c). Every allocation is checked and freed before the program ends.
 *
 *   f-astra-daily-window FOLDER NAME...
 *
 * FOLDER must lie under /run/f-astra-daily-* (exit 64 otherwise). With daily-cpu.txt (a cpufreq governor) and a
 * writable cpu0 scaling_governor the PS runs at it and goes back to the governor before at every exit. Per NAME,
 * each stage between timeline marks (timeline.txt: board uptime and the mark):
 *   prep   ./f-astra-pd-zt pdzt-NAME.txt (when that file exists) or ./f-astra-weekly-prep daily-NAME.txt, stdout to
 *          prep-NAME.jsonl, stderr to prep-NAME.log
 *   pre    the encoder's pre stage: ./f-astra-gru-encode with the arguments of encode-pre-NAME.args (one argument per
 *          line, written by the packager; the sh form ran encode-pre-NAME.sh, a one-line exec of the same command),
 *          stdout to pre-NAME.json, stderr to pre-NAME.log
 *   check  every product the day wrote must equal the host's: products-NAME.sha256 checked as busybox sha256sum -c
 *          does (lines "FILE: OK" or "FILE: FAILED" into products-NAME.check; its messages on stderr)
 * then "DAILY_PASS NAME" on stdout. The first failing step ends the window with the status the sh form had: the
 * program's own exit code, 128 + N when signal N killed it, 127 not found, 126 not executable, 1 a failed check,
 * 2 a file the window could not create or read.
 * Build: cc -std=gnu99 -O2 -Wall -Wextra -Werror f_astra_daily_window.c f_astra_ed25519.c -o f-astra-daily-window
 * (host tests may move the folder prefix and the governor file: -DDAILY_ROOT='"..."' -DGOVERNOR='"..."')
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "f_astra_ed25519.h"

#ifndef DAILY_ROOT
#define DAILY_ROOT "/run/f-astra-daily-"
#endif
#ifndef GOVERNOR
#define GOVERNOR "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
#endif
#define CPU_FILE "daily-cpu.txt"
#define TIMELINE "timeline.txt"
#define ARGS_MAX (64 * 1024)           /* an encoder argument file is a few hundred bytes */
#define PATH_LEN 4096

static char pd_zt[] = "./f-astra-pd-zt";
static char weekly_prep[] = "./f-astra-weekly-prep";
static char encoder[] = "./f-astra-gru-encode";
static char governor_before[256];
static int governor_restore;
static unsigned char hash_buf[64 * 1024];

static void message(const char *what, const char *path, int e)
{
    fprintf(stderr, "f-astra-daily-window: %s %s: %s\n", what, path, strerror(e));
}

/* trap 'echo "$before" > "$governor"' EXIT */
static void restore_governor(void)
{
    if (!governor_restore)
        return;
    governor_restore = 0;
    char line[sizeof governor_before + 1];
    int n = snprintf(line, sizeof line, "%s\n", governor_before);
    int fd = open(GOVERNOR, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        message("can't create", GOVERNOR, errno);
        return;
    }
    ssize_t w = write(fd, line, (size_t)n);
    int e = errno;
    if (close(fd) || w != n)
        message("write error", GOVERNOR, w < 0 ? e : EIO);
}

static int is_regular(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* NAME into a FORMAT with one %s; 0 when it does not fit */
static int name_path(char *buf, const char *format, const char *name)
{
    int n = snprintf(buf, PATH_LEN, format, name);
    if (n < 0 || n >= PATH_LEN) {
        fprintf(stderr, "f-astra-daily-window: %s: name too long\n", name);
        return 0;
    }
    return 1;
}

/* the PS governor before (before=$(cat "$governor")), then daily-cpu.txt into it (cat daily-cpu.txt > "$governor") */
static int set_governor(void)
{
    int fd = open(GOVERNOR, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        message("can't open", GOVERNOR, errno);
        return 1;
    }
    ssize_t n = read(fd, governor_before, sizeof governor_before - 1);
    int e = errno;
    close(fd);
    if (n < 0) {
        message("can't read", GOVERNOR, e);
        return 1;
    }
    while (n > 0 && governor_before[n - 1] == '\n')
        n--;
    governor_before[n] = 0;
    governor_restore = 1;
    int out = open(GOVERNOR, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (out < 0) {
        message("can't create", GOVERNOR, errno);
        return 2;
    }
    int in = open(CPU_FILE, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        message("can't open", CPU_FILE, errno);
        close(out);
        return 1;
    }
    char buf[4096];
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) {
            message("write error", GOVERNOR, errno);
            rc = 1;
            break;
        }
    if (n < 0) {
        message("can't read", CPU_FILE, errno);
        rc = 1;
    }
    close(in);
    if (close(out) && !rc) {
        message("write error", GOVERNOR, errno);
        rc = 1;
    }
    return rc;
}

/* read -r up _ < /proc/uptime; echo "$up WHAT NAME" >> timeline.txt */
static int timeline(const char *what, const char *name)
{
    char up[64], line[PATH_LEN + 128];
    int fd = open("/proc/uptime", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 1;
    ssize_t n = read(fd, up, sizeof up - 1);
    close(fd);
    if (n <= 0)
        return 1;
    up[n] = 0;
    char *first = up + strspn(up, " \t\n");
    first[strcspn(first, " \t\n")] = 0;
    int len = snprintf(line, sizeof line, "%s %s %s\n", first, what, name);
    if (len < 0 || (size_t)len >= sizeof line)
        return 2;
    fd = open(TIMELINE, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd < 0) {
        message("can't create", TIMELINE, errno);
        return 2;
    }
    ssize_t w = write(fd, line, (size_t)len);
    int e = errno;
    if (close(fd) || w != len) {
        message("write error", TIMELINE, w < 0 ? e : EIO);
        return 2;
    }
    return 0;
}

/* > PATH (create, truncate); -1 and a message when it cannot be created */
static int create(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0)
        message("can't create", path, errno);
    return fd;
}

/* FD as the child's descriptor TARGET (dup2; the same descriptor only loses its close-on-exec flag) */
static int as_fd(int fd, int target)
{
    if (fd == target)
        return fcntl(fd, F_SETFD, 0);
    return dup2(fd, target) < 0 ? -1 : 0;
}

/* ARGV[0] ARGV... > OUT 2> ERR: the exit status as the shell reports it */
static int run(char *const argv[], int out, int err)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        message("can't fork", argv[0], errno);
        return 2;
    }
    if (pid == 0) {
        if (as_fd(out, 1) || as_fd(err, 2))
            _exit(126);
        execv(argv[0], argv);
        int e = errno;
        dprintf(2, "f-astra-daily-window: %s: %s\n", argv[0], strerror(e));
        _exit(e == ENOENT ? 127 : 126);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return 2;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 2;
}

/* stage prep: f-astra-pd-zt pdzt-NAME.txt or f-astra-weekly-prep daily-NAME.txt > prep-NAME.jsonl 2> prep-NAME.log */
static int prep(const char *name)
{
    char conf[PATH_LEN], out_path[PATH_LEN], err_path[PATH_LEN];
    if (!name_path(conf, "pdzt-%s.txt", name) || !name_path(out_path, "prep-%s.jsonl", name)
        || !name_path(err_path, "prep-%s.log", name))
        return 2;
    char *program = pd_zt;
    if (!is_regular(conf)) {
        program = weekly_prep;
        if (!name_path(conf, "daily-%s.txt", name))
            return 2;
    }
    char *argv[] = {program, conf, NULL};
    int out = create(out_path);
    if (out < 0)
        return 2;
    int err = create(err_path);
    if (err < 0) {
        close(out);
        return 2;
    }
    int rc = run(argv, out, err);
    close(out);
    close(err);
    return rc;
}

/* the encoder's arguments: TEXT holds the file, ARGV (NULL-terminated, ARGV[0] the encoder) points into it; the
 * caller frees both. 0, or 2 with the reason on ERR (the stage's log, as the sh form's error went there). */
static int load_args(const char *path, int err, char **text, char ***argv)
{
    *text = NULL;
    *argv = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        dprintf(err, "f-astra-daily-window: can't open %s: %s\n", path, strerror(errno));
        return 2;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > ARGS_MAX) {
        dprintf(err, "f-astra-daily-window: %s: not an argument file of at most %d bytes\n", path, ARGS_MAX);
        close(fd);
        return 2;
    }
    size_t size = (size_t)st.st_size, used = 0;
    char *buf = malloc(size + 1);
    if (!buf) {
        dprintf(err, "f-astra-daily-window: %s: out of memory\n", path);
        close(fd);
        return 2;
    }
    ssize_t n = 0;
    while (used < size && (n = read(fd, buf + used, size - used)) > 0)
        used += (size_t)n;
    close(fd);
    if (n < 0 || used != size || memchr(buf, 0, size)) {
        dprintf(err, "f-astra-daily-window: %s: unreadable or not text\n", path);
        free(buf);
        return 2;
    }
    buf[size] = 0;
    size_t count = 0;
    for (size_t i = 0; i < size; i++)
        count += buf[i] == '\n';
    if (size && buf[size - 1] != '\n')
        count++;                                     /* a last line without its newline */
    char **list = calloc(count + 2, sizeof *list);
    if (!list) {
        dprintf(err, "f-astra-daily-window: %s: out of memory\n", path);
        free(buf);
        return 2;
    }
    list[0] = encoder;
    char *line = buf;
    for (size_t k = 1; k <= count; k++) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        list[k] = line;
        line = end ? end + 1 : line + strlen(line);
    }
    *text = buf;
    *argv = list;
    return 0;
}

/* stage pre: the encoder with encode-pre-NAME.args > pre-NAME.json 2> pre-NAME.log */
static int pre(const char *name)
{
    char args_path[PATH_LEN], out_path[PATH_LEN], err_path[PATH_LEN];
    if (!name_path(args_path, "encode-pre-%s.args", name) || !name_path(out_path, "pre-%s.json", name)
        || !name_path(err_path, "pre-%s.log", name))
        return 2;
    int out = create(out_path);
    if (out < 0)
        return 2;
    int err = create(err_path);
    if (err < 0) {
        close(out);
        return 2;
    }
    char *text, **argv;
    int rc = load_args(args_path, err, &text, &argv);
    if (!rc)
        rc = run(argv, out, err);
    free(argv);
    free(text);
    close(out);
    close(err);
    return rc;
}

/* the file's SHA-256 as lowercase hex (busybox hash_file); 0 and a message when it cannot be opened or read */
static int hash_file(const char *path, char hex[65])
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "sha256sum: can't open '%s': %s\n", path, strerror(errno));
        return 0;
    }
    fa_sha256_ctx ctx;
    fa_sha256_init(&ctx);
    ssize_t n;
    while ((n = read(fd, hash_buf, sizeof hash_buf)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0)
            fa_sha256_update(&ctx, hash_buf, (size_t)n);
    int e = errno;
    close(fd);
    if (n < 0) {
        fprintf(stderr, "sha256sum: can't read '%s': %s\n", path, strerror(e));
        return 0;
    }
    uint8_t digest[32];
    fa_sha256_final(&ctx, digest);
    for (int i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return 1;
}

/* stage check: sha256sum -c products-NAME.sha256 > products-NAME.check (busybox: "HASH  FILE" or "HASH *FILE"
 * lines; an unparsable line counts as failed without a report line; 1 when any failed) */
static int check(const char *name)
{
    char sums[PATH_LEN], report_path[PATH_LEN];
    if (!name_path(sums, "products-%s.sha256", name) || !name_path(report_path, "products-%s.check", name))
        return 2;
    int out = create(report_path);
    if (out < 0)
        return 2;
    FILE *list = fopen(sums, "re");
    if (!list) {
        fprintf(stderr, "sha256sum: can't open '%s': %s\n", sums, strerror(errno));
        close(out);
        return 1;
    }
    FILE *report = fdopen(out, "w");
    if (!report) {
        message("can't write", report_path, errno);
        close(out);
        fclose(list);
        return 2;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int total = 0, failed = 0;
    while ((n = getline(&line, &cap, list)) >= 0) {
        if (n > 0 && line[n - 1] == '\n')
            line[n - 1] = 0;
        total++;
        char *sep = strstr(line, "  ");
        if (!sep)
            sep = strstr(line, " *");
        if (!sep) {
            failed++;
            continue;
        }
        *sep = 0;
        const char *file = sep + 2;
        char hex[65];
        int ok = hash_file(file, hex) && !strcmp(hex, line);
        fprintf(report, "%s: %s\n", file, ok ? "OK" : "FAILED");
        failed += !ok;
    }
    int read_error = ferror(list);
    free(line);
    fclose(list);
    int write_error = fclose(report) != 0;
    if (read_error)
        fprintf(stderr, "sha256sum: can't read '%s'\n", sums);
    if (write_error)
        message("write error", report_path, errno);
    if (failed)
        fprintf(stderr, "sha256sum: WARNING: %d of %d computed checksums did NOT match\n", failed, total);
    return failed || read_error || write_error ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fputs("usage: f-astra-daily-window FOLDER NAME...\n", stderr);
        return 2;
    }
    const char *root = argv[1];
    if (strncmp(root, DAILY_ROOT, strlen(DAILY_ROOT)))
        return 64;
    if (chdir(root)) {
        message("can't cd to", root, errno);
        return 2;
    }
    if (atexit(restore_governor))
        return 2;
    int rc;
    if (is_regular(CPU_FILE) && access(GOVERNOR, W_OK) == 0 && (rc = set_governor()))
        return rc;
    for (int k = 2; k < argc; k++) {
        const char *name = argv[k];
        if ((rc = timeline("begin prep", name)) || (rc = prep(name)) || (rc = timeline("end prep", name))
            || (rc = timeline("begin pre", name)) || (rc = pre(name)) || (rc = timeline("end pre", name))
            || (rc = check(name)))
            return rc;
        if (printf("DAILY_PASS %s\n", name) < 0 || fflush(stdout))
            return 1;
    }
    return 0;
}
