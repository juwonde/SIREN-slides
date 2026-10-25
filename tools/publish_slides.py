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
TAG = "slides-2026-10-25-r2"
PDF_NAME = "SIREN-KSS2026-slides-20261025.pdf"
REPO = "https://github.com/juwonde/SIREN_2026_Fall_conf"

def load():
    data = json.loads(INDEX.read_text(encoding="utf-8"))
    assert len(data["slides"]) == data["main_slides"] + data["appendix_slides"] == 37
    assert [s["n"] for s in data["slides"]] == list(range(1, 38))
    assert data["speaker_notes_included"] is False
    assert data["hidden_slides_included"] is False
    for slide in data["slides"]:
        assert "notes" not in slide and "speakerNotes" not in slide
        path = Path(slide["preview"])
        assert not path.is_absolute() and ".." not in path.parts
        assert slide["section"] == ("main" if slide["n"] <= 20 else "appendix")
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
        "# Onboard AI Payload of PADO-SAT: SIREN", "",
        "한국우주과학회 2026 가을 학술대회 발표 자료입니다. "
        "PADO-SAT의 온보드 AI 탑재체 SIREN이 수행하는 고에너지 대전입자 위험 탐지, "
        "BUS HK 이상 탐지, 과학데이터 압축과 초기 구현 결과를 소개합니다.", "",
        f"**2026-10-25 공개본: 본편 20장 + 부록 17장.** 아래에서 전체 슬라이드를 볼 수 있습니다.", "",
        f"- [전체 슬라이드 PDF (37장)]({REPO}/releases/download/{TAG}/{PDF_NAME})",
        f"- [현재 릴리스]({REPO}/releases/tag/{TAG})",
        f"- [Figma Slides 원본]({data['source']})",
        "- [본편](#main-slides) · [부록](#appendix) · [자료 구성](#source-and-data)", "",
        "PDF와 슬라이드 이미지는 발표자 노트를 포함하지 않습니다. "
        "Figma의 숨김 슬라이드는 이 공개본에서 제외했습니다.", "",
        "## Main slides", "",
    ]
    for slide in data["slides"]:
        if slide["n"] == 21:
            lines += ["## Appendix", ""]
        n, title = slide["n"], slide["title"]
        lines += [f"### {n:02d}. {title}", "",
                  f"![Slide {n:02d}: {title}]({slide['preview']})", ""]
    lines += [
        "## Source and data", "",
        "| 경로 | 내용 |", "|---|---|",
        "| [docs/slide-index.json](docs/slide-index.json) | 공개 순서, 제목, 이미지 크기와 SHA-256 |",
        "| [assets/slides/](assets/slides/) | README와 PDF에 사용한 전체 슬라이드 이미지 |",
        "| [docs/numbers.md](docs/numbers.md) | 발표 수치와 근거. 상단 갱신 기록을 우선 |",
        "| [docs/power-selection-20261024.md](docs/power-selection-20261024.md) | 대표 전력·운용 시험의 선택과 측정 범위 |",
        "| [docs/mode-integration-appendix-20261024.md](docs/mode-integration-appendix-20261024.md) | 부록 운용모드 통합 검토의 조건과 한계 |",
        "| [assets/](assets/) · [figures/](figures/) | 관측 자료 기반 그림, 모델 시각화, 축약 측정 결과 |",
        "| [tools/](tools/) | 그림 생성과 공개본 검사 도구 |",
        "| [docs/legacy-html-workflow.md](docs/legacy-html-workflow.md) | 이전 HTML 초안의 생성 방법과 이력 |", "",
        "현행 발표본은 위 Figma 스냅샷과 PDF입니다. "
        "기존 slides/deck.html과 과거 릴리스는 이전 초안으로 보존합니다.", "",
        "### Rebuild the public snapshot", "",
        "Python 3와 Pillow가 필요하며 PDF 생성에는 ReportLab이 추가로 필요합니다.", "",
        "```bash", "python tools/publish_slides.py --check",
        "python tools/publish_slides.py --pdf", "```", "",
        f"PDF 결과는 dist/{PDF_NAME}이며 GitHub 릴리스에 첨부합니다. "
        "원고, 발표자 노트, 인쇄 연습본과 임시 빌드 결과는 공개본에 넣지 않습니다.", "",
    ]
    return "\n".join(lines)

def pdf(data):
    from reportlab.pdfgen import canvas
    out = ROOT / "dist" / PDF_NAME
    out.parent.mkdir(exist_ok=True)
    c = canvas.Canvas(str(out), pagesize=(960, 540), pageCompression=1)
    c.setTitle("Onboard AI Payload of PADO-SAT: SIREN - 2026-10-25")
    c.setAuthor("PADO-SAT / SIREN")
    c.setSubject("Public slides only: 20 main slides and 17 appendix slides. No speaker notes.")
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
        print("Public gallery verified: 37 images, sequence, hashes, README links; no speaker notes.")
    else:
        INDEX.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        (ROOT / "README.md").write_text(expected, encoding="utf-8")
        print("Updated public README and 37-slide image index.")
    if args.pdf:
        result = pdf(data)
        print(f"Slides-only PDF: {result.relative_to(ROOT)} ({result.stat().st_size} bytes)")

if __name__ == "__main__":
    main()
