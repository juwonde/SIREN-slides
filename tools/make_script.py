"""Practice script: each slide picture on top, the Korean script below with a cumulative goal time per line.

    python tools/make_script.py [--label alpha-N]

Reads docs/script-ko.json and the slide pictures in build/figma/slides/NN.png (exported from the Figma deck).
Writes build/script-ko.html and dist/SIREN-KSS2026-script-ko[-label].pdf (Chrome print, A4 portrait, script text 17 pt).
"""
import html
import json
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from build import BUILD, DIST, ROOT, chrome  # noqa: E402

CSS = """
@page { size: A4 portrait; margin: 12mm 14mm; }
body { font-family: "Noto Serif KR", "Malgun Gothic", serif; color: #16181d; margin: 0; }
h1 { font-size: 15pt; color: #20386e; margin: 0 0 2mm; }
.target { font-size: 12pt; color: #353b47; margin: 0 0 4mm; }
.slide { break-inside: avoid; margin: 0 0 6mm; }
.slide img { width: 100%; border: 1px solid #7c8594; display: block; }
table { width: 100%; border-collapse: collapse; margin-top: 3mm; font-size: 17pt; line-height: 1.5; }
td { padding: 2mm 2mm; vertical-align: top; border-bottom: 1px solid #c4c9d2; }
td.t { width: 22mm; font-family: Consolas, monospace; font-weight: 700; color: #84161a; white-space: nowrap; }
td.n { width: 22mm; color: #353b47; font-size: 13pt; }
.skip td { color: #353b47; font-style: italic; }
"""


def main():
    label = ""
    if "--label" in sys.argv:
        label = "-" + sys.argv[sys.argv.index("--label") + 1]
    data = json.loads((ROOT / "docs" / "script-ko.json").read_text(encoding="utf-8"))
    shots = BUILD / "figma" / "slides"
    parts = [f"<h1>{html.escape(data['title'])}</h1><div class='target'>{html.escape(data['target'])}</div>"]
    for s in data["slides"]:
        img = shots / f"{s['n']:02d}.png"
        if not img.exists():
            sys.exit(f"missing {img.relative_to(ROOT)}: export the Figma slides first")
        rows = []
        for t, line in s["lines"]:
            cls = " class='skip'" if line.startswith("(바로 넘김)") else ""
            rows.append(f"<tr{cls}><td class='t'>{t}</td><td>{html.escape(line)}</td></tr>")
        parts.append(f"<div class='slide'><img src='{img.as_uri()}' alt='slide {s['n']}'>"
                     f"<table><tr><td class='n'>#{s['n']}</td><td></td></tr>{''.join(rows)}</table></div>")
    page = BUILD / "script-ko.html"
    page.write_text(f"<!doctype html><meta charset='utf-8'><title>SIREN script (KO)</title><style>{CSS}</style>"
                    + "".join(parts), encoding="utf-8")
    DIST.mkdir(exist_ok=True)
    out = DIST / f"SIREN-KSS2026-script-ko{label}.pdf"
    profile = tempfile.mkdtemp(prefix="siren-script-")
    try:
        chrome(["--no-pdf-header-footer", f"--print-to-pdf={out}", "--virtual-time-budget=8000", page.as_uri()], profile)
    finally:
        shutil.rmtree(profile, ignore_errors=True)
    print(f"script: {page.relative_to(ROOT)} · {out.relative_to(ROOT)} ({out.stat().st_size // 1024} KiB)")


if __name__ == "__main__":
    main()
