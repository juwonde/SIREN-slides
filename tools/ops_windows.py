"""Orbit download window and weekly NN window power on the board, as two editable SVGs (operation slide).

    python tools/ops_windows.py MISSION_JSON MISSION_TRACE ORBIT_JSON ORBIT_TRACE OUT_DIR

MISSION_* is the stitched weekly NN window (cold boot, PL load to the end of the NN window, shutdown; t from power on)
from the F-ASTRA weekly measurements, ORBIT_* one power-off orbit cycle (cold boot, download over CAN, preprocessing,
store, shutdown) from tools/orbit_cycle.py. Both are INA228 guard samples (12 V board input, 100 Hz) with JSON phases
(kind, begin_s, end_s) and a trace CSV (t_s,power_w). Both panels share one power axis, the trace is coloured by phase,
every 2 px column keeps its lowest and highest sample. Writes orbit-window.svg/.csv and weekly-window.svg/.csv (the
plotted samples).
- 10/22 (F-ASTRA 6.144.0-6.145.0, bpg-r31e final configuration, >1000 pfu week): mission-gt1000-r3boot.{json,
  -trace.csv}; orbit cycle oc3 via tools/orbit_cycle.py
- 10/21 (6.140.1, bpg-r30e): mission-gt1000-mission and the daily window daily-gt1000-v1 (replaced by the orbit window)
"""
import json
import pathlib
import sys

W, H, L, R, T, B = 800, 520, 88, 20, 50, 400
P0, P1 = 1.5, 6.0
INK, INK6 = "#16181D", "#353B47"
COL = {"boot": INK6, "idle": INK6, "shutdown": INK6, "plload": "#2F6FBF", "prep": "#2F6FBF", "model": "#2F6FBF",
       "hybrid": "#B3202A", "gruseq": "#8E5BB5", "grubatch": "#8E5BB5", "encode": "#8E5BB5", "store": "#1F6F6B", "download": "#20386E", "pre": "#1F6F6B",
       "dprep": "#1F6F6B"}
F = "font-family:'Source Serif 4';"


def panel(rows, phases, x1, ticks, labels, peak_dx):
    px = lambda t: L + t / x1 * (W - L - R)
    py = lambda w: B - (w - P0) / (P1 - P0) * (B - T)
    kind = lambda t: next((COL[p["kind"]] for p in phases if p["begin_s"] <= t <= p["end_s"]), INK6)
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">', f'<rect width="{W}" height="{H}" fill="#FFFFFF"/>']
    for w in (2, 3, 4, 5, 6):
        s.append(f'<text x="{L-14}" y="{py(w)+10:.1f}" text-anchor="end" style="{F}font-size:28px" fill="{INK}">{w}</text>')
    for t in ticks:
        s.append(f'<line x1="{px(t):.1f}" x2="{px(t):.1f}" y1="{B}" y2="{B+8}" stroke="{INK}" stroke-width="2"/>')
        s.append(f'<text x="{px(t):.1f}" y="{B+40}" text-anchor="middle" style="{F}font-size:28px" fill="{INK}">{t}</text>')
    s.append(f'<line x1="{L}" x2="{L}" y1="{T-10}" y2="{B}" stroke="{INK}" stroke-width="2"/><line x1="{L}" x2="{W-R}" y1="{B}" y2="{B}" stroke="{INK}" stroke-width="2"/>')
    cols = {}
    for t, w in rows:
        cols.setdefault(int(px(t) // 2), []).append((t, w))
    paths = {}
    for k in sorted(cols):
        c = cols[k]
        lo, hi = min(w for _, w in c), max(w for _, w in c)
        paths.setdefault(kind(c[len(c) // 2][0]), []).append(f"M{2*k+1:.0f},{py(hi):.1f} L{2*k+1:.0f},{py(lo):.1f}")
    for col, d in paths.items():
        s.append(f'<path d="{" ".join(d)}" fill="none" stroke="{col}" stroke-width="2"/>')
    for text, t, w, anchor, col in labels:  # phase names in the phase colour, above the trace
        s.append(f'<text x="{px(t):.1f}" y="{py(w):.1f}" text-anchor="{anchor}" style="{F}font-size:28px;font-weight:700" fill="{col}">{text}</text>')
    tp, wp = max(rows, key=lambda r: r[1])
    s.append(f'<circle cx="{px(tp):.1f}" cy="{py(wp):.1f}" r="9" fill="#B3202A" stroke="{INK}" stroke-width="3"/>')
    s.append(f'<text x="{px(tp)+peak_dx:.1f}" y="{py(wp)-12:.1f}" text-anchor="{"start" if peak_dx > 0 else "end"}" style="{F}font-size:28px;font-weight:700" fill="#B3202A">peak {wp:.2f} W</text>')
    s.append(f'<text x="{L-14}" y="{T-20}" text-anchor="end" style="{F}font-size:26px" fill="{INK6}">W</text>')
    s.append(f'<text x="{W-R}" y="{B+90}" text-anchor="end" style="{F}font-size:28px" fill="{INK6}">time (s)</text>')
    return "\n".join(s + ["</svg>"]) + "\n", (tp, wp)


def trace(path, a, b, t0=0.0):
    rows = [tuple(map(float, l.split(","))) for l in open(path, encoding="ascii").read().splitlines()[1:]]
    return [(t - t0, w) for t, w in rows if a <= t - t0 <= b]


def main():
    mj, mt, oj, ot, out = sys.argv[1:6]
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    m = json.load(open(mj, encoding="utf-8"))
    ph = m["phases"]; end = max(p["end_s"] for p in ph)
    get = lambda k: [p for p in ph if p["kind"] == k]
    boot, pd, off = get("boot")[0], get("hybrid")[0], get("shutdown")[0]
    comp0 = min(p["begin_s"] for p in ph if p["kind"] in ("gruseq", "grubatch", "encode")); comp1 = off["begin_s"]
    rows = trace(mt, 0.05, end)  # from 50 ms after power on (inrush sample excluded, as in F-ASTRA)
    labels = [(f"boot · {boot['seconds']:.1f} s", 1.0, 5.85, "start", INK6),
              (f"AI #2 · {get('model')[0]['seconds']:.1f} s", 1.0, 5.45, "start", COL["model"]),
              (f"AI #1 · {pd['seconds']:.1f} s", (pd["begin_s"] + pd["end_s"]) / 2 + 6, 5.85, "middle", COL["hybrid"]),
              (f"AI #3 · {comp1 - comp0:.1f} s", (comp0 + comp1) / 2, 5.85, "middle", COL["gruseq"]),
              (f"off · {off['seconds']:.1f} s", (off["begin_s"] + off["end_s"]) / 2, 5.45, "middle", INK6),
              ("spare", (end + 180) / 2, 3.3, "middle", INK6), (f"{180 - end:.1f} s", (end + 180) / 2, 2.9, "middle", INK6)]
    svg, (tp, wp) = panel(rows, ph, 180.0, (0, 30, 60, 90, 120, 150, 180), labels, -16)
    ai2 = get("model")[0]["begin_s"]
    svg = svg.replace("</svg>", f'<line x1="{L + ai2 / 180 * (W - L - R):.1f}" x2="{L + ai2 / 180 * (W - L - R):.1f}" y1="{B - (5.45 - P0) / (P1 - P0) * (B - T) + 8:.1f}" y2="{B - (4.85 - P0) / (P1 - P0) * (B - T) - 4:.1f}" stroke="{COL["model"]}" stroke-width="3"/>\n</svg>')
    (out / "weekly-window.svg").write_text(svg, encoding="utf-8", newline="\n")
    (out / "weekly-window.csv").write_text("t_s,power_W\n" + "\n".join(f"{t:.3f},{w:.4f}" for t, w in rows) + "\n", encoding="utf-8", newline="\n")
    print(f"weekly: {len(rows)} samples, power on to off {end:.1f} s, peak {wp:.4f} W at {tp:.1f} s")

    o = json.load(open(oj, encoding="utf-8"))
    oph, end = o["phases"], o["mission_s"]
    rows = trace(ot, 0.0, end)
    got = lambda k: [p for p in oph if p["kind"] == k]
    boot, off = got("boot")[0], got("shutdown")[0]
    dl = sum(p["seconds"] for p in got("download")); pre = sum(p["seconds"] for p in got("dprep") + got("store"))
    labels = [(f"boot · {boot['seconds']:.1f} s", 3.0, 4.75, "start", INK6),
              (f"download over CAN · {dl:.0f} s", 310.0, 4.35, "middle", COL["download"]),
              (f"preprocess + store · {pre:.1f} s", 630.0, 5.45, "end", COL["pre"]),
              (f"off · {off['seconds']:.1f} s", 630.0, 5.0, "end", INK6)]
    svg, (tp, wp) = panel(rows, oph, 630.0, (0, 100, 200, 300, 400, 500, 600), labels, 16)
    (out / "orbit-window.svg").write_text(svg, encoding="utf-8", newline="\n")
    (out / "orbit-window.csv").write_text("t_s,power_W\n" + "\n".join(f"{t:.3f},{w:.4f}" for t, w in rows) + "\n", encoding="utf-8", newline="\n")
    print(f"orbit: {len(rows)} samples, power on to off {end:.1f} s, mean {o['mean_w']:.4f} W, peak {wp:.4f} W at {tp:.1f} s")


if __name__ == "__main__":
    main()
