"""Figma variants of the figures: Figma deck palette, Source Serif 4 text, no tinted fills, no orange.

    python tools/figma_assets.py [--preview]

Writes build/figma/ (ignored by git):
- svg/<name>.svg       every SVG figure, inline attributes only, ready for figma.createNodeFromSvg
                       (Mermaid figures: computed styles inlined, arrowheads as polygons instead of markers)
- F16-chip-map@3x.png  placement grid at 3x (too many cells for a vector import)
- routing-r27e.png     Vivado routing picture (adopted bitstream bpg-r27e) with the block colours of the deck
- preview/<name>.png   with --preview: each SVG rendered by Chrome, for a quick visual check

Block colours (chip map, architecture, routing, legend) are chosen to read on white and on the black
Vivado background (contrast >= 3:1 on both): engine #b3202a, guards/SEM #2e7d7a; the others are unchanged.
Needs Chrome (see build.py) and build/vendor/mermaid.min.js (see render_mermaid.py). Standard library only.
"""
import colorsys
import html
import json
import pathlib
import re
import shutil
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from build import BUILD, ROOT, chrome  # noqa: E402
import render_mermaid  # noqa: E402

FIG = ROOT / "figures"
OUT = BUILD / "figma"
FONT = "Source Serif 4"
ENGINE, GUARD = "#b3202a", "#2e7d7a"

# make_figures palette -> Figma palette (lower-case hex)
COLOURS = {
    "#1a1a1a": "#16181d",  # ink
    "#4a4f5c": "#353b47",  # secondary text and arrows (11.2:1)
    "#1b2a57": "#20386e",  # navy (headings)
    "#1f5fbf": "#4a64a0",  # key blue -> navy 500
    "#9c1b1f": "#84161a",  # crimson
    "#c25e00": ENGINE,     # orange (engine, pending) -> crimson red
    "#d9822b": ENGINE,
    "#c0392b": GUARD,
    "#1a7f43": "#2e7d4f",  # green (achieved)
    "#c9cfdb": "#7c8594",  # faint lines -> 3:1 line grey
    "#9aa3b2": "#7c8594",
    "#b4bcc8": "#7c8594",
    # tinted fills -> white (shapes keep their outline)
    "#e3ecfa": "#ffffff", "#e6f2ea": "#ffffff", "#f3f5f9": "#ffffff", "#fbe9d7": "#ffffff",
    "#e6e9ef": "#ffffff", "#f7f8fb": "#ffffff", "#eef6f0": "#ffffff",
}
# fills that carry meaning without an outline get a real colour instead of white
SPECIAL = {
    "F03-radiation-environment": {"#eef3fb": "#ffffff", "#7fa7d9": "#4a64a0", "#f7c948": "#84161a"},
    "F14-anomaly-illustration": {"#f8d7d7": "#ffffff"},
}
MERMAID_CLASSES = {  # Mermaid classDef colours for the Figma variant
    "#e3ecfa,stroke:#1b2a57,color:#1b2a57": "#ffffff,stroke:#20386e,color:#20386e",
    "#ffffff,stroke:#9aa3b2,color:#1a1a1a": "#ffffff,stroke:#7c8594,color:#16181d",
    "#fbe9d7,stroke:#c25e00,color:#1a1a1a": "#ffffff,stroke:#b3202a,color:#b3202a",
    "#d9822b,stroke:#a8601a,color:#ffffff": "#b3202a,stroke:#7a1418,color:#ffffff",
    "#c0392b,stroke:#8e2a20,color:#ffffff": "#2e7d7a,stroke:#1f5755,color:#ffffff",
    "#eef6f0,stroke:#1a7f43,color:#1a1a1a": "#ffffff,stroke:#20386e,color:#16181d",
}


def recolour(text, extra=None):
    table = dict(COLOURS, **(extra or {}))
    return re.sub(r"#[0-9a-fA-F]{6}\b", lambda m: table.get(m.group(0).lower(), m.group(0).lower()), text)


def plain_svg(path):
    """make_figures SVG: swap font and colours."""
    text = path.read_text(encoding="utf-8")
    text = re.sub(r'font-family="[^"]*"', f'font-family="{FONT}"', text)
    return recolour(text, SPECIAL.get(path.stem))


FLATTEN_JS = r"""
const host = document.getElementById('host');
host.innerHTML = SVG_TEXT;
const svg = host.querySelector('svg');
const NS = 'http://www.w3.org/2000/svg';
const keep = ['fill', 'stroke', 'stroke-width', 'stroke-dasharray', 'opacity', 'fill-opacity', 'stroke-opacity',
              'stroke-linecap', 'stroke-linejoin'];
const textKeep = ['font-size', 'font-weight', 'font-style', 'text-anchor', 'dominant-baseline'];
const heads = [], plan = [];
// pass 1: read every computed style while all classes are still in place
for (const el of Array.from(svg.querySelectorAll('*'))) {
  const tag = el.tagName.toLowerCase();
  if (['style', 'defs', 'marker', 'title', 'desc'].includes(tag)) continue;
  if (el.closest('defs') || el.closest('marker')) continue;
  const cs = getComputedStyle(el);
  if (cs.display === 'none' || cs.visibility === 'hidden') { plan.push([el, null]); continue; }
  const attrs = {};
  const shape = ['path', 'rect', 'circle', 'ellipse', 'polygon', 'line', 'polyline'].includes(tag);
  if (shape) {
    for (const p of keep) {
      let v = cs.getPropertyValue(p);
      if (!v || v === 'none' && p === 'stroke-dasharray') continue;
      if (p === 'stroke-width' && v === '0px') v = '0';
      attrs[p] = v.replace(/px$/, '');
    }
    for (const end of ['marker-end', 'marker-start']) {
      if (el.getAttribute(end) || (cs.getPropertyValue(end) && cs.getPropertyValue(end) !== 'none')) {
        if (tag === 'path') heads.push([el, end === 'marker-end', cs.getPropertyValue('stroke')]);
        attrs[end] = null;
      }
    }
  }
  if (tag === 'text' || tag === 'tspan') {
    for (const p of textKeep) {
      const v = cs.getPropertyValue(p);
      if (v && v !== 'auto' && v !== 'normal' || p === 'font-weight') attrs[p] = v.replace(/px$/, '');
    }
    attrs['fill'] = cs.getPropertyValue('fill');
    attrs['font-family'] = 'FONT_NAME';
  }
  plan.push([el, attrs]);
}
// pass 2: write the attributes, then drop classes and inline styles
for (const [el, attrs] of plan) {
  if (attrs === null) { el.remove(); continue; }
  for (const [k, v] of Object.entries(attrs)) { if (v === null) el.removeAttribute(k); else el.setAttribute(k, v); }
}
for (const [el, attrs] of plan) { if (attrs) { el.removeAttribute('style'); el.removeAttribute('class'); } }
for (const el of Array.from(svg.querySelectorAll('g'))) { el.removeAttribute('style'); el.removeAttribute('class'); }
for (const [el, atEnd, colour] of heads) {
  const L = el.getTotalLength();
  if (L < 2) continue;
  const a = el.getPointAtLength(atEnd ? L : 0), b = el.getPointAtLength(atEnd ? L - 2 : 2);
  const dx = a.x - b.x, dy = a.y - b.y, n = Math.hypot(dx, dy) || 1, ux = dx / n, uy = dy / n;
  const size = 11, tipx = a.x + ux * 3, tipy = a.y + uy * 3;
  const bx = tipx - ux * size, by = tipy - uy * size;
  const pts = [[tipx, tipy], [bx - uy * size * 0.55, by + ux * size * 0.55], [bx + uy * size * 0.55, by - ux * size * 0.55]];
  // the arrowhead lives in the same coordinate system as the path
  const poly = document.createElementNS(NS, 'polygon');
  poly.setAttribute('points', pts.map(p => p[0].toFixed(1) + ',' + p[1].toFixed(1)).join(' '));
  poly.setAttribute('fill', colour);
  el.parentNode.insertBefore(poly, el.nextSibling);
}
// Figma misplaces nested tspans: one plain <text> per line, positioned in root user space
const rootInv = svg.getScreenCTM().inverse();
for (const tx of Array.from(svg.querySelectorAll('text'))) {
  const outer = Array.from(tx.children).filter(c => c.tagName.toLowerCase() === 'tspan');
  const lines = outer.length ? outer : [tx];
  const m = rootInv.multiply(tx.getScreenCTM());
  const cs = getComputedStyle(tx);
  const anchor = cs.getPropertyValue('text-anchor') || 'start';
  for (const ln of lines) {
    const content = ln.textContent.replace(/\s+/g, ' ').trim();
    if (!content || !ln.getNumberOfChars()) continue;
    const p0 = ln.getStartPositionOfChar(0), len = ln.getComputedTextLength();
    const lx = anchor === 'middle' ? p0.x + len / 2 : anchor === 'end' ? p0.x + len : p0.x;
    const X = m.a * lx + m.c * p0.y + m.e, Y = m.b * lx + m.d * p0.y + m.f;
    const nt = document.createElementNS(NS, 'text');
    nt.setAttribute('x', X.toFixed(1)); nt.setAttribute('y', Y.toFixed(1));
    for (const a of ['fill', 'font-size', 'font-weight', 'font-style', 'font-family']) { const v = tx.getAttribute(a) || ln.getAttribute(a); if (v) nt.setAttribute(a, v); }
    nt.setAttribute('text-anchor', anchor);
    nt.textContent = content;
    svg.appendChild(nt);
  }
  tx.remove();
}
for (const el of Array.from(svg.querySelectorAll('style, defs, marker, title, desc'))) el.remove();
for (const el of Array.from(svg.querySelectorAll('*'))) {
  for (const a of Array.from(el.attributes)) if (a.name.startsWith('aria-') || a.name.startsWith('data-') || a.name === 'id') el.removeAttribute(a.name);
}
svg.removeAttribute('aria-roledescription'); svg.removeAttribute('role'); svg.removeAttribute('style'); svg.removeAttribute('class');
document.getElementById('out').textContent = svg.outerHTML;
"""


def mermaid_svg(src, profile):
    """Render one Mermaid source with the Figma palette and return a flattened SVG."""
    text = src.read_text(encoding="utf-8")
    for a, b in MERMAID_CLASSES.items():
        text = text.replace(a, b)
    m = re.search(r"%%\s*px=(\d+)", text)
    px = int(m.group(1)) if m else 20
    cfg = render_mermaid.config(px)
    # lay out with Cambria (installed, similar width); the output names the Figma font
    cfg["themeVariables"].update({
        "primaryColor": "#ffffff", "primaryTextColor": "#16181d", "primaryBorderColor": "#20386e",
        "lineColor": "#4a4f5c", "secondaryColor": "#ffffff", "clusterBkg": "#ffffff", "clusterBorder": "#7c8594",
        "titleColor": "#20386e"})
    page = BUILD / f"figma-mmd-{src.stem}.html"
    flatten = FLATTEN_JS.replace("FONT_NAME", FONT)
    page.write_text(
        "<!doctype html><meta charset='utf-8'><body><div id='host'></div><pre id='out'></pre>"
        "<script src='vendor/mermaid.min.js'></script><script>"
        f"mermaid.initialize({json.dumps(cfg)});"
        f"mermaid.render({json.dumps('f-' + src.stem.split('-')[0])}, {json.dumps(text)})"
        ".then(r => { const SVG_TEXT = r.svg;" + flatten + "})"
        ".catch(e => { document.getElementById('out').textContent = 'ERROR ' + e; });"
        "</script></body>", encoding="utf-8")
    run = chrome(["--virtual-time-budget=10000", "--dump-dom", page.as_uri()], profile)
    found = re.search(r'<pre id="out">(.*?)</pre>', run.stdout, re.S)
    if not found or not found.group(1).strip() or found.group(1).startswith("ERROR"):
        sys.exit(f"{src.stem}: render failed: {found.group(1)[:300] if found else run.stderr[-500:]}")
    svg = html.unescape(found.group(1))
    vb = re.search(r'viewBox="([-\d.]+) ([-\d.]+) ([\d.]+) ([\d.]+)"', svg)
    w, h = float(vb.group(3)), float(vb.group(4))
    svg = re.sub(r'<svg([^>]*?) width="100%"', rf'<svg\1 width="{w:.1f}" height="{h:.1f}"', svg, count=1)
    svg = re.sub(r"(\d+\.\d{2})\d+", r"\1", svg)  # shorter numbers for the Figma import
    return svg


def chip_map(profile):
    text = (FIG / "F16-chip-map.svg").read_text(encoding="utf-8")
    text = recolour(text, {"#b4bcc8": "#7c8594"})
    # ARM processor corner: white with a diagonal hatch (no tinted fill)
    hatch = ('<defs><pattern id="arm" width="8" height="8" patternUnits="userSpaceOnUse" '
             'patternTransform="rotate(45)"><line x1="0" y1="0" x2="0" y2="8" stroke="#7c8594" stroke-width="2"/>'
             '</pattern></defs>')
    text = text.replace('fill="#e9ecf1"/>', 'fill="url(#arm)" stroke="#4a4f5c" stroke-width="1.2"/>', 1)
    text = re.sub(r"(<svg[^>]*>)", r"\1" + hatch, text, count=1)
    path = OUT / "svg" / "F16-chip-map.svg"
    path.write_text(text, encoding="utf-8", newline="\n")
    w, h = (float(v) for v in re.search(r'width="([\d.]+)" height="([\d.]+)"', text).groups())
    raster(path, OUT / "F16-chip-map@3x.png", w, h, 3, profile)


def raster(svg_path, png_path, w, h, scale, profile):
    page = BUILD / f"figma-raster-{svg_path.stem}.html"
    page.write_text("<!doctype html><meta charset='utf-8'><style>html,body{margin:0;background:#fff}</style>"
                    f"<img src='{svg_path.as_uri()}' width='{w:.1f}' height='{h:.1f}'>", encoding="utf-8")
    chrome([f"--screenshot={png_path}", f"--window-size={int(w + 0.99)},{int(h + 0.99)}",
            f"--force-device-scale-factor={scale}", "--hide-scrollbars", "--default-background-color=FFFFFFFF",
            page.as_uri()], profile)


def read_png(path):
    data = path.read_bytes()
    w, h, depth, ctype = struct.unpack(">IIBB", data[16:26])
    if depth != 8 or ctype not in (2, 6):
        sys.exit(f"{path.name}: only 8-bit RGB/RGBA PNG supported")
    chunks, pos = [], 8
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if kind == b"IDAT":
            chunks.append(data[pos + 8:pos + 8 + n])
        pos += 12 + n
    raw, bpp = zlib.decompress(b"".join(chunks)), 3 if ctype == 2 else 4
    stride, rows, prev = w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, bpp, rows


def write_png(path, w, h, bpp, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
    head = struct.pack(">IIBBBBB", w, h, 8, 2 if bpp == 3 else 6, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(raw, 9))
                     + chunk(b"IEND", b""))


ROUTING = "routing-r27e.png"  # Vivado routing capture of the adopted bitstream (bpg-r27e)


def routing():
    """Vivado routing picture: orange engine -> ENGINE, red guards -> GUARD (pixels within a small distance)."""
    src = ROOT / "assets" / "hw" / ROUTING
    w, h, bpp, rows = read_png(src)
    swaps = [(tuple(int(c[i:i + 2], 16) for i in (1, 3, 5)), tuple(int(n[i:i + 2], 16) for i in (1, 3, 5)))
             for c, n in (("#d9822b", ENGINE), ("#c0392b", GUARD))]
    engine = swaps[0][1]
    changed = 0
    for line in rows:
        for x in range(0, w * bpp, bpp):
            px = (line[x], line[x + 1], line[x + 2])
            for old, new in swaps:
                if abs(px[0] - old[0]) + abs(px[1] - old[1]) + abs(px[2] - old[2]) <= 36:
                    line[x:x + 3] = bytes(new)
                    changed += 1
                    break
            else:
                # anti-aliased engine wires: any remaining orange (hue 12-32 deg) -> engine colour, same brightness
                hue, sat, val = colorsys.rgb_to_hsv(*(v / 255 for v in px))
                if 12 / 360 <= hue <= 32 / 360 and sat > 0.4 and val > 0.45:
                    k = val / (max(engine) / 255)
                    line[x:x + 3] = bytes(min(255, int(c * k)) for c in engine)
                    changed += 1
    write_png(OUT / ROUTING, w, h, bpp, rows)
    print(f"routing: {changed} pixels recoloured")


def main():
    preview = "--preview" in sys.argv
    (OUT / "svg").mkdir(parents=True, exist_ok=True)
    profile = tempfile.mkdtemp(prefix="siren-figma-")
    try:
        mermaid = {p.stem for p in (FIG / "src").glob("*.mmd")}
        for path in sorted(FIG.glob("F*.svg")):
            if path.stem == "F16-chip-map":
                continue
            svg = mermaid_svg(FIG / "src" / f"{path.stem}.mmd", profile) if path.stem in mermaid else plain_svg(path)
            out = OUT / "svg" / path.name
            out.write_text(svg, encoding="utf-8", newline="\n")
            print(f"{path.stem}: {len(svg) / 1024:.1f} KiB")
            if preview:
                (OUT / "preview").mkdir(exist_ok=True)
                w, h = (float(v) for v in re.search(r'<svg[^>]*?width="([\d.]+)"[^>]*?height="([\d.]+)"', svg).groups())
                raster(out, OUT / "preview" / f"{path.stem}.png", w, h, 1, profile)
        chip_map(profile)
        routing()
    finally:
        shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    main()
