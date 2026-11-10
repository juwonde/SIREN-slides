// F-ASTRA PD input pipeline of the residual student on the board PS (directrtl_pd_zt): every day the PS turns the
// day's five-minute summary into the model's input codes and keeps the background of the onboard residual (zt), the
// daily median of log10(flux + eps) per 5-degree |magnetic latitude| bin and channel over the last days. One job per
// line of the configuration file:
//   sum NAME ROWS GEO SUN SUMMARY_OUT YYYYMMDD
//     the day's five-minute summary from its raw records. ROWS: POES l1a rows (281 bytes: time, frame and position
//     fields, 63 float32 with the 46 count rates, 7 int8 with mep_IFC_on; directrtl_orbit.orbit_object). GEO: the
//     ground-uploaded table (ASTRAGT1: per flux channel its count-rate index and count-rate-to-flux factor, -1 where
//     the board cannot make the channel; on a 1-degree geographic grid at the satellite's altitude |mlat|, min(L, 20)
//     and the cosine and sine of the magnetic longitude). SUN: the day's 1440 x 2 float32 cosine and sine of the sun's
//     magnetic longitude per UT minute. MLT = 12 h + (satellite - sun magnetic longitude) / 15 deg, so its sine and
//     cosine come from products of the two unit vectors (no trigonometric call). Per step: flux means of the valid
//     samples (finite, >= 0), the tel0 p6 mean over polar samples, sample count, IFC, polar and SAA shares, median
//     |mlat| and L, mean MLT sine and cosine; NaN where the board cannot make a column or a step has no sample.
//   sumc NAME ROWS GEO SUN SUMMARY_OUT YYYYMMDD
//     as sum, ROWS in the compact form: per row only the bytes sum reads, in row order (time, latitude and longitude,
//     the count rates of the indices GEO maps in ascending order, mep_IFC_on: 17 + 4 k bytes for k indices;
//     directrtl_pd_zt.compact_rows). Expanded to 281-byte rows with every other byte zero, the same summary.
//   sumt NAME ROWS GEO SUN SUMMARY_OUT YYYYMMDD
//     as sumc, the compact rows transposed: byte 0 of every row, then byte 1, ... (directrtl_pd_zt.transpose_rows;
//     like bytes side by side compress to about half).
//   day NAME SUMMARY STATE TABLES CODES_OUT YYYYMMDD
//     SUMMARY: the day's 288 five-minute steps, float32 LE, 36 columns (23 linear flux means, 5 polar-cap means, valid
//     samples, IFC share, polar share, median |mlat|, median L, MLT sin, MLT cos, SAA share). STATE: the background
//     state, read when present and written back with the day's medians. TABLES: eps, per-year scale tables, the
//     model's feature columns, normalization and input point. CODES_OUT: int16 LE, 288 x C codes
//     floor((h - mu) / sd * 2^QPOS + 0.5) saturated, h the feature as float16 (the dataset's storage).
//   win NAME OUT WINDOW STRIDE CHANNELS FIRST DAY_CODES...
//     the days' codes (int16 LE, CHANNELS per row) in order form one stream; OUT gets its windows starting at FIRST,
//     FIRST + STRIDE, ... (WINDOW rows each).
//   winpad NAME OUT WINDOW STRIDE CHANNELS FIRST FRAMES DAY_CODES...
//     FRAMES windows from FIRST at STRIDE, rows past the stream's end as code 0 (a window shorter than the stride
//     plus the receptive field covers the stream's end this way; the LayerNorm accelerator's frame is fixed).
//   mtcn NAME PARAMS OUT T Q AUX [T Q AUX ...]
//     Comp (SIREN Model 3) as the ModernTCN hybrid: each day's counts Q (int32 LE, T x C) and auxiliary inputs AUX
//     (float32 LE, T x 6, as the weekly prep forms them) become input frames (int16 LE, ROWS + RF rows x INPUTS): frame
//     f holds the day's rows f ROWS - RF .. (f + 1) ROWS - 1 (rows before the day's start or past its end as code 0);
//     a row's codes are floor(x 2^QPOS + 0.5) saturated to 16 bits of x = (ln((q + SCALE) / SCALE) - mean) / sd per
//     channel, then the auxiliary inputs, then zeros. PARAMS: ASTRACM1 (below).
//   mtcnpred NAME PARAMS OUTPUTS FIRST T PRED [FIRST T PRED ...]
//     each day's next-step predictions (int32 LE, (T - 1) x C) from the hybrid's output frames (int16 LE, ROWS + RF rows
//     x C, the day's frames from frame FIRST): day row t (frame t / ROWS, row RF + t % ROWS) predicts row t + 1 as
//     rint(max(e^y - 1, 0) SCALE) saturated to int32, y = code 2^-OQPOS sd + mean.
//   gxc NAME THRESHOLDS OUT T Q AUX
//     Comp (SIREN Model 3) as the GRU predictor on the PL (directrtl_weekly.comp_batch_days): the day's x codes (int16
//     LE, (T - 1) x (C + 6), the steps 0..T - 2) from its counts Q (int32 LE, T x C) and auxiliary inputs AUX (float32
//     LE, T x 6): per channel the number of its count thresholds at or below the count, per auxiliary input the number
//     of its thresholds at or below it (float32 comparison), each minus XH. THRESHOLDS: ASTRAGX1 (below).
// The host reference is directrtl_pd_zt.day_summary, day_codes and windows, directrtl_comp_mtcn.frame_codes and
// predictions and directrtl_comp_ps.x_codes (byte for byte). Prints one JSON line
// per job (elapsed CLOCK_MONOTONIC ns) and exits 0; 2 on any failure.
//   f-astra-pd-zt CONFIG
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STEPS 288
#define COLUMNS 36
#define NFLUX 23
#define NBINS 18
#define FEATURES 79
#define ZT 56            // first zt column of the feature vector (raw 0-22, geo 23-30, orb 31-53, qual 54-55)
#define C_VALID 28
#define C_IFC 29
#define C_POLAR 30
#define C_MLAT 31
#define C_L 32
#define C_SIN 33
#define C_COS 34
#define C_SAA 35
#define MAX_HISTORY 64
#define MAX_YEARS 16
#define MAX_COLS FEATURES
#define EMPTY_DAY INT32_MIN
#define PAD_COL 0xFFFF   // a padding input channel of the contract (zero weights): code 0

static void fail(const char *what)
{
    fprintf(stderr, "f-astra-pd-zt: %s\n", what);
    exit(2);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) fail(path);
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET)) fail(path);
    void *data = calloc(size ? (size_t)size : 1, 1);      // zeroed: -fanalyzer cannot see fread fill it
    if (!data) fail("memory");
    if (size && fread(data, 1, (size_t)size, f) != (size_t)size) fail(path);
    fclose(f);
    *n = (size_t)size;
    return data;
}

static void store(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, n, f) != n || fclose(f)) fail(path);
}

// float64 -> binary16, round to nearest even (numpy's double to half; float32 values convert exactly to double first)
static uint16_t to_half(double d)
{
    uint64_t b;
    memcpy(&b, &d, 8);
    uint16_t sign = (uint16_t)((b >> 48) & 0x8000);
    uint64_t a = b & 0x7FFFFFFFFFFFFFFFull;
    if (a >= 0x7FF0000000000000ull) return sign | 0x7C00 | (a > 0x7FF0000000000000ull ? 0x200 : 0);
    int e = (int)(a >> 52) - 1023;
    uint64_t m = (a & 0xFFFFFFFFFFFFFull) | (1ull << 52);
    if (e > 15) return sign | 0x7C00;
    if (e < -25) return sign;
    int shift = e >= -14 ? 42 : 28 - e;       // to 11 significant bits (normal) or to units of 2^-24 (subnormal)
    uint64_t q = m >> shift, r = m & ((1ull << shift) - 1), half = 1ull << (shift - 1);
    if (r > half || (r == half && (q & 1))) q++;
    if (e < -14) return sign | (uint16_t)q;     // a carry into 0x400 is the smallest normal, encoded alike
    int biased = e + 15;
    if (q >> 11) { q >>= 1; biased++; }
    if (biased >= 31) return sign | 0x7C00;
    return sign | (uint16_t)(biased << 10) | (uint16_t)(q & 0x3FF);
}

// log10 from IEEE +, -, *, / only (the host reference does the same operations in numpy, so the two agree bit for bit;
// libm log10 differs in the last bit between libraries): v = m * 2^e with m in [sqrt(1/2), sqrt(2)),
// ln m = 2 s (1 + s^2/3 + ... + s^22/23), s = (m - 1) / (m + 1); det_log10 is det_ln times log10(e)
static double det_ln(double v)
{
    if (v != v || v < 0) return NAN;
    if (v == 0) return -INFINITY;
    if (isinf(v)) return INFINITY;
    int e;
    double m = frexp(v, &e);
    if (m < 0.7071067811865476) { m *= 2.0; e -= 1; }
    double s = (m - 1.0) / (m + 1.0), s2 = s * s, p = 1.0 / 23.0;
    static const double inv[11] = {1.0 / 21.0, 1.0 / 19.0, 1.0 / 17.0, 1.0 / 15.0, 1.0 / 13.0, 1.0 / 11.0, 1.0 / 9.0,
                                    1.0 / 7.0, 1.0 / 5.0, 1.0 / 3.0, 1.0};
    for (int k = 0; k < 11; k++) p = p * s2 + inv[k];
    double ln_m = 2.0 * s * p;
    return (double)e * 0.6931471805599453 + ln_m;
}

static double det_log10(double v)
{
    return det_ln(v) * 0.4342944819032518;
}

// e^y - 1 from IEEE +, -, *, / and exact scaling by 2^k (the host reference, directrtl_comp_mtcn.det_expm1, does the
// same operations): y = k ln 2 + r, k = floor(y / ln 2 + 0.5), r = (y - k LN2_HI) - k LN2_LO (Cody-Waite, |r| < 0.35),
// e^r from its Taylor series to r^13 / 13! (Horner); no subnormal result in the range kept
static double det_expm1(double y)
{
    if (y != y) return NAN;
    if (y > 709.0) return INFINITY;
    if (y < -708.0) return -1.0;
    static const double fact[14] = {1.0, 1.0, 2.0, 6.0, 24.0, 120.0, 720.0, 5040.0, 40320.0, 362880.0, 3628800.0,
                                    39916800.0, 479001600.0, 6227020800.0};
    double k = floor(y * 1.4426950408889634 + 0.5);
    double r = (y - k * 6.93147180369123816490e-01) - k * 1.90821492927058770002e-10;
    double s = 1.0 / fact[13];
    for (int n = 12; n >= 1; n--) s = s * r + 1.0 / fact[n];
    s = s * r + 1.0;
    return ldexp(s, (int)k) - 1.0;
}

static double from_half(uint16_t h)
{
    int e = (h >> 10) & 0x1F, m = h & 0x3FF;
    double v = e == 0 ? ldexp(m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexp(m | 0x400, e - 25);
    return h & 0x8000 ? -v : v;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// median of n sorted-or-not finite values (n > 0): the middle one, or the mean of the two middle ones
static double median(double *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_double);
    return n & 1 ? v[n / 2] : (v[n / 2] + v[n / 2 - 1]) / 2.0;
}

// days since 1970-01-01 of a proleptic Gregorian date
static int32_t civil_days(int y, int mo, int d)
{
    y -= mo <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

typedef struct {
    int history, min_periods, qpos, bits, ncols, nyears;
    double eps[NFLUX];
    uint16_t cols[MAX_COLS];
    float mu[MAX_COLS], sd[MAX_COLS];
    int32_t years[MAX_YEARS];
    double scale[MAX_YEARS][NBINS * NFLUX];
} Tables;

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void read_tables(const char *path, Tables *t)
{
    size_t n;
    uint8_t *b = load(path, &n);
    if (!b || n < 48 || memcmp(b, "ASTRAZT1", 8) || u32(b + 8) != 1) fail("tables: ASTRAZT1 version 1 required");
    if (u32(b + 12) != NFLUX || u32(b + 16) != NBINS) fail("tables: 23 channels and 18 bins");
    t->history = (int)u32(b + 20); t->min_periods = (int)u32(b + 24); t->qpos = (int32_t)u32(b + 28);
    t->bits = (int)u32(b + 32); t->ncols = (int)u32(b + 36); t->nyears = (int)u32(b + 40);
    if (t->history < 1 || t->history > MAX_HISTORY || t->min_periods < 1 || t->min_periods > t->history) fail("tables: history");
    if (t->bits < 2 || t->bits > 16 || t->ncols < 1 || t->ncols > MAX_COLS || t->nyears < 1 || t->nyears > MAX_YEARS)
        fail("tables: sizes");
    size_t need = 48 + 8 * NFLUX + 2 * (size_t)t->ncols + 8 * (size_t)t->ncols
                  + (size_t)t->nyears * (4 + 8 * NBINS * NFLUX);
    if (n != need) fail("tables: size");
    const uint8_t *p = b + 48;
    memcpy(t->eps, p, 8 * NFLUX); p += 8 * NFLUX;
    memcpy(t->cols, p, 2 * (size_t)t->ncols); p += 2 * (size_t)t->ncols;
    memcpy(t->mu, p, 4 * (size_t)t->ncols); p += 4 * (size_t)t->ncols;
    memcpy(t->sd, p, 4 * (size_t)t->ncols); p += 4 * (size_t)t->ncols;
    for (int k = 0; k < t->ncols; k++)
        if (t->cols[k] != PAD_COL && (t->cols[k] >= FEATURES || (t->cols[k] >= 31 && t->cols[k] < 54)))
            fail("tables: a column outside raw, geo, qual and zt");
    for (int y = 0; y < t->nyears; y++) {
        memcpy(&t->years[y], p, 4); p += 4;
        memcpy(t->scale[y], p, 8 * NBINS * NFLUX); p += 8 * NBINS * NFLUX;
    }
    free(b);
}

typedef struct {
    int history;
    int32_t day[MAX_HISTORY];
    float med[MAX_HISTORY][NBINS * NFLUX];
} State;

static void read_state(const char *path, State *s, int history)
{
    size_t n;
    uint8_t *b = load(path, &n);
    s->history = history;
    for (int k = 0; k < history; k++) {
        s->day[k] = EMPTY_DAY;
        for (int j = 0; j < NBINS * NFLUX; j++) s->med[k][j] = NAN;
    }
    if (!b) return;                             // the first day: no background yet
    size_t entry = 4 + 4 * NBINS * NFLUX;
    if (n != 24 + (size_t)history * entry || memcmp(b, "ASTRAZS1", 8) || u32(b + 8) != 1 || (int)u32(b + 12) != history
        || u32(b + 16) != NBINS || u32(b + 20) != NFLUX)
        fail("state: ASTRAZS1 of the tables' history required");
    for (int k = 0; k < history; k++) {
        memcpy(&s->day[k], b + 24 + k * entry, 4);
        memcpy(s->med[k], b + 24 + k * entry + 4, 4 * NBINS * NFLUX);
    }
    free(b);
}

static void write_state(const char *path, const State *s)
{
    size_t entry = 4 + 4 * NBINS * NFLUX, n = 24 + (size_t)s->history * entry;
    uint8_t *b = malloc(n);
    if (!b) fail("memory");
    uint32_t head[4] = {1, (uint32_t)s->history, NBINS, NFLUX};
    memcpy(b, "ASTRAZS1", 8);
    memcpy(b + 8, head, 16);
    for (int k = 0; k < s->history; k++) {
        memcpy(b + 24 + k * entry, &s->day[k], 4);
        memcpy(b + 24 + k * entry + 4, s->med[k], 4 * NBINS * NFLUX);
    }
    store(path, b, n);
    free(b);
}

// the 5-degree |mlat| bin (numpy digitize of nan_to_num(|mlat|) on 0, 5, ..., 90, less one, clipped to 0..17)
static int bin_of(float am)
{
    double a = am != am ? 0.0 : (double)am;
    int i = 0;
    while (i < 19 && 5.0 * i <= a) i++;
    i -= 1;
    return i < 0 ? 0 : i > NBINS - 1 ? NBINS - 1 : i;
}

#define ROW 281
#define GLAT 181
#define GLON 361

typedef struct {
    int32_t src[NFLUX];
    double factor[NFLUX];
    float *grid[4];             // |mlat|, min(L, 20), cos and sin of the magnetic longitude
} Geo;

static void read_geo(const char *path, Geo *g)
{
    size_t n, cells = (size_t)GLAT * GLON;
    uint8_t *b = load(path, &n);
    if (!b || n != 32 + 4 * NFLUX + 8 * NFLUX + 16 * cells || memcmp(b, "ASTRAGT1", 8) || u32(b + 8) != 1
        || u32(b + 12) != GLAT || u32(b + 16) != GLON || u32(b + 20) != NFLUX)
        fail("geo: ASTRAGT1 version 1, 181 x 361 grid, 23 channels required");
    memcpy(g->src, b + 32, 4 * NFLUX);
    memcpy(g->factor, b + 32 + 4 * NFLUX, 8 * NFLUX);
    for (int k = 0; k < 4; k++) {
        g->grid[k] = malloc(4 * cells);
        if (!g->grid[k]) fail("memory");
        memcpy(g->grid[k], b + 32 + 12 * NFLUX + 4 * cells * k, 4 * cells);
    }
    for (int c = 0; c < NFLUX; c++)
        if (g->src[c] < -1 || g->src[c] >= 46) fail("geo: count-rate index");
    free(b);
}

// bilinear interpolation on the 1-degree grid (rows: latitude -90..90, columns: longitude 0..360)
static double grid_at(const float *q, double lat, double lon)
{
    if (!isfinite(lat) || !isfinite(lon)) return NAN;
    double y = lat + 90.0, x = lon - 360.0 * floor(lon / 360.0);
    long i0 = (long)floor(y), j0 = (long)floor(x);
    i0 = i0 < 0 ? 0 : i0 > GLAT - 2 ? GLAT - 2 : i0;
    j0 = j0 < 0 ? 0 : j0 > GLON - 2 ? GLON - 2 : j0;
    double fy = y - (double)i0, fx = x - (double)j0;
    const float *r0 = q + i0 * GLON, *r1 = r0 + GLON;
    return (double)r0[j0] * (1.0 - fy) * (1.0 - fx) + (double)r1[j0] * fy * (1.0 - fx) + (double)r0[j0 + 1] * (1.0 - fy) * fx
           + (double)r1[j0 + 1] * fy * fx;
}

static double median_or_nan(double *v, int n)
{
    return n ? median(v, n) : NAN;
}

// the count-rate indices GEO maps (used[46]) and the compact row's width, 17 + 4 k bytes
static size_t compact_width(const Geo *g, int *used)
{
    int k = 0;
    for (int s = 0; s < 46; s++) used[s] = 0;
    for (int ch = 0; ch < NFLUX; ch++)
        if (g->src[ch] >= 0) used[g->src[ch]] = 1;
    for (int s = 0; s < 46; s++) k += used[s];
    return 17 + 4 * (size_t)k;
}

// transposed compact rows (sumt: byte 0 of every row, then byte 1, ...) back to compact rows; frees d
static uint8_t *untranspose(uint8_t *d, size_t n, const Geo *g)
{
    int used[46];
    size_t w = compact_width(g, used);
    if (!d || n % w) fail("rows: whole compact records required");
    size_t nrow = n / w;
    uint8_t *c = malloc(n ? n : 1);
    if (!c) fail("memory");
    for (size_t i0 = 0; i0 < nrow; i0 += 256) {          // 256 rows (13.6 KB of output) at a time: they stay in L1
        size_t i1 = i0 + 256 < nrow ? i0 + 256 : nrow;
        for (size_t j = 0; j < w; j++)
            for (size_t i = i0; i < i1; i++) c[i * w + j] = d[j * nrow + i];
    }
    free(d);
    return c;
}

// compact rows (sumc) to 281-byte rows, every byte sum does not read zero; frees c
static uint8_t *expand_compact(uint8_t *c, size_t *n, const Geo *g)
{
    int used[46];
    size_t w = compact_width(g, used);
    if (!c || *n % w) fail("rows: whole compact records required");
    size_t nrow = *n / w;
    uint8_t *r = calloc(nrow ? nrow : 1, ROW);
    if (!r) fail("memory");
    for (size_t i = 0; i < nrow; i++) {
        const uint8_t *p = c + i * w;
        uint8_t *q = r + i * ROW;
        memcpy(q, p, 8);
        memcpy(q + 22 + 4, p + 8, 8);
        size_t o = 16;
        for (int s = 0; s < 46; s++)
            if (used[s]) { memcpy(q + 22 + 4 * (3 + s), p + o, 4); o += 4; }
        q[22 + 63 * 4 + 4] = p[o];
    }
    free(c);
    *n = nrow * ROW;
    return r;
}

static void sum_job(char **a, int form)
{
    // a: NAME ROWS GEO SUN SUMMARY_OUT YYYYMMDD; form 0 sum (281-byte rows), 1 sumc (compact), 2 sumt (transposed compact)
    static const char *names[] = {"sum", "sumc", "sumt"};
    double t0 = now_s();
    static Geo g;
    static int geo_loaded = 0;
    if (!geo_loaded) { read_geo(a[2], &g); geo_loaded = 1; }
    size_t n, ns;
    uint8_t *rows = load(a[1], &n);
    if (form == 2) rows = untranspose(rows, n, &g);
    if (form) rows = expand_compact(rows, &n, &g);
    float *sun = load(a[3], &ns);
    if (!rows || n % ROW) fail("rows: whole 281-byte records required");
    if (!sun || ns != 1440 * 2 * 4) fail("sun: 1440 x 2 float32 required");
    int ymd = atoi(a[5]);
    int64_t day0 = (int64_t)civil_days(ymd / 10000, ymd / 100 % 100, ymd % 100) * 86400000LL;
    long nrow = (long)(n / ROW);
    static double flux_sum[STEPS][NFLUX], pc_sum[STEPS], s_sin[STEPS], s_cos[STEPS];
    static long flux_n[STEPS][NFLUX], pc_n[STEPS], n_trig[STEPS], cnt[STEPS], ifc[STEPS], polar[STEPS], saa[STEPS];
    memset(flux_sum, 0, sizeof flux_sum); memset(pc_sum, 0, sizeof pc_sum); memset(s_sin, 0, sizeof s_sin);
    memset(s_cos, 0, sizeof s_cos); memset(flux_n, 0, sizeof flux_n); memset(pc_n, 0, sizeof pc_n);
    memset(n_trig, 0, sizeof n_trig); memset(cnt, 0, sizeof cnt); memset(ifc, 0, sizeof ifc); memset(polar, 0, sizeof polar);
    memset(saa, 0, sizeof saa);
    double *am = malloc(sizeof(double) * (size_t)(nrow ? nrow : 1)), *Lv = malloc(sizeof(double) * (size_t)(nrow ? nrow : 1));
    int *st = malloc(sizeof(int) * (size_t)(nrow ? nrow : 1));
    if (!am || !Lv || !st) fail("memory");
    for (long r = 0; r < nrow; r++) {
        const uint8_t *p = rows + (size_t)r * ROW;
        uint64_t t;
        float f[63];
        memcpy(&t, p, 8);
        memcpy(f, p + 22, sizeof f);
        int8_t ifc_on = (int8_t)p[22 + 63 * 4 + 4];
        st[r] = -1;
        if ((int64_t)t < day0) continue;
        int64_t k = ((int64_t)t - day0) / 300000;
        if (k >= STEPS) continue;
        st[r] = (int)k;
        double lat = (double)f[1], lon = (double)f[2];
        am[r] = grid_at(g.grid[0], lat, lon);
        Lv[r] = grid_at(g.grid[1], lat, lon);
        cnt[k]++;
        ifc[k] += ifc_on > 0;
        int is_polar = am[r] >= 60.0;
        polar[k] += is_polar;
        double lon180 = lon > 180.0 ? lon - 360.0 : lon;
        saa[k] += lat > -50.0 && lat < 0.0 && lon180 > -90.0 && lon180 < 40.0 && Lv[r] < 2.5;
        for (int c = 0; c < NFLUX; c++) {
            if (g.src[c] < 0) continue;
            float v = f[3 + g.src[c]];
            if (!isfinite(v) || v < 0) continue;
            double fl = (double)v * g.factor[c];
            flux_sum[k][c] += fl;
            flux_n[k][c]++;
            if (c == 5 && is_polar) { pc_sum[k] += fl; pc_n[k]++; }
        }
        // MLT: theta = pi + (lambda_sat - lambda_sun); sin(theta) = -(sin a cos b - cos a sin b), cos(theta) = -(cos a cos b + sin a sin b)
        double ca = grid_at(g.grid[2], lat, lon), sa = grid_at(g.grid[3], lat, lon), norm = sqrt(ca * ca + sa * sa);
        int64_t m = ((int64_t)t - day0) / 60000;
        m = m > 1439 ? 1439 : m;
        double cb = (double)sun[2 * m], sb = (double)sun[2 * m + 1];
        if (norm > 0) {
            ca = ca / norm; sa = sa / norm;
            double s = -(sa * cb - ca * sb), c = -(ca * cb + sa * sb);
            if (isfinite(s) && isfinite(c)) { s_sin[k] += s; s_cos[k] += c; n_trig[k]++; }
        }
    }
    static float out[STEPS][COLUMNS];
    double *tmp = malloc(sizeof(double) * (size_t)(nrow ? nrow : 1));
    if (!tmp) fail("memory");
    long used = 0;
    for (int k = 0; k < STEPS; k++) {
        for (int c = 0; c < COLUMNS; c++) out[k][c] = NAN;
        if (!cnt[k]) { out[k][C_VALID] = 0.0f; continue; }
        used += cnt[k];
        for (int c = 0; c < NFLUX; c++)
            if (flux_n[k][c]) out[k][c] = (float)(flux_sum[k][c] / (double)flux_n[k][c]);
        if (pc_n[k]) out[k][23] = (float)(pc_sum[k] / (double)pc_n[k]);
        out[k][C_VALID] = (float)cnt[k];
        out[k][C_IFC] = (float)((double)ifc[k] / (double)cnt[k]);
        out[k][C_POLAR] = (float)((double)polar[k] / (double)cnt[k]);
        out[k][C_SAA] = (float)((double)saa[k] / (double)cnt[k]);
        for (int q = 0; q < 2; q++) {
            const double *v = q ? Lv : am;
            int mm = 0;
            for (long r = 0; r < nrow; r++) {
                if (st[r] != k) continue;
                if (q) tmp[mm++] = isfinite(v[r]) && v[r] < 20.0 ? v[r] : 20.0;     // L: missing or above 20 counts as 20
                else if (isfinite(v[r])) tmp[mm++] = v[r];
            }
            out[k][q ? C_L : C_MLAT] = (float)median_or_nan(tmp, mm);
        }
        if (n_trig[k]) {
            out[k][C_SIN] = (float)(s_sin[k] / (double)n_trig[k]);
            out[k][C_COS] = (float)(s_cos[k] / (double)n_trig[k]);
        }
    }
    store(a[4], out, sizeof out);
    printf("{\"job\":\"%s\",\"name\":\"%s\",\"day\":%d,\"rows\":%ld,\"rows_in_day\":%ld,\"ns\":%.0f}\n", names[form],
           a[0], ymd, nrow, used, (now_s() - t0) * 1e9);
    free(rows); free(sun); free(am); free(Lv); free(st); free(tmp);
}

static void day_job(char **a)
{
    // a: NAME SUMMARY STATE TABLES CODES_OUT YYYYMMDD
    double t0 = now_s();
    static Tables t;
    static State s;
    read_tables(a[3], &t);
    read_state(a[2], &s, t.history);
    size_t n;
    float *x = load(a[1], &n);
    if (!x || n != (size_t)STEPS * COLUMNS * 4) fail("summary: 288 x 36 float32 required");
    int ymd = atoi(a[5]);
    int year = ymd / 10000, month = ymd / 100 % 100, dom = ymd % 100;
    if (year < 1970 || month < 1 || month > 12 || dom < 1 || dom > 31) fail("day: YYYYMMDD");
    int32_t day = civil_days(year, month, dom);
    int ys = -1;
    for (int k = 0; k < t.nyears; k++) if (t.years[k] == year - 1) ys = k;
    if (ys < 0) for (int k = 0; k < t.nyears; k++) if (t.years[k] == year) ys = k;
    if (ys < 0) fail("tables: no scale table of the previous or the same year");
    const double *scl = t.scale[ys];
    // background: per bin and channel the median of the stored daily medians of the previous history days
    static double bg[NBINS * NFLUX];
    double vals[MAX_HISTORY];
    int cells = 0;
    for (int j = 0; j < NBINS * NFLUX; j++) {
        int m = 0;
        for (int k = 0; k < s.history; k++)
            if (s.day[k] != EMPTY_DAY && s.day[k] < day && s.day[k] >= day - s.history && s.med[k][j] == s.med[k][j])
                vals[m++] = (double)s.med[k][j];
        bg[j] = m >= t.min_periods ? median(vals, m) : NAN;
        cells += m >= t.min_periods;
    }
    static uint16_t h[STEPS][FEATURES];
    static double lfm[STEPS][NFLUX];            // log flux of the residual: bad steps missing
    static int bins[STEPS];
    int since = STEPS;
    for (int k = 0; k < STEPS; k++) {
        const float *r = x + (size_t)k * COLUMNS;
        float am = r[C_MLAT], valid = r[C_VALID], ifc = r[C_IFC];
        int bad_q = ifc > 0 || !(valid >= 75);
        int bad_z = bad_q || !isfinite(am);
        bins[k] = bin_of(am);
        for (int c = 0; c < NFLUX; c++) {
            double lf = det_log10((double)r[c] + t.eps[c]);
            // nan_to_num, then float32, then float16 (an infinity stays one)
            float fv = lf != lf ? 0.0f : isinf(lf) ? (lf > 0 ? INFINITY : -INFINITY) : (float)lf;
            h[k][c] = to_half((double)fv);
            lfm[k][c] = bad_z || !isfinite(r[c]) ? NAN : lf;
        }
        float L = r[C_L];
        float geo[8] = {am / 90.0f, (L != L ? L : L < 20.0f ? L : 20.0f) / 20.0f, r[C_SIN], r[C_COS], r[C_POLAR], r[C_SAA],
                        ifc, valid / 150.0f};
        for (int g = 0; g < 8; g++) h[k][23 + g] = to_half(geo[g] != geo[g] ? 0.0 : (double)geo[g]);
        for (int o = 31; o < 54; o++) h[k][o] = 0;
        since = am >= 60.0f ? 0 : (since + 1 < STEPS ? since + 1 : STEPS);
        h[k][54] = to_half(bad_q ? 1.0 : 0.0);
        h[k][55] = to_half((double)(float)(since / 288.0));
    }
    for (int k = 0; k < STEPS; k++)
        for (int c = 0; c < NFLUX; c++) {
            int j = bins[k] * NFLUX + c;
            double z = (lfm[k][c] - bg[j]) / scl[j];
            z = z != z ? 0.0 : z;
            z = z < -20.0 ? -20.0 : z > 50.0 ? 50.0 : z;
            h[k][ZT + c] = to_half(z);
        }
    // the model's input codes
    long low = -(1L << (t.bits - 1)), high = (1L << (t.bits - 1)) - 1;
    double unit = ldexp(1.0, t.qpos);
    int16_t *codes = malloc((size_t)STEPS * t.ncols * 2);
    if (!codes) fail("memory");
    long clipped = 0;
    for (int k = 0; k < STEPS; k++)
        for (int m = 0; m < t.ncols; m++) {
            if (t.cols[m] == PAD_COL) { codes[(size_t)k * t.ncols + m] = 0; continue; }
            double f = (from_half(h[k][t.cols[m]]) - (double)t.mu[m]) / (double)t.sd[m];
            double v = floor(f * unit + 0.5);
            if (v < low || v > high) clipped++;
            codes[(size_t)k * t.ncols + m] = (int16_t)(v < low ? low : v > high ? high : (long)v);
        }
    store(a[4], codes, (size_t)STEPS * t.ncols * 2);
    // the day's medians enter the state (its slot held the day that has just left the window)
    int slot = (int)(((day % s.history) + s.history) % s.history);
    s.day[slot] = day;
    double tmp[STEPS];
    for (int j = 0; j < NBINS; j++)
        for (int c = 0; c < NFLUX; c++) {
            int m = 0;
            for (int k = 0; k < STEPS; k++)
                if (bins[k] == j && lfm[k][c] == lfm[k][c]) tmp[m++] = lfm[k][c];
            s.med[slot][j * NFLUX + c] = m ? (float)median(tmp, m) : NAN;
        }
    write_state(a[2], &s);
    printf("{\"job\":\"day\",\"name\":\"%s\",\"day\":%d,\"background_cells\":%d,\"clipped\":%ld,\"ns\":%.0f}\n",
           a[0], ymd, cells, clipped, (now_s() - t0) * 1e9);
    free(x); free(codes);
}

static void win_job(char **a, int argc, int pad)
{
    // a: NAME OUT WINDOW STRIDE CHANNELS FIRST [FRAMES] DAY_CODES...
    double t0 = now_s();
    long window = atol(a[2]), stride = atol(a[3]), ch = atol(a[4]), first = atol(a[5]), want = pad ? atol(a[6]) : 0;
    int day0 = pad ? 7 : 6;
    if (window < 1 || stride < 1 || ch < 1 || first < 0 || (pad && want < 1) || argc < day0 + 1)
        fail("win: window, stride, channels, first, frames, days");
    size_t total = 0;
    int16_t *stream = NULL;
    for (int d = day0; d < argc; d++) {
        size_t n;
        int16_t *day = load(a[d], &n);
        if (!day || n % ((size_t)ch * 2)) fail(a[d]);
        stream = realloc(stream, total + n);
        if (!stream) fail("memory");
        memcpy((uint8_t *)stream + total, day, n);
        total += n;
        free(day);
    }
    long rows = (long)(total / ((size_t)ch * 2)), frames = 0;
    if (pad) frames = want;
    else for (long st = first; st + window <= rows; st += stride) frames++;
    if (!frames || first >= rows) fail("win: no window");
    int16_t *out = calloc((size_t)frames * window * ch, 2);
    if (!out) fail("memory");
    for (long f = 0; f < frames; f++) {
        long st = first + f * stride, n = st >= rows ? 0 : (rows - st < window ? rows - st : window);
        memcpy(out + (size_t)f * window * ch, stream + (size_t)st * ch, (size_t)n * ch * 2);
    }
    store(a[1], out, (size_t)frames * window * ch * 2);
    printf("{\"job\":\"win\",\"name\":\"%s\",\"frames\":%ld,\"ns\":%.0f}\n", a[0], frames, (now_s() - t0) * 1e9);
    free(stream); free(out);
}

// ---- Comp (SIREN Model 3) as the ModernTCN hybrid: ASTRACM1 parameters (directrtl_comp_mtcn.params_bytes): "ASTRACM1",
// u32 channels, inputs, rows, receptive field, i32 input and output points, u32 count scale, u32 0, then float64 mean
// and standard deviation per channel of ln(1 + q / scale)
#define MTCN_MAX 64
#define MTCN_MEMO 65536
typedef struct { uint32_t ch, inputs, rows, rf; int32_t iq, oq; uint32_t scale; double mean[MTCN_MAX], sd[MTCN_MAX]; } mtcn_t;

static mtcn_t read_mtcn(const char *path)
{
    size_t n;
    uint8_t *b = load(path, &n);
    mtcn_t p;
    if (!b || n < 40 || memcmp(b, "ASTRACM1", 8)) fail("mtcn: parameters");
    memcpy(&p.ch, b + 8, 4); memcpy(&p.inputs, b + 12, 4); memcpy(&p.rows, b + 16, 4); memcpy(&p.rf, b + 20, 4);
    memcpy(&p.iq, b + 24, 4); memcpy(&p.oq, b + 28, 4); memcpy(&p.scale, b + 32, 4);
    if (p.ch < 1 || p.ch > MTCN_MAX || p.inputs < p.ch + 6 || p.rows < 1 || p.iq < 0 || p.iq > 30 || p.oq < 0 || p.oq > 30
        || p.scale < 1 || n != 40 + 16 * (size_t)p.ch)
        fail("mtcn: parameters");
    memcpy(p.mean, b + 40, 8 * (size_t)p.ch);
    memcpy(p.sd, b + 40 + 8 * (size_t)p.ch, 8 * (size_t)p.ch);
    for (uint32_t c = 0; c < p.ch; c++)
        if (!isfinite(p.mean[c]) || !(p.sd[c] > 0) || !isfinite(p.sd[c])) fail("mtcn: parameters");
    free(b);
    return p;
}

// floor(v + 0.5) saturated to 16 bits
static int16_t code16(double v)
{
    double f = floor(v + 0.5);
    return f >= 32767.0 ? 32767 : f <= -32768.0 ? -32768 : (int16_t)f;
}

static void mtcn_job(char **a, int argc)
{
    // a: NAME PARAMS OUT (T Q AUX)...
    double t0 = now_s();
    if (argc < 6 || (argc - 3) % 3) fail("mtcn: days as T Q AUX");
    mtcn_t p = read_mtcn(a[1]);
    size_t len = (size_t)p.rows + p.rf, width = p.inputs, frames = 0;
    double up = ldexp(1.0, p.iq), sc = (double)p.scale;
    int16_t *out = NULL;
    // a count's code per channel once (counts below MTCN_MEMO): the same operations give the same code every time
    int16_t *memo = malloc((size_t)p.ch * MTCN_MEMO * 2);
    uint8_t *seen = calloc((size_t)p.ch * MTCN_MEMO, 1);
    if (!memo || !seen) fail("memory");
    for (int d = 3; d < argc; d += 3) {
        long t_len = atol(a[d]);
        size_t nq, na;
        int32_t *q = load(a[d + 1], &nq);
        float *aux = load(a[d + 2], &na);
        if (t_len < 2 || !q || !aux || nq != (size_t)t_len * p.ch * 4 || na != (size_t)t_len * 24)
            fail("mtcn: a day's counts or auxiliary inputs");
        size_t nf = ((size_t)t_len + p.rows - 1) / p.rows;
        out = realloc(out, (frames + nf) * len * width * 2);
        if (!out) fail("memory");
        int16_t *o = out + frames * len * width;
        memset(o, 0, nf * len * width * 2);
        for (size_t f = 0; f < nf; f++)
            for (size_t j = 0; j < len; j++) {
                long t = (long)(f * p.rows + j) - (long)p.rf;
                if (t < 0 || t >= t_len) continue;
                int16_t *r = o + (f * len + j) * width;
                for (uint32_t c = 0; c < p.ch; c++) {
                    int32_t v = q[(size_t)t * p.ch + c];
                    if (v < 0) fail("mtcn: a negative count");
                    size_t k = (size_t)c * MTCN_MEMO + (size_t)v;
                    if (v < MTCN_MEMO && seen[k]) { r[c] = memo[k]; continue; }
                    r[c] = code16((det_ln(((double)v + sc) / sc) - p.mean[c]) / p.sd[c] * up);
                    if (v < MTCN_MEMO) { memo[k] = r[c]; seen[k] = 1; }
                }
                for (uint32_t k = 0; k < 6; k++) {
                    double x = aux[(size_t)t * 6 + k];
                    if (!isfinite(x)) fail("mtcn: an auxiliary input");
                    r[p.ch + k] = code16(x * up);
                }
            }
        frames += nf;
        free(q); free(aux);
    }
    store(a[2], out, frames * len * width * 2);
    printf("{\"job\":\"mtcn\",\"name\":\"%s\",\"frames\":%zu,\"ns\":%.0f}\n", a[0], frames, (now_s() - t0) * 1e9);
    free(out); free(memo); free(seen);
}

static void mtcnpred_job(char **a, int argc)
{
    // a: NAME PARAMS OUTPUTS (FIRST T PRED)...
    double t0 = now_s();
    if (argc < 6 || (argc - 3) % 3) fail("mtcnpred: days as FIRST T PRED");
    mtcn_t p = read_mtcn(a[1]);
    size_t n, len = (size_t)p.rows + p.rf;
    int16_t *o = load(a[2], &n);
    if (!o || n % (len * p.ch * 2)) fail("mtcnpred: the hybrid's outputs");
    // a code's prediction per channel once
    int32_t *memo = malloc((size_t)p.ch * 65536 * 4);
    uint8_t *seen = calloc((size_t)p.ch * 65536, 1);
    if (!memo || !seen) fail("memory");
    double down = ldexp(1.0, -p.oq), sc = (double)p.scale;
    long steps = 0;
    for (int d = 3; d < argc; d += 3) {
        long first = atol(a[d]), t_len = atol(a[d + 1]);
        if (first < 0 || t_len < 2 || n / (len * p.ch * 2) < (size_t)first + ((size_t)t_len + p.rows - 1) / p.rows)
            fail("mtcnpred: the hybrid's outputs of a day");
        int32_t *pred = malloc((size_t)(t_len - 1) * p.ch * 4);
        if (!pred) fail("memory");
        for (long t = 0; t + 1 < t_len; t++) {
            const int16_t *r = o + (((size_t)first + (size_t)t / p.rows) * len + p.rf + (size_t)t % p.rows) * p.ch;
            for (uint32_t c = 0; c < p.ch; c++) {
                size_t k = (size_t)c * 65536 + (size_t)(r[c] + 32768);
                if (!seen[k]) {
                    double e = det_expm1((double)r[c] * down * p.sd[c] + p.mean[c]);
                    double v = (e > 0 ? e : 0.0) * sc;
                    memo[k] = v >= 2147483647.0 ? INT32_MAX : (int32_t)nearbyint(v);
                    seen[k] = 1;
                }
                pred[(size_t)t * p.ch + c] = memo[k];
            }
        }
        store(a[d + 2], pred, (size_t)(t_len - 1) * p.ch * 4);
        steps += t_len - 1;
        free(pred);
    }
    printf("{\"job\":\"mtcnpred\",\"name\":\"%s\",\"steps\":%ld,\"ns\":%.0f}\n", a[0], steps, (now_s() - t0) * 1e9);
    free(o); free(memo); free(seen);
}

// ---- Comp as the GRU predictor: ASTRAGX1 thresholds (directrtl_pd_zt.gx_threshold_bytes): "ASTRAGX1", u32 C, u32
// count levels, u32 auxiliary inputs (6), u32 auxiliary levels, i32 XH, int64 count thresholds C x levels, float32
// auxiliary thresholds 6 x levels (all LE).
static void gxc_job(char **a)
{
    // a: NAME THRESHOLDS OUT T Q AUX
    double t0 = now_s();
    size_t nb, nq, na;
    uint8_t *b = load(a[1], &nb);
    uint32_t h[4];
    int32_t xh;
    if (!b || nb < 28 || memcmp(b, "ASTRAGX1", 8)) fail("gxc: thresholds");
    memcpy(h, b + 8, 16);
    memcpy(&xh, b + 24, 4);
    uint32_t ch = h[0], cl = h[1], ax = h[2], al = h[3];
    if (!ch || ch > 64 || !cl || cl > 256 || ax != 6 || !al || al > 256 || nb != 28 + (size_t)ch * cl * 8 + (size_t)ax * al * 4)
        fail("gxc: thresholds");
    const uint8_t *cb = b + 28, *ab = cb + (size_t)ch * cl * 8;
    long t_len = atol(a[3]);
    int32_t *q = load(a[4], &nq);
    float *aux = load(a[5], &na);
    if (t_len < 2 || !q || !aux || nq != (size_t)t_len * ch * 4 || na != (size_t)t_len * ax * 4)
        fail("gxc: the day's counts or auxiliary inputs");
    size_t w = (size_t)ch + ax;
    int16_t *out = malloc((size_t)(t_len - 1) * w * 2);
    int64_t *ct = malloc((size_t)ch * cl * 8);
    float *at = malloc((size_t)ax * al * 4);
    if (!out || !ct || !at) fail("memory");
    memcpy(ct, cb, (size_t)ch * cl * 8);
    memcpy(at, ab, (size_t)ax * al * 4);
    // a row of thresholds in non-decreasing order (no NaN) is counted by binary search (the number at or below the
    // value), any other row by comparing every threshold (the same count; 20 s a day at 333 MHz for 256 levels)
    int sorted = 1;
    for (uint32_t c = 0; c < ch; c++)
        for (uint32_t k = 0; k + 1 < cl; k++) sorted &= ct[(size_t)c * cl + k] <= ct[(size_t)c * cl + k + 1];
    for (uint32_t c = 0; c < ax; c++)
        for (uint32_t k = 0; k + 1 < al; k++) sorted &= at[(size_t)c * al + k] <= at[(size_t)c * al + k + 1];
    for (long t = 0; t + 1 < t_len; t++) {
        int16_t *r = out + (size_t)t * w;
        for (uint32_t c = 0; c < ch; c++) {
            int64_t v = q[(size_t)t * ch + c];
            const int64_t *row = ct + (size_t)c * cl;
            uint32_t lo = 0, hi = cl;
            if (sorted) {
                while (lo < hi) { uint32_t m = (lo + hi) / 2; if (row[m] <= v) lo = m + 1; else hi = m; }
            } else {
                for (uint32_t k = 0; k < cl; k++) lo += v >= row[k];
            }
            r[c] = (int16_t)((int32_t)lo - xh);
        }
        for (uint32_t c = 0; c < ax; c++) {
            float x = aux[(size_t)t * ax + c];
            const float *row = at + (size_t)c * al;
            uint32_t lo = 0, hi = al;
            if (sorted) {
                while (lo < hi) { uint32_t m = (lo + hi) / 2; if (row[m] <= x) lo = m + 1; else hi = m; }
            } else {
                for (uint32_t k = 0; k < al; k++) lo += x >= row[k];
            }
            r[ch + c] = (int16_t)((int32_t)lo - xh);
        }
    }
    store(a[2], out, (size_t)(t_len - 1) * w * 2);
    printf("{\"job\":\"gxc\",\"name\":\"%s\",\"steps\":%ld,\"sorted\":%d,\"ns\":%.0f}\n", a[0], t_len - 1, sorted,
           (now_s() - t0) * 1e9);
    free(out); free(ct); free(at); free(q); free(aux); free(b);
}

int main(int argc, char **argv)
{
    if (argc != 2) fail("usage: f-astra-pd-zt CONFIG");
    FILE *f = fopen(argv[1], "r");
    if (!f) fail(argv[1]);
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        char *tok[256];
        int n = 0;
        for (char *p = strtok(line, " \t\r\n"); p && n < 256; p = strtok(NULL, " \t\r\n")) tok[n++] = p;
        if (!n) continue;
        if (!strcmp(tok[0], "sum") && n == 7) sum_job(tok + 1, 0);
        else if (!strcmp(tok[0], "sumc") && n == 7) sum_job(tok + 1, 1);
        else if (!strcmp(tok[0], "sumt") && n == 7) sum_job(tok + 1, 2);
        else if (!strcmp(tok[0], "day") && n == 7) day_job(tok + 1);
        else if (!strcmp(tok[0], "win") && n >= 8) win_job(tok + 1, n - 1, 0);
        else if (!strcmp(tok[0], "winpad") && n >= 9) win_job(tok + 1, n - 1, 1);
        else if (!strcmp(tok[0], "mtcn") && n >= 7) mtcn_job(tok + 1, n - 1);
        else if (!strcmp(tok[0], "mtcnpred") && n >= 7) mtcnpred_job(tok + 1, n - 1);
        else if (!strcmp(tok[0], "gxc") && n == 7) gxc_job(tok + 1);
        else fail("unknown or malformed job");
        fflush(stdout);
    }
    fclose(f);
    return 0;
}
