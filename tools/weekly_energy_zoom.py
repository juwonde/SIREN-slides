"""Zoom the selected weekly trace from PL setup through AI #1's first 10 s.

Inputs are read-only measurement records. The cumulative energy retains its
power-on origin; only the displayed time and energy ranges change.
"""
import argparse
import csv
import json
from pathlib import Path

from ops_windows import COL
from selected_mode_figures import stitch


def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument("--record", type=Path, required=True)
    ap.add_argument("--trace", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    record = json.loads(args.record.read_text(encoding="utf-8"))
    phases = record["phases"]
    one = lambda kind: next(p for p in phases if p["kind"] == kind)
    boot, pl, pd, off = [one(k) for k in ("boot", "plload", "hybrid", "shutdown")]
    work_end = max(p["end_s"] for p in phases if p["kind"] == "encode")
    with args.trace.open() as f:
        raw = [(float(r[0]), float(r[1])) for r in list(csv.reader(f))[1:]]
    rows, _ = stitch(raw, [(0, boot["end_s"]), (pl["begin_s"], work_end), (off["begin_s"], off["end_s"])])
    shift = boot["end_s"] - pl["begin_s"]
    shown_phases = [dict(p, begin_s=p["begin_s"] + shift, end_s=p["end_s"] + shift)
                    for p in phases if p["kind"] not in ("boot", "test", "harness", "shutdown")]
    start, pd_start = boot["end_s"], pd["begin_s"] + shift
    end = pd_start + 10
    cumulative, total = [], 0.0
    for (ta, wa), (tb, wb) in zip([rows[0]] + rows[:-1], rows):
        total += (tb - ta) * (wa + wb) / 2 / 3600
        cumulative.append((tb, total))
    def at(t):
        for (ta, ea), (tb, eb) in zip(cumulative, cumulative[1:]):
            if ta <= t <= tb:
                return ea + (eb-ea) * (t-ta) / (tb-ta)
        raise ValueError(t)
    points = [(start, at(start))] + [(t, e) for t, e in cumulative if start < t < end] + [(end, at(end))]
    px = lambda t: 88 + (t-start)/(end-start)*692
    py = lambda e: 145 - (e-.015)/.03*95
    colour = lambda t: next((COL[p["kind"]] for p in shown_phases if p["begin_s"] <= t <= p["end_s"]), "#353B47")
    runs = []
    for t, e in points:
        c = colour(t)
        if not runs or runs[-1][0] != c:
            runs.append((c, [runs[-1][1][-1]] if runs else []))
        runs[-1][1].append((px(t), py(e)))
    f = "font-family='Source Serif 4' font-size='28' fill='#16181D'"
    parts = ["<svg xmlns='http://www.w3.org/2000/svg' width='800' height='235' viewBox='0 0 800 235'>"]
    parts.append(f"<text x='88' y='24' {f} font-weight='600'>Energy zoom · {start:.1f}–{end:.1f} s</text>")
    parts.append(f"<text x='74' y='24' {f} text-anchor='end'>Wh</text>")
    for e in (.02, .03, .04):
        parts.append(f"<text x='74' y='{py(e)+9:.2f}' {f} text-anchor='end'>{e:g}</text>")
    parts.append("<path d='M88,40 V145 H780' fill='none' stroke='#16181D' stroke-width='2'/>")
    for t in (start, 25, 30, 35, end):
        label = f"{t:.1f}" if t in (start, end) else str(t)
        x = px(t)
        anchor = "end" if t == end else "middle"
        parts.append(f"<path d='M{x:.2f},145 v8' stroke='#16181D' stroke-width='2'/>")
        parts.append(f"<text x='{x:.2f}' y='183' {f} text-anchor='{anchor}'>{label}</text>")
    for c, pts in runs:
        if len(pts) > 1:
            sampled = {int(x // 2): (x, y) for x, y in pts}
            pts = [pts[0], *sampled.values(), pts[-1]]
            coords = " ".join(f"{x:.2f},{y:.2f}" for x, y in pts)
            parts.append(f"<polyline points='{coords}' fill='none' stroke='{c}' stroke-width='4' stroke-linejoin='round'/>")
    parts.append(f"<text x='780' y='223' {f} text-anchor='end'>time (s)</text></svg>")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(parts), encoding="utf-8")
    receipt = {"source_run": "quiet-r32boot2", "start_s": start, "end_s": end,
               "ai1_start_s": pd_start, "ai1_seconds_shown": 10,
               "cumulative_start_Wh": at(start), "cumulative_end_Wh": at(end),
               "energy_origin": "power on, same stitched intervals as the full weekly figure",
               "measurement": "12 V board input, INA228 100 Hz"}
    args.out.with_suffix(".json").write_text(json.dumps(receipt, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(receipt))


if __name__ == "__main__":
    main()
