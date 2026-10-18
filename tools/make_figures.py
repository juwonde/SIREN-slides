"""Generate the first-draft SVG figures (F01-F12) into figures/.

After the first run the SVG files are the source of truth (edited in Figma).
Existing files are kept; pass --force to overwrite them.
Every SVG is drawn at its placed size (1 user unit = 1 CSS px on the 1280x720 slide),
so a font-size of 20 or more stays at 15 pt or more on the slide.
Arrowheads are polygons (no markers) and styles are inline attributes, for Figma import.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "figures"
FONT = "Cambria, Georgia, Times New Roman, serif"
NAVY, RED, INK, MUTED, LINE, PANEL = "#1b2a57", "#9c1b1f", "#1a1a1a", "#4a4f5c", "#c9cfdb", "#f3f5f9"
K, OK, TODO = "#1f5fbf", "#1a7f43", "#c25e00"
FINN_FILL, ENGINE_FILL, PS_FILL = "#e3ecfa", "#fbe9d7", "#e6f2ea"
MIN_SIZE = 20
WARN = []


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Svg:
    def __init__(self, name, w, h, min_size=MIN_SIZE):
        self.name, self.w, self.h, self.parts, self.min_size = name, w, h, [], min_size

    def text(self, x, y, s, size=22, weight=400, color=INK, anchor="middle", italic=False):
        if size < self.min_size:
            raise ValueError(f"{self.name}: font size {size} < {self.min_size}")
        style = ' font-style="italic"' if italic else ""
        self.parts.append(
            f'<text x="{x:g}" y="{y:g}" font-family="{FONT}" font-size="{size}" font-weight="{weight}" '
            f'fill="{color}" text-anchor="{anchor}"{style}>{esc(s)}</text>')

    def box(self, x, y, w, h, lines, fill=PANEL, stroke=LINE, rx=10, size=22, dash=False, stroke_w=2):
        d = ' stroke-dasharray="8 6"' if dash else ""
        self.parts.append(f'<rect x="{x:g}" y="{y:g}" width="{w:g}" height="{h:g}" rx="{rx}" fill="{fill}" '
                          f'stroke="{stroke}" stroke-width="{stroke_w}"{d}/>')
        lines = [ln if isinstance(ln, tuple) else (ln,) for ln in lines]
        sizes = [ln[1] if len(ln) > 1 else size for ln in lines]
        total = sum(s * 1.2 for s in sizes)
        cy = y + (h - total) / 2
        for ln, s in zip(lines, sizes):
            weight = ln[2] if len(ln) > 2 else 400
            color = ln[3] if len(ln) > 3 else INK
            cy += s * 1.2
            self.text(x + w / 2, cy - s * 0.28, ln[0], s, weight, color)
            est = len(ln[0]) * s * (0.6 if weight >= 700 else 0.55)
            if est > w - 12:
                WARN.append(f"{self.name}: '{ln[0]}' ~{est:.0f}px in box {w}px")

    def line(self, x1, y1, x2, y2, color=MUTED, width=3, dash=False):
        d = ' stroke-dasharray="8 6"' if dash else ""
        self.parts.append(f'<line x1="{x1:g}" y1="{y1:g}" x2="{x2:g}" y2="{y2:g}" stroke="{color}" '
                          f'stroke-width="{width}"{d}/>')

    def arrow(self, x1, y1, x2, y2, color=MUTED, width=3, dash=False, head=12):
        import math
        a = math.atan2(y2 - y1, x2 - x1)
        bx, by = x2 - head * math.cos(a), y2 - head * math.sin(a)
        self.line(x1, y1, bx, by, color, width, dash)
        p1 = (bx + head * 0.55 * math.sin(a), by - head * 0.55 * math.cos(a))
        p2 = (bx - head * 0.55 * math.sin(a), by + head * 0.55 * math.cos(a))
        self.parts.append(f'<polygon points="{x2:g},{y2:g} {p1[0]:.1f},{p1[1]:.1f} {p2[0]:.1f},{p2[1]:.1f}" '
                          f'fill="{color}"/>')

    def raw(self, s):
        self.parts.append(s)

    def save(self, force):
        path = OUT / f"{self.name}.svg"
        if path.exists() and not force:
            print("keep", path.name)
            return
        body = "\n  ".join(self.parts)
        path.write_text(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
            f'viewBox="0 0 {self.w} {self.h}">\n  {body}\n</svg>\n', encoding="utf-8", newline="\n")
        print("write", path.name)




def f02():
    s = Svg("F02-siren-in-padosat", 560, 460)
    s.box(0, 0, 170, 80, [("CREST", 22, 700), ("detector", 22)])
    s.box(195, 0, 170, 80, [("Bus", 22, 700), ("HK", 22)])
    s.box(390, 0, 170, 80, [("S-band", 22, 700), ("transceiver", 22)])
    s.box(0, 140, 365, 70, [("On-board computer (OBC)", 22, 700)], fill="#ffffff", stroke=NAVY)
    s.arrow(85, 80, 85, 138)
    s.arrow(280, 80, 280, 138)
    s.box(190, 260, 370, 196, [], fill=FINN_FILL, stroke=NAVY, stroke_w=3)
    s.text(375, 290, "SIREN AI Processing Unit", 22, 700, NAVY)
    for i, (t, c) in enumerate([("PD", "hazard detection"), ("BUS", "anomaly detection"), ("COMP", "compression")]):
        y = 306 + i * 48
        s.box(206, y, 338, 40, [], fill="#ffffff", stroke=K, rx=6)
        s.text(222, y + 28, t, 20, 700, K, "start")
        s.text(290, y + 28, c, 20, 400, INK, "start")
    s.arrow(250, 212, 250, 258)
    s.arrow(320, 258, 320, 212)
    s.text(330, 242, "CAN", 20, 700, MUTED, "start")
    s.arrow(475, 258, 475, 82)
    s.text(485, 176, "RS-422", 20, 700, MUTED, "start")
    s.box(0, 260, 165, 196, [("SIREN data", 20, 700), ("logged and", 20), ("downlinked", 20), ("only", 20)], fill=PANEL)
    return s


def f03():
    import math
    s = Svg("F03-radiation-environment", 560, 446)
    cx, cy, re_px = 320, 215, 16           # Earth centre, Earth radius in px (not to scale beyond this)
    # magnetopause: compressed on the dayside (left), stretched into the tail (right)
    s.raw(f'<path d="M 560 52 C 330 40 170 110 170 {cy} C 170 320 330 390 560 378" fill="#eef3fb" '
          f'stroke="{NAVY}" stroke-width="3"/>')
    # dipole field lines r = L cos^2(lat), dayside and nightside
    for L in (2.0, 3.2, 4.4):
        for side in (-1, 1):
            pts = []
            for k in range(0, 61):
                lat = -math.radians(70) + k * math.radians(140) / 60
                r = L * math.cos(lat) ** 2 * re_px
                pts.append(f"{cx + side * r * math.cos(lat):.1f},{cy - r * math.sin(lat):.1f}")
            s.raw(f'<polyline points="{" ".join(pts)}" fill="none" stroke="{K}" stroke-width="1.6" opacity="0.8"/>')
    # open tail field lines on the night side
    for dy in (-1, 1):
        for k, off in enumerate((44, 70, 96)):
            y_end = cy + dy * off
            s.raw(f'<path d="M {cx + 4} {cy + dy * (re_px - 2)} C {cx + 60} {cy + dy * (off + 30)} {cx + 140} {y_end} '
                  f'550 {y_end}" fill="none" stroke="{K}" stroke-width="1.6" opacity="0.8"/>')
    s.raw(f'<circle cx="{cx}" cy="{cy}" r="{re_px}" fill="#7fa7d9" stroke="{NAVY}" stroke-width="2"/>')
    s.raw(f'<circle cx="{cx}" cy="{cy}" r="{re_px + 3}" fill="none" stroke="{RED}" stroke-width="2.5" stroke-dasharray="4 3"/>')
    # Sun, solar wind and SEP protons
    s.raw(f'<circle cx="34" cy="{cy}" r="30" fill="#f7c948" stroke="{TODO}" stroke-width="2"/>')
    s.text(34, cy + 54, "Sun", 20, 700, INK)
    for y in (cy - 70, cy, cy + 70):
        s.arrow(76, y, 150, y, color=TODO, width=2)
    s.text(8, 40, "Solar wind", 20, 700, TODO, "start")
    s.text(8, 64, "SEP protons", 20, 700, TODO, "start")
    s.text(540, 40, "Magnetopause", 20, 700, NAVY, "end")
    s.text(540, 406, "Magnetotail", 20, 700, NAVY, "end")
    s.line(cx + 10, cy + 17, 300, 392, color=RED, width=2)
    s.text(300, 414, "PADO-SAT orbit", 20, 700, RED)
    s.text(300, 438, "500 km SSO (assumed)", 20, 400, RED)
    s.text(8, 438, "not to scale", 20, 400, MUTED, "start")
    return s








def f06():
    s = Svg("F06-siren-unit", 560, 430)
    s.box(130, 0, 300, 300, [], fill="#ffffff", stroke=NAVY, stroke_w=3)
    s.text(280, 32, "Zynq-7000 XC7Z030", 22, 700, NAVY)
    s.box(150, 50, 260, 90, [("PS: 2x Cortex-A9", 22, 700), ("Linux · FM services", 22)], fill=PS_FILL, stroke=OK)
    s.arrow(250, 140, 250, 168)
    s.arrow(310, 168, 310, 140)
    s.text(370, 160, "AXI", 20, 400, MUTED)
    s.box(150, 168, 260, 112, [("PL: FPGA fabric", 22, 700), ("PD · BUS · COMP", 22, 700, K), ("accelerators + guards", 22)],
          fill=FINN_FILL, stroke=NAVY)
    s.box(0, 320, 170, 100, [("DDR3L", 22, 700), ("1 GiB", 22)])
    s.box(195, 320, 170, 100, [("eMMC", 22, 700), ("64 GiB", 22)])
    s.box(390, 320, 170, 100, [("Power unit", 22, 700), ("5 V supply", 22)])
    s.line(85, 320, 85, 300, width=2)
    s.line(85, 300, 130, 300, width=2)
    s.line(280, 320, 280, 300, width=2)
    s.line(475, 320, 475, 300, width=2)
    s.line(475, 300, 430, 300, width=2)
    s.box(0, 40, 110, 90, [("OBC", 22, 700)])
    s.arrow(110, 85, 148, 85)
    s.arrow(148, 100, 110, 100)
    s.text(55, 156, "CAN", 20, 700, MUTED)
    s.box(450, 40, 110, 90, [("S-band", 22, 700)])
    s.arrow(412, 85, 448, 85)
    s.text(505, 156, "RS-422", 20, 700, MUTED)
    return s


def f07():
    s = Svg("F07-build-flow", 1168, 250)
    steps = [
        ("1  Train", "float model · GPU"),
        ("2  Quantize (QAT)", "8-bit W · 8/16-bit A"),
        ("3  Integer reference", "bit-exact oracle"),
        ("4  Generate hardware", "FINN + shared engine"),
        ("5  One bitstream", "3 models + FM guards"),
        ("6  Verify on board", "byte-exact · GOPS"),
    ]
    for i, (t, sub) in enumerate(steps):
        row, col = divmod(i, 3)
        x, y = col * 400, row * 135
        fill = FINN_FILL if i in (3, 4) else PANEL
        s.box(x, y, 340, 105, [(t, 24, 700, NAVY), (sub, 22)], fill=fill, stroke=NAVY if i in (3, 4) else LINE)
        if col < 2:
            s.arrow(x + 340, y + 52, x + 398, y + 52)
    s.line(970, 105, 970, 120)
    s.line(970, 120, 170, 120)
    s.arrow(170, 120, 170, 133)
    return s


def f08():
    s = Svg("F08-pd-hybrid", 600, 400)
    s.box(0, 0, 600, 70, [("ARM (PS): schedules segments, checks outputs", 22, 700)], fill=PS_FILL, stroke=OK)
    s.box(0, 150, 180, 140, [("FINN streams", 22, 700, NAVY), ("LayerNorm x 7", 22), ("8-bit · 16-bit", 20, 400, MUTED)],
          fill=FINN_FILL, stroke=NAVY)
    s.box(230, 170, 140, 100, [("DDR", 24, 700), ("activations", 20)])
    s.box(420, 150, 180, 140, [("Shared engine", 22, 700, TODO), ("pointwise conv", 20), ("depthwise conv", 20),
                               ("risk head", 20)], fill=ENGINE_FILL, stroke=TODO)
    s.arrow(182, 205, 228, 205)
    s.arrow(228, 235, 182, 235)
    s.arrow(372, 205, 418, 205)
    s.arrow(418, 235, 372, 235)
    s.arrow(300, 72, 300, 168)
    s.text(310, 126, "DMA", 20, 400, MUTED, "start")
    s.box(0, 320, 600, 74, [("Per block (x 3): LayerNorm → depthwise conv", 20),
                            ("→ LayerNorm → FFN, all through DDR", 20)], fill=PANEL)
    return s


def f09():
    s = Svg("F09-bus-dataflow", 600, 390)
    s.box(0, 0, 600, 70, [("ARM (PS): input window, anomaly score", 22, 700)], fill=PS_FILL, stroke=OK)
    names = ["Block 1", "Block 2", "Block 3", "Head"]
    for i, n in enumerate(names):
        x = i * 155
        s.box(x, 160, 135, 90, [(n, 22, 700, NAVY), ("FINN", 20, 400, MUTED)], fill=FINN_FILL, stroke=NAVY)
        if i < 3:
            s.arrow(x + 135, 205, x + 155, 205)
    s.arrow(30, 72, 30, 158)
    s.arrow(570, 158, 570, 72)
    s.text(40, 104, "DMA in", 20, 400, MUTED, "start")
    s.text(560, 104, "DMA out", 20, 400, MUTED, "end")
    s.text(300, 140, "streaming · 2 MACs per DSP slice", 20, 400, MUTED)
    for i in range(3):
        s.arrow(i * 155 + 67, 314, i * 155 + 67, 252, color=K, dash=True)
    s.box(0, 314, 445, 74, [("MAC weights reload from ARM", 22, 700, K), ("AXI-Lite · no new bitstream", 20)],
          fill="#ffffff", stroke=K, dash=True)
    return s


def f10():
    s = Svg("F10-gru-path", 600, 400)
    s.box(0, 0, 600, 90, [("ARM (PS): h(t) feedback · residual", 22, 700), ("zigzag · BZ2 packet", 22)], fill=PS_FILL, stroke=OK)
    s.box(0, 170, 180, 104, [("Shared engine", 22, 700, TODO), ("gate matrix", 22), ("products", 22)],
          fill=ENGINE_FILL, stroke=TODO)
    s.box(225, 190, 130, 64, [("Aligner", 22, 700)])
    s.box(400, 150, 200, 144, [("FINN tail", 22, 700, NAVY), ("GRU cell", 22), ("output layer", 22), ("prediction", 22)],
          fill=FINN_FILL, stroke=NAVY)
    s.arrow(180, 222, 223, 222)
    s.arrow(355, 222, 398, 222)
    s.arrow(90, 92, 90, 168)
    s.text(100, 140, "x(t), h(t-1)", 20, 400, MUTED, "start")
    s.arrow(500, 148, 500, 92)
    s.text(490, 125, "h(t), x̂(t+1)", 20, 400, MUTED, "end")
    s.box(0, 320, 600, 74, [("Packet decoded on ground with the same", 22), ("integer predictor → exact counts", 22, 700, OK)],
          fill=PANEL)
    return s




def f12():
    s = Svg("F12-digital-twin", 1168, 270)
    s.text(0, 26, "Digital Twin I · CREST data", 24, 700, NAVY, "start")
    row1 = [("Flux-field", "based model"), ("PADO-SAT orbit", "sampling"), ("CREST", "response"),
            ("Count", "time series"), ("PD · COMP", "retrain · fine-tune")]
    for i, (a, b) in enumerate(row1):
        x = i * 236
        s.box(x, 42, 200, 84, [(a, 22, 700), (b, 22)], fill=FINN_FILL if i == 4 else PANEL,
              stroke=NAVY if i == 4 else LINE)
        if i < 4:
            s.arrow(x + 200, 84, x + 234, 84)
    s.text(0, 176, "Digital Twin II · bus HK data", 24, 700, NAVY, "start")
    s.box(0, 192, 436, 72, [("Bus HK generation", 22, 700), ("method under study", 22, 400, TODO)],
          fill=PANEL, stroke=TODO, dash=True)
    s.arrow(436, 228, 470, 228)
    s.box(472, 192, 200, 72, [("BUS", 22, 700), ("retrain · fine-tune", 22)], fill=FINN_FILL, stroke=NAVY)
    return s


def f17():
    s = Svg("F17-ground-vs-onboard", 560, 400)
    s.text(0, 24, "Ground-only processing", 22, 700, INK, "start")
    steps = ["Observe", "Store", "Wait for pass", "Downlink raw", "Analyse"]
    for i, t in enumerate(steps):
        x = i * 116
        s.box(x, 40, 96, 76, [(w, 20, 700 if i == 0 else 400) for w in t.split(" ", 1)], fill=PANEL)
        if i < 4:
            s.arrow(x + 96, 78, x + 114, 78)
    s.arrow(0, 140, 556, 140, color=TODO)
    s.text(556, 170, "hazards known hours later", 20, 700, TODO, "end")
    s.line(0, 200, 560, 200, color=LINE, width=2)
    s.text(0, 238, "Onboard AI (SIREN)", 22, 700, K, "start")
    steps2 = ["Observe", "Infer on board", "Send results"]
    for i, t in enumerate(steps2):
        x = i * 116
        s.box(x, 254, 96, 76, [(w, 20, 700 if i == 0 else 400) for w in t.split(" ", 1)],
              fill=FINN_FILL if i == 1 else PANEL, stroke=NAVY if i == 1 else LINE)
        if i < 2:
            s.arrow(x + 96, 292, x + 114, 292)
    s.arrow(0, 354, 324, 354, color=OK)
    s.text(324, 384, "minutes by design", 20, 700, OK, "end")
    return s


C_BUS, C_LN, C_ENG, C_GRU, C_MOVE, C_SAFE = "#2f6fbf", "#3fa34d", "#d9822b", "#8e5bb5", "#5f6b7a", "#c0392b"
WHITE = "#ffffff"






def _trace(s, x0, x1, ybase, amp, fn, color=INK, width=2.5):
    pts = []
    n = 94
    for i in range(n + 1):
        x = x0 + (x1 - x0) * i / n
        pts.append(f"{x:.1f},{ybase - amp * fn(i / n):.1f}")
    s.raw(f'<polyline points="{" ".join(pts)}" fill="none" stroke="{color}" stroke-width="{width}"/>')


def f14():
    import math
    s = Svg("F14-anomaly-illustration", 548, 350)
    x0, x1, w0, w1 = 80, 540, 300, 390
    s.raw(f'<rect x="{w0}" y="34" width="{w1 - w0}" height="240" fill="#f8d7d7"/>')

    def win(t):
        return 1.0 if (w0 - x0) / (x1 - x0) <= t <= (w1 - x0) / (x1 - x0) else 0.0
    rows = [("ch A", lambda t: 0.25 + 0.08 * math.sin(41 * t) + 0.35 * win(t)),
            ("ch B", lambda t: 0.3 + 0.1 * math.sin(23 * t + 1) + 0.4 * win(t) * abs(math.sin(60 * t))),
            ("ch C", lambda t: 0.3 + 0.06 * math.sin(17 * t) - 0.3 * win(t))]
    for i, (name, fn) in enumerate(rows):
        yb = 92 + i * 80
        s.text(0, yb - 20, name, 20, 700, INK, "start")
        _trace(s, x0, x1, yb, 60, fn)
    s.line(x0, 40, x1, 40, color=TODO, width=2, dash=True)
    s.text(x0, 22, "fixed limit: not crossed", 20, 400, TODO, "start")
    s.text(w0 + (w1 - w0) / 2, 298, "labelled anomaly", 20, 700, RED)
    s.text(0, 338, "score", 20, 700, K, "start")
    _trace(s, x0, x1, 342, 30, lambda t: 0.1 + 0.9 * win(t), color=K, width=3)
    return s


def f15():
    import math
    s = Svg("F15-compression-illustration", 548, 230)
    x0, x1 = 10, 540

    def sig(t):
        return 0.5 + 0.3 * math.sin(6 * t) + 0.08 * math.sin(37 * t)

    def pred(t):
        return 0.5 + 0.3 * math.sin(6 * t) + 0.08 * math.sin(37 * t - 0.25)
    _trace(s, x0, x1, 130, 110, sig, color=NAVY, width=3)
    _trace(s, x0, x1, 130, 110, pred, color=TODO, width=2.5)
    s.text(x0, 22, "x(t) original", 20, 700, NAVY, "start")
    s.text(x1, 22, "x̂(t) GRU prediction", 20, 700, TODO, "end")
    s.line(x0, 190, x1, 190, color=LINE, width=2)
    for i in range(48):
        t = i / 47
        r = (sig(t) - pred(t)) * 140
        x = x0 + (x1 - x0) * t
        s.raw(f'<rect x="{x - 3:.1f}" y="{min(190, 190 - r):.1f}" width="6" height="{abs(r) + 0.5:.1f}" fill="{K}"/>')
    s.text(x1, 226, "residual e = x - x̂: small integers", 20, 700, K, "end")
    return s


def f18():
    s = Svg("F18-gpu-finn-engine", 600, 446)
    s.text(0, 22, "GPU · temporal architecture (SIMT)", 22, 700, INK, "start")
    s.box(0, 36, 150, 70, [("Shared SMs", 22, 700)], fill="#e6e9ef", stroke=MUTED)
    for i in range(3):
        s.box(180 + i * 125, 36, 115, 70, [(f"Layer {i + 1}", 20, 700)], fill="#e6e9ef", stroke=MUTED, rx=4)
    s.arrow(180, 124, 556, 124)
    s.text(556, 148, "layers take turns", 20, 400, MUTED, "end")
    s.text(0, 188, "FINN · streaming architecture", 22, 700, NAVY, "start")
    for i in range(3):
        x = i * 210
        s.box(x, 202, 170, 80, [(f"Layer {i + 1}", 22, 700, NAVY), ("PE x SIMD", 20)], fill=FINN_FILL, stroke=NAVY)
        if i < 2:
            s.arrow(x + 170, 242, x + 208, 242)
    s.text(590, 304, "all layers busy at once", 20, 400, MUTED, "end")
    s.text(0, 344, "Vitis AI DPU · single computation engine", 22, 700, TODO, "start")
    s.box(0, 358, 170, 76, [("MAC array", 22, 700), ("+ buffers", 20)], fill=ENGINE_FILL, stroke=TODO)
    s.arrow(170, 386, 214, 386)
    s.arrow(214, 406, 170, 406)
    s.box(216, 366, 84, 60, [("DDR", 22, 700)])
    for i in range(3):
        s.box(330 + i * 88, 366, 80, 60, [(f"L{i + 1}", 22, 700)], fill="#ffffff", stroke=TODO, rx=4)
    s.arrow(330, 440, 592, 440, color=MUTED, width=2)
    return s


def f19():
    s = Svg("F19-quantization", 548, 300)
    s.text(0, 24, "x ≈ s · q,  q = round(x / s)", 22, 700, INK, "start")
    s.line(20, 90, 528, 90, color=INK, width=2)
    for k in range(9):
        x = 20 + k * 63.5
        s.line(x, 80, x, 100, color=INK, width=2)
    s.text(20, 126, "-128", 20, 400, MUTED)
    s.text(274, 126, "0", 20, 400, MUTED)
    s.text(528, 126, "127", 20, 400, MUTED)
    for fx in (60, 175, 300, 420, 495):
        tick = 20 + round((fx - 20) / 63.5) * 63.5
        s.raw(f'<circle cx="{fx}" cy="48" r="7" fill="{K}"/>')
        s.arrow(fx, 56, tick, 86, color=K, width=2, head=9)
    s.text(548, 24, "float → int8", 20, 700, K, "end")
    s.text(0, 176, "Integer MAC pipeline", 20, 700, INK, "start")
    steps = [("int8 × int8", ""), ("int32 sum", ""), ("× scale", ""), ("int8 / int16", "")]
    for i, (a, _) in enumerate(steps):
        x = i * 142
        s.box(x, 192, 122, 64, [(a, 20, 700)], fill=FINN_FILL if i < 2 else PANEL, stroke=NAVY if i < 2 else LINE)
        if i < 3:
            s.arrow(x + 122, 224, x + 140, 224)
    s.text(0, 292, "exact on the FPGA → bit-exact reference", 20, 400, MUTED, "start")
    return s


# F04a, F04b, F04c and F13 come from Mermaid sources (figures/src, tools/render_mermaid.py)
ALL = (f02, f03, f06, f07, f08, f09, f10, f12, f14, f15, f17, f18, f19)


def main():
    force = "--force" in sys.argv
    only = None
    for a in sys.argv[1:]:
        if a.startswith("--only="):
            only = {x.strip() for x in a.split("=", 1)[1].split(",")}
    OUT.mkdir(exist_ok=True)
    for make in ALL:
        fig = make()
        if only is not None:
            if fig.name.split("-")[0] in only:
                fig.save(True)
        else:
            fig.save(force)
    for w in WARN:
        print("WARN", w)


if __name__ == "__main__":
    main()
