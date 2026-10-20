"""PD time-power trace around the highest 100 Hz sample of a PD run, as an editable SVG (trade-off slide, right).

    python tools/pd_peak_trace.py GUARD_POWER_CSV PD_START_NS PD_END_NS [OUT_SVG]

GUARD_POWER_CSV is the INA228 guard log (host_utc_ns, ..., power_W; 12 V board input, 100 Hz, time-sorted). It can be
hundreds of MB: the PD window is found with a seek-based binary search and only that window is read, so a running guard
is not disturbed. PD_START_NS/PD_END_NS come from the power run record (phases.PD.windows). Writes the SVG (default
assets/hw/pd-tpower-e27.svg) and the 2 s cut next to it as CSV. Adopted design bpg-r27e (e27, 10/20 05:14 UTC run):
    1792472879631365525 1792473054971365525
"""
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def cut(path, w0, w1):
    f = open(path, "rb")
    size = os.path.getsize(path)

    def ts_at(pos):
        f.seek(pos); f.readline(); line = f.readline()
        return int(line.split(b",", 1)[0]) if line and line[:1].isdigit() else None
    lo, hi = 0, size
    while hi - lo > 4096:
        mid = (lo + hi) // 2; t = ts_at(mid)
        if t is None or t >= w0 - 2_000_000_000: hi = mid
        else: lo = mid
    f.seek(lo); f.readline(); rows = []
    for line in f:
        p = line.split(b","); t = int(p[0])
        if t < w0 - 2_000_000_000: continue
        if t > w1 + 2_000_000_000: break
        rows.append((t, float(p[7])))
    inside = [r for r in rows if w0 <= r[0] <= w1]
    tp, wp = max(inside, key=lambda r: r[1])
    win = [((t - (tp - 1_000_000_000)) / 1e9, w) for t, w in rows if tp - 1_000_000_000 <= t <= tp + 1_000_000_000]
    return win, wp, sum(w for _, w in inside) / len(inside)


def svg(rows):
    W, H, L, R, T, B = 740, 520, 96, 20, 50, 420
    X0, X1, Y0, Y1 = 0.0, 2.0, 3.5, 7.0
    px = lambda t: L + (t - X0) / (X1 - X0) * (W - L - R)
    py = lambda w: B - (w - Y0) / (Y1 - Y0) * (B - T)
    NAVY, CRIM, RED, INK, INK6, GRID = "#20386E", "#84161A", "#B3202A", "#16181D", "#353B47", "#C4C9D2"
    f = "font-family:'Source Serif 4';"
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">', f'<rect width="{W}" height="{H}" fill="#FFFFFF"/>']
    for w in (4.0, 5.0, 6.0, 7.0):
        s.append(f'<line x1="{L}" x2="{W-R}" y1="{py(w):.1f}" y2="{py(w):.1f}" stroke="{GRID}" stroke-width="1.5"/>')
        s.append(f'<text x="{L-14}" y="{py(w)+10:.1f}" text-anchor="end" style="{f}font-size:28px" fill="{INK}">{w:.0f}</text>')
    for t in (0.0, 0.5, 1.0, 1.5, 2.0):
        s.append(f'<line x1="{px(t):.1f}" x2="{px(t):.1f}" y1="{B}" y2="{B+8}" stroke="{INK}" stroke-width="2"/>')
        s.append(f'<text x="{px(t):.1f}" y="{B+40}" text-anchor="middle" style="{f}font-size:28px" fill="{INK}">{t:g}</text>')
    s.append(f'<line x1="{L}" x2="{L}" y1="{T-10}" y2="{B}" stroke="{INK}" stroke-width="2"/><line x1="{L}" x2="{W-R}" y1="{B}" y2="{B}" stroke="{INK}" stroke-width="2"/>')
    s.append(f'<line x1="{L}" x2="{W-R}" y1="{py(6.5):.1f}" y2="{py(6.5):.1f}" stroke="{CRIM}" stroke-width="2.5" stroke-dasharray="12 8"/>')
    s.append(f'<text x="{W-R}" y="{py(6.5)-12:.1f}" text-anchor="end" style="{f}font-size:28px;font-weight:700" fill="{CRIM}">limit 6.5 W</text>')
    s.append('<path d="M' + " L".join(f"{px(t):.1f},{py(w):.1f}" for t, w in rows) + f'" fill="none" stroke="{NAVY}" stroke-width="2.5" stroke-linejoin="round"/>')
    tp, wp = max(rows, key=lambda r: r[1])
    s.append(f'<circle cx="{px(tp):.1f}" cy="{py(wp):.1f}" r="11" fill="{RED}" stroke="{INK}" stroke-width="3"/>')
    s.append(f'<text x="{px(tp)+18:.1f}" y="{py(wp)-14:.1f}" style="{f}font-size:28px;font-weight:700" fill="{RED}">peak {wp:.2f} W</text>')
    s.append(f'<text x="{L+12}" y="150.0" style="{f}font-size:28px;font-weight:700" fill="{NAVY}">compute level \u2248 5.4 W</text>')
    s.append(f'<text x="{L+12}" y="{py(3.65):.1f}" style="{f}font-size:28px;font-weight:700" fill="{INK6}">dips: LayerNorm on FINN \u2248 4.5 W</text>')
    s.append(f'<text x="{W-R}" y="{B+80}" text-anchor="end" style="{f}font-size:28px" fill="{INK6}">time (s)</text>')
    s.append(f'<text x="{L-14}" y="{T-20}" text-anchor="end" style="{f}font-size:26px" fill="{INK6}">W</text>')
    return "\n".join(s + ["</svg>"]) + "\n"


def main():
    src, w0, w1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    out = pathlib.Path(sys.argv[4]) if len(sys.argv) > 4 else ROOT / "assets" / "hw" / "pd-tpower-e27.svg"
    rows, wp, mean = cut(src, w0, w1)
    out.write_text(svg(rows), encoding="utf-8", newline="\n")
    out.with_suffix(".csv").write_text("t_s,power_W\n" + "\n".join(f"{t:.4f},{w:.4f}" for t, w in rows) + "\n", encoding="utf-8", newline="\n")
    print(f"PD peak {wp:.4f} W, PD mean {mean:.4f} W, {len(rows)} samples in the 2 s window -> {out.name}")


if __name__ == "__main__":
    main()
