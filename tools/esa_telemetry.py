"""Real ESA-ADB telemetry window for the anomaly-detection slide, as an editable SVG.

    python tools/esa_telemetry.py ESA_ADB_ROOT [EVENT_ID] [OUT]

ESA_ADB_ROOT holds metadata/Mission1/labels.csv and raw/Mission1/channels/channel_N.zip (ESA Anomaly Dataset,
Zenodo 10.5281/zenodo.15237121, CC BY 3.0 IGO). Needs pandas. Defaults: event id_127, assets/models/esa-m1-id127.svg.
Navy = normal telemetry, crimson = the expert-labelled part; first three labelled channels, no y values (anonymised).
"""
import csv
import pathlib
import sys
import zipfile

import pandas as pd

ROOT = pathlib.Path(__file__).resolve().parents[1]
data = pathlib.Path(sys.argv[1])
meta = str(data / "metadata" / "Mission1")
ev_id = sys.argv[2] if len(sys.argv) > 2 else "id_127"
out = sys.argv[3] if len(sys.argv) > 3 else str(ROOT / "assets" / "models" / "esa-m1-id127.svg")
pad_min = 0


def load(ch):
    with zipfile.ZipFile(data / "raw" / "Mission1" / "channels" / f"{ch}.zip") as z, z.open(ch) as f:
        return pd.read_pickle(f)[ch]

rows = [r for r in csv.DictReader(open(meta + "/labels.csv")) if r["ID"] == ev_id]
chs = sorted({r["Channel"] for r in rows}, key=lambda c: int(c.split("_")[1]))[:3]
ts = lambda t: pd.Timestamp(t.replace("Z", "")).tz_localize(None)
s0 = min(ts(r["StartTime"]) for r in rows); e0 = max(ts(r["EndTime"]) for r in rows)
dur = (e0 - s0).total_seconds() / 60
pad = pd.Timedelta(minutes=max(pad_min, 2.5 * dur, 90))
w0, w1 = s0 - pad, e0 + pad
W, H = 820, 560
L, Rm, T, PH, GAP = 120, 20, 10, 128, 26
NAVY, CRIM, INK, GRID = "#20386E", "#B3202A", "#16181D", "#7C8594"
font = "font-family:'Source Serif 4';"
svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">', f'<rect width="{W}" height="{H}" fill="#FFFFFF"/>']
X = lambda t: L + (t - w0).total_seconds() / (w1 - w0).total_seconds() * (W - L - Rm)
for i, ch in enumerate(chs):
    df = load(ch)
    seg = df[(df.index >= w0) & (df.index <= w1)]
    ref = df[(df.index >= w0 - pd.Timedelta(days=7)) & (df.index < w0)]
    lo, hi = float(ref.quantile(0.005)), float(ref.quantile(0.995))
    vmin, vmax = min(lo, float(seg.min())), max(hi, float(seg.max()))
    span = (vmax - vmin) or 1.0; vmin -= 0.08 * span; vmax += 0.08 * span
    y0 = T + i * (PH + GAP)
    Y = lambda v: y0 + PH - (v - vmin) / (vmax - vmin) * PH
    for v in ():  # range lines off
        svg.append(f'<line x1="{L}" x2="{W-Rm}" y1="{Y(v):.1f}" y2="{Y(v):.1f}" stroke="{GRID}" stroke-width="2" stroke-dasharray="8 6"/>')
    svg.append(f'<line x1="{L}" x2="{L}" y1="{y0}" y2="{y0+PH}" stroke="{INK}" stroke-width="2"/>')
    lab = [(ts(r["StartTime"]), ts(r["EndTime"])) for r in rows if r["Channel"] == ch]
    inlab = lambda t: any(a <= t <= b for a, b in lab)
    pts = [(X(t), Y(float(v)), inlab(t)) for t, v in seg.items()]
    runs, cur = [], [pts[0]]
    for p in pts[1:]:
        if p[2] != cur[-1][2]: runs.append(cur); cur = [cur[-1], p]
        else: cur.append(p)
    runs.append(cur)
    for r in runs:
        d = "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y, _ in r)
        svg.append(f'<path d="{d}" fill="none" stroke="{CRIM if r[-1][2] else NAVY}" stroke-width="{3.5 if r[-1][2] else 2.5}" stroke-linejoin="round"/>')
    svg.append(f'<text x="{L-16}" y="{y0+PH/2+10}" text-anchor="end" style="{font}font-size:30px;font-weight:700" fill="{INK}">ch {ch.split("_")[1]}</text>')
ya = T + 3 * PH + 2 * GAP
svg.append(f'<line x1="{L}" x2="{W-Rm}" y1="{ya}" y2="{ya}" stroke="{INK}" stroke-width="2"/>')
step = pd.Timedelta(hours=1) if (w1 - w0) <= pd.Timedelta(hours=8) else pd.Timedelta(hours=2)
t = w0.ceil("h")
while t <= w1:
    x = X(t); svg.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{ya}" y2="{ya+8}" stroke="{INK}" stroke-width="2"/>')
    svg.append(f'<text x="{x:.1f}" y="{ya+38}" text-anchor="middle" style="{font}font-size:28px" fill="{INK}">{t:%H:%M}</text>'); t += step
xs, xe = X(s0), X(e0)
svg.append(f'<path d="M{xs:.1f},{ya+50} L{xs:.1f},{ya+62} L{xe:.1f},{ya+62} L{xe:.1f},{ya+50}" fill="none" stroke="{CRIM}" stroke-width="3"/>')
svg.append(f'<text x="{(xs+xe)/2:.1f}" y="{ya+96}" text-anchor="middle" style="{font}font-size:28px;font-weight:700" fill="{CRIM}">expert label</text>')
svg.append(f'<text x="{W-Rm}" y="{ya+96}" text-anchor="end" style="{font}font-size:28px" fill="{INK}">{s0:%Y-%m-%d} UTC</text>')
svg.append("</svg>")
pathlib.Path(out).write_text("\n".join(svg) + "\n", encoding="utf-8", newline="\n")
print(ev_id, chs, f"{dur:.0f} min", s0, "window", w0, w1)
