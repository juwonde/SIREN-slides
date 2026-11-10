/* F-ASTRA FM scheduler in C (11/02, user: the board's shell layer in C): the C form of f_astra_fm_schedule.sh, the
 * same options, turns, console and log lines, exit codes and PL rest; tests/test_fm_schedule_c.py runs both forms on the
 * same spools and compares them.
 *
 * FM always-on stage D (docs/f-astra-fault-management.md 8.2): the models of one bitstream run one at a time (the rule
 * of 10/15). Each model is an FM serve configuration (its spool, guard and windows) with serve_idle_exit_ms set; a model
 * with batches waiting in its spool gets a turn, in which its FM service serves the spool until it has stayed empty for
 * serve_idle_exit_ms. The models are looked at in the order given, round robin.
 *   track A (always on): runs until the stop file appears
 *   track B (one window): --until-empty ends once no spool holds a batch
 * A Safehold of any model (exit 3) ends the scheduler with 3. Another failure of a model's service ends it with 2, and
 * so does a turn that did not take the spool's first batch.
 * The spools are read with opendir (no process per look). With --pl-idle CMD the PL rests while no model has a batch:
 * once a pass found no batch, "CMD idle"; before the next turn "CMD active"; the PL is left active whenever it ends.
 * usage: f-astra-fm-schedule [--until-empty] [--poll-ms N] [--stop FILE] [--log FILE] [--pl-idle CMD] CONF...
 * FM_RUN (environment) is the service command, default "./f-astra-fm" (e.g. "./f-astra-watchdog 600 ./f-astra-fm").
 * One line a turn: "turn N CONF exit=CODE waiting=BATCHES seconds=S" (the log moves to LOG.1 past 1 MiB); the latest
 * turn's service summary goes to CONF-without-.conf-turn.json and its stderr to the same name with .log.
 * Build: cc -std=gnu99 -O2 -Wall -Wextra -Werror f_astra_fm_schedule.c -o f-astra-fm-schedule
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ARGS 32
#define MAX_CONFS 16
#define LOG_LIMIT 1048576L

static volatile sig_atomic_t signalled;     /* 143 (TERM) or 130 (INT): the exit code once the current step is done */
static char *pl_idle_argv[MAX_ARGS];
static int pl_idle_set, pl_idle_state;      /* 1 while the PL rests */
static char *pl_idle_copy;

static void on_signal(int sig) { signalled = sig == SIGTERM ? 143 : 130; }

/* split a command line on blanks (the sh's word splitting of $run and $pl_idle); argv points into text */
static int split(char *text, char **argv, int max)
{
    int n = 0;
    for (char *t = strtok(text, " \t\n"); t && n < max - 1; t = strtok(NULL, " \t\n"))
        argv[n++] = t;
    argv[n] = NULL;
    return n;
}

/* run argv with stdout and stderr to the given files (NULL: inherited); its exit status (sh's $?) */
static int run(char **argv, const char *out, const char *err)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0)
        return 127;
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (out) {
            int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0 || dup2(fd, 1) < 0)
                _exit(2);
            close(fd);
        }
        if (err) {
            int fd = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0 || dup2(fd, 2) < 0)
                _exit(2);
            close(fd);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0)        /* a signal waits for the child, as the sh's trap does */
        if (errno != EINTR)
            return 127;
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 127;
}

static int pl_idle(const char *state)
{
    char *argv[MAX_ARGS + 1];
    int n = 0;
    while (pl_idle_argv[n])
        argv[n] = pl_idle_argv[n], n++;
    argv[n++] = (char *)state;
    argv[n] = NULL;
    return run(argv, NULL, NULL);
}

/* every exit: the PL is left active (the sh's EXIT trap) */
static void finish(int code)
{
    if (pl_idle_set && pl_idle_state && pl_idle("active"))
        fprintf(stderr, "PL active failed at exit\n");
    free(pl_idle_copy);
    exit(code);
}

/* the configuration's last spool= value; 0 when the file lacks serve_idle_exit_ms=[1-9]... or a spool */
static int read_conf(const char *conf, char *spool, size_t size)
{
    FILE *f = fopen(conf, "r");
    if (!f) {
        fprintf(stderr, "%s: serve_idle_exit_ms is not set\n", conf);
        return 0;
    }
    char line[4096];
    int idle = 0, has_spool = 0;
    spool[0] = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "serve_idle_exit_ms=", 19) && line[19] >= '1' && line[19] <= '9')
            idle = 1;
        if (!strncmp(line, "spool=", 6)) {
            if (line[6])
                has_spool = 1;
            snprintf(spool, size, "%s", line + 6);
        }
    }
    fclose(f);
    if (!idle) {
        fprintf(stderr, "%s: serve_idle_exit_ms is not set\n", conf);
        return 0;
    }
    if (!has_spool) {
        fprintf(stderr, "%s: no spool\n", conf);
        return 0;
    }
    return 1;
}

/* a batch's sequence: [1-9][0-9]*.batch */
static int sequence(const char *name, char *digits, size_t size)
{
    size_t n = strspn(name, "0123456789");
    if (!n || name[0] == '0' || strcmp(name + n, ".batch") || n >= size)
        return 0;
    memcpy(digits, name, n);
    digits[n] = 0;
    return 1;
}

/* numeric order of two decimal strings without leading zeros */
static int less(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    return la != lb ? la < lb : strcmp(a, b) < 0;
}

/* the spool's batches and its oldest (lowest) sequence */
static int scan(const char *spool, char *oldest, size_t size)
{
    int count = 0;
    char digits[64];
    oldest[0] = 0;
    DIR *d = opendir(spool);
    if (!d)
        return 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!sequence(e->d_name, digits, sizeof digits))
            continue;
        count++;
        if (!oldest[0] || less(digits, oldest))
            snprintf(oldest, size, "%s", digits);
    }
    closedir(d);
    return count;
}

/* the uptime in centiseconds (/proc/uptime has two decimals) */
static long now_cs(void)
{
    char buf[64] = "";
    int fd = open("/proc/uptime", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof buf - 1);
        buf[n > 0 ? n : 0] = 0;
        close(fd);
    }
    char *dot = strchr(buf, '.');
    long s = strtol(buf, NULL, 10), frac = dot ? strtol(dot + 1, NULL, 10) : 0;
    return s * 100 + frac;
}

static void append(const char *path, const char *line)
{
    FILE *f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static int exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

int main(int argc, char **argv)
{
    int until_empty = 0, i = 1;
    long poll_ms = 200;
    const char *stop = "/run/f-astra-fm/schedule.stop", *log = "/dev/null";
    while (i < argc) {
        if (!strcmp(argv[i], "--until-empty"))
            until_empty = 1, i++;
        else if (!strcmp(argv[i], "--poll-ms") && i + 1 < argc)
            poll_ms = strtol(argv[i + 1], NULL, 10), i += 2;
        else if (!strcmp(argv[i], "--stop") && i + 1 < argc)
            stop = argv[i + 1], i += 2;
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            log = argv[i + 1], i += 2;
        else if (!strcmp(argv[i], "--pl-idle") && i + 1 < argc) {
            free(pl_idle_copy);
            pl_idle_copy = strdup(argv[i + 1]);
            if (!pl_idle_copy)
                return 2;
            pl_idle_set = split(pl_idle_copy, pl_idle_argv, MAX_ARGS) > 0;
            i += 2;
        } else
            break;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: %s [--until-empty] [--poll-ms N] [--stop FILE] [--log FILE] [--pl-idle CMD] CONF...\n", argv[0]);
        free(pl_idle_copy);
        return 64;
    }
    int nconf = argc - i;
    if (nconf > MAX_CONFS) {
        fprintf(stderr, "at most %d configurations\n", MAX_CONFS);
        free(pl_idle_copy);
        return 64;
    }
    char **confs = argv + i;
    static char spools[MAX_CONFS][4096];
    for (int k = 0; k < nconf; k++)
        if (!read_conf(confs[k], spools[k], sizeof spools[k])) {
            free(pl_idle_copy);
            return 64;
        }
    const char *fm = getenv("FM_RUN");
    char run_text[1024];
    snprintf(run_text, sizeof run_text, "%s", fm && *fm ? fm : "./f-astra-fm");
    char *run_argv[MAX_ARGS + 2];
    int nrun = split(run_text, run_argv, MAX_ARGS);
    struct timespec nap = {poll_ms / 1000, (poll_ms % 1000) * 1000000L};
    long logbytes = 0;
    struct stat st;
    if (strcmp(log, "/dev/null") && stat(log, &st) == 0 && S_ISREG(st.st_mode))
        logbytes = (long)st.st_size;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    int turn = 0;
    for (;;) {
        int ran = 0;
        for (int k = 0; k < nconf; k++) {
            if (signalled)
                finish(signalled);
            if (exists(stop))
                finish(0);
            char oldest[64];
            int count = scan(spools[k], oldest, sizeof oldest);
            if (count <= 0)
                continue;
            turn++;
            ran = 1;
            char base[4096], out[4200], err[4200];
            snprintf(base, sizeof base, "%s", confs[k]);
            size_t bl = strlen(base);
            if (bl >= 5 && !strcmp(base + bl - 5, ".conf"))
                base[bl - 5] = 0;
            snprintf(out, sizeof out, "%s-turn.json", base);
            snprintf(err, sizeof err, "%s-turn.log", base);
            long t0 = now_cs();
            if (pl_idle_set && pl_idle_state) {
                if (pl_idle("active")) {
                    fprintf(stderr, "PL active failed\n");
                    finish(2);
                }
                pl_idle_state = 0;
            }
            run_argv[nrun] = confs[k];
            run_argv[nrun + 1] = "serve";
            run_argv[nrun + 2] = NULL;
            int code = run(run_argv, out, err);
            long d = now_cs() - t0;
            char line[4600];
            snprintf(line, sizeof line, "turn %d %s exit=%d waiting=%d seconds=%ld.%02ld", turn, confs[k], code, count, d / 100,
                     d % 100);
            printf("%s\n", line);
            fflush(stdout);
            if (strcmp(log, "/dev/null")) {
                append(log, line);
                logbytes += (long)strlen(line) + 1;
            }
            if (signalled)
                finish(signalled);
            if (code == 3)
                finish(3);
            if (code)
                finish(2);
            char first[4200];
            /* the bounds as precisions (a spool path < 4096, a sequence < 64): with a variable k the ARM GCC 9 takes
             * spools[k] as up to the whole table (65,535 B) and fails -Werror=format-truncation (6.215.0) */
            snprintf(first, sizeof first, "%.4095s/%.63s.batch", spools[k], oldest);
            if (exists(first)) {
                snprintf(line, sizeof line, "turn %d %s took no batch (%s.batch still waits)", turn, confs[k], oldest);
                append(log, line);
                fprintf(stderr, "%s\n", line);
                finish(2);
            }
            if (logbytes > LOG_LIMIT) {
                char old[4200];
                snprintf(old, sizeof old, "%s.1", log);
                rename(log, old);
                logbytes = 0;
            }
        }
        if (!ran) {
            if (until_empty)
                finish(0);
            if (exists(stop))
                finish(0);
            if (pl_idle_set && !pl_idle_state) {
                if (pl_idle("idle")) {
                    fprintf(stderr, "PL idle failed\n");
                    finish(2);
                }
                pl_idle_state = 1;
            }
            nanosleep(&nap, NULL);
            if (signalled)
                finish(signalled);
        }
    }
}
