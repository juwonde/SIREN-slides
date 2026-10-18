"""Chip map for the talk: where each SIREN block sits on the XC7Z030 (clean version, no arrows, no labels).

Input: ``sites.csv`` exported from the routed design (columns site, site_type, grid_x, grid_y, owner).
Output: an SVG drawn at its slide size. The fabric is cut into a grid (CELL_COLS site columns x CELL_ROWS tile
rows per cell); a cell takes the colour of the function that owns most of its sites, and stays white when at
least half of its sites are empty. Areas without sites (the ARM processor corner, the clock spine) have no grid.
The legend lives on the slide, not in the picture.

    python tools/chip_map.py SITES_CSV figures/F16-chip-map.svg
"""
import collections
import csv
import sys
from pathlib import Path

GROUPS = [  # colour, owners
    ("#2f6fbf", ["accelerator0"]),
    ("#3fa34d", ["accelerator1", "feeder1"]),
    ("#d9822b", ["engine2", "rebase2_0"]),
    ("#8e5bb5", ["accelerator3", "zip3"]),
    ("#5f6b7a", ["dma0", "dma1", "dma3", "memory_interconnect", "memory_interconnect_hp1", "memory_interconnect_hp2",
                 "control_interconnect", "control_interconnect_1"]),
    ("#c0392b", ["guard0", "guard1", "guard3", "sem", "sem_clock_converter", "sem_reset", "control0", "control1",
                 "control3", "accel_reset"]),
]
EMPTY, GRID, PS_FILL = "#ffffff", "#b4bcc8", "#e9ecf1"
CELL_COLS, CELL_ROWS = 2, 5
CELL_W, CELL_H = 9.0, 12.0
PAD = 2


def main():
    src, out = sys.argv[1], Path(sys.argv[2])
    sites = [s for s in csv.DictReader(open(src, encoding="utf-8")) if not s["site_type"].startswith("IOB")]
    colour = {o: c for c, owners in GROUPS for o in owners}
    cols = {x: k for k, x in enumerate(sorted({int(s["grid_x"]) for s in sites}))}
    y0 = min(int(s["grid_y"]) for s in sites)
    cells = collections.defaultdict(list)
    for s in sites:
        key = (cols[int(s["grid_x"])] // CELL_COLS, (int(s["grid_y"]) - y0) // CELL_ROWS)
        cells[key].append(colour.get(s["owner"], EMPTY) if s["owner"] else EMPTY)
    nx = max(k[0] for k in cells) + 1
    ny = max(k[1] for k in cells) + 1
    width, height = nx * CELL_W + 2 * PAD, ny * CELL_H + 2 * PAD
    parts, empty_cells = [], 0
    # the ARM processor (PS) corner: rows above the first site of the leftmost cell column, columns left of the
    # first cell that has sites in those rows
    first_row = min(r for (c, r) in cells if c == 0)
    first_col = min(c for (c, r) in cells if r < first_row)
    parts.append(f'<rect x="{PAD}" y="{PAD}" width="{first_col * CELL_W:.1f}" height="{first_row * CELL_H:.1f}" '
                 f'fill="{PS_FILL}"/>')
    for (cx, cy), owners in sorted(cells.items()):
        used = [o for o in owners if o != EMPTY]
        if len(used) * 2 <= len(owners):
            fill = EMPTY
            empty_cells += 1
        else:
            fill = collections.Counter(used).most_common(1)[0][0]
        parts.append(f'<rect x="{PAD + cx * CELL_W:.1f}" y="{PAD + cy * CELL_H:.1f}" width="{CELL_W:.1f}" '
                     f'height="{CELL_H:.1f}" fill="{fill}" stroke="{GRID}" stroke-width="0.8"/>')
    out.write_text(f'<svg xmlns="http://www.w3.org/2000/svg" width="{width:.1f}" height="{height:.1f}" '
                   f'viewBox="0 0 {width:.1f} {height:.1f}">\n  ' + "\n  ".join(parts) + "\n</svg>\n",
                   encoding="utf-8", newline="\n")
    empty_sites = sum(1 for s in sites if not s["owner"])
    print(dict(cells=len(cells), grid=(nx, ny), empty_cells=empty_cells, empty_sites=empty_sites, sites=len(sites),
               size=(round(width), round(height))))


if __name__ == "__main__":
    main()
