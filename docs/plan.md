# 작업 계획 (alpha-13, 2026-10-18)

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
19. 저자는 초록 기준, **발표자만 강조** (발표자 Woncheol Joo 확인)
20. 작업 위치: F 드라이브
21. (alpha-2) **배선도**와 git의 **mermaid 연산기 구조도(요약)** 포함
22. (alpha-2) 모델마다 **GIF/이미지 시각화** (PDR 모델 슬라이드와 같은 방식)
23. (alpha-2) **Background 보강**, 전체적으로 **친절하게** (진행 표시, 약어 풀이)
24. (alpha-2) 글꼴: **깔끔한 세리프** → Cambria
25. (alpha-3) 슬라이드 아래 **정리 박스 넣지 않음** (AI가 만든 느낌)
26. PPT 변환은 **아직 하지 않음** (요청 시)
27. (alpha-4) Vivado 배선 화면 앞에 **깔끔한 배치도** 한 장: 화살표·그림 속 라벨 없음, 그리드, 흰 칸 = 빈 칸, 범례는 오른쪽
28. (alpha-4) 바닥글 왼쪽 문구 **PADO/SIREN**
29. SVG 관련 작업은 묻지 않고 진행
30. (alpha-5) 모든 모델: **Baseline 모델과 성능을 표로** 비교
31. (alpha-5) **Thank you 슬라이드 별도**
32. (alpha-6) Introduction **빌드업**: 문제 → 기존 탑재 AI와 빈 곳 → 접근 → 플랫폼
33. (alpha-6) 그림에 **제품명 쓰지 않음** ("SIREN AI Processing Unit", OBC, S-band transceiver)
34. (alpha-6) 배치 슬라이드와 배선 슬라이드 제목에 차이 표시 (Placement / Routing)
35. (alpha-6) Future works: 쉬운 표현, "Flux-field based" + Ref. VERB-3D (Subbotin & Shprits, 2009)
36. (alpha-6) **FINN 설명**: GPU = temporal (SIMT, 공유 SM) vs FINN = streaming (계층마다 블록) vs 공유 엔진 = single computation engine (Sze 2017, Venieris 2018, Umuroglu 2017)
37. (alpha-6) Fault management를 Appendix에서 **본문**으로
38. (alpha-6) 우주 환경 그림은 **자기권 모양만** (500 km SSO 궤도가 방사선대 안쪽으로 보이지 않게)
39. (alpha-6) 마지막 소속: **School of Space Research, Kyung Hee University** (우주탐사학과 공식 영문명)
40. (alpha-6) **TE0715 보드로 실험**한다는 내용을 본문에 표시
41. (alpha-7) 오른쪽 위 **구간 진행 표시 삭제**
42. (alpha-7) 실험 보드는 **TE0715 SoM + TE0705 carrier board** 둘 다 명시
43. (alpha-7) **전력 trade-off 그래프 자리** 한 장 (모델별 3개, 짧게)
44. (alpha-7) 구조도: git mermaid 그림을 **가로**로, 이 그림만 18 px 허용
45. (alpha-7) 표지·Thank you에 **QR 코드 자리**, Thank you에 이메일 juwonde@khu.ac.kr과 GitHub 두 개
46. (alpha-7) 빌드업: 기존 위성은 입자 검출기 + EO 또는 + 태양 관측으로 downlink 공유 → 실무에선 부족, PADO-SAT은 S-band 여유 → 탑재 AI 검증
47. (alpha-7) 빌드업: **in-situ 탐지** → 다음 단계 위성 전체 운용 변경·유인 임무, **Forecasting은 성능 문제로 검토 중**
48. (alpha-7) FM 슬라이드: **현재 구현 기준** 항목 요약, Appendix Abstract에도 한 줄
49. 작업 완료 후 저장소 정리, 공개 전환 예정 (사용자)
50. (alpha-8) 가중치 업로드: fine-tuning 후 **일부 계층만**, 세 모델 합 **≤ 1 MB** (목표)
51. (alpha-8) 배치·배선 슬라이드 **한 장으로** 합침 (사용자 허용), GOPS 막대·요구조건 표는 Summary로
52. (alpha-8) 전력 슬라이드: **GOPS vs (x)**, W별 Pareto front. x 추천: FPGA 자원 사용률 (사용자 결정 대기)
54. (alpha-10) 저장소 이름 **SIREN_2026_Fall_conf** (항공우주학회 발표에도 사용). Thank you의 GitHub 링크는 이 저장소 하나. 코드 공유 저장소는 공개하지 않음, 공개 적합 자료만 이 저장소로 옮김
61. (alpha-13) **참고문헌**: 각 페이지 왼쪽 아래 작은 글씨(14 px 허용, `.ref`)
62. (alpha-13) 지표마다 **⇑ 높을수록 좋음 / ⇓ 낮을수록 좋음** 표시
63. (alpha-13) COMP: 블록·채널마다 **raw · delta · GRU residual** 세 후보 중 BZ2 후 가장 작은 것 (8번 Mermaid 그림, 18번 글머리표)
64. (alpha-13) **Digital Twin 빌드업**: 4번(학습 데이터는 지금 대리 데이터 → Digital Twin), 모델 슬라이드(대리 데이터 표시), 23번(왜 필요한가)
65. (alpha-13) 13번 구조도 아래 **박스 설명** 8개 (18 px), SEM은 설명으로만
57. (alpha-12) 전력 슬라이드 x축 **LUT 사용률** 확정, 제목 "Performance trade-off per model"
58. (alpha-12) 모델 이름 GRU → **COMP** (네트워크 종류는 GRU 그대로). 제목: "BUS: Housekeeping Anomaly Detection", "COMP: Compression via Prediction", "PD: Particle Risk Detection", "Placement & Routing"
59. (alpha-12) 구조도(13번)와 9번 그림은 **Mermaid로 렌더** (화살표 자동 배치)
60. (alpha-12) 로고 세 개 **오른쪽 위 정렬, 보이는 크기 같게**. 휘장은 공식 AI 원본의 숨김 레이어가 SVG 변환에 섞여 잘려 보였음 → 투명 PNG
55. (alpha-11) 궤도 500 km SSO는 **가정** (최근 누리호 발사 궤도 기준, rideshare라 주탑재체에 따름)
56. (alpha-11) 20번에 **자원 여유 표** (LUT 22.4 %, FF 44.2 %, BRAM 31.9 %, DSP 39.5 % 남음)
53. (alpha-9) FM 슬라이드에 Safe 상태(Safe Output → Safehold)와 보호 영역(TrustZone T1 non-secure, golden 읽기 전용 + CRC), 페이지 추가 없음

## 1. 발표 정보 (초록집 기준)

- 2026 한국우주과학회 가을 학술대회, 구두 II-2 (우주응용활용/우주지상국, 운용), 아라(8층)
- II-2-7, 10월 27일(화) 17:25 (날짜는 초록집 순서로 추정)
- 저자: **Woncheol Joo**¹ (발표자, 확인됨), Subin Kim¹, Jeongin Yeo¹, Nayeong Hwang¹, Yangha Ju¹, Jiwon Lee¹, Young-Joo Song¹ ², Yong-Jae Moon¹ ²

## 2. 발표 흐름 (본편 25장: 제목 + 내용 23장 + Thank you)

| # | 구간 | 제목 (EN) |
|---|---|---|
| 1 | Title | Onboard AI Payload of PADO-SAT: SIREN |
| 2 | Introduction · Problem | Small science satellites share a narrow downlink |
| 3 | Introduction · Prior work | Onboard AI so far, and what is missing |
| 4 | Introduction · Approach | PADO-SAT and SIREN |
| 5 | Introduction · Platform | SIREN AI Processing Unit and requirements |
| 6 | Background · Space environment | Charged particles around a LEO CubeSat (자기권 그림) |
| 7 | Background · Spacecraft health | Anomaly detection in housekeeping telemetry |
| 8 | Background · Science data | Predictive compression of particle counts |
| 9 | Background · Deep learning | Time-series models in SIREN |
| 10 | Background · Hardware | Quantization: from float to integer |
| 11 | Background · Hardware | FINN: one hardware block per layer |
| 12 | Background · Method | How SIREN is built |
| 13 | Results · Architecture | SIREN hardware on one chip |
| 14–19 | Results · PD / BUS / GRU | 모델 + Baseline 표 / FPGA 구현 (DNN 6장) |
| 20 | Results · Chip | Placement and routing: where blocks sit, how they are wired |
| 21 | Results · Power | Power budget: GOPS vs resource use per model (그래프 자리) |
| 22 | Results · Reliability | Fault management as implemented |
| 23 | Future works | Toward the final version |
| 24 | Summary | SIREN at a glance (GOPS 막대, 요구조건 표) |
| 25 | Closing | Thank you (연락처, QR 자리) |

- 예상 시간 약 10분 40초 (내용 23장, 전력 슬라이드는 짧게)
- 더 줄이는 후보: 7·8번 합치기(−30초), 10번을 11·12번에 흡수(−30초)
- Appendix: Abstract · Why an FPGA SoC · Correctness · Resources/weight update · Abbreviations

## 3. 친절도 장치

- 약어는 처음 나오는 슬라이드 아래 회색 줄에 풀이, Appendix에 약어표
- 모델 결과 슬라이드: 그림(무엇을 하는지) → 숫자(얼마나 잘하는지) 순서
- 내부 용어 풀어 쓰기: pointwise (1×1) / depthwise conv, "outputs = integer reference"

## 4. 정확도 3가지 판 (12, 14, 16번)

| 판 | 내용 | 근거 |
|---|---|---|
| 공통 | Baseline vs SIREN 표 (PDR 시험셋, float): PD CRATERRE · BUS EWMA with robust threshold · GRU NASA 8-bit log | PDR p.48, 초록 |
| A | 공통 표만 | — |
| B | 공통 표 + 검증셋 float vs 정수 표 | 공유 저장소 결과 문서 |
| C | 공통 표 + 정수화 변화량 한 줄 | 같은 문서 |

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
| F16 | 18 | 그리드 배치도 (2열 × 5행 칸, 다수 기능 색, 흰 칸 = 절반 이상 빈 칸) | `tools/chip_map.py` + 배치 export |
| `assets/hw/routing-150mhz.png` | 19 | Vivado 배선 결과 | 공유 저장소 `docs/visual` |

- PDF에는 GIF 대신 대표 프레임(`*-still.png`), `build/present-*.html`에서는 GIF 재생
- **주의**: 모델 GIF/이미지와 배선도는 래스터라 글자 크기 자동 검사 대상이 아님. 축 눈금 글자가 15 pt보다 작음 → 원본 스크립트로 큰 글꼴 재생성 필요 (모델 담당)

## 6. 제작 방식

- 캔버스 1280×720 CSS px = 13.333 × 7.5 in. **15 pt = 20 px**
- 글꼴 **Cambria** (Windows·Office 기본, Figma 데스크톱에서 로컬 글꼴로 사용). 제목 40 px · 본문 28 px · 최소 20 px
- 강조: `<b class="k">` 핵심(파랑) · `<b class="ok">` 달성(초록) · `<b class="todo">` 미완(주황)
- 검사 (`tools/build.py`, 커밋 전 필수): 글자 ≥ 20 px, 슬라이드·바닥글 침범, 금지어, 절대 경로
- PPTX: 사용자가 요청할 때 진행

## 7. 판과 일정

| 판 | 내용 | 시점 |
|---|---|---|
| alpha-0 | 계획, 저장소 | 10/18 |
| alpha-1 | First Draft 17장, 정확도 A/B/C | 10/18 |
| alpha-2 | 이론 보강(본편 20장), 구조도·배선도·모델 GIF, 진행 표시, Cambria | 10/18 |
| alpha-3 | 아래쪽 정리 박스 제거, 발표자 확인 | 10/18 |
| alpha-4 | 그리드 배치도 슬라이드, 바닥글 PADO/SIREN | 10/18 |
| alpha-5 | 모델별 Baseline 표, Thank you 분리 | 10/18 |
| alpha-6 | Introduction 빌드업, FINN·양자화 슬라이드, FM 본문, 자기권 그림, 제품명 제거, 공식 소속명 | 10/18 |
| alpha-7 | 진행 표시 삭제, 가로 구조도, 전력 그래프 자리, QR·연락처, downlink·in-situ 빌드업, 구현 기준 FM | 10/18 |
| alpha-8 | 가중치 업로드 ≤ 1 MB 목표, 배치·배선 합침, Summary 상태표, 전력 슬라이드 축 | 10/18 |
| alpha-9 | FM 슬라이드: Safe 상태, 보호 영역 | 10/18 |
| alpha-10 | 저장소 이름 변경, 연락처 링크 하나 | 10/18 |
| alpha-11 | 궤도 가정 표시, 자원 여유 표 | 10/18 |
| alpha-12 | COMP 이름, 새 제목, Mermaid 그림, 로고 정렬, LUT 축 | 10/18 |
| **alpha-13** | **참고문헌, ⇑⇓ 표시, COMP 세 후보, Digital Twin 빌드업, 구조도 설명** | **10/18** |
| alpha-14 | 지도교수 검토 반영, 모델 그림 큰 글꼴 재생성, 공개 전 저장소 정리 | 검토 후 |
| beta-1 | 전력 측정 반영, 정확도 판 확정 | 측정 후 |
| v1.0 | 리허설 반영 최종판 | 10/26 이전 |

## 8. 남은 확인 사항

1. PADO-SAT 로고 파일 (자리만 둠)
2. 정확도 판 A/B/C 선택 (전력 측정 후)
3. 모델 GIF/이미지의 작은 축 글자 → 원본 스크립트로 재생성할지
