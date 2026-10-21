"""AI #1 (PD) power over one whole PD run and its cumulative energy, as an editable SVG (trade-off slide, right).

    python tools/pd_run_energy.py GUARD_POWER_CSV PD_START_NS PD_END_NS OUT_SVG

Top panel: 12 V board input power from the INA228 guard log (100 Hz); every 2 px column keeps its lowest and highest
sample, so the peak survives the decimation. Bottom panel: cumulative energy (trapezoid over the guard timestamps) in
Wh. Only the PD window of the log is read (tools/pd_peak_trace.read_window). Writes the SVG and the whole window as CSV
(t_s, power_W, energy_Wh). Like the slide, no grid lines and no limit line.
- bpg-r30e (e74, 10/21 02:26 UTC run, guard13): 1792548885599745825 1792549069969745825, 1,100 runs
"""
import pathlib
import sys

from pd_peak_trace import read_window

RUNS = 1100  # PD frames in the e74 run (session record)


def svg(rows):
    W, H, L, R = 740, 520, 96, 20
    T1, B1, T2, B2 = 40, 250, 310, 430  # power panel, energy panel
    X1, P0, P1, E1 = 190.0, 2.5, 6.5, 0.25
    px = lambda t: L + t / X1 * (W - L - R)
    pw = lambda w: B1 - (w - P0) / (P1 - P0) * (B1 - T1)
    pe = lambda e: B2 - e / E1 * (B2 - T2)
    NAVY, RED, INK, INK6 = "#20386E", "#B3202A", "#16181D", "#353B47"
    f = "font-family:'Source Serif 4';"
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">', f'<rect width="{W}" height="{H}" fill="#FFFFFF"/>']
    for w in (3, 4, 5, 6):
        s.append(f'<text x="{L-14}" y="{pw(w)+10:.1f}" text-anchor="end" style="{f}font-size:28px" fill="{INK}">{w}</text>')
    for e in (0.0, 0.1, 0.2):
        s.append(f'<text x="{L-14}" y="{pe(e)+10:.1f}" text-anchor="end" style="{f}font-size:28px" fill="{INK}">{e:g}</text>')
    for t in (0, 50, 100, 150):
        s.append(f'<line x1="{px(t):.1f}" x2="{px(t):.1f}" y1="{B2}" y2="{B2+8}" stroke="{INK}" stroke-width="2"/>')
        s.append(f'<text x="{px(t):.1f}" y="{B2+40}" text-anchor="middle" style="{f}font-size:28px" fill="{INK}">{t}</text>')
    for top, bot in ((T1, B1), (T2, B2)):
        s.append(f'<line x1="{L}" x2="{L}" y1="{top-10}" y2="{bot}" stroke="{INK}" stroke-width="2"/><line x1="{L}" x2="{W-R}" y1="{bot}" y2="{bot}" stroke="{INK}" stroke-width="2"/>')
    cols = {}
    for t, w, e in rows:
        cols.setdefault(int(px(t) // 2), []).append((t, w, e))
    band = []
    for k in sorted(cols):
        lo, hi = min(c[1] for c in cols[k]), max(c[1] for c in cols[k])
        band.append(f"M{2*k+1:.0f},{pw(hi):.1f} L{2*k+1:.0f},{pw(lo):.1f}")
    s.append(f'<path d="{" ".join(band)}" fill="none" stroke="{NAVY}" stroke-width="2"/>')
    s.append('<path d="M' + " L".join(f"{px(c[-1][0]):.1f},{pe(c[-1][2]):.1f}" for c in (cols[k] for k in sorted(cols))) + f'" fill="none" stroke="{RED}" stroke-width="4" stroke-linejoin="round"/>')
    tp, wp, _ = max(rows, key=lambda r: r[1])
    mean = sum(r[1] for r in rows) / len(rows); te, ee = rows[-1][0], rows[-1][2]
    s.append(f'<circle cx="{px(tp):.1f}" cy="{pw(wp):.1f}" r="10" fill="{RED}" stroke="{INK}" stroke-width="3"/>')
    anchor, dx = ("end", -16) if px(tp) > W / 2 else ("start", 16)
    s.append(f'<text x="{px(tp)+dx:.1f}" y="{pw(wp)-14:.1f}" text-anchor="{anchor}" style="{f}font-size:28px;font-weight:700" fill="{RED}">peak {wp:.2f} W</text>')
    s.append(f'<text x="{L+12}" y="{T1+20}" style="{f}font-size:28px;font-weight:700" fill="{NAVY}">mean {mean:.2f} W</text>')
    s.append(f'<text x="{px(te)-12:.1f}" y="{pe(ee)-14:.1f}" text-anchor="end" style="{f}font-size:28px;font-weight:700" fill="{RED}">{ee:.3f} Wh</text>')
    s.append(f'<text x="{L+12}" y="{T2+24}" style="{f}font-size:28px" fill="{INK6}">{RUNS:,} runs \u00b7 {ee / RUNS * 1000:.2f} mWh each</text>')
    s.append(f'<text x="{L-14}" y="{T1-18}" text-anchor="end" style="{f}font-size:26px" fill="{INK6}">W</text>')
    s.append(f'<text x="{L-14}" y="{T2-18}" text-anchor="end" style="{f}font-size:26px" fill="{INK6}">Wh</text>')
    s.append(f'<text x="{W-R}" y="{B2+80}" text-anchor="end" style="{f}font-size:28px" fill="{INK6}">time (s)</text>')
    return "\n".join(s + ["</svg>"]) + "\n"


def main():
    src, w0, w1, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), pathlib.Path(sys.argv[4])
    raw = read_window(src, w0, w1)
    rows, e = [((raw[0][0] - w0) / 1e9, raw[0][1], 0.0)], 0.0
    for (ta, wa), (tb, wb) in zip(raw, raw[1:]):
        e += (tb - ta) / 1e9 * (wa + wb) / 2 / 3600
        rows.append(((tb - w0) / 1e9, wb, e))
    out.write_text(svg(rows), encoding="utf-8", newline="\n")
    out.with_suffix(".csv").write_text("t_s,power_W,energy_Wh\n" + "\n".join(f"{t:.4f},{w:.4f},{x:.6f}" for t, w, x in rows) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(rows)} samples over {rows[-1][0]:.1f} s, peak {max(r[1] for r in rows):.4f} W, mean {sum(r[1] for r in rows) / len(rows):.4f} W, energy {e:.5f} Wh -> {out.name}")


if __name__ == "__main__":
    main()
