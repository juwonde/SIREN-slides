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
    def __init__(self, name, w, h):
        self.name, self.w, self.h, self.parts = name, w, h, []

    def text(self, x, y, s, size=22, weight=400, color=INK, anchor="middle", italic=False):
        if size < MIN_SIZE:
            raise ValueError(f"{self.name}: font size {size} < {MIN_SIZE}")
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


def f01():
    s = Svg("F01-onboard-ai", 470, 470)
    s.box(35, 0, 400, 92, [("Raw data", 22, 700), ("CREST counts · bus HK", 22)], fill=PANEL)
    s.arrow(235, 92, 235, 132)
    s.box(35, 132, 400, 92, [("SIREN onboard inference", 22, 700, NAVY), ("3 DNNs on one FPGA SoC", 22)],
          fill=FINN_FILL, stroke=NAVY)
    for i, (t, c) in enumerate([("Risk", "score"), ("Anomaly", "flag"), ("Compressed", "packet")]):
        x = 5 + i * 158
        s.arrow(235, 224, x + 72, 264)
        s.box(x, 264, 144, 92, [(t, 22, 700, K), (c, 22)], fill="#ffffff", stroke=K)
    s.arrow(235, 356, 235, 396)
    s.box(35, 396, 400, 70, [("Downlink: information, not raw data", 22, 700)], fill=PANEL)
    return s


def f02():
    s = Svg("F02-siren-in-padosat", 560, 460)
    s.box(0, 0, 170, 80, [("CREST", 22, 700), ("detector", 22)])
    s.box(195, 0, 170, 80, [("Bus", 22, 700), ("HK", 22)])
    s.box(390, 0, 170, 80, [("S-band", 22, 700), ("SRS-4", 22)])
    s.box(0, 140, 365, 70, [("OBC (A3200)", 22, 700)], fill="#ffffff", stroke=NAVY)
    s.arrow(85, 80, 85, 138)
    s.arrow(280, 80, 280, 138)
    s.box(190, 260, 370, 196, [], fill=FINN_FILL, stroke=NAVY, stroke_w=3)
    s.text(375, 290, "SIREN APU · Z7000-P3", 22, 700, NAVY)
    for i, (t, c) in enumerate([("PD", "risk detection"), ("BUS", "anomaly detection"), ("GRU", "compression")]):
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
    s = Svg("F03-radiation-environment", 560, 430)
    cx, cy = 340, 220
    for dx in (-1, 1):
        s.raw(f'<ellipse cx="{cx + dx * 130}" cy="{cy}" rx="58" ry="108" fill="#f6d5c3" stroke="{TODO}" stroke-width="2"/>')
        s.raw(f'<ellipse cx="{cx + dx * 64}" cy="{cy}" rx="20" ry="44" fill="#d6e2f5" stroke="{K}" stroke-width="2"/>')
    s.raw(f'<circle cx="{cx}" cy="{cy}" r="34" fill="#7fa7d9" stroke="{NAVY}" stroke-width="2"/>')
    s.raw(f'<circle cx="{cx}" cy="{cy}" r="41" fill="none" stroke="{RED}" stroke-width="3" stroke-dasharray="6 5"/>')
    s.text(cx, cy + 7, "Earth", 20, 700, "#ffffff")
    s.text(cx + 130, 70, "Outer belt", 22, 700, TODO)
    s.text(cx + 130, 94, "electrons", 20, 400, INK)
    s.line(cx - 70, cy + 30, cx - 150, 372, color=K, width=2)
    s.text(cx - 150, 396, "Inner belt", 22, 700, K)
    s.text(cx - 150, 420, "protons", 20, 400, INK)
    s.line(cx + 30, cy + 30, cx + 70, 372, color=RED, width=2)
    s.text(cx + 70, 396, "LEO orbit", 22, 700, RED)
    s.text(cx + 70, 420, "CubeSat", 20, 400, INK)
    s.raw(f'<circle cx="40" cy="40" r="26" fill="#f7c948" stroke="{TODO}" stroke-width="2"/>')
    s.text(40, 92, "Sun", 20, 700, INK)
    s.arrow(72, 46, 186, 112, color=TODO)
    s.text(8, 136, "SEP protons", 20, 700, TODO, "start")
    s.text(8, 160, "minutes", 20, 400, INK, "start")
    s.text(8, 184, "to hours", 20, 400, INK, "start")
    return s


def f04a():
    s = Svg("F04a-moderntcn-block", 548, 196)
    labels = [("DW Conv", "time"), ("ConvFFN1", "features"), ("ConvFFN2", "variables")]
    for i, (t, sub) in enumerate(labels):
        x = 40 + i * 168
        s.box(x, 40, 140, 62, [(t, 22, 700, NAVY)], fill=FINN_FILL, stroke=NAVY)
        s.text(x + 70, 128, sub, 20, 400, MUTED)
        if i < 2:
            s.arrow(x + 140, 71, x + 168, 71)
    s.arrow(0, 71, 40, 71)
    s.arrow(516, 71, 548, 71)
    s.line(20, 71, 20, 150)
    s.line(20, 150, 530, 150)
    s.arrow(530, 150, 530, 85)
    s.text(275, 182, "residual path", 20, 400, MUTED)
    s.raw(f'<circle cx="530" cy="71" r="12" fill="#ffffff" stroke="{MUTED}" stroke-width="3"/>')
    s.text(530, 79, "+", 22, 700, MUTED)
    s.text(0, 24, "ModernTCN block", 20, 700, INK, "start")
    return s


def f04b():
    s = Svg("F04b-predictive-coding", 548, 190)
    s.text(0, 24, "Predictive coding (lossless)", 20, 700, INK, "start")
    s.box(0, 40, 92, 56, [("x(t)", 22, 700)])
    s.arrow(92, 68, 116, 68)
    s.box(116, 40, 92, 56, [("GRU", 22, 700, NAVY)], fill=FINN_FILL, stroke=NAVY)
    s.arrow(208, 68, 232, 68)
    s.box(232, 40, 120, 56, [("x - x̂", 22, 700)])
    s.arrow(352, 68, 376, 68)
    s.box(376, 40, 172, 56, [("zigzag · BZ2", 22, 700, K)], fill="#ffffff", stroke=K)
    s.arrow(462, 96, 462, 126)
    s.box(260, 126, 288, 56, [("ground: same GRU", 22, 700), ], fill=PS_FILL, stroke=OK)
    s.text(130, 160, "exact x(t) restored", 20, 700, OK)
    s.arrow(256, 154, 236, 154, color=OK)
    return s


def f05():
    s = Svg("F05-hardware-styles", 548, 330)
    s.text(0, 24, "Dataflow: one block per layer", 22, 700, NAVY, "start")
    for i in range(3):
        x = 20 + i * 180
        s.box(x, 40, 140, 64, [(f"Layer {i + 1}", 22, 700)], fill=FINN_FILL, stroke=NAVY)
        if i < 2:
            s.arrow(x + 140, 72, x + 180, 72)
    s.text(274, 136, "streams in parallel · PE x SIMD folding", 20, 400, MUTED)
    s.line(0, 160, 548, 160, color=LINE, width=2)
    s.text(0, 196, "Shared engine: one MAC array", 22, 700, TODO, "start")
    s.box(20, 212, 170, 100, [("MAC array", 22, 700), ("+ buffers", 22)], fill=ENGINE_FILL, stroke=TODO)
    s.arrow(190, 262, 236, 262)
    s.arrow(236, 280, 190, 280)
    s.box(236, 230, 80, 64, [("DDR", 22, 700)])
    for i in range(3):
        x = 340 + i * 69
        s.box(x, 236, 66, 52, [(f"L{i + 1}", 22, 700)], fill="#ffffff", stroke=TODO, rx=4)
    s.arrow(340, 306, 548, 306, color=MUTED)
    s.text(444, 228, "time-multiplexed", 20, 400, MUTED)
    return s


def f06():
    s = Svg("F06-siren-unit", 560, 430)
    s.box(130, 0, 300, 300, [], fill="#ffffff", stroke=NAVY, stroke_w=3)
    s.text(280, 32, "Zynq-7000 XC7Z030", 22, 700, NAVY)
    s.box(150, 50, 260, 90, [("PS: 2x Cortex-A9", 22, 700), ("Linux · FM services", 22)], fill=PS_FILL, stroke=OK)
    s.arrow(250, 140, 250, 168)
    s.arrow(310, 168, 310, 140)
    s.text(370, 160, "AXI", 20, 400, MUTED)
    s.box(150, 168, 260, 112, [("PL: FPGA fabric", 22, 700), ("PD · BUS · GRU", 22, 700, K), ("accelerators + guards", 22)],
          fill=FINN_FILL, stroke=NAVY)
    s.box(0, 320, 170, 100, [("DDR3L", 22, 700), ("1 GiB", 22)])
    s.box(195, 320, 170, 100, [("eMMC", 22, 700), ("64 GiB", 22)])
    s.box(390, 320, 170, 100, [("PDU (P60)", 22, 700), ("5 V supply", 22)])
    s.line(85, 320, 85, 300, width=2)
    s.line(85, 300, 130, 300, width=2)
    s.line(280, 320, 280, 300, width=2)
    s.line(475, 320, 475, 300, width=2)
    s.line(475, 300, 430, 300, width=2)
    s.box(0, 40, 110, 90, [("OBC", 22, 700), ("A3200", 22)])
    s.arrow(110, 85, 148, 85)
    s.arrow(148, 100, 110, 100)
    s.text(55, 156, "CAN", 20, 700, MUTED)
    s.box(450, 40, 110, 90, [("S-band", 22, 700), ("SRS-4", 22)])
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
    s.box(200, 150, 200, 100, [("DDR", 24, 700), ("activations · weights", 20)])
    s.box(0, 290, 260, 104, [("FINN streams", 22, 700, NAVY), ("LayerNorm x 7", 22), ("8-bit · 16-bit", 20, 400, MUTED)],
          fill=FINN_FILL, stroke=NAVY)
    s.box(340, 290, 260, 104, [("Shared engine", 22, 700, TODO), ("pw · dw conv · head", 22), ("8-bit W · 16-bit A", 20, 400, MUTED)],
          fill=ENGINE_FILL, stroke=TODO)
    s.arrow(150, 288, 230, 252)
    s.arrow(250, 252, 170, 288)
    s.arrow(450, 288, 370, 252)
    s.arrow(350, 252, 430, 288)
    s.arrow(300, 72, 300, 148)
    s.text(310, 118, "DMA", 20, 400, MUTED, "start")
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


def f11():
    s = Svg("F11-floorplan-placeholder", 470, 400)
    s.box(0, 0, 470, 400, [("Placement map", 24, 700, MUTED), ("of the shared bitstream", 22, 400, MUTED),
                           ("(SVG from placement data,", 20, 400, TODO), ("next draft)", 20, 400, TODO)],
          fill="#ffffff", stroke=LINE, dash=True)
    return s


def f12():
    s = Svg("F12-digital-twin", 1168, 270)
    s.text(0, 26, "Digital Twin I · CREST data", 24, 700, NAVY, "start")
    row1 = [("VERB-3D-style", "flux field"), ("PADO-SAT orbit", "sampling"), ("CREST", "response"),
            ("Count", "time series"), ("PD · GRU", "retrain · fine-tune")]
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


C_BUS, C_LN, C_ENG, C_GRU, C_MOVE, C_SAFE = "#2f6fbf", "#3fa34d", "#d9822b", "#8e5bb5", "#5f6b7a", "#c0392b"
WHITE = "#ffffff"


def f04c():
    s = Svg("F04c-gru-cell", 548, 210)
    s.text(0, 20, "GRU cell", 20, 700, INK, "start")
    s.box(0, 40, 90, 50, [("x(t)", 22, 700)])
    s.box(0, 118, 90, 50, [("h(t-1)", 22, 700)])
    s.line(90, 65, 120, 65)
    s.line(90, 143, 120, 143)
    s.line(120, 50, 120, 170)
    for i, t in enumerate(["reset r · σ", "update z · σ", "cand. h̃ · tanh"]):
        y = 30 + i * 56
        s.arrow(120, y + 23, 148, y + 23)
        s.box(150, y, 200, 46, [(t, 20, 700, NAVY)], fill=FINN_FILL, stroke=NAVY)
    for y, ty in ((53, 96), (109, 109), (165, 122)):
        s.arrow(350, y, 418, ty, width=2)
    s.box(420, 84, 56, 50, [("mix", 20, 700)])
    s.arrow(476, 109, 496, 109)
    s.box(496, 84, 52, 50, [("h(t)", 20, 700, K)], fill="#ffffff", stroke=K)
    s.line(522, 134, 522, 200, color=K, width=2)
    s.line(522, 200, 45, 200, color=K, width=2)
    s.arrow(45, 200, 45, 170, color=K, width=2)
    return s


def f13():
    s = Svg("F13-hw-architecture", 1168, 460)
    s.box(0, 0, 220, 150, [("PS · ARM x 2", 22, 700), ("Linux runner", 20), ("FM service", 20)], fill=PS_FILL, stroke=OK)
    s.box(0, 280, 220, 120, [("DDR3", 22, 700), ("inputs · weights", 20), ("intermediate data", 20)])
    s.line(110, 150, 110, 280, width=3)
    s.raw(f'<rect x="250" y="0" width="918" height="460" rx="12" fill="none" stroke="{NAVY}" stroke-width="2" stroke-dasharray="10 6"/>')
    s.text(1150, 30, "PL · XC7Z030 @ 150 MHz", 22, 700, NAVY, "end")
    s.arrow(220, 50, 318, 50)
    s.text(232, 40, "control", 20, 400, MUTED, "start")
    s.raw(f'<rect x="275" y="70" width="26" height="340" rx="4" fill="{C_MOVE}"/>')
    s.text(262, 444, "DDR ports", 20, 700, C_MOVE, "start")
    s.arrow(220, 340, 273, 340)
    s.arrow(273, 356, 220, 356)
    lanes = [(100, "DMA"), (215, "DMA"), (330, "DMA")]
    for y, t in lanes:
        s.arrow(301, y, 318, y)
        s.box(320, y - 28, 90, 56, [(t, 20, 700, WHITE)], fill=C_MOVE, stroke=C_MOVE)
        s.arrow(410, y, 428, y)
        s.box(430, y - 28, 100, 56, [("guard", 20, 700, WHITE)], fill=C_SAFE, stroke=C_SAFE)
        s.arrow(530, y, 553, y)
    s.box(555, 68, 330, 64, [("BUS · FINN accelerator", 22, 700, WHITE), ("8-bit streaming", 20, 400, WHITE)],
          fill=C_BUS, stroke=C_BUS)
    s.box(905, 68, 245, 64, [("SEM", 22, 700, WHITE), ("config. scrubbing", 20, 400, WHITE)], fill=C_SAFE, stroke=C_SAFE)
    s.box(555, 183, 200, 64, [("PD · FINN", 22, 700, WHITE), ("LayerNorm", 20, 400, WHITE)], fill=C_LN, stroke=C_LN)
    s.box(900, 160, 250, 210, [("Shared engine", 22, 700, WHITE), ("256 MAC lanes", 20, 400, WHITE),
                               ("2x clock", 20, 400, WHITE), ("conv · GELU", 20, 400, WHITE),
                               ("residual add", 20, 400, WHITE), ("GRU gate sums", 20, 400, WHITE)],
          fill=C_ENG, stroke=C_ENG)
    s.arrow(757, 222, 898, 222, color=C_ENG, dash=True)
    s.arrow(898, 208, 757, 208, color=C_ENG, dash=True)
    s.text(828, 196, "via DDR", 20, 400, MUTED)
    s.box(555, 298, 130, 64, [("Aligner", 22, 700, WHITE)], fill=C_GRU, stroke=C_GRU)
    s.arrow(685, 330, 703, 330)
    s.box(705, 298, 175, 64, [("GRU tail", 22, 700, WHITE), ("FINN", 20, 400, WHITE)], fill=C_GRU, stroke=C_GRU)
    s.line(960, 370, 960, 400, color=C_ENG)
    s.line(960, 400, 620, 400, color=C_ENG)
    s.arrow(620, 400, 620, 364, color=C_ENG)
    s.text(790, 428, "gate sums", 20, 400, MUTED)
    return s


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


ALL = (f01, f02, f03, f04a, f04b, f04c, f05, f06, f07, f08, f09, f10, f11, f12, f13, f14, f15)


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
