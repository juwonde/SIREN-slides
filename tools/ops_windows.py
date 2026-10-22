"""Orbit download window and weekly NN window power on the board, as two editable SVGs (operation slide).

    python tools/ops_windows.py MISSION_JSON MISSION_TRACE ORBIT_JSON ORBIT_TRACE OUT_DIR

MISSION_* is the stitched weekly NN window (cold boot, PL load to the end of the NN window, shutdown; t from power on)
from the F-ASTRA weekly measurements, ORBIT_* one power-off orbit cycle (cold boot, download over CAN, preprocessing,
store, shutdown) from tools/orbit_cycle.py. Both are INA228 guard samples (12 V board input, 100 Hz) with JSON phases
(kind, begin_s, end_s) and a trace CSV (t_s,power_w). Each panel has the power trace on top (one power axis for both
panels, coloured by phase, every 2 px column keeps its lowest and highest sample) and the cumulative energy below
(trapezoid over the same samples, in Wh, coloured by phase), with the energy per phase written underneath. Writes
orbit-window.svg/.csv and weekly-window.svg/.csv (the plotted samples).
- 10/22 (F-ASTRA 6.144.0-6.145.0, bpg-r31e final configuration, >1000 pfu week): mission-gt1000-r3boot.{json,
  -trace.csv}; orbit cycle oc3 via tools/orbit_cycle.py
- 10/21 (6.140.1, bpg-r30e): mission-gt1000-mission and the daily window daily-gt1000-v1 (replaced by the orbit window)
"""
import json
import pathlib
import sys

W, H, L, R = 800, 540, 88, 20
T, B, T2, B2 = 40, 250, 300, 405  # power panel, energy panel
P0, P1 = 1.5, 6.5
INK, INK6 = "#16181D", "#353B47"
COL = {"boot": INK6, "idle": INK6, "shutdown": INK6, "plload": "#2F6FBF", "prep": "#2F6FBF", "model": "#2F6FBF",
       "hybrid": "#B3202A", "gruseq": "#8E5BB5", "grubatch": "#8E5BB5", "encode": "#8E5BB5", "store": "#1F6F6B",
       "download": "#20386E", "pre": "#1F6F6B", "dprep": "#1F6F6B"}
F = "font-family:'Source Serif 4';"
py = lambda w: B - (w - P0) / (P1 - P0) * (B - T)


def energy(rows, a, b):
    """Trapezoid integral of the samples with a <= t <= b, in J."""
    seg = [r for r in rows if a <= r[0] <= b]
    return sum((t2 - t1) * (w1 + w2) / 2 for (t1, w1), (t2, w2) in zip(seg, seg[1:]))


def panel(rows, phases, x1, ticks, labels, peak_dx, e_max, e_ticks, e_label_right, footer=""):
    px = lambda t: L + t / x1 * (W - L - R)
    pe = lambda e: B2 - e / e_max * (B2 - T2)
    kind = lambda t: next((COL[p["kind"]] for p in phases if p["begin_s"] <= t <= p["end_s"]), INK6)
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">', f'<rect width="{W}" height="{H}" fill="#FFFFFF"/>']
    for w in (2, 3, 4, 5, 6):
        s.append(f'<text x="{L-14}" y="{py(w)+10:.1f}" text-anchor="end" style="{F}font-size:28px" fill="{INK}">{w}</text>')
    for e in e_ticks:
        s.append(f'<text x="{L-14}" y="{pe(e)+10:.1f}" text-anchor="end" style="{F}font-size:28px" fill="{INK}">{e:g}</text>')
    for t in ticks:
        s.append(f'<line x1="{px(t):.1f}" x2="{px(t):.1f}" y1="{B2}" y2="{B2+8}" stroke="{INK}" stroke-width="2"/>')
        s.append(f'<text x="{px(t):.1f}" y="{B2+38}" text-anchor="middle" style="{F}font-size:28px" fill="{INK}">{t}</text>')
    for top, bot in ((T, B), (T2, B2)):
        s.append(f'<line x1="{L}" x2="{L}" y1="{top-10}" y2="{bot}" stroke="{INK}" stroke-width="2"/><line x1="{L}" x2="{W-R}" y1="{bot}" y2="{bot}" stroke="{INK}" stroke-width="2"/>')
    cum, e = [], 0.0
    for (ta, wa), (tb, wb) in zip([rows[0]] + rows[:-1], rows):
        e += (tb - ta) * (wa + wb) / 2 / 3600; cum.append(e)
    cols = {}
    for (t, w), c in zip(rows, cum):
        cols.setdefault(int(px(t) // 2), []).append((t, w, c))
    paths, runs, prev = {}, [], None
    for k in sorted(cols):
        col = cols[k]
        lo, hi = min(v[1] for v in col), max(v[1] for v in col)
        colour = kind(col[len(col) // 2][0])
        paths.setdefault(colour, []).append(f"M{2*k+1:.0f},{py(hi):.1f} L{2*k+1:.0f},{py(lo):.1f}")
        pt = (2 * k + 1, pe(col[-1][2]))
        if not runs or runs[-1][0] != colour:
            runs.append((colour, [prev] if prev else []))
        runs[-1][1].append(pt); prev = pt
    for colour, d in paths.items():
        s.append(f'<path d="{" ".join(d)}" fill="none" stroke="{colour}" stroke-width="2"/>')
    for colour, pts in runs:  # cumulative energy, one segment per phase colour
        if len(pts) > 1:
            s.append('<polyline points="' + " ".join(f"{x:.1f},{y:.1f}" for x, y in pts) + f'" fill="none" stroke="{colour}" stroke-width="4" stroke-linejoin="round"/>')
    xe, ye = prev
    lx, anchor = (xe + 12, "start") if e_label_right else (xe - 12, "end")
    s.append(f'<text x="{lx:.1f}" y="{ye - 10 if not e_label_right else ye + 10:.1f}" text-anchor="{anchor}" style="{F}font-size:28px;font-weight:700" fill="{INK}">{e:.2f} Wh</text>')
    for text, t, w, anch, colour in labels:  # phase names in the phase colour, above the trace
        s.append(f'<text x="{px(t):.1f}" y="{py(w):.1f}" text-anchor="{anch}" style="{F}font-size:28px;font-weight:700" fill="{colour}">{text}</text>')
    tp, wp = max(rows, key=lambda r: r[1])
    s.append(f'<circle cx="{px(tp):.1f}" cy="{py(wp):.1f}" r="9" fill="#B3202A" stroke="{INK}" stroke-width="3"/>')
    s.append(f'<text x="{px(tp)+peak_dx:.1f}" y="{py(wp)-12:.1f}" text-anchor="{"start" if peak_dx > 0 else "end"}" style="{F}font-size:28px;font-weight:700" fill="#B3202A">peak {wp:.2f} W</text>')
    s.append(f'<text x="{L-14}" y="{T-16}" text-anchor="end" style="{F}font-size:26px" fill="{INK6}">W</text>')
    s.append(f'<text x="{L-14}" y="{T2-16}" text-anchor="end" style="{F}font-size:26px" fill="{INK6}">Wh</text>')
    s.append(f'<text x="{W-R}" y="{B2+78}" text-anchor="end" style="{F}font-size:28px" fill="{INK6}">time (s)</text>')
    if footer:  # energy per phase, integrated from the same samples
        s.append(f'<text x="{L-60}" y="{H-10}" style="{F}font-size:26px" fill="{INK6}">energy (mWh) {footer}</text>')  # Figma: label bold
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
    labels = [(f"boot · {boot['seconds']:.1f} s", 1.0, 6.3, "start", INK6),
              (f"AI #2 · {get('model')[0]['seconds']:.1f} s", 1.0, 5.6, "start", COL["model"]),
              (f"AI #1 · {pd['seconds']:.1f} s", (pd["begin_s"] + pd["end_s"]) / 2 + 6, 6.3, "middle", COL["hybrid"]),
              (f"AI #3 · {comp1 - comp0:.1f} s", (comp0 + comp1) / 2, 6.3, "middle", COL["gruseq"]),
              (f"off · {off['seconds']:.1f} s", (off["begin_s"] + off["end_s"]) / 2, 5.6, "middle", INK6),
              ("spare", (end + 180) / 2, 3.5, "middle", INK6), (f"{180 - end:.1f} s", (end + 180) / 2, 2.8, "middle", INK6)]
    mwh = lambda a, b: energy(rows, a, b) / 3.6
    ai2 = get("model")[0]
    foot = (f"boot {mwh(0, boot['end_s']):.0f} · AI #2 {mwh(ai2['begin_s'], ai2['end_s']):.1f} · AI #1 {mwh(pd['begin_s'], pd['end_s']):.0f}"
            f" · AI #3 {mwh(comp0, comp1):.0f} · off {mwh(off['begin_s'], off['end_s']):.0f}")
    svg, (tp, wp) = panel(rows, ph, 180.0, (0, 30, 60, 90, 120, 150, 180), labels, -16, 0.2, (0, 0.1, 0.2), True, foot)
    xa = L + ai2["begin_s"] / 180 * (W - L - R)
    svg = svg.replace("</svg>", f'<line x1="{xa:.1f}" x2="{xa:.1f}" y1="{py(5.6) + 8:.1f}" y2="{py(4.85) - 4:.1f}" stroke="{COL["model"]}" stroke-width="3"/>\n</svg>')
    (out / "weekly-window.svg").write_text(svg, encoding="utf-8", newline="\n")
    (out / "weekly-window.csv").write_text("t_s,power_W\n" + "\n".join(f"{t:.3f},{w:.4f}" for t, w in rows) + "\n", encoding="utf-8", newline="\n")
    print(f"weekly: {len(rows)} samples, power on to off {end:.1f} s, peak {wp:.4f} W at {tp:.1f} s, {mwh(0, end):.1f} mWh")

    o = json.load(open(oj, encoding="utf-8"))
    oph, end = o["phases"], o["mission_s"]
    rows = trace(ot, 0.0, end)
    got = lambda k: [p for p in oph if p["kind"] == k]
    boot, off = got("boot")[0], got("shutdown")[0]
    dl = sum(p["seconds"] for p in got("download")); pre = sum(p["seconds"] for p in got("dprep") + got("store"))
    labels = [(f"boot · {boot['seconds']:.1f} s", 3.0, 5.5, "start", INK6),
              (f"download over CAN · {dl:.0f} s", 310.0, 4.9, "middle", COL["download"]),
              (f"preprocess + store · {pre:.1f} s", 630.0, 6.3, "end", COL["pre"]),
              (f"off · {off['seconds']:.1f} s", 630.0, 5.6, "end", INK6)]
    mwh = lambda a, b: energy(rows, a, b) / 3.6
    span = lambda ps: sum(mwh(p["begin_s"], p["end_s"]) for p in ps)
    foot = (f"boot {mwh(0, boot['end_s']):.0f} · download {span(got('download')):.0f} · preprocess + store {span(got('dprep') + got('store')):.0f}"
            f" · off {mwh(off['begin_s'], off['end_s']):.0f}")
    svg, (tp, wp) = panel(rows, oph, 630.0, (0, 100, 200, 300, 400, 500, 600), labels, 16, 0.55, (0, 0.25, 0.5), False, foot)
    (out / "orbit-window.svg").write_text(svg, encoding="utf-8", newline="\n")
    (out / "orbit-window.csv").write_text("t_s,power_W\n" + "\n".join(f"{t:.3f},{w:.4f}" for t, w in rows) + "\n", encoding="utf-8", newline="\n")
    print(f"orbit: {len(rows)} samples, power on to off {end:.1f} s, mean {o['mean_w']:.4f} W, peak {wp:.4f} W at {tp:.1f} s, {mwh(0, end):.1f} mWh")


if __name__ == "__main__":
    main()
