/* F-ASTRA per-orbit preprocessing on the TE0715 PS (directrtl_orbit): an orbit's downloaded POES-format raw records
 * (281-byte rows: time, frame and position fields, 63 float32 fields, 7 int8 fields; the l1a variables in file order)
 * reduced to 50 s bins of the 46 count-rate channels (MEPED 22, TED 16 + 8 eflux): per bin the channel means, the
 * mean position, an 8-bit log2 code per channel and the count of fill samples (NaN or negative). CRC-32 of the input.
 * The host reference is directrtl_orbit.orbit_features (byte for byte).
 * usage: f-astra-orbit-prep IN OUT_FEATURES [BIN_ROWS]   prints one JSON line */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROW 281
#define CPS 46
#define FIRST_CPS 3   /* float fields: alt, lat, lon, then the 46 count-rate channels */

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: orbit_prep IN OUT_FEATURES [BIN_ROWS]\n"); return 2; }
    int bin_rows = argc > 3 ? atoi(argv[3]) : 25;
    if (bin_rows < 1 || bin_rows > 100000) return 2;
    double t0 = now_s();
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) { fprintf(stderr, "open failed\n"); return 1; }
    crc_init();
    uint8_t row[ROW];
    double sum[CPS], lat = 0, lon = 0;
    uint32_t fill[CPS];
    memset(sum, 0, sizeof sum);
    memset(fill, 0, sizeof fill);
    uint32_t crc = 0, rows = 0, bins = 0, in_bin = 0, fills = 0;
    size_t got;
    while ((got = fread(row, 1, ROW, in)) == ROW) {
        crc = crc32_update(crc, row, ROW);
        float f[FIRST_CPS + CPS];
        memcpy(f, row + 22, sizeof f);
        lat += f[1];
        lon += f[2];
        for (int c = 0; c < CPS; c++) {
            float v = f[FIRST_CPS + c];
            if (isnan(v) || v < 0) { fill[c]++; continue; }
            sum[c] += v;
        }
        rows++;
        if (++in_bin == (uint32_t)bin_rows) {
            /* bin record: 46 float32 means, lat, lon (float32), 46 uint8 codes, 46 uint8 fill counts */
            float mean[CPS + 2];
            uint8_t code[CPS], nfill[CPS];
            for (int c = 0; c < CPS; c++) {
                uint32_t good = in_bin - fill[c];
                mean[c] = good ? (float)(sum[c] / good) : -1.0f;
                double l = mean[c] > 0 ? log2(1.0 + mean[c]) * 8.0 : 0.0;
                code[c] = (uint8_t)(l > 255 ? 255 : l + 0.5);
                nfill[c] = (uint8_t)(fill[c] > 255 ? 255 : fill[c]);
                fills += fill[c];
            }
            mean[CPS] = (float)(lat / in_bin);
            mean[CPS + 1] = (float)(lon / in_bin);
            if (fwrite(mean, sizeof mean, 1, out) != 1 || fwrite(code, CPS, 1, out) != 1 || fwrite(nfill, CPS, 1, out) != 1) {
                fprintf(stderr, "write failed\n");
                return 1;
            }
            memset(sum, 0, sizeof sum);
            memset(fill, 0, sizeof fill);
            lat = lon = 0;
            in_bin = 0;
            bins++;
        }
    }
    crc = crc32_update(crc, row, got);
    if (fclose(out) || fclose(in)) return 1;
    printf("{\"rows\": %u, \"tail_bytes\": %u, \"bins\": %u, \"bin_rows\": %d, \"fill_samples\": %u, \"crc32\": \"%08x\", "
           "\"seconds\": %.4f}\n", rows, (unsigned)got, bins, bin_rows, fills, crc, now_s() - t0);
    return 0;
}
