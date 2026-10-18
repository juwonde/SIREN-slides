# 작업 계획 (alpha-1, 2026-10-18)

## 0. 고정 조건

1. 제목: **Onboard AI Payload of PADO-SAT: SIREN**
   - 초록집 제목은 "A Multifunctional Onboard AI Payload for PADO-SAT: SIREN" (II-2-7). 발표 제목은 조건 1 유지
2. DNN(BUS, PD, GRU)마다 1–2장, DNN 슬라이드 합계 최대 6장
3. 큐브위성 경연대회 PDR 발표자료 기반, **SIREN 중심**
4. 슬라이드 순서도 SIREN 중심
5. 내부 도구 이름 사용 금지. SIREN 제작 **방법**으로 설명
6. 임시 자료: 지금 **Alpha**, 전력 측정 후 최종판
7. 그림 **SVG**(Figma 편집), 슬라이드 HTML(headless) → PDF/PPT
8. 발표 **10분 목표**, 슬라이드 **영어**
   - 배정 15분(질의응답 포함), 세션 후반 발표라 단축 가능성 → 10분 목표
9. 모든 글자 **15 pt 이상** (본문, 그림)
10. 구성: **Introduction – Background – Results (Preliminary) – Future works**
11. Digital Twin: DT-I CREST(VERB-3D 개념, flux field) · DT-II Bus HK(방법 미정)
12. 모든 글 **개조식**
13. **질문 대응용 Abstract** 포함 (Appendix)
14. 강조 단어 **Bold + 색상**
15. (10/18 추가) Background에 **이론적 배경** 포함
16. (10/18 추가) 정확도 수치: **3가지 판(A/B/C)으로 First Draft**, 전력 측정 후 하나로 확정
17. (10/18 추가) 전력 **추정치는 넣지 않음**. 측정값만
18. (10/18 추가) 로고: 경희대 **대학휘장**(홈페이지 공식 파일, 한글 이름 없음) · 연구실 패치 · PADO-SAT 로고 **자리만**
19. (10/18 추가) 저자는 초록 기준, **발표자만 강조**
20. (10/18 추가) 작업 위치: F 드라이브

## 1. 발표 정보 (초록집 기준)

- 2026 한국우주과학회 가을 학술대회, 구두 II-2 (우주응용활용/우주지상국, 운용), 아라(8층)
- II-2-7, 10월 27일(화) 17:25 (날짜는 초록집 순서로 추정)
- 저자: **Woncheol Joo**¹ (발표자 표시), Subin Kim¹, Jeongin Yeo¹, Nayeong Hwang¹, Yangha Ju¹, Jiwon Lee¹, Young-Joo Song¹ ², Yong-Jae Moon¹ ²
  - ¹ G-LAMP NEXUS Institute / School of Space Research, Kyung Hee University
  - ² Department of Astronomy and Space Science, Kyung Hee University

## 2. 발표 흐름 (본편 17장, 10분)

| # | 구간 | 제목 (EN) | 시간 |
|---|---|---|---|
| 1 | Title | Onboard AI Payload of PADO-SAT: SIREN | 0:10 |
| 2 | Introduction | Why onboard AI on a CubeSat | 0:40 |
| 3 | Introduction | PADO-SAT and SIREN | 0:40 |
| 4 | Background · Theory | Space radiation environment | 0:35 |
| 5 | Background · Theory | Time-series models in SIREN (ModernTCN, GRU) | 0:40 |
| 6 | Background · Theory | Integer inference on an FPGA (QAT, dataflow vs shared engine) | 0:40 |
| 7 | Background | SIREN processing unit and requirements | 0:35 |
| 8 | Background · Method | How SIREN is built | 0:35 |
| 9 | Results · PD | PD: high-energy particle risk detection (정확도 A/B/C) | 0:30 |
| 10 | Results · PD | PD on the FPGA | 0:30 |
| 11 | Results · BUS | BUS: equipment anomaly detection (정확도 A/B/C) | 0:30 |
| 12 | Results · BUS | BUS on the FPGA | 0:30 |
| 13 | Results · GRU | GRU: lossless CREST data compression (정확도 A/B/C) | 0:30 |
| 14 | Results · GRU | GRU on the FPGA | 0:30 |
| 15 | Results · System | One bitstream, three models | 0:40 |
| 16 | Future works | Toward the final version (Digital Twin I/II) | 0:40 |
| 17 | Summary | SIREN at a glance · Thank you | 0:15 |

- 합계 약 9:30. 리허설에서 넘치면 감축 순서
  1. 7번 요구조건 표를 15번 상태표로 합침
  2. 12번(BUS 하드웨어)을 11번에 합침 → DNN 5장
- Appendix (질문 대응, 10분 밖): Abstract · Why an FPGA SoC · Correctness · Fault management · Resources/timing/weight update

## 3. 정확도 3가지 판 (9, 11, 13번)

| 판 | 내용 | 근거 |
|---|---|---|
| A | PDR 시험셋 float 모델 vs 기준선 (제출 초록과 같은 수치) | PDR p.45–48, 초록 |
| B | 검증셋 float vs 정수 모델 (양자화 영향) | 공유 저장소 결과 문서 |
| C | A의 기준선 비교 + B의 정수화 변화량 한 줄 | 둘 다 |

- 빌드: `python tools/build.py --label alpha-1` → `dist/SIREN-KSS2026-alpha-1-{A,B,C}.pdf`
- 전력 측정 후 판 하나 선택, 나머지 블록 삭제

## 4. 그림 (SVG, `figures/`)

| ID | 슬라이드 | 내용 | 상태 |
|---|---|---|---|
| F01 | 2 | 원시 데이터 → SIREN → 정보 | 초안 |
| F02 | 3 | PADO-SAT 안 SIREN 위치 | 초안 |
| F03 | 4 | 방사선대, LEO, SEP | 초안 |
| F04a/b | 5 | ModernTCN 블록 · 예측 부호화 | 초안 |
| F05 | 6 | 데이터플로 vs 공유 엔진 | 초안 |
| F06 | 7 | Z7000-P3 블록도 | 초안 |
| F07 | 8 | 제작 흐름 6단계 | 초안 |
| F08 | 10 | PD 하이브리드 | 초안 |
| F09 | 12 | BUS FINN 데이터플로 + 가중치 경로 | 초안 |
| F10 | 14 | GRU 경로 | 초안 |
| F11 | 15 | 배치도 | **자리표시**. 배치 데이터로 SVG 생성 예정 |
| F12 | 16 | Digital Twin I/II | 초안 |

- `tools/make_figures.py`는 초안 생성용. 이후 SVG 파일이 원본 (Figma에서 편집 후 같은 이름으로 저장)
- Figma에서 SVG 내보낼 때 **Outline text 해제**

## 5. 제작 방식

- 캔버스 1280×720 CSS px = 13.333 × 7.5 in. **15 pt = 20 px**
- 글꼴 Arial (Windows·Figma 공통). 제목 40 px · 본문 28 px · 최소 20 px
- 강조: `<b class="k">` 핵심(파랑) · `<b class="ok">` 달성(초록) · `<b class="todo">` 미완(주황). SVG는 인라인 `font-weight`·`fill`
- 검사 (`tools/build.py`, 커밋 전 필수)
  - 렌더된 모든 글자 ≥ 20 px (HTML, SVG)
  - 글자가 슬라이드 밖이나 바닥글 영역으로 넘치지 않음
  - 금지어(내부 도구 이름, 내부 비트스트림 이름)
  - 저장소 전체 금지어·절대 경로
- PPTX: 다음 판에서 장별 고해상도 이미지 + 발표 노트로 생성 예정

## 6. 판과 일정

| 판 | 내용 | 시점 |
|---|---|---|
| alpha-0 | 계획, 저장소 | 10/18 |
| **alpha-1** | **First Draft 17장 + Appendix 5장, 정확도 A/B/C 3종 PDF** | **10/18** |
| alpha-2 | 지도교수 검토 반영, 배치도 SVG, PPTX, 발표 노트 | 검토 후 |
| beta-1 | 전력 측정 반영, 정확도 판 하나로 확정 | 측정 후 |
| v1.0 | 리허설 반영 최종판 | 10/26 이전 |

## 7. 남은 확인 사항

1. 발표자 표시: 초록 제1저자(Woncheol Joo)로 둠. 다르면 수정
2. PADO-SAT 로고 파일 (자리만 둠)
3. 정확도 판 A/B/C 선택 (전력 측정 후)
