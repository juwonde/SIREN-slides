"""Real ESA-ADB telemetry window for the anomaly-detection slide, as an editable SVG.

    python tools/esa_telemetry.py ESA_ADB_ROOT [EVENT_ID] [OUT] [MISSION] [CHANNELS] [PAD] [WIDE]

ESA_ADB_ROOT holds metadata/<MISSION>/labels.csv and raw/<MISSION>/channels/channel_N.zip (ESA Anomaly Dataset,
Zenodo 10.5281/zenodo.15237121, CC BY 3.0 IGO). Needs pandas. Defaults: Mission2 event id_637, channels 73,74,81
(three signals freeze for 75 min while staying in range), out assets/models/esa-m2-id637.svg.
Navy = normal telemetry, crimson = the labeled anomaly interval; no y values (anonymised). Curves keep the min and max
of every 2 px column so the SVG stays small. PAD is the context on each side in event durations (default 2.5, at
least 90 min). WIDE = "WxH" draws one channel in a wide panel of that size (mission slide: 856x300).
- mission slide: id_8 Mission1 channel 29 (2003-01-12 anomaly, a dip then a new level), PAD 1.5, 856x300
"""
import csv
import pathlib
import sys
import zipfile

import pandas as pd

ROOT = pathlib.Path(__file__).resolve().parents[1]
data = pathlib.Path(sys.argv[1])
ev_id = sys.argv[2] if len(sys.argv) > 2 else "id_637"
out = sys.argv[3] if len(sys.argv) > 3 else str(ROOT / "assets" / "models" / "esa-m2-id637.svg")
mission = sys.argv[4] if len(sys.argv) > 4 else "Mission2"
meta = str(data / "metadata" / mission)
pick = (sys.argv[5] if len(sys.argv) > 5 else ("73,74,81" if ev_id == "id_637" else "")).split(",")
pad_factor = float(sys.argv[6]) if len(sys.argv) > 6 else 2.5
wide = tuple(int(v) for v in sys.argv[7].split("x")) if len(sys.argv) > 7 else None
pad_min = 0


def load(ch):
    with zipfile.ZipFile(data / "raw" / mission / "channels" / f"{ch}.zip") as z, z.open(ch) as f:
        return pd.read_pickle(f)[ch]


rows = [r for r in csv.DictReader(open(meta + "/labels.csv")) if r["ID"] == ev_id]
chs = [f"channel_{c}" for c in pick if c] or sorted({r["Channel"] for r in rows}, key=lambda c: int(c.split("_")[1]))[:3]
ts = lambda t: pd.Timestamp(t.replace("Z", "")).tz_localize(None)
s0 = min(ts(r["StartTime"]) for r in rows); e0 = max(ts(r["EndTime"]) for r in rows)
dur = (e0 - s0).total_seconds() / 60
pad = pd.Timedelta(minutes=max(pad_min, pad_factor * dur, 90))
w0, w1 = s0 - pad, e0 + pad
n = len(chs)
NAVY, CRIM, INK = "#20386E", "#B3202A", "#16181D"
font = "font-family:'Source Serif 4';"
grid = n == 4  # 2 x 2 small multiples, no bracket (the slide title carries event and date)
if grid:
    W, H, CW, PH = 820, 326, 390, 90
elif wide and n == 1:
    W, H = wide; L, Rm, T = 100, 8, 12; PH, GAP = H - 108 - T, 0
else:
    PH, GAP = (128, 26) if n >= 3 else (100, 20)
    L, Rm, T = 120, 20, 10
    W, H = 820, T + n * PH + (n - 1) * GAP + 110
svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">', f'<rect width="{W}" height="{H}" fill="#FFFFFF"/>']


def tick_step(x0, x1, most):
    for h in (1, 2, 3, 4, 6, 8, 12, 24):
        if (w1 - w0) / pd.Timedelta(hours=h) <= most:
            return pd.Timedelta(hours=h)
    return pd.Timedelta(hours=24)


def curve(ch, x0, x1, y0, ph):
    """Draw one channel into the box [x0, x1] x [y0, y0 + ph]; returns the time-to-x map."""
    X = lambda t: x0 + (t - w0).total_seconds() / (w1 - w0).total_seconds() * (x1 - x0)
    df = load(ch)
    seg = df[(df.index >= w0) & (df.index <= w1)]
    vmin, vmax = float(seg.min()), float(seg.max())
    span = (vmax - vmin) or 1.0; vmin -= 0.08 * span; vmax += 0.08 * span
    Y = lambda v: y0 + ph - (v - vmin) / (vmax - vmin) * ph
    svg.append(f'<line x1="{x0}" x2="{x0}" y1="{y0}" y2="{y0+ph}" stroke="{INK}" stroke-width="2"/>')
    lab = [(ts(r["StartTime"]), ts(r["EndTime"])) for r in rows if r["Channel"] == ch]
    inlab = lambda t: any(a <= t <= b for a, b in lab)
    raw = [(X(t), Y(float(v)), inlab(t)) for t, v in seg.items()]
    pts, col = [], []
    for p in raw:  # keep first, min, max of every 2 px column (order kept), split at label edges
        if col and (int(p[0] // 2) != int(col[0][0] // 2) or p[2] != col[0][2]):
            keep = {id(col[0]), id(min(col, key=lambda q: q[1])), id(max(col, key=lambda q: q[1])), id(col[-1])}
            pts += [q for q in col if id(q) in keep]; col = []
        col.append(p)
    pts += col
    runs, cur = [], [pts[0]]
    for p in pts[1:]:
        if p[2] != cur[-1][2]: runs.append(cur); cur = [cur[-1], p]
        else: cur.append(p)
    runs.append(cur)
    for r in runs:
        d = "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y, _ in r)
        svg.append(f'<path d="{d}" fill="none" stroke="{CRIM if r[-1][2] else NAVY}" stroke-width="{3.5 if r[-1][2] else 2.5}" stroke-linejoin="round"/>')
    return X


def axis(X, x0, x1, ya, most):
    svg.append(f'<line x1="{x0}" x2="{x1}" y1="{ya}" y2="{ya}" stroke="{INK}" stroke-width="2"/>')
    step, t = tick_step(x0, x1, most), w0.ceil("h")
    while t.hour % (step.total_seconds() // 3600):
        t += pd.Timedelta(hours=1)
    while t <= w1:
        x = X(t)
        if not x0 + 36 <= x <= x1 - 36:  # keep tick labels inside the box
            t += step; continue
        svg.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{ya}" y2="{ya+8}" stroke="{INK}" stroke-width="2"/>')
        svg.append(f'<text x="{x:.1f}" y="{ya+38}" text-anchor="middle" style="{font}font-size:28px" fill="{INK}">{t:%H:%M}</text>'); t += step


if grid:
    for i, ch in enumerate(chs):
        r, c = divmod(i, 2)
        x0, y0 = 10 + c * (CW + 30), r * 140 + 40
        svg.append(f'<text x="{x0}" y="{y0-12}" style="{font}font-size:28px;font-weight:700" fill="{INK}">ch {ch.split("_")[1]}</text>')
        X = curve(ch, x0, x0 + CW, y0, PH)
        if r == 1:
            axis(X, x0, x0 + CW, y0 + PH, 3)
else:
    for i, ch in enumerate(chs):
        y0 = T + i * (PH + GAP)
        X = curve(ch, L, W - Rm, y0, PH)
        svg.append(f'<text x="{L-16}" y="{y0+PH/2+10}" text-anchor="end" style="{font}font-size:30px;font-weight:700" fill="{INK}">ch {ch.split("_")[1]}</text>')
    ya = T + n * PH + (n - 1) * GAP
    axis(X, L, W - Rm, ya, 7)
    xs, xe = X(s0), X(e0)
    svg.append(f'<path d="M{xs:.1f},{ya+50} L{xs:.1f},{ya+62} L{xe:.1f},{ya+62} L{xe:.1f},{ya+50}" fill="none" stroke="{CRIM}" stroke-width="3"/>')
    svg.append(f'<text x="{(xs+xe)/2:.1f}" y="{ya+96}" text-anchor="middle" style="{font}font-size:28px;font-weight:700" fill="{CRIM}">anomaly label</text>')
    svg.append(f'<text x="{W-Rm}" y="{ya+96}" text-anchor="end" style="{font}font-size:28px" fill="{INK}">{s0:%Y-%m-%d} UTC</text>')
svg.append("</svg>")
pathlib.Path(out).write_text("\n".join(svg) + "\n", encoding="utf-8", newline="\n")
print(ev_id, chs, f"{dur:.0f} min", s0, "window", w0, w1)
