# Onboard AI Payload of PADO-SAT: SIREN

한국우주과학회 2026 가을 학술대회 발표 자료 저장소입니다.

- 발표: 10분, 영어 슬라이드
- 구성: Introduction – Background – Results (Preliminary) – Future works
- 현재 판: **alpha-0** (작업 계획과 저장소 뼈대). 전력 측정 결과가 들어간 뒤 최종판을 만듭니다.

## 폴더

| 폴더 | 내용 |
|---|---|
| `docs/plan.md` | 작업 계획: 슬라이드 구성, 그림 목록, 제작 방식, 일정 |
| `docs/numbers.md` | 슬라이드에 들어가는 모든 수치와 출처 |
| `slides/` | 슬라이드 HTML (1280×720 CSS px, 16:9) |
| `figures/` | 그림 SVG. Figma에서 편집하고 같은 이름으로 다시 저장합니다 |
| `tools/` | 빌드(PDF, PPTX)와 검사(글자 크기, 금지어, 절대 경로) 스크립트 |
| `dist/` | 빌드 결과 (git 제외). PDF는 GitHub release에 첨부합니다 |

## 규칙

- 모든 글자는 15 pt 이상입니다. 본문과 그림 안 글자 모두 해당합니다. 1280×720 캔버스에서 15 pt는 20 CSS px입니다.
- 내부 도구 이름은 쓰지 않습니다. SIREN을 어떻게 만들었는지 방법으로 설명합니다.
- 모든 수치는 `docs/numbers.md`에 출처와 함께 적은 뒤 슬라이드에 넣습니다.
- 판 이름은 `alpha-N` → `beta-N` → `v1.0`(최종) 순서로 tag를 붙입니다.
