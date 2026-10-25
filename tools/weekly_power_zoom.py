"""Power inset of the selected weekly run: setup through AI #1's first 10 s.

Read-only inputs; keep each display column's minimum and maximum sample.
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
    a = ap.parse_args()
    phases = json.loads(a.record.read_text(encoding="utf-8"))["phases"]
    one = lambda k: next(p for p in phases if p["kind"] == k)
    boot, pl, pd, off = [one(k) for k in ("boot", "plload", "hybrid", "shutdown")]
    work_end = max(p["end_s"] for p in phases if p["kind"] == "encode")
    with a.trace.open() as f:
        raw = [(float(r[0]), float(r[1])) for r in list(csv.reader(f))[1:]]
    rows, _ = stitch(raw, [(0, boot["end_s"]), (pl["begin_s"], work_end), (off["begin_s"], off["end_s"])])
    shift = boot["end_s"] - pl["begin_s"]
    shown = [dict(p, begin_s=p["begin_s"]+shift, end_s=p["end_s"]+shift)
             for p in phases if p["kind"] not in ("boot", "test", "harness", "shutdown")]
    start, pd_start = boot["end_s"], pd["begin_s"]+shift
    end = pd_start+10
    focus = [(t, w) for t, w in rows if start <= t <= end]
    px = lambda t: 88+(t-start)/(end-start)*692
    py = lambda w: 145-(w-3)/2*95
    colour = lambda t: next((COL[p["kind"]] for p in shown if p["begin_s"] <= t <= p["end_s"]), "#353B47")
    cols = {}
    for t, w in focus:
        cols.setdefault(int((px(t)-88)//2), []).append((t, w))
    paths = {}
    for k, values in sorted(cols.items()):
        low, high = min(w for _, w in values), max(w for _, w in values)
        low, high = max(3, low), min(5, high)
        if low <= high:
            x = 89+2*k
            c = colour(values[len(values)//2][0])
            paths.setdefault(c, []).append(f"M{x},{py(high):.2f} V{py(low):.2f}")
    f = "font-family='Source Serif 4' font-size='28' fill='#16181D'"
    parts = ["<svg xmlns='http://www.w3.org/2000/svg' width='800' height='235' viewBox='0 0 800 235'>",
             f"<text x='88' y='24' {f} font-weight='600'>Power zoom · {start:.1f}–{end:.1f} s</text>",
             f"<text x='74' y='24' {f} text-anchor='end'>W</text>",
             "<path d='M88,40 V145 H780' fill='none' stroke='#16181D' stroke-width='2'/>"]
    for w in (3, 4, 5):
        parts.append(f"<text x='74' y='{py(w)+9:.2f}' {f} text-anchor='end'>{w}</text>")
    for t in (start, 25, 30, 35, end):
        x, anchor = px(t), "end" if t == end else "middle"
        label = f"{t:.1f}" if t in (start, end) else str(t)
        parts += [f"<path d='M{x:.2f},145 v8' stroke='#16181D' stroke-width='2'/>",
                  f"<text x='{x:.2f}' y='183' {f} text-anchor='{anchor}'>{label}</text>"]
    for c, paths_in_colour in paths.items():
        parts.append(f"<path d='{' '.join(paths_in_colour)}' fill='none' stroke='{c}' stroke-width='2'/>")
    parts.append(f"<text x='780' y='223' {f} text-anchor='end'>time (s)</text></svg>")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text("\n".join(parts), encoding="utf-8")
    receipt = {"source_run":"quiet-r32boot2", "start_s":start, "end_s":end,
               "ai1_start_s":pd_start, "ai1_seconds_shown":10,
               "display_power_min_W":3, "display_power_max_W":5,
               "sample_min_W":min(w for _, w in focus), "sample_max_W":max(w for _, w in focus),
               "samples":len(focus), "measurement":"12 V board input, INA228 100 Hz",
               "scope":"Same measured power trace as the full weekly panel; time and power axes zoomed."}
    a.out.with_suffix(".json").write_text(json.dumps(receipt, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(receipt))


if __name__ == "__main__":
    main()
