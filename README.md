# Onboard AI Payload of PADO-SAT: SIREN

한국우주과학회 2026 가을 학술대회 발표 자료 (구두 II-2-7) 저장소입니다.

- 발표: 10분 목표 (배정 15분, 질의응답 포함), 영어 슬라이드
- 구성: Introduction – Background – Results (Preliminary) – Future works
- 현재 판: **alpha-8** (본편 25장 + Appendix 5장, 정확도 수치 A/B/C 3종). 전력 측정 결과가 들어간 뒤 최종판을 만듭니다.

## 폴더

| 폴더 | 내용 |
|---|---|
| `docs/plan.md` | 작업 계획: 조건, 슬라이드 구성, 그림 목록, 일정 |
| `docs/numbers.md` | 슬라이드에 들어가는 모든 수치와 출처 |
| `slides/deck.html`, `slides/style.css` | 슬라이드 원본 (1280×720 CSS px, 16:9) |
| `figures/` | 그림 SVG. Figma에서 편집하고 같은 이름으로 다시 저장 |
| `assets/logos/` | 경희대 대학휘장(SVG), 연구실 패치 |
| `assets/models/`, `assets/hw/` | 모델 시각화(GIF, 이미지, PDF용 대표 프레임), 배선도 |
| `tools/` | 빌드·검사 (`build.py`, `lint.js`), 초안 그림 생성 (`make_figures.py`), 배치도 생성 (`chip_map.py`), 로고 정리 (`prep_svg_logo.py`) |
| `build/`, `dist/` | 빌드 결과 (git 제외). PDF는 GitHub release에 첨부 |

## 빌드

Python 3 (표준 라이브러리만)과 Chrome 또는 Edge가 필요합니다. 브라우저는 `CHROME_PATH`, `PATH`, 표준 설치 위치 순서로 찾습니다.

```bash
python tools/build.py --label alpha-8
```

- 검사가 먼저 돌고, 문제가 있으면 PDF를 만들지 않습니다.
  - 렌더된 모든 글자 20 px(15 pt) 이상
  - 글자가 슬라이드 밖이나 바닥글로 넘치지 않음
  - 금지어(내부 도구 이름 등), 절대 경로
- 결과: `dist/SIREN-KSS2026-<label>-A.pdf`, `-B.pdf`, `-C.pdf` (정확도 판 3종)
- 발표용 HTML: `build/present-A.html` 등 (GIF 재생). PDF에는 GIF 대표 프레임이 들어갑니다
- 검사만: `python tools/build.py --lint-only`

## 규칙

- 모든 글자 15 pt 이상 (본문, 그림). 글꼴 Cambria
- 래스터 이미지(모델 GIF, 배선도) 속 글자는 자동 검사 대상이 아님
- 모든 글은 개조식
- 강조: `<b class="k">` 핵심(파랑), `<b class="ok">` 달성(초록), `<b class="todo">` 미완(주황). SVG는 `font-weight="700"`과 `fill` 인라인 속성
- 내부 도구 이름은 쓰지 않고, SIREN을 만든 방법으로 설명
- 모든 수치는 `docs/numbers.md`에 출처와 함께 적은 뒤 슬라이드에 넣음
- 판 이름: `alpha-N` → `beta-N` → `v1.0`(최종), 판마다 tag
