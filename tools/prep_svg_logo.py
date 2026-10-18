"""Turn a cairo-converted Illustrator page (SVG 1.2 pageSet) into a cropped plain SVG.

Usage: python prep_svg_logo.py SRC.svg OUT.svg X Y W H
X Y W H is the artwork box in the source page units (pt).
"""
import re
import sys

src, out = sys.argv[1], sys.argv[2]
x, y, w, h = (float(v) for v in sys.argv[3:7])
text = open(src, encoding="utf-8").read()
text = re.sub(r"</?pageSet>\s*|</?page>\s*", "", text)
text = re.sub(r'version="1\.2"', 'version="1.1"', text, count=1)
text = re.sub(r'width="[^"]+" height="[^"]+" viewBox="[^"]+"',
              f'width="{w:g}" height="{h:g}" viewBox="{x:g} {y:g} {w:g} {h:g}"', text, count=1)
open(out, "w", encoding="utf-8", newline="\n").write(text)
