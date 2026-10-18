# 작업 계획 (alpha-2, 2026-10-18)

## 0. 고정 조건

1. 제목: **Onboard AI Payload of PADO-SAT: SIREN**
   - 초록집 제목은 "A Multifunctional Onboard AI Payload for PADO-SAT: SIREN" (II-2-7). 발표 제목은 조건 1을 따름
2. DNN(BUS, PD, GRU)마다 1–2장, DNN 슬라이드 합계 최대 6장
3. 큐브위성 경연대회 PDR 발표자료 기반, **SIREN 중심**
4. 슬라이드 순서도 SIREN 중심
5. 내부 도구 이름 사용 금지. SIREN 제작 **방법**으로 설명
6. 임시 자료: 지금 **Alpha**, 전력 측정 후 최종판
7. 그림 **SVG**(Figma 편집), 슬라이드 HTML(headless) → PDF/PPT
8. 발표 **10분 목표**, 슬라이드 **영어** (배정 15분, 질의응답 포함)
9. 모든 글자 **15 pt 이상** (본문, 그림)
10. 구성: **Introduction – Background – Results (Preliminary) – Future works**
11. Digital Twin: DT-I CREST(VERB-3D 개념, flux field) · DT-II Bus HK(방법 미정)
12. 모든 글 **개조식**
13. **질문 대응용 Abstract** 포함 (Appendix)
14. 강조 단어 **Bold + 색상**
15. Background에 **이론적 배경**
16. 정확도 수치 **A/B/C 3가지 판**으로 First Draft, 전력 측정 후 확정
17. 전력 **추정치 넣지 않음**
18. 로고: 경희대 **대학휘장**(공식 파일) · 연구실 패치 · PADO-SAT 로고 **자리만**
19. 저자는 초록 기준, **발표자만 강조**
20. 작업 위치: F 드라이브
21. (alpha-2) **배선도**와 git의 **mermaid 연산기 구조도(요약)** 포함
22. (alpha-2) 모델마다 **GIF/이미지 시각화** (PDR 모델 슬라이드와 같은 방식)
23. (alpha-2) **Background 보강**, 전체적으로 **친절하게** (진행 표시, 슬라이드별 핵심 메시지, 약어 풀이)
24. (alpha-2) 글꼴: **깔끔한 세리프** → Cambria

## 1. 발표 정보 (초록집 기준)

- 2026 한국우주과학회 가을 학술대회, 구두 II-2 (우주응용활용/우주지상국, 운용), 아라(8층)
- II-2-7, 10월 27일(화) 17:25 (날짜는 초록집 순서로 추정)
- 저자: **Woncheol Joo**¹ (발표자 표시), Subin Kim¹, Jeongin Yeo¹, Nayeong Hwang¹, Yangha Ju¹, Jiwon Lee¹, Young-Joo Song¹ ², Yong-Jae Moon¹ ²

## 2. 발표 흐름 (본편 20장)

| # | 구간 | 제목 (EN) | 시간 |
|---|---|---|---|
| 1 | Title | Onboard AI Payload of PADO-SAT: SIREN | 0:10 |
| 2 | Introduction | Why onboard AI on a CubeSat | 0:35 |
| 3 | Introduction | PADO-SAT and SIREN | 0:35 |
| 4 | Introduction | SIREN processing unit and requirements | 0:30 |
| 5 | Background · Space environment | Charged particles around a LEO CubeSat | 0:35 |
| 6 | Background · Spacecraft health | Anomaly detection in housekeeping telemetry | 0:30 |
| 7 | Background · Science data | Predictive compression of particle counts | 0:30 |
| 8 | Background · Deep learning | Time-series models in SIREN (ModernTCN, GRU) | 0:35 |
| 9 | Background · Hardware | Integer inference on an FPGA | 0:35 |
| 10 | Background · Method | How SIREN is built | 0:30 |
| 11 | Results · Architecture | SIREN hardware on one chip (mermaid 구조도 요약) | 0:30 |
| 12 | Results · PD | PD: high-energy particle risk detection (GIF, 정확도 A/B/C) | 0:30 |
| 13 | Results · PD | PD on the FPGA | 0:25 |
| 14 | Results · BUS | BUS: equipment anomaly detection (GIF, 정확도 A/B/C) | 0:30 |
| 15 | Results · BUS | BUS on the FPGA | 0:25 |
| 16 | Results · GRU | GRU: predictive compression of CREST data (이미지, A/B/C) | 0:30 |
| 17 | Results · GRU | GRU on the FPGA | 0:25 |
| 18 | Results · System | One bitstream, three models (배선도) | 0:35 |
| 19 | Future works | Toward the final version (Digital Twin I/II) | 0:30 |
| 20 | Summary | SIREN at a glance · Thank you | 0:10 |

- 합계 약 9:55. 배정 15분 안에서 질의응답 약 5분
- 시간 초과 시 감축 순서: 4번을 18번 상태표에 합침 → 6·7번을 한 장으로 → 15번을 14번에 합침(DNN 5장)
- Appendix: Abstract · Why an FPGA SoC · Correctness · Fault management · Resources/weight update · Abbreviations

## 3. 친절도 장치

- 모든 본편 슬라이드 오른쪽 위 **구간 진행 표시** (Introduction / Background / Results / Future works)
- 모든 본편 슬라이드 아래 **핵심 메시지 한 줄** (→ …)
- 약어는 처음 나오는 슬라이드 아래 회색 줄에 풀이, Appendix에 약어표
- 모델 결과 슬라이드: 그림(무엇을 하는지) → 숫자(얼마나 잘하는지) 순서
- 내부 용어 풀어 쓰기: pointwise (1×1) / depthwise conv, "outputs = integer reference"

## 4. 정확도 3가지 판 (12, 14, 16번)

| 판 | 내용 | 근거 |
|---|---|---|
| A | PDR 시험셋 float 모델 vs 기준선 (제출 초록과 같은 수치) | PDR p.45–48, 초록 |
| B | 검증셋 float vs 정수 모델 (양자화 영향) | 공유 저장소 결과 문서 |
| C | A의 기준선 비교 + B의 정수화 변화량 한 줄 | 둘 다 |

## 5. 그림과 이미지

| ID | 슬라이드 | 내용 | 원천 |
|---|---|---|---|
| F01–F12 | 2–19 | 개념도, 블록도, 데이터 흐름 (SVG) | `tools/make_figures.py` 초안 |
| F04c | 8 | GRU 셀 | 새로 작성 |
| F13 | 11 | 하드웨어 구조도: 공유 저장소 README mermaid 그림 1 요약 (같은 색 체계) | 새로 작성 |
| F14 | 6 | 이상 탐지 개념도 (합성 예시) | 새로 작성 |
| F15 | 7 | 예측 부호화 개념도 (합성 예시) | 새로 작성 |
| `assets/models/pd-risk-score.gif` | 12 | PD 위험도 GIF (2025-11-11 SEP) | PDR 본편 p.45 |
| `assets/models/bus-anomaly.gif` | 14 | BUS 이상 탐지 GIF (ESA-ADB Mission 1, 187) | PDR 본편 p.46 |
| `assets/models/gru-reconstruction.png` | 16 | GRU 복원 그림 (KSEM A5) | PDR 본편 p.47 |
| `assets/hw/routing-150mhz.png` | 18 | Vivado 배선 결과 | 공유 저장소 `docs/visual` |

- PDF에는 GIF 대신 대표 프레임(`*-still.png`), `build/present-*.html`에서는 GIF 재생
- **주의**: 모델 GIF/이미지와 배선도는 래스터라 글자 크기 자동 검사 대상이 아님. 축 눈금 글자가 15 pt보다 작음 → 원본 스크립트로 큰 글꼴 재생성 필요 (모델 담당)

## 6. 제작 방식

- 캔버스 1280×720 CSS px = 13.333 × 7.5 in. **15 pt = 20 px**
- 글꼴 **Cambria** (Windows·Office 기본, Figma 데스크톱에서 로컬 글꼴로 사용). 제목 40 px · 본문 28 px · 최소 20 px
- 강조: `<b class="k">` 핵심(파랑) · `<b class="ok">` 달성(초록) · `<b class="todo">` 미완(주황)
- 검사 (`tools/build.py`, 커밋 전 필수): 글자 ≥ 20 px, 슬라이드·바닥글·핵심 메시지 침범, 금지어, 절대 경로
- PPTX: 다음 판 (장별 고해상도 이미지 + GIF + 발표 노트)

## 7. 판과 일정

| 판 | 내용 | 시점 |
|---|---|---|
| alpha-0 | 계획, 저장소 | 10/18 |
| alpha-1 | First Draft 17장, 정확도 A/B/C | 10/18 |
| **alpha-2** | **이론 보강(본편 20장), 구조도·배선도·모델 GIF, 진행 표시·핵심 메시지, Cambria** | **10/18** |
| alpha-3 | 지도교수 검토 반영, PPTX, 모델 그림 큰 글꼴 재생성 | 검토 후 |
| beta-1 | 전력 측정 반영, 정확도 판 확정 | 측정 후 |
| v1.0 | 리허설 반영 최종판 | 10/26 이전 |

## 8. 남은 확인 사항

1. 발표자 표시: 초록 제1저자(Woncheol Joo)로 둠. 다르면 수정
2. PADO-SAT 로고 파일 (자리만 둠)
3. 정확도 판 A/B/C 선택 (전력 측정 후)
4. 모델 GIF/이미지의 작은 축 글자 → 원본 스크립트로 재생성할지
