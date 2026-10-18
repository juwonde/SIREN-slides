# 작업 계획 (alpha-0, 2026-10-18)

## 0. 고정 조건

1. 제목: **Onboard AI Payload of PADO-SAT: SIREN**
2. DNN(BUS, PD, GRU)마다 1–2장, DNN 슬라이드 합계 최대 6장
3. 큐브위성 경연대회 PDR 발표자료 기반, 위성 전체가 아닌 **SIREN 중심**
4. 슬라이드 순서도 SIREN 중심으로 재구성
5. 내부 도구 이름 사용 금지. SIREN 제작 **방법**으로 설명
6. 임시 자료: 지금 **Alpha**, 전력 측정 후 최종판
7. 그림은 **SVG**(Figma 편집). 슬라이드는 HTML(headless) 작성 후 PDF/PPT 변환
8. 발표 **10분**, 슬라이드 **영어**
9. 모든 글자 **15 pt 이상**. 본문과 그림 안 글자 모두
10. 구성: **Introduction – Background – Results (Preliminary) – Future works**
11. Digital Twin 데이터 생성 진행 중
    - DT-I: CREST 데이터(PD, GRU 압축용). VERB-3D 개념, flux field 기반
    - DT-II: Bus HK 생성. 방법 미정
12. 모든 글 **개조식**. 슬라이드, 그림 라벨, Abstract 모두
13. **질문 대응용 Abstract** 포함. 마지막 슬라이드 뒤 Appendix. 10분 밖
14. 강조 단어 **Bold + 색상** 표시 가능. 슬라이드 HTML과 SVG에 같은 표기

## 1. 발표 흐름 (본편 15장, 10분)

- 시간 배분: Intro 1:45 · Background 2:15 · Results 5:00 · Future 0:45 · 마무리 0:15
- 장당 평균 약 40초. 발표 대본은 분당 약 130단어 기준 총 1,300단어 이내

| # | 구간 | 슬라이드 제목 (EN) | 핵심 내용 (개조식) | 그림 | 시간 |
|---|---|---|---|---|---|
| 1 | Title | Onboard AI Payload of PADO-SAT: SIREN | 부제 Space artificial Intelligence with Resource Efficient Network · 저자 · 소속 · "Preliminary (Alpha)" | 미션 패치/로고 (사용자 제공) | 0:15 |
| 2 | Intro | Why onboard AI on a CubeSat | LEO 대전입자 위험 · CREST 최대 50 MB/day vs 제한된 downlink · 원시 데이터 → 위험도/이상/압축 패킷 · 선행 임무(Φ-Sat-1, CogniSAT-6, OPS-SAT, BlueBON) 대비 위치 | F1 데이터 깔때기 + 선행 임무 띠 | 0:50 |
| 3 | Intro | PADO-SAT and SIREN | 3U CubeSat · 탑재체 CREST + SIREN · SIREN 3대 임무(PD 위험 탐지, BUS 이상 탐지, GRU 압축) · 성공 기준 Minimum/Basic/Extended | F2 위성 내 SIREN 위치 | 0:55 |
| 4 | Background | SIREN processing unit and requirements | Z7000-P3 (Zynq-7000 XC7Z030, ARM + FPGA) · OBC CAN, S-band RS-422 · 요구조건 ≤5 W, ≥50 GOPS, 지상 모델 일치율 ≥99 %, 비트스트림 교체 없는 가중치 갱신, FM · 시험 보드 TE0715 (동일 XC7Z030) | F3 처리 장치 블록도 | 0:45 |
| 5 | Background | Operation concept | 모드 Initial → Standby → Mission ↔ Safehold · 1회 구동 순서: TC → 데이터 수신 → PD → BUS → GRU (한 번에 한 모델) → OBC/S-band · FM 3단계 L0 HW / L1 SW / L2 지상 | F4 모드 + 구동 순서 | 0:40 |
| 6 | Background | How SIREN is built | ① GPU float 학습 ② 양자화 인지 학습(8-bit 가중치, 8/16-bit 활성, 혼합 정밀도) ③ 정수 기준 모델 = 정답 기준 ④ FINN 스트리밍 연산자 생성 + 일부 층만 공유 연산 엔진(빠르고 출력 동일할 때만) ⑤ 세 모델 + FM 가드 + SEM 한 비트스트림 ⑥ 보드 byte 일치 검증 · 원칙: 선언 정밀도 축소 금지 | F5 제작 흐름 | 0:50 |
| 7 | Results · PD | PD: high-energy particle risk detection | 입력 24 h 창, 5분 평균 count (POES/MetOp 12채널) · ModernTCN · 출력 t+5 min 위험도(0–1) · 기준선 CRATERRE 대비 F1/CSI · 양자화 후 정확도 유지 | F6 ModernTCN 블록, F12a 정확도 막대 | 0:50 |
| 8 | Results · PD | PD on the FPGA | LayerNorm 7개 FINN 스트림 + pw/dw/head 공유 엔진 · 같은 구조 FINN 대비 측정(예: FFN pw 3,822 ms → 119.8 ms) · 전 corpus byte 일치 · 150 MHz GOPS | F7 PD 하이브리드 데이터 흐름 | 0:50 |
| 9 | Results · BUS | BUS: equipment anomaly detection | 입력 Bus HK 1 h/3 h 창 (ESA-ADB) · ModernTCN · 이상 점수 · 기준선 EWMA 대비 F1, 경보/오경보 · 양자화 후 변화 | F6 재사용, F12b 정확도 막대 | 0:50 |
| 10 | Results · BUS | BUS on the FPGA | 전 층 FINN 데이터플로 · DSP 1개당 곱 2개 packing · 150 MHz GOPS · 단독 최대 109.6 GOPS · 비트스트림 교체 없는 가중치 재적재(A/B byte 일치) | F8 BUS 데이터플로 + 가중치 경로 | 0:40 |
| 11 | Results · GRU | GRU: lossless CREST compression | GRU 1층(hidden 32) 다음 값 예측 → 잔차 → zigzag → BZ2 · 지상도 같은 정수 예측기 → 완전 복원 · 기준선(NASA log) 대비 압축률 · 정수화 압축률 변화 −0.002 % | F9 압축 파이프라인, F12c 압축률 막대 | 0:50 |
| 12 | Results · GRU | GRU on the FPGA | 게이트 행렬곱 공유 엔진 → 정렬기 → FINN 꼬리 · step 중앙값 33.6 µs · ARM 패킷 인코더 byte 일치 · 150 MHz GOPS | F10 GRU 하드웨어 경로 | 0:40 |
| 13 | Results · System | One bitstream, three models | 세 모델 모두 >50 GOPS (150 MHz) · 자원 LUT/FF/BRAM/DSP % · FM 23개 중 22개 보드 확인 · 요구조건 상태표 (전력 **pending**) | F11 floorplan, F12d GOPS 막대 | 0:50 |
| 14 | Future works | Toward the final version | DT-I CREST (VERB-3D flux field → 검출기 응답 → count 시계열) · DT-II Bus HK (방법 검토 중) · DT 데이터로 재학습/미세조정 · 12 V 입력 전력 측정 → 최종판 · 비행 모델(Z7000-P3) 이식 | F13 Digital Twin 흐름 | 0:45 |
| 15 | Closing | Summary | 3줄 요약 · Thank you · 연락처 | — | 0:15 |

- DNN 슬라이드: 7–12, 합계 6장 (조건 2 상한)
- 리허설에서 시간 초과 시 첫 감축 후보: 10번(BUS 하드웨어)을 9번에 합침 → DNN 5장

## 2. Appendix (질문 대응, 10분 밖)

| # | 제목 (EN) | 내용 |
|---|---|---|
| A1 | Abstract | 배경 · 목표 · 방법 · 결과 · 향후 계획, 개조식 8–10줄 |
| A2 | Why an FPGA SoC | ≤5 W, 결정적 지연, SEM 기반 구성 메모리 보호 · Myriad/Jetson/DPU 대비 |
| A3 | Integer equivalence check | 정수 기준 모델 · 비교 corpus 규모 · byte 일치 판정 |
| A4 | Fault management | L0/L1/L2 23개 항목 · 보드 확인 범위 |
| A5 | Resources, timing, thermal | 자원 %, WNS, 150 MHz, 방열판 후 온도 |
| A6 | Onboard weight update | 가중치 파일 교체 경로 · 교체 가능 범위 · 데이터량 |
| A7 | Why a shared engine for PD | 같은 구조 FINN vs 엔진 측정, 매핑 조건 |

- 문서판 `docs/abstract.md`: Abstract 본문 + 예상 질문과 답(개조식)
- Appendix에는 DNN 상세 슬라이드를 넣지 않음 (조건 2 보호). 필요하면 사용자 확인 후 추가

## 3. 그림 목록 (SVG)

| ID | 슬라이드 | 내용 | 원천 |
|---|---|---|---|
| F1 | 2 | 데이터량 깔때기, 선행 임무 띠 | 새로 작성 |
| F2 | 3 | PADO-SAT 안 SIREN 위치 (CREST → OBC → SIREN → S-band) | PDR 별첨 85 재작성 |
| F3 | 4 | Z7000-P3 블록도 (PS, PL, CAN, RS-422, eMMC) | PDR 별첨 86 재작성 |
| F4 | 5 | 운용 모드 + 1회 구동 순서 | PDR 본편 운용 모드 재작성 |
| F5 | 6 | 제작 흐름 6단계 | 새로 작성 |
| F6 | 7, 9 | ModernTCN 블록 (DW Conv → ConvFFN1 → ConvFFN2, 잔차) | PDR 별첨 88 재작성 |
| F7 | 8 | PD 하이브리드: FINN LayerNorm ↔ 공유 엔진 | 공유 저장소 구조도 재작성 |
| F8 | 10 | BUS FINN 데이터플로 + AXI-Lite 가중치 경로 | 새로 작성 |
| F9 | 11 | GRU 예측 → 잔차 → zigzag → BZ2 → 패킷, 지상 복원 | PDR 별첨 89, 92 재작성 |
| F10 | 12 | GRU 엔진 G4 → 정렬기 → FINN 꼬리 | 공유 저장소 구조도 재작성 |
| F11 | 13 | 한 비트스트림 floorplan (모듈별 색) | 기존 floorplan 그림을 SVG로 재생성 |
| F12 | 7, 9, 11, 13 | 막대 그래프: 정확도·압축률 vs 기준선, GOPS vs 50 GOPS | 수치 원장에서 생성 |
| F13 | 14 | Digital Twin I/II 흐름 | 새로 작성 |

- 보드 사진(시험 장비)은 선택. 사진을 쓰면 SVG 안에 raster로 넣고 라벨은 SVG 글자로

## 4. 제작 방식

- 캔버스: 1280×720 CSS px = 13.333 × 7.5 in (PowerPoint 16:9). **15 pt = 20 px**
  - 글자 크기 기준: 제목 40 px(30 pt) · 본문 28 px(21 pt) · 최소 20 px(15 pt)
  - 그림 SVG도 슬라이드에 놓인 크기 기준으로 20 px 이상
- 글꼴: Inter (OFL, Figma 기본 제공). woff2를 `fonts/`에 포함해 오프라인 빌드
- 강조 표기 (조건 14)
  - HTML: `<b class="k">` 핵심어(파랑) · `<b class="ok">` 달성(초록) · `<b class="todo">` 미완/예정(주황)
  - SVG: `<tspan font-weight="700" fill="…">` 인라인 속성. Figma가 CSS class보다 인라인 속성을 안정적으로 읽음
  - 색상표는 `slides/style.css`와 `docs/style.md`에 한 번만 정의
- 개조식 (조건 12): 명사형 종결, 한 줄 한 정보, 문장부호 최소. 슬라이드당 본문 6줄 이내
- 빌드 (`tools/`)
  - `build`: headless Chrome으로 PDF(장당 1쪽)와 장별 PNG
  - `pptx`: pptxgenjs로 장별 고해상도 이미지 + 발표 노트 (발표장 PC 대비용)
  - Figma: `figures/*.svg`를 Figma로 가져와 편집 → 같은 이름으로 SVG 내보내기. 내보낼 때 **Outline text 해제** (글자 크기 검사 가능하도록)
- 검사 (커밋 전 필수)
  - 글자 크기: 렌더 후 모든 HTML 글자와 SVG 글자의 실제 크기 ≥ 20 px
  - 금지어: 내부 도구 이름, 내부 비트스트림/세션 이름
  - 절대 경로, 비밀 정보 없음
  - 수치 원장: 슬라이드의 수치마다 `docs/numbers.md` 항목 존재

## 5. 판과 일정

| 판 | 내용 | 시점 |
|---|---|---|
| alpha-0 | 계획, 저장소 뼈대 | 10/18 |
| alpha-1 | 15장 초안 전체 (그림 자리표시 포함), PDF | 계획 확정 후 1–2일 |
| alpha-2 | SVG 그림 완성, 글자 크기·금지어 검사 통과, 지도교수 검토용 | alpha-1 + 2일 |
| beta-1 | 전력 측정 결과 반영 (12 V 입력, INA226) | 측정 후 |
| v1.0 | 리허설 반영 최종판 | 학회 전 |

- 커밋/푸시: 판마다 + 큰 단위 작업마다. 판마다 tag, PDF는 GitHub release 첨부

## 6. 확인 필요

1. 학회 날짜, 세션, 발표 시간(10분에 질의응답 포함 여부)
2. 저자 목록, 발표자, 소속 표기, 로고/미션 패치 파일
3. 정확도 수치 기준 (아래 `docs/numbers.md` §2 불일치)
4. 13번 슬라이드에 Vivado 전력 추정치 표시 여부 (측정값 아님 표기 필수)
