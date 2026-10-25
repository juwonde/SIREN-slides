// F-ASTRA mission preprocessing on the board PS: in each day's window (10 minutes with the download) the day's data
// become the Comp and PD input codes; in the weekly NN window the stored days become the PD windows and the week's BUS
// telemetry the BUS windows. One job per line of the configuration file:
//   comp NAME Q POS THRESHOLDS AUX_OUT XCODES_OUT ROWS CHANNELS LEVELS XH
//     SIREN Model 3 (Comp): Q holds the day's counts (int32 LE, ROWS x CHANNELS, the encoder reads the same file), POS
//     the positions (float32 LE, ROWS x 3: latitude, longitude, satellite direction). AUX_OUT gets the auxiliary
//     inputs (float32 LE, ROWS x 6) exactly as gru_evaluation.read_day forms them, XCODES_OUT the x codes of steps
//     0..ROWS-2 (int16 LE, (ROWS - 1) x (CHANNELS + 6)) as gru_predictor.Integer.x_codes: per value the number of
//     thresholds it reaches minus XH. THRESHOLDS: CHANNELS x LEVELS count thresholds (int64 LE), then 6 x LEVELS
//     auxiliary thresholds (float32 LE).
//   pdq NAME RAW OUT ROWS CHANNELS QPOS BITS
//     PD, daily: RAW is a day's features (float32 LE, ROWS x CHANNELS); OUT gets their int16 LE codes: round half to
//     even of x * 2^QPOS, saturated to BITS signed bits (pd_contract.quantize_inputs).
//   pdwin NAME OUT WINDOW STRIDE CHANNELS FIRST DAY_CODES...
//     PD, weekly: the days' codes (int16 LE, CHANNELS per row) in order form the stream (the last stored day of the
//     previous week first, as the context of the week's first points); OUT gets its windows starting at FIRST,
//     FIRST + STRIDE, ... (WINDOW rows each).
//   bus NAME RAW OUT QPOS BITS
//     BUS (EIRSAT-1 housekeeping), weekly (its grid points need the next telemetry row): RAW is the week's telemetry
//     ("ASTRAWK1", u32 version, channels, rows, grid samples, start OBT, period, window, stride, float32 mean and
//     scale per channel, then per row a u32 OBT and the channels as float32); OUT gets the windows of the
//     standardized OBT grid as BITS-bit codes in 16-bit LE words: floor(x * 2^QPOS + 0.5), saturated.
// Prints one JSON line per job (elapsed CLOCK_MONOTONIC_RAW ns) and exits 0; 2 on any failure.
//   f-astra-weekly-prep CONFIG
#define _GNU_SOURCE
#include <errno.h>
#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC     /* a host build for checks */
#endif

static void fail(const char *what)
{
    fprintf(stderr, "F_ASTRA_WEEKLY_PREP_FAILED %s errno=%d\n", what, errno);
    exit(2);
}

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void *load(const char *path, size_t want)
{
    FILE *f = fopen(path, "rb");
    struct stat st;
    void *data;
    if (!f || fstat(fileno(f), &st)) fail(path);
    if ((size_t)st.st_size != want) { errno = EINVAL; fail(path); }
    data = malloc(want ? want : 1);
    if (!data || fread(data, 1, want, f) != want) fail(path);
    fclose(f);
    return data;
}

static void store(const char *path, const void *data, size_t n)
{
    FILE *f;
    unlink(path);
    f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n || fclose(f)) fail(path);
}

// codes of x * 2^qpos: nearest, ties to even (the default rounding mode), saturated
static long code(float x, int qpos, long low, long high)
{
    double v = nearbyint((double)x * ldexp(1.0, qpos));
    return v < low ? low : v > high ? high : (long)v;
}

static void comp(char **a)
{
    const char *name = a[0];
    long rows = atol(a[6]), ch = atol(a[7]), levels = atol(a[8]), xh = atol(a[9]);
    if (rows < 2 || ch < 1 || ch > 64 || levels < 1 || levels > 4096) { errno = EINVAL; fail(name); }
    int32_t *q = load(a[1], (size_t)rows * ch * 4);
    float *pos = load(a[2], (size_t)rows * 3 * 4);
    uint8_t *thr = load(a[3], (size_t)ch * levels * 8 + (size_t)6 * levels * 4);
    const int64_t *cthr = (const int64_t *)thr;
    const float *athr = (const float *)(thr + (size_t)ch * levels * 8);
    float *aux = malloc((size_t)rows * 6 * 4);
    long nx = ch + 6;
    int16_t *xc = malloc((size_t)(rows - 1) * nx * 2);
    if (!aux || !xc) fail("memory");
    // the satellite direction as read, or (a constant column) from the sign of the latitude gradient
    int constant = 1;
    for (long t = 1; t < rows; t++)
        if (pos[t * 3 + 2] != pos[2]) { constant = 0; break; }
    for (long t = 0; t < rows; t++) {
        double lat = pos[t * 3], lon = pos[t * 3 + 1];
        float dir = pos[t * 3 + 2];
        if (constant) {
            // numpy.gradient: one-sided differences at the ends, central ones inside (the halving keeps the sign)
            double g = t == 0 ? (double)pos[3] - (double)pos[0]
                     : t == rows - 1 ? (double)pos[t * 3] - (double)pos[(t - 1) * 3]
                     : ((double)pos[(t + 1) * 3] - (double)pos[(t - 1) * 3]) / 2.0;
            dir = g < 0 ? 1.0f : 0.0f;
        }
        double ph = dir < 0.5f ? (lat + 90.0) / 360.0 : 0.5 + (90.0 - lat) / 360.0;
        ph = fmod(ph, 1.0);
        if (ph < 0) ph += 1.0;                 // numpy.mod: the result takes the divisor's sign
        double la = lat * (M_PI / 180.0), lo = lon * (M_PI / 180.0);
        float *o = aux + t * 6;
        o[0] = (float)sin(la); o[1] = (float)cos(la); o[2] = (float)sin(lo); o[3] = (float)cos(lo);
        o[4] = dir; o[5] = (float)ph;
    }
    for (long t = 0; t + 1 < rows; t++) {
        int16_t *o = xc + t * nx;
        for (long c = 0; c < ch; c++) {
            int64_t v = q[t * ch + c];
            const int64_t *th = cthr + c * levels;
            long lo = 0, hi = levels;          // thresholds ascend: count those <= v
            while (lo < hi) { long m = (lo + hi) / 2; if (th[m] <= v) lo = m + 1; else hi = m; }
            o[c] = (int16_t)(lo - xh);
        }
        for (long k = 0; k < 6; k++) {
            float v = aux[t * 6 + k];
            const float *th = athr + k * levels;
            long lo = 0, hi = levels;
            while (lo < hi) { long m = (lo + hi) / 2; if (th[m] <= v) lo = m + 1; else hi = m; }
            o[ch + k] = (int16_t)(lo - xh);
        }
    }
    store(a[4], aux, (size_t)rows * 6 * 4);
    store(a[5], xc, (size_t)(rows - 1) * nx * 2);
    free(q); free(pos); free(thr); free(aux); free(xc);
}

static void pdq(char **a)
{
    const char *name = a[0];
    long rows = atol(a[3]), ch = atol(a[4]);
    int qpos = atoi(a[5]), bits = atoi(a[6]);
    if (rows < 1 || ch < 1 || bits < 2 || bits > 16) { errno = EINVAL; fail(name); }
    float *raw = load(a[1], (size_t)rows * ch * 4);
    long low = -(1L << (bits - 1)), high = (1L << (bits - 1)) - 1;
    int16_t *codes = malloc((size_t)rows * ch * 2);
    if (!codes) fail("memory");
    for (long i = 0; i < rows * ch; i++) codes[i] = (int16_t)code(raw[i], qpos, low, high);
    store(a[2], codes, (size_t)rows * ch * 2);
    free(raw); free(codes);
}

static void pdwin(char **a, int files)
{
    const char *name = a[0];
    long window = atol(a[2]), stride = atol(a[3]), ch = atol(a[4]), first = atol(a[5]), rows = 0;
    int16_t *stream = NULL;
    if (window < 1 || stride < 1 || ch < 1 || first < 0 || files < 1) { errno = EINVAL; fail(name); }
    for (int i = 0; i < files; i++) {
        struct stat st;
        if (stat(a[6 + i], &st) || st.st_size % (ch * 2)) { errno = EINVAL; fail(a[6 + i]); }
        long r = (long)st.st_size / (ch * 2);
        int16_t *day = load(a[6 + i], (size_t)st.st_size);
        stream = realloc(stream, (size_t)(rows + r) * ch * 2);
        if (!stream) fail("memory");
        memcpy(stream + rows * ch, day, (size_t)st.st_size);
        rows += r;
        free(day);
    }
    if (first + window > rows) { errno = EINVAL; fail(name); }
    long frames = (rows - first - window) / stride + 1;
    size_t frame = (size_t)window * ch * 2;
    uint8_t *out = malloc(frames * frame);
    if (!out) fail("memory");
    for (long f = 0; f < frames; f++) memcpy(out + f * frame, stream + (first + f * stride) * ch, frame);
    store(a[1], out, frames * frame);
    free(stream); free(out);
}

static uint32_t u32_at(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void bus(char **a)
{
    const char *name = a[0];
    int qpos = atoi(a[3]), bits = atoi(a[4]);
    struct stat st;
    if (bits < 2 || bits > 16 || qpos < 0 || qpos > 16 || stat(a[1], &st) || st.st_size < 40) { errno = EINVAL; fail(name); }
    uint8_t *raw = load(a[1], (size_t)st.st_size);
    if (memcmp(raw, "ASTRAWK1", 8)) { errno = EINVAL; fail(a[1]); }
    uint32_t version = u32_at(raw + 8), ch = u32_at(raw + 12), rows = u32_at(raw + 16), grid = u32_at(raw + 20),
             start = u32_at(raw + 24), period = u32_at(raw + 28), window = u32_at(raw + 32), stride = u32_at(raw + 36);
    size_t head = 40 + (size_t)ch * 8, rec = 4 + (size_t)ch * 4;
    if (version != 1 || ch < 1 || ch > 64 || rows < 2 || window < 1 || window > grid || stride < 1
        || (size_t)st.st_size != head + rows * rec) { errno = EINVAL; fail(a[1]); }
    float mean[64], scale[64];
    memcpy(mean, raw + 40, ch * 4);
    memcpy(scale, raw + 40 + ch * 4, ch * 4);
    float *g = malloc((size_t)grid * ch * 4);
    if (!g) fail("memory");
    // float32 linear interpolation onto the OBT grid, then the standardizer (the week script's ARM float contract)
    uint32_t row = 0;
    for (uint32_t s = 0; s < grid; s++) {
        uint32_t target = start + s * period;
        while (row + 1 < rows && u32_at(raw + head + (row + 1) * rec) < target) row++;
        if (row + 1 >= rows) { errno = ERANGE; fail("telemetry does not bracket the grid"); }
        uint32_t t0 = u32_at(raw + head + row * rec), t1 = u32_at(raw + head + (row + 1) * rec);
        float v0[64], v1[64];
        memcpy(v0, raw + head + row * rec + 4, ch * 4);
        memcpy(v1, raw + head + (row + 1) * rec + 4, ch * 4);
        for (uint32_t c = 0; c < ch; c++) {
            float v;
            if (t0 == target) v = v0[c];
            else if (t1 == target) v = v1[c];
            else {
                if (!(t0 < target && target < t1)) { errno = EINVAL; fail("OBT rows are not sorted"); }
                float fraction = (float)(target - t0) / (float)(t1 - t0);
                float delta = v1[c] - v0[c];
                float step = fraction * delta;
                v = v0[c] + step;
            }
            g[s * ch + c] = (v - mean[c]) / scale[c];
        }
    }
    // windows; codes floor(x * 2^qpos + 0.5) saturated (nearest, ties up), as BITS-bit words
    long frames = (long)(grid - window) / stride + 1, low = -(1L << (bits - 1)), high = (1L << (bits - 1)) - 1;
    uint16_t mask = (uint16_t)((1u << bits) - 1);
    uint16_t *out = malloc((size_t)frames * window * ch * 2);
    if (!out) fail("memory");
    float unit = (float)(1u << qpos);
    for (long f = 0; f < frames; f++)
        for (uint32_t i = 0; i < window * ch; i++) {
            float x = g[(size_t)f * stride * ch + i] * unit + 0.5f;
            double v = floor((double)x);
            long k = v < low ? low : v > high ? high : (long)v;
            out[(size_t)f * window * ch + i] = (uint16_t)((uint16_t)k & mask);
        }
    store(a[2], out, (size_t)frames * window * ch * 2);
    free(raw); free(g); free(out);
}

int main(int argc, char **argv)
{
    char line[4096], *a[64];
    FILE *f;
    if (argc != 2 || !(f = fopen(argv[1], "r"))) { fprintf(stderr, "usage: f-astra-weekly-prep CONFIG\n"); return 2; }
    if (fesetround(FE_TONEAREST)) fail("rounding mode");
    while (fgets(line, sizeof line, f)) {
        int n = 0;
        char *kind = strtok(line, " \t\r\n");
        if (!kind) continue;
        while (n < 64 && (a[n] = strtok(NULL, " \t\r\n"))) n++;
        uint64_t t0 = now_ns();
        if (!strcmp(kind, "comp") && n == 10) comp(a);
        else if (!strcmp(kind, "pdq") && n == 7) pdq(a);
        else if (!strcmp(kind, "pdwin") && n >= 7) pdwin(a, n - 6);
        else if (!strcmp(kind, "bus") && n == 5) bus(a);
        else { errno = EINVAL; fail(kind); }
        printf("{\"job\":\"%s\",\"name\":\"%s\",\"ns\":%llu}\n", kind, a[0], (unsigned long long)(now_ns() - t0));
        fflush(stdout);
    }
    fclose(f);
    return 0;
}
