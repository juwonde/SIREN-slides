"""Build the public slide gallery and a slides-only PDF from the Figma export.

python tools/publish_slides.py --pdf
python tools/publish_slides.py --check

PNG exports and docs/slide-index.json are the publication snapshot. Speaker
notes, private scripts and generated PDFs are not Git inputs.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INDEX = ROOT / "docs" / "slide-index.json"
PDF_NAME = "SIREN-slides.pdf"
REPO = "https://github.com/juwonde/SIREN-slides"

def load():
    data = json.loads(INDEX.read_text(encoding="utf-8"))
    total = data["main_slides"] + data["appendix_slides"]
    assert data["main_slides"] > 0 and data["appendix_slides"] >= 0
    assert len(data["slides"]) == total
    assert [s["n"] for s in data["slides"]] == list(range(1, total + 1))
    assert data["speaker_notes_included"] is False
    assert data["hidden_slides_included"] is False
    for slide in data["slides"]:
        assert "notes" not in slide and "speakerNotes" not in slide
        path = Path(slide["preview"])
        assert not path.is_absolute() and ".." not in path.parts
        assert slide["preview"] == f"assets/slides/{slide['n']:02d}.png"
        assert slide["section"] == ("main" if slide["n"] <= data["main_slides"] else "appendix")
    return data

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def image_checks(data, check_hash):
    from PIL import Image
    for slide in data["slides"]:
        path = ROOT / slide["preview"]
        with Image.open(path) as im:
            assert im.size == (slide["width"], slide["height"]), path
            im.verify()
        sha = digest(path)
        if check_hash:
            assert slide.get("sha256") == sha, path
        else:
            slide["sha256"] = sha
        slide["bytes"] = path.stat().st_size

def readme(data):
    lines = [
        "# SIREN", "",
        "PADO-SAT의 온보드 AI 탑재체 SIREN 발표 자료입니다.", "",
        f"[PDF]({REPO}/releases/latest/download/{PDF_NAME}) · "
        f"[Figma]({data['source']}) · [본편](#main-slides) · [부록](#appendix)", "",
        "## Main slides", "",
    ]
    for slide in data["slides"]:
        if slide["n"] == data["main_slides"] + 1:
            lines += ["## Appendix", ""]
        n, title = slide["n"], slide["title"]
        lines += [f"### {n:02d}. {title}", "",
                  f"![Slide {n:02d}: {title}]({slide['preview']})", ""]
    return "\n".join(lines)

def pdf(data):
    from reportlab.pdfgen import canvas
    out = ROOT / "dist" / PDF_NAME
    out.parent.mkdir(exist_ok=True)
    c = canvas.Canvas(str(out), pagesize=(960, 540), pageCompression=1)
    c.setTitle("SIREN - Onboard AI Payload of PADO-SAT")
    c.setAuthor("PADO-SAT / SIREN")
    c.setSubject("SIREN presentation slides")
    for s in data["slides"]:
        key = f"slide-{s['n']:02d}"
        c.bookmarkPage(key)
        c.addOutlineEntry(f"{s['n']:02d}. {s['title']}", key)
        c.drawImage(str(ROOT / s["preview"]), 0, 0, width=960, height=540, mask="auto")
        c.showPage()
    c.save()
    return out

def main():
    ap = argparse.ArgumentParser(__doc__)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--pdf", action="store_true")
    args = ap.parse_args()
    data = load()
    image_checks(data, args.check)
    expected = readme(data)
    if args.check:
        assert (ROOT / "README.md").read_text(encoding="utf-8") == expected
        print(f"Public gallery verified: {len(data['slides'])} images, sequence, hashes and README links.")
    else:
        INDEX.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        (ROOT / "README.md").write_text(expected, encoding="utf-8")
        print(f"Updated public README and {len(data['slides'])}-slide image index.")
    if args.pdf:
        result = pdf(data)
        print(f"Slides-only PDF: {result.relative_to(ROOT)} ({result.stat().st_size} bytes)")

if __name__ == "__main__":
    main()
