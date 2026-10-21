"""Daily window and weekly NN window power on the board, as two editable SVGs (operation slide).

    python tools/ops_windows.py MISSION_JSON MISSION_TRACE DAILY_JSON DAILY_TRACE OUT_DIR

MISSION_* is the stitched weekly NN window (cold boot, PL load to the end of the NN window, shutdown; t from power on)
and DAILY_* one daily window run (download over CAN, preprocessing, encoder pre stage), both from the F-ASTRA weekly
measurements (INA228 guard, 12 V board input, 100 Hz single samples; JSON phases with begin_s/end_s, trace CSV
t_s,power_w). Both panels share one power axis, the trace is coloured by phase, every 2 px column keeps its lowest and
highest sample. Writes daily-window.svg/.csv and weekly-window.svg/.csv (the plotted samples).
- 10/21 (F-ASTRA 6.140.1, bpg-r30e, >1000 pfu week): mission-gt1000-mission.{json,-trace.csv},
  daily-gt1000-v1.{json,-trace.csv} (first day 2013-05-21, PL deep idle)
"""
import json
import pathlib
import sys

W, H, L, R, T, B = 800, 520, 88, 20, 50, 400
P0, P1 = 1.5, 6.0
INK, INK6 = "#16181D", "#353B47"
COL = {"boot": INK6, "idle": INK6, "shutdown": INK6, "plload": "#2F6FBF", "prep": "#2F6FBF", "model": "#2F6FBF",
       "hybrid": "#B3202A", "gruseq": "#8E5BB5", "encode": "#8E5BB5", "download": "#20386E", "pre": "#1F6F6B",
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
    mj, mt, dj, dt, out = sys.argv[1:6]
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    m = json.load(open(mj, encoding="utf-8"))
    ph = m["phases"]; end = max(p["end_s"] for p in ph)
    get = lambda k: [p for p in ph if p["kind"] == k]
    boot, pd, off = get("boot")[0], get("hybrid")[0], get("shutdown")[0]
    comp0 = min(p["begin_s"] for p in ph if p["kind"] in ("gruseq", "encode")); comp1 = off["begin_s"]
    rows = trace(mt, 0.0, end)
    labels = [(f"boot · {boot['seconds']:.1f} s", 1.0, 5.85, "start", INK6),
              (f"AI #2 · {get('model')[0]['seconds']:.1f} s", 1.0, 5.45, "start", COL["model"]),
              (f"AI #1 · {pd['seconds']:.1f} s", (pd["begin_s"] + pd["end_s"]) / 2 + 6, 5.85, "middle", COL["hybrid"]),
              (f"AI #3 · {comp1 - comp0:.1f} s", (comp0 + comp1) / 2, 5.85, "middle", COL["gruseq"]),
              (f"off · {off['seconds']:.1f} s", (off["begin_s"] + off["end_s"]) / 2, 5.45, "middle", INK6),
              ("spare", (end + 180) / 2, 3.3, "middle", INK6), (f"{180 - end:.1f} s", (end + 180) / 2, 2.9, "middle", INK6)]
    svg, (tp, wp) = panel(rows, ph, 180.0, (0, 30, 60, 90, 120, 150, 180), labels, 16)
    ai2 = get("model")[0]["begin_s"]
    svg = svg.replace("</svg>", f'<line x1="{L + ai2 / 180 * (W - L - R):.1f}" x2="{L + ai2 / 180 * (W - L - R):.1f}" y1="{B - (5.45 - P0) / (P1 - P0) * (B - T) + 8:.1f}" y2="{B - (4.85 - P0) / (P1 - P0) * (B - T) - 4:.1f}" stroke="{COL["model"]}" stroke-width="3"/>\n</svg>')
    (out / "weekly-window.svg").write_text(svg, encoding="utf-8", newline="\n")
    (out / "weekly-window.csv").write_text("t_s,power_W\n" + "\n".join(f"{t:.3f},{w:.4f}" for t, w in rows) + "\n", encoding="utf-8", newline="\n")
    print(f"weekly: {len(rows)} samples, power on to off {end:.1f} s, peak {wp:.4f} W at {tp:.1f} s")

    d = json.load(open(dj, encoding="utf-8"))
    day = d["days"][0]["name"]; per = d["per_day"][day]
    dph = [dict(p, kind="dprep" if p["kind"] == "prep" else p["kind"]) for p in d["phases"] if p["name"] == day]
    t0 = min(p["begin_s"] for p in dph); dph = [dict(p, begin_s=p["begin_s"] - t0, end_s=p["end_s"] - t0) for p in dph]
    rows = trace(dt, 0.0, 330.0, t0)
    dl = next(p for p in dph if p["kind"] == "download")
    pre0 = min(p["begin_s"] for p in dph if p["kind"] in ("dprep", "pre")); pre1 = max(p["end_s"] for p in dph if p["kind"] == "pre")
    labels = [("idle", 1.5, 3.75, "start", INK6),
              (f"download (CAN) · {dl['seconds']:.0f} s", (dl["begin_s"] + dl["end_s"]) / 2, 4.4, "middle", COL["download"]),
              (f"preprocessing · {pre1 - pre0:.1f} s", 330.0, 5.0, "end", COL["pre"])]
    svg, (tp, wp) = panel(rows, dph, 330.0, (0, 60, 120, 180, 240, 300), labels, -16)
    (out / "daily-window.svg").write_text(svg, encoding="utf-8", newline="\n")
    (out / "daily-window.csv").write_text("t_s,power_W\n" + "\n".join(f"{t:.3f},{w:.4f}" for t, w in rows) + "\n", encoding="utf-8", newline="\n")
    print(f"daily {day}: {len(rows)} samples, active {per['active_s']:.1f} s mean {per['active_mean_w']:.4f} W, "
          f"10-min mean {per['window_10min_mean_w']:.4f} W, idle {per['idle_w']:.4f} W, highest sample {wp:.4f} W at {tp:.1f} s")


if __name__ == "__main__":
    main()
