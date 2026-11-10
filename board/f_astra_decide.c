/*
 * f-astra-decide: the orbit's NN decisions for the OBC (F-ASTRA 6.214.0; user 11/06: the OBC gets the PD and BUS results
 * over CAN, data close to raw is left out). Only decisions leave the payload; scores and logits stay on board.
 *
 *   BUS: per window (output-bus.bin, int32 LE, the score's logit at 2^-BUS_QPOS) p = 1 / (1 + exp(-logit)) as float32,
 *        then the deployed student's alert policy (hk_anomaly_protocol.apply_temporal_postprocessor): the causal EWMA
 *        s = alpha p + (1 - alpha) s, started by the first window ever (STATE keeps s across orbits), anomaly when
 *        s >= BUS_THRESHOLD.
 *   PD:  per row of the frame (output-pd.bin, int16 LE, the risk head at 2^-PD_QPOS) alarm when code >= PD_CODE_MIN
 *        (the code of the validation threshold's logit, nn_decision.pd_code_min); the orbit's rows are the frame's last
 *        PD_NEW rows.
 *   Record (16 B, big-endian), appended to RESULTS (the CAN CMD node sends it as TM 0xC030):
 *        version 1 . summary (bit 0 a PD alarm, bit 1 a BUS anomaly) . orbit u16 . unix time u32 .
 *        PD rows u8 . PD bits u24 (bit i = the orbit's row i, oldest first) . BUS windows u8 . BUS bits u24.
 *
 * usage: f-astra-decide CONF BUS_OUT PD_OUT RESULTS STATE ORBIT
 *   CONF: key=value lines bus_qpos, bus_alpha, bus_threshold, pd_qpos (recorded only), pd_code_min, pd_new.
 * Exit 0 with the record appended; 2 usage or configuration; 3 an input missing or of a bad size; 4 an output failed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_WINDOWS 24
#define MAX_PD_NEW 24
#define MAX_PD_ROWS 4096

struct conf {
    int bus_qpos, pd_qpos, pd_new;
    long pd_code_min;
    double bus_alpha, bus_threshold;
};

static int conf_read(const char *path, struct conf *c)
{
    char line[256];
    int seen = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    memset(c, 0, sizeof(*c));
    while (fgets(line, sizeof line, f)) {
        char key[64];
        double value;
        if (line[0] == '#' || sscanf(line, " %63[a-z_] = %lf", key, &value) != 2)
            continue;
        if (!strcmp(key, "bus_qpos")) { c->bus_qpos = (int)value; seen |= 1; }
        else if (!strcmp(key, "bus_alpha")) { c->bus_alpha = value; seen |= 2; }
        else if (!strcmp(key, "bus_threshold")) { c->bus_threshold = value; seen |= 4; }
        else if (!strcmp(key, "pd_qpos")) { c->pd_qpos = (int)value; seen |= 8; }
        else if (!strcmp(key, "pd_code_min")) { c->pd_code_min = (long)value; seen |= 16; }
        else if (!strcmp(key, "pd_new")) { c->pd_new = (int)value; seen |= 32; }
    }
    fclose(f);
    if (seen != 63 || c->bus_qpos < 0 || c->bus_qpos > 30 || !(c->bus_alpha > 0.0 && c->bus_alpha <= 1.0)
        || c->pd_new < 1 || c->pd_new > MAX_PD_NEW || c->pd_code_min < -32768 || c->pd_code_min > 32768)
        return -1;
    return 0;
}

/* the whole file, of a size that is a multiple of unit and at most limit units */
static long load(const char *path, void *buf, size_t unit, size_t limit)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) || st.st_size <= 0 || (size_t)st.st_size % unit || (size_t)st.st_size / unit > limit) {
        close(fd);
        return -1;
    }
    size_t want = (size_t)st.st_size, got = 0;
    while (got < want) {
        ssize_t n = read(fd, (char *)buf + got, want - got);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);
    return (long)(want / unit);
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }

int main(int argc, char **argv)
{
    struct conf c;
    if (argc != 7 || conf_read(argv[1], &c)) {
        fprintf(stderr, "usage: f-astra-decide CONF BUS_OUT PD_OUT RESULTS STATE ORBIT (CONF: bus_qpos bus_alpha "
                        "bus_threshold pd_qpos pd_code_min pd_new)\n");
        return 2;
    }
    char *end;
    unsigned long orbit = strtoul(argv[6], &end, 10);
    if (*end || orbit > 0xFFFF)
        return 2;
    static int32_t bus[MAX_WINDOWS];
    static int16_t pd[MAX_PD_ROWS];
    long windows = load(argv[2], bus, 4, MAX_WINDOWS);
    long rows = load(argv[3], pd, 2, MAX_PD_ROWS);
    if (windows < 1 || rows < c.pd_new) {
        fprintf(stderr, "f-astra-decide: BUS or PD output missing or of a bad size\n");
        return 3;
    }
    /* BUS: the EWMA state of the previous orbits (absent: the first window starts it) */
    double s = 0.0;
    int started = 0;
    FILE *f = fopen(argv[5], "r");
    if (f) {
        started = fscanf(f, "%lf", &s) == 1 && isfinite(s);
        fclose(f);
    }
    uint32_t bus_bits = 0, pd_bits = 0;
    for (long i = 0; i < windows; i++) {
        float p = (float)(1.0 / (1.0 + exp(-ldexp((double)bus[i], -c.bus_qpos))));
        s = started ? c.bus_alpha * (double)p + (1.0 - c.bus_alpha) * s : (double)p;
        started = 1;
        if ((float)s >= (float)c.bus_threshold)
            bus_bits |= 1u << i;
    }
    for (int i = 0; i < c.pd_new; i++)
        if ((long)pd[rows - c.pd_new + i] >= c.pd_code_min)
            pd_bits |= 1u << i;
    uint8_t record[16];
    memset(record, 0, sizeof record);
    record[0] = 1;
    record[1] = (uint8_t)((pd_bits ? 1 : 0) | (bus_bits ? 2 : 0));
    put16(record + 2, (uint32_t)orbit);
    put32(record + 4, (uint32_t)time(NULL));
    record[8] = (uint8_t)c.pd_new;
    put24(record + 9, pd_bits);
    record[12] = (uint8_t)windows;
    put24(record + 13, bus_bits);
    /* the state first (an orbit's record never goes out with a state that misses its windows), written in place:
     * the SD card's FAT keeps no tmp-and-rename (a rename cut by a reset left two entries with one short name) */
    int fd = open(argv[5], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    char text[64];
    int len = snprintf(text, sizeof text, "%.17g\n", s);
    if (fd < 0 || write(fd, text, (size_t)len) != len || fsync(fd) || close(fd)) {
        fprintf(stderr, "f-astra-decide: state %s: %s\n", argv[5], strerror(errno));
        return 4;
    }
    fd = open(argv[4], O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0 || write(fd, record, sizeof record) != (ssize_t)sizeof record || close(fd)) {
        fprintf(stderr, "f-astra-decide: results %s: %s\n", argv[4], strerror(errno));
        return 4;
    }
    printf("{\"orbit\":%lu,\"pd_rows\":%d,\"pd_bits\":%u,\"bus_windows\":%ld,\"bus_bits\":%u,\"bus_ewma\":%.9g}\n", orbit,
           c.pd_new, (unsigned)pd_bits, windows, (unsigned)bus_bits, s);
    return 0;
}
