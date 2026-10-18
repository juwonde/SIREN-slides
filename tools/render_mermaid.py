"""Render Mermaid sources (figures/src/*.mmd) to SVG figures with headless Chrome.

    python tools/render_mermaid.py                # all sources
    python tools/render_mermaid.py F13 F04a       # selected figures (file name prefix)

Each source starts with a comment line ``%% px=18`` (font size in px, default 20).
Labels are SVG text (htmlLabels off) so the SVG stays editable in Figma.
The SVG gets fixed width/height equal to its viewBox, so the font size on the slide is the configured one.

Needs build/vendor/mermaid.min.js (mermaid 11.4.1, ignored by git). Fetch it once with:
    curl -o build/vendor/mermaid.min.js https://cdn.jsdelivr.net/npm/mermaid@11.4.1/dist/mermaid.min.js
"""
import html
import json
import pathlib
import re
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from build import BUILD, ROOT, chrome  # noqa: E402

SRC = ROOT / "figures" / "src"
OUT = ROOT / "figures"
VENDOR = BUILD / "vendor" / "mermaid.min.js"
FONT = "Cambria, Georgia, Times New Roman, serif"


def config(px):
    return {
        "startOnLoad": False,
        "theme": "base",
        "securityLevel": "strict",
        "htmlLabels": False,
        "fontFamily": FONT,
        "themeVariables": {
            "fontFamily": FONT, "fontSize": f"{px}px",
            "primaryColor": "#e3ecfa", "primaryTextColor": "#1a1a1a", "primaryBorderColor": "#1b2a57",
            "lineColor": "#4a4f5c", "secondaryColor": "#f3f5f9", "tertiaryColor": "#ffffff",
            "clusterBkg": "#f7f8fb", "clusterBorder": "#9aa3b2", "edgeLabelBackground": "#ffffff",
            "titleColor": "#1b2a57",
        },
        "flowchart": {"htmlLabels": False, "curve": "basis", "nodeSpacing": 12, "rankSpacing": 30,
                      "padding": 10, "diagramPadding": 4, "wrappingWidth": 400},
    }


def render(path, profile):
    text = path.read_text(encoding="utf-8")
    m = re.search(r"%%\s*px=(\d+)", text)
    px = int(m.group(1)) if m else 20
    name = path.stem
    page = BUILD / f"mmd-{name}.html"
    page.write_text(
        "<!doctype html><meta charset='utf-8'><body><pre id='out'></pre>"
        "<script src='vendor/mermaid.min.js'></script><script>"
        f"mermaid.initialize({json.dumps(config(px))});"
        f"mermaid.render({json.dumps('m-' + name.split('-')[0])}, {json.dumps(text)})"
        ".then(r => { document.getElementById('out').textContent = r.svg; })"
        ".catch(e => { document.getElementById('out').textContent = 'ERROR ' + e; });"
        "</script></body>", encoding="utf-8")
    run = chrome(["--virtual-time-budget=10000", "--dump-dom", page.as_uri()], profile)
    found = re.search(r"<pre id=\"out\">(.*?)</pre>", run.stdout, re.S)
    if not found or found.group(1).startswith("ERROR"):
        sys.exit(f"{name}: render failed: {found.group(1)[:300] if found else run.stderr[-500:]}")
    svg = html.unescape(found.group(1))
    vb = re.search(r'viewBox="([-\d.]+) ([-\d.]+) ([\d.]+) ([\d.]+)"', svg)
    w, h = float(vb.group(3)), float(vb.group(4))
    svg = re.sub(r'<svg([^>]*?) width="100%"', rf'<svg\1 width="{w:.1f}" height="{h:.1f}"', svg, count=1)
    svg = re.sub(r' style="max-width: [\d.]+px;"', "", svg, count=1)
    (OUT / f"{name}.svg").write_text(svg + "\n", encoding="utf-8", newline="\n")
    print(f"{name}: {w:.0f} x {h:.0f} px, font {px} px")


def main():
    if not VENDOR.exists():
        sys.exit(f"missing {VENDOR.relative_to(ROOT)}: see the docstring")
    picks = sys.argv[1:]
    sources = sorted(SRC.glob("*.mmd"))
    if picks:
        sources = [s for s in sources if any(s.stem.startswith(p) for p in picks)]
    profile = tempfile.mkdtemp(prefix="siren-mmd-")
    try:
        for s in sources:
            render(s, profile)
    finally:
        shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    main()
