// The runners' fine timer (11/04). Image r23's clock source is the TTC at 54 kHz: clock_gettime moves in 18.4 us steps,
// so a 0.1 ms Comp run read as 4 or 6 steps and BUS's 24 windows (0.81 ms) as 43 or 44 (51.85 or 50.67 GOPS). The
// Cortex-A9 global timer (64 bits at CPU_3x2x, the CPU clock / 2 / (prescaler + 1)) runs at a few ns a count; Linux
// leaves it disabled with cpufreq, so gt_start enables the counter (never its comparator or interrupt). A count
// becomes ns with the CPU clock (scaling_cur_freq) read before a timed span and after it; a span whose CPU clock
// changed keeps the clock_gettime time (gt_ns returns 0).
#ifndef F_ASTRA_GT_H
#define F_ASTRA_GT_H

#include <stdint.h>
#include <stdio.h>

#define GT_PAGE 0xF8F00000u
enum { GT_LO = 0x200, GT_HI = 0x204, GT_CTRL = 0x208 };
#ifndef GT_CPU_FREQ
#define GT_CPU_FREQ "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"
#endif

static volatile uint32_t *gt;                   // the SCU page holding the global timer, or NULL (clock_gettime only)
static unsigned gt_div;                          // CPU clock / timer clock: 2 (prescaler + 1)

static unsigned long gt_cpu_khz(void)
{
    unsigned long khz = 0;
    FILE *f = fopen(GT_CPU_FREQ, "r");
    if (!f || fscanf(f, "%lu", &khz) != 1) khz = 0;
    if (f) fclose(f);
    return khz;
}

// ``page``: the mapped GT_PAGE (NULL: no fine timer). The counter is started when it is off.
static void gt_start(volatile uint32_t *page)
{
    gt = page && gt_cpu_khz() ? page : NULL;
    if (!gt) return;
    if (!(gt[GT_CTRL / 4] & 1u)) gt[GT_CTRL / 4] = gt[GT_CTRL / 4] | 1u;
    gt_div = 2u * (((gt[GT_CTRL / 4] >> 8) & 0xFFu) + 1u);
}

static uint64_t gt_read(void)
{
    uint32_t hi, lo, again;
    if (!gt) return 0;
    do {
        hi = gt[GT_HI / 4];
        lo = gt[GT_LO / 4];
        again = gt[GT_HI / 4];
    } while (hi != again);
    return (uint64_t)hi << 32 | lo;
}

// ns of the counts c0..c1 at the CPU clock ``khz`` (the caller knows that it held), or 0 without the timer
static inline uint64_t gt_ns_at(uint64_t c0, uint64_t c1, unsigned long khz)
{
    if (!gt || !khz) return 0;
    return (uint64_t)((double)(c1 - c0) * gt_div * 1e6 / (double)khz + 0.5);
}

// ns of the counts c0..c1 at the CPU clock ``khz`` read before the span, or 0 when the clock is unknown or has changed
static uint64_t gt_ns(uint64_t c0, uint64_t c1, unsigned long khz)
{
    if (!gt || !khz || gt_cpu_khz() != khz) return 0;
    return gt_ns_at(c0, c1, khz);
}

#endif
