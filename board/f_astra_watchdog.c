// F-ASTRA bounded child execution for long board runs (FINN loop models take seconds per frame). Same behaviour as the
// recovery set's watchdog (SIGTERM at the deadline, SIGKILL 2 s later, 124 when the deadline expired, the child's status
// otherwise, 128 + signal for a signalled child), with budgets up to MAX_SECONDS and a message for every refusal.
// No MMIO.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_SECONDS 86400L

static double monotonic_seconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        perror("clock_gettime");
        exit(125);
    }
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    char *end;
    long seconds;
    pid_t child;
    double start;
    int expired = 0, killed = 0, status = 0;

    if (argc < 3) {
        fprintf(stderr, "F_ASTRA_WATCHDOG_USAGE %s SECONDS COMMAND [ARG...]\n", argv[0]);
        return 125;
    }
    errno = 0;
    seconds = strtol(argv[1], &end, 10);
    if (errno || end == argv[1] || *end || seconds < 1 || seconds > MAX_SECONDS) {
        fprintf(stderr, "F_ASTRA_WATCHDOG_BAD_BUDGET %s (1..%ld s)\n", argv[1], MAX_SECONDS);
        return 125;
    }
    child = fork();
    if (child < 0) {
        perror("fork");
        return 125;
    }
    if (!child) {
        if (setpgid(0, 0)) _exit(125);
        execvp(argv[2], argv + 2);
        perror("execvp");
        _exit(127);
    }
    // the parent also sets the group, so a signal cannot reach the child before its own setpgid
    if (setpgid(child, child) && errno != EACCES && errno != ESRCH) return 125;
    start = monotonic_seconds();
    for (;;) {
        struct timespec pause = {0, 10000000};
        double elapsed;
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (expired) return 124;
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        if (result < 0 && errno != EINTR) {
            perror("waitpid");
            return 125;
        }
        elapsed = monotonic_seconds() - start;
        if (!expired && elapsed >= seconds) {
            fprintf(stderr, "F_ASTRA_WATCHDOG_DEADLINE_EXPIRED\n");
            kill(-child, SIGTERM);
            expired = 1;
        }
        if (!killed && elapsed >= seconds + 2) {
            kill(-child, SIGKILL);
            killed = 1;
        }
        // a stuck kernel request is not made safe by waiting longer
        if (elapsed >= seconds + 5) {
            fprintf(stderr, "F_ASTRA_WATCHDOG_CHILD_NOT_QUIESCENT\n");
            return 124;
        }
        nanosleep(&pause, NULL);
    }
}
