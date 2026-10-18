"""Build and check the SIREN deck.

    python tools/build.py                 # lint + PDF for variants A, B, C
    python tools/build.py --variants A    # one variant
    python tools/build.py --lint-only     # checks only
    python tools/build.py --label alpha-1 # name used in the PDF file names

Steps
1. Inline figures/*.svg into slides/deck.html (one copy per accuracy variant) -> build/
2. In-page lint in headless Chrome (text >= 20 px = 15 pt, nothing outside the slide, banned names)
3. Repository scan (banned names, machine-specific absolute paths)
4. PDF per variant -> dist/

Chrome is found from CHROME_PATH, PATH, or the standard install folders.
Only the Python standard library is used.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD, DIST = ROOT / "build", ROOT / "dist"
VARIANTS = ("A", "B", "C")
BANNED = [re.compile("F[- ]?" + "ASTRA", re.I), re.compile("f_" + "astra", re.I),
          re.compile("Direct" + "RTL", re.I), re.compile(r"\bbpg-r\d", re.I)]
# Built from pieces so the scan does not flag its own source.
ABS_PATH = re.compile(r"(?<![A-Za-z])[A-Za-z]:[\\/](?!/)" + "|/" + r"Users/[^/\s]+/" + "|/" + r"home/[^/\s]+/"
                      + r"|\\" + r"Users\\")
SCAN_SUFFIXES = {".md", ".html", ".css", ".js", ".py", ".svg", ".json", ".txt", ".gitignore"}


def find_chrome():
    env = os.environ.get("CHROME_PATH")
    if env and pathlib.Path(env).exists():
        return env
    for name in ("chrome", "google-chrome", "chromium", "chromium-browser", "msedge"):
        hit = shutil.which(name)
        if hit:
            return hit
    for base in ("PROGRAMFILES", "PROGRAMFILES(X86)", "LOCALAPPDATA"):
        root = os.environ.get(base)
        if not root:
            continue
        for rel in ("Google/Chrome/Application/chrome.exe", "Microsoft/Edge/Application/msedge.exe"):
            p = pathlib.Path(root) / rel
            if p.exists():
                return str(p)
    sys.exit("Chrome or Edge not found: set CHROME_PATH")


def inline_figures(html):
    def repl(m):
        name = m.group(1)
        svg = (ROOT / "figures" / name).read_text(encoding="utf-8")
        svg = re.sub(r"<\?xml[^>]*\?>\s*", "", svg)
        svg = svg.replace("<svg ", f'<svg data-name="{name}" ', 1)
        return f'<div class="fig">{svg}</div>'
    return re.sub(r'<div class="fig" data-svg="([^"]+)"></div>', repl, html)


def render(variant, lint):
    html = (ROOT / "slides" / "deck.html").read_text(encoding="utf-8")
    html = re.sub(r'<html lang="en" data-variant="[A-Z]">', f'<html lang="en" data-variant="{variant}">', html, count=1)
    html = inline_figures(html)
    if lint:
        js = (ROOT / "tools" / "lint.js").read_text(encoding="utf-8")
        html = html.replace("</body>", f"<script>{js}</script>\n</body>")
    BUILD.mkdir(exist_ok=True)
    out = BUILD / f"{'lint' if lint else 'deck'}-{variant}.html"
    out.write_text(html, encoding="utf-8", newline="\n")
    return out


def chrome(args, profile):
    cmd = [find_chrome(), "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
           "--allow-file-access-from-files", f"--user-data-dir={profile}"] + args
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=180)


def lint_variant(variant, profile):
    page = render(variant, lint=True)
    run = chrome(["--virtual-time-budget=8000", "--dump-dom", page.as_uri()], profile)
    found = re.findall(r'<pre id="lint-result">(\{.*?\})</pre>', run.stdout, re.S)
    if not found:
        print(run.stderr[-2000:])
        sys.exit(f"lint {variant}: no result from Chrome")
    res = json.loads(found[-1].replace("&quot;", '"').replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">"))
    problems = 0
    for key in ("small", "outside", "banned"):
        for item in res[key]:
            print(f"  [{variant}] {key}: {item}")
            problems += 1
    print(f"lint {variant}: {res['slides']} slides, {problems} problem(s)")
    return problems


def scan_repo():
    problems = 0
    files = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard"], cwd=ROOT,
                           capture_output=True, text=True, encoding="utf-8").stdout.split("\n")
    for rel in filter(None, files):
        p = ROOT / rel
        if p.suffix not in SCAN_SUFFIXES and p.name not in SCAN_SUFFIXES or not p.exists():
            continue
        if rel.startswith("assets/"):
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        for pat in BANNED:
            for m in pat.finditer(text):
                print(f"  banned name in {rel}: {m.group(0)}")
                problems += 1
        for m in ABS_PATH.finditer(text):
            line = text[max(0, m.start() - 30):m.end() + 30].replace("\n", " ")
            print(f"  absolute path in {rel}: ...{line}...")
            problems += 1
    print(f"repository scan: {problems} problem(s)")
    return problems


def pdf_variant(variant, label, profile):
    page = render(variant, lint=False)
    DIST.mkdir(exist_ok=True)
    out = DIST / f"SIREN-KSS2026-{label}-{variant}.pdf"
    run = chrome(["--no-pdf-header-footer", "--virtual-time-budget=5000", f"--print-to-pdf={out}", page.as_uri()], profile)
    if not out.exists():
        print(run.stderr[-2000:])
        sys.exit(f"pdf {variant}: failed")
    print(f"pdf {variant}: {out.relative_to(ROOT)} ({out.stat().st_size // 1024} KiB)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variants", default=",".join(VARIANTS))
    ap.add_argument("--label", default="draft")
    ap.add_argument("--lint-only", action="store_true")
    ap.add_argument("--no-lint", action="store_true")
    a = ap.parse_args()
    variants = [v.strip().upper() for v in a.variants.split(",") if v.strip()]
    profile = tempfile.mkdtemp(prefix="siren-chrome-")
    try:
        if not a.no_lint:
            problems = scan_repo() + sum(lint_variant(v, profile) for v in variants)
            if problems:
                sys.exit(f"{problems} problem(s): fix before building or committing")
        if not a.lint_only:
            for v in variants:
                pdf_variant(v, a.label, profile)
    finally:
        shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    main()
