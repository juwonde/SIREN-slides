"""Trade-off scatter (GOPS vs peak power) of every measured board run, as an editable SVG (trade-off slide, left).

    python tools/tradeoff_scatter.py POINTS_JSON ADOPTED_RUN OUT_SVG

POINTS_JSON comes from tools/tradeoff_data.py (one row per run and model: model, gops, peak_w, bitstream, run).
ADOPTED_RUN is the run label whose three points are drawn large with a black outline. AI #1 circles, AI #2 squares,
AI #3 diamonds; each model's Pareto front (highest GOPS reached at or below each power) is a line in its colour.
Dashed crimson lines mark 50 GOPS and 5.5 W. The legend sits in the top 30 px, so the plot keeps the slide's old
geometry when the SVG is placed 30 px higher. No grid lines.
"""
import json
import sys

W, H, TOP = 900, 620, 30
L, R, B = 96, 892, 467 + TOP
X0, X1, Y0, PXG = 4.25, 7.75, 30.0, 9.55
COL = {"AI #1": "#B3202A", "AI #2": "#2F6FBF", "AI #3": "#8E5BB5"}
INK, INK6, RED = "#16181D", "#353B47", "#B3202A"
F = "font-family:'Source Serif 4';"
px = lambda w: L + (w - X0) / (X1 - X0) * (R - L)
py = lambda g: B - (g - Y0) * PXG


def marker(model, x, y, big=False):
    c, k = COL[model], (1.9 if big else 1.0)
    st = f' stroke="{INK}" stroke-width="3"' if big else ""
    if model == "AI #1":
        return f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{6.5 * k:.1f}" fill="{c}"{st}/>'
    if model == "AI #2":
        h = 5.5 * k
        return f'<rect x="{x - h:.1f}" y="{y - h:.1f}" width="{2 * h:.1f}" height="{2 * h:.1f}" fill="{c}"{st}/>'
    h = 7.5 * k
    return f'<polygon points="{x:.1f},{y - h:.1f} {x + h:.1f},{y:.1f} {x:.1f},{y + h:.1f} {x - h:.1f},{y:.1f}" fill="{c}"{st}/>'


def main():
    rows, adopted, out = json.load(open(sys.argv[1], encoding="utf-8")), sys.argv[2], sys.argv[3]
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">', f'<rect width="{W}" height="{H}" fill="#FFFFFF"/>']
    for g in (30, 40, 50, 60, 70):
        s.append(f'<text x="70" y="{py(g) + 10:.1f}" text-anchor="end" style="{F}font-size:28px" fill="{INK}">{g}</text>')
    for w in (4.5, 5.0, 5.5, 6.0, 6.5, 7.0, 7.5):
        s.append(f'<line x1="{px(w):.1f}" x2="{px(w):.1f}" y1="{B}" y2="{B + 8}" stroke="{INK}" stroke-width="2"/>')
        s.append(f'<text x="{px(w):.1f}" y="{B + 40}" text-anchor="middle" style="{F}font-size:28px" fill="{INK}">{w:.1f}</text>')
    s.append(f'<line x1="{L}" x2="{L}" y1="{py(75):.1f}" y2="{B}" stroke="{INK}" stroke-width="2"/><line x1="{L}" x2="{R}" y1="{B}" y2="{B}" stroke="{INK}" stroke-width="2"/>')
    s.append(f'<line x1="{L}" x2="{R}" y1="{py(50):.1f}" y2="{py(50):.1f}" stroke="{RED}" stroke-width="2.5" stroke-dasharray="10 7"/>')
    s.append(f'<line x1="{px(5.5):.1f}" x2="{px(5.5):.1f}" y1="{py(75):.1f}" y2="{B}" stroke="{RED}" stroke-width="2.5" stroke-dasharray="10 7"/>')
    s.append(f'<text x="{R}" y="{py(50) + 34:.1f}" text-anchor="end" style="{F}font-size:26px;font-weight:700" fill="{RED}">≥ 50 GOPS</text>')
    s.append(f'<text x="{px(5.5) + 10:.1f}" y="{B - 14}" style="{F}font-size:26px;font-weight:700" fill="{RED}">≤ 5.5 W</text>')
    for model in ("AI #2", "AI #3", "AI #1"):
        for r in rows:
            if r["model"] == model:
                s.append(marker(model, px(r["peak_w"]), py(r["gops"])))
    for model in ("AI #1", "AI #2", "AI #3"):
        front, best = [], -1.0
        for r in sorted((r for r in rows if r["model"] == model), key=lambda r: (r["peak_w"], -r["gops"])):
            if r["gops"] > best + 1e-6:
                front.append(r); best = r["gops"]
        if len(front) > 1:
            s.append('<polyline points="' + " ".join(f"{px(r['peak_w']):.1f},{py(r['gops']):.1f}" for r in front) + f'" fill="none" stroke="{COL[model]}" stroke-width="2.5" stroke-linejoin="round"/>')
    for r in rows:
        if r["run"] == adopted:
            s.append(marker(r["model"], px(r["peak_w"]), py(r["gops"]), big=True))
    lx = 230
    for model in ("AI #1", "AI #2", "AI #3"):  # legend
        s.append(marker(model, lx - 14, 20))
        s.append(f'<text x="{lx}" y="30" style="{F}font-size:28px;font-weight:700" fill="{COL[model]}">{model}</text>')
        lx += 122
    s.append(f'<circle cx="{lx - 10}" cy="20" r="10" fill="#FFFFFF" stroke="{INK}" stroke-width="3"/><text x="{lx + 8}" y="30" style="{F}font-size:28px" fill="{INK}">adopted</text>')
    s.append(f'<text x="6" y="30" style="{F}font-size:26px" fill="{INK6}">GOPS</text>')
    s.append(f'<text x="{(L + R) / 2:.0f}" y="{B + 82}" text-anchor="middle" style="{F}font-size:28px" fill="{INK6}">peak power (W): highest 100 Hz sample of each run</text>')
    open(out, "w", encoding="utf-8", newline="\n").write("\n".join(s + ["</svg>"]) + "\n")
    print(len(rows), "points,", len({r["run"] for r in rows}), "runs,", len({r["bitstream"] for r in rows}), "bitstreams; adopted",
          [(r["model"], r["gops"], r["peak_w"]) for r in rows if r["run"] == adopted])


if __name__ == "__main__":
    main()
