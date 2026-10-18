"""Render the LaTeX equations of the talk (figures/src/equations.json) to SVG with MathJax in headless Chrome.

    python tools/render_tex.py

Writes build/figma/tex/<name>.svg: glyphs as paths (no font needed), size in px, ink colour, ready for the
Figma import. Each entry of equations.json: {"name": ..., "tex": ..., "px": font size in px on the 1920 slide}.
Needs build/vendor/mathjax-tex-svg.js (MathJax 3.2.2, ignored by git). Fetch it once with:
    curl -o build/vendor/mathjax-tex-svg.js https://cdn.jsdelivr.net/npm/mathjax@3.2.2/es5/tex-svg-full.js
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

SRC = ROOT / "figures" / "src" / "equations.json"
OUT = BUILD / "figma" / "tex"
VENDOR = BUILD / "vendor" / "mathjax-tex-svg.js"
INK = "#16181d"

PAGE = """<!doctype html><meta charset='utf-8'><body><div id='host'></div><pre id='out'></pre>
<script>
window.MathJax = { svg: { fontCache: 'none' }, startup: { typeset: false, ready: () => {
  MathJax.startup.defaultReady();
  MathJax.startup.promise.then(() => {
    const eqs = EQS, out = {};
    const host = document.getElementById('host');
    for (const e of eqs) {
      const box = document.createElement('div');
      box.style.fontSize = e.px + 'px'; box.style.color = 'INK'; box.style.display = 'inline-block';
      host.appendChild(box);
      const node = MathJax.tex2svg(e.tex, { display: e.display !== false });
      box.appendChild(node);
      const svg = node.querySelector('svg');
      const r = svg.getBoundingClientRect();
      svg.setAttribute('width', r.width.toFixed(1)); svg.setAttribute('height', r.height.toFixed(1));
      svg.removeAttribute('style'); svg.removeAttribute('role'); svg.removeAttribute('focusable');
      svg.removeAttribute('aria-hidden');
      out[e.name] = svg.outerHTML.replace(/currentColor/g, 'INK');
    }
    document.getElementById('out').textContent = JSON.stringify(out);
  }).catch(err => { document.getElementById('out').textContent = 'ERROR ' + err; });
} } };
</script>
<script src='vendor/mathjax-tex-svg.js'></script></body>"""


def main():
    if not VENDOR.exists():
        sys.exit(f"missing {VENDOR.relative_to(ROOT)}: see the docstring")
    eqs = json.loads(SRC.read_text(encoding="utf-8"))
    page = BUILD / "render-tex.html"
    page.write_text(PAGE.replace("EQS", json.dumps(eqs)).replace("INK", INK), encoding="utf-8")
    profile = tempfile.mkdtemp(prefix="siren-tex-")
    try:
        run = chrome(["--virtual-time-budget=15000", "--dump-dom", page.as_uri()], profile)
    finally:
        shutil.rmtree(profile, ignore_errors=True)
    found = re.search(r'<pre id="out">(.*?)</pre>', run.stdout, re.S)
    if not found or not found.group(1).startswith("{"):
        sys.exit(f"render failed: {found.group(1)[:300] if found else run.stderr[-500:]}")
    svgs = json.loads(html.unescape(found.group(1)))
    OUT.mkdir(parents=True, exist_ok=True)
    for name, svg in svgs.items():
        (OUT / f"{name}.svg").write_text(svg + "\n", encoding="utf-8", newline="\n")
        w, h = re.search(r'width="([\d.]+)" height="([\d.]+)"', svg).groups()
        print(f"{name}: {w} x {h} px")


if __name__ == "__main__":
    main()
