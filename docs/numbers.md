# 수치 원장 (alpha-1, 2026-10-18)

- 슬라이드의 모든 수치는 이 표에 먼저 적고 넣음
- 출처 표기
  - `PDR p.N`: 2026 큐브위성 경연대회 PDR 발표자료 본편 N쪽
  - `PDR 별첨 p.N`: 같은 발표 별첨자료 N쪽
  - `shared:`: 공유 저장소 `juwonde/pd-bus-moderntcn-fpga`의 파일
- 시험 보드: TE0715-05-71I33-L SoM + TE0705-04 carrier (XC7Z030, 비행 모델 Z7000-P3와 같은 칩). 모든 보드 결과는 이 시험 보드 기준

## 0. 채택 비트스트림 기준 (10/22 r31e, 슬라이드의 성능 수치는 모두 이것; 슬라이드 번호는 10/22 정리 뒤 본편 21장·부록 11장 기준)

- 요구조건 (사용자 10/21): **전처리 평균 ≤ 3.3 W, DNN(추론) peak ≤ 5.5 W** (12 V 보드 입력, peak = INA228 100 Hz 단일 샘플 최대, 전원 ON 뒤 50 ms의 돌입 샘플 제외), 모델마다 ≥ 50 GOPS
- 채택 (10/22): `bpg-r31e` (엔진 섬 PWM, F-ASTRA 6.142.0–6.145.0) 최종 구성 = CHANGELOG 6.145.0 r3 (PD PWM 램프, BUS → PD → Comp → 인코더). 모델 변형: PD FFN ReLU (검증셋 F1 0.7569 → 0.7615), Comp Δ 입력 + 7단계 세그먼트 일괄 실행. 대표 실행: >1000 pfu 주(2013-05-21–27) 전원 ON → 종료 사이클 `gt1000-r3boot` (10/22 02:5x UTC, guard60–62). 슬라이드 8·12의 정확도는 실수형 모델 테스트셋 값 그대로이며, 탑재형 변형(ReLU, Δ)의 테스트셋 지표는 아직 계산하지 않음

| 항목 | 값 | 출처 | 슬라이드 |
|---|---|---|---|
| GOPS | AI #2 62.51 / AI #1 51.00 / AI #3 50.92 (7회 엔진 실행 중 최소, 50.92–51.05), byte 일치, 패킷 무손실 | `runtime/power-tests/weekly-20261021/weekly-gt1000-r3boot.json` (F-ASTRA runtime) | 13, 15, 25, 20, 22 |
| 자원 (배선 후, bpg-r31e) | LUT 58,578 (74.5 %) · FF 79,886 (50.8 %) · BRAM36 179 (67.5 %) · DSP 372 (93.0 %); 여유 25.5 / 49.2 / 32.5 / **7.0 %** | `harness/bpg-r31e/route-utilization.rpt` | 28, 27 |
| 모델별 peak (대표 실행) | AI #2 4.61 · AI #1 4.89 · AI #3 4.92 W; 전원 ON → 종료 전체 4.92 W | 같은 기록 phases.max_w | 13, 17 |
| 전원 ON → 종료 (주간 NN 창) | 147.33 s (냉부팅 20.49 + 작업 117.57 + 종료 9.27), 608 J = 0.169 Wh, 평균 4.13 W; 단계별 전력량 부팅 18 · AI #2 0.1 · AI #1 105 · AI #3 34 · 종료 10 mWh; 저장소 읽기 약 4 s 미포함 | `mission-gt1000-r3boot.{json,-trace.csv}` → `tools/ops_windows.py` → `assets/ops/weekly-window.*` | 18 |
| 추론 전력량 (주간 NN 창 1회) | AI #2 0.1 mWh (0.08 s) · AI #1 105 mWh (86.23 s, 504회, 1회 0.21 mWh) · AI #3 34 mWh (29.67 s, GRU 4.34 s + 인코더) · NN 창 전체 505 J = 140 mWh (117.27 s) | weekly 기록 summary | 17 |
| AI #1 시간-전력 그림 | 86.2 s, 8,624 샘플, 최대 4.89 W, 평균 4.39 W, 누적 0.105 Wh | `boot-gt1000-r3boot-trace.csv` 115.076–201.306 s → `tools/pd_run_energy.py` → `assets/hw/pd-run-energy-r31e.*` | 17 |
| trade-off 산점도 | 전력 기록이 있는 비트스트림 13개(r21tzb–r32e)의 보드 실행 124회, 406점(모델별 byte 일치만). 진단 실험(가중치·입력을 0으로 둔 inv/ext/zd)은 제외. 10/19 이전 비트스트림(r04–r21tz)은 100 Hz 전력 기록이 없어 빠짐. 50 GOPS 아래 점은 페이싱·디더 진단 | `pareto-20261019/results.jsonl` + `weekly-20261021/weekly-*.json` + `harness/*/bundle.json`(bitstream_record_sha256) → `tools/tradeoff_data.py` → `assets/hw/tradeoff-points.json` → `tools/tradeoff_scatter.py` | 17 |
| 전처리 = 궤도 수신 창 (6.144.0) | 전원 차단 궤도 사이클 oc3: 냉부팅 20.54 s → CAN 수신 3.82 MB 580.6 s (6.68 KB/s) → 전처리 1.0 s + 저장 3.6 s → 종료 9.31 s = 616.0 s, 1,786 J = 0.50 Wh (부팅 18 · 수신 466 · 전처리+저장 4 · 종료 8 mWh), **평균 2.90 W (≤ 3.3 W)**, 최대 3.84 W(부팅); 궤도(100분) 평균 0.30 W | alpha-5 `runtime/power-stages/orbit/{oc3.out,orbit-results.jsonl}` + guard37 → `tools/orbit_cycle.py` → `assets/ops/orbit-oc3.*`, `orbit-window.*` | 13, 17, 18, 22 |
| 궤도당 에너지 (전력 예산 슬라이드) | 수신·전처리 10 % × 3.3 W + 추론 2 % × 5.5 W, 궤도 98.8분 → 약 0.72 Wh, 발전량 24.80 Wh의 2.9 % (계획 가정, 측정 아님) | 설계 단계 예산 가정 + 새 요구조건 | 5 |
| COMP 게이트 엔진 근거 | FINN LUT 20,978 vs 엔진 몫 1,069 (약 20배) | 엔진 매핑 receipt 문서 §3 | 25 |

이전 채택(10/21 e74, bpg-r30e): GOPS 62.57/51.01/50.71, peak 4.99/5.13/5.38 W, LUT 73.5 % — 아래 기록과 git 이력에 남김.

## 1. 확정 수치

| 항목 | 값 | 출처 | 슬라이드 |
|---|---|---|---|
| 궤도 가정 | 500 km SSO: 최근 누리호 발사 궤도 기준 가정. rideshare라 최종 궤도는 주탑재체에 따름 | 사용자 (10/18) | 6 |
| 처리 장치 | Space Inventor Z7000-P3, XC7Z030, 64 GiB eMMC | PDR p.9, 별첨 p.86 | 7 |
| 처리 장치 선정 기준 | 최대 ≤5 W, ≥50 GOPS, 캐리어 보드·차폐 포함 (Z7000-P3: 4.5 W peak, >100 GOPS 제조사 값) | PDR p.42 | 7 |
| 요구조건 SIR00100 | AI 추론 시 최대 5 W | PDR 별첨 p.19 | 7, 15 |
| 요구조건 SIR01000 | 지상 기준 모델 출력 일치율 ≥99 % | PDR 별첨 p.20 | 7, 15 |
| 요구조건 SIR01100 | ≥50 GOPS | PDR 별첨 p.20 | 7, 15 |
| 요구조건 SIR00900 | 전체 비트스트림 재전송 없이 가중치 갱신 | PDR 별첨 p.19 | 7, 12, 15 |
| 요구조건 SIR00300 | 6개월 30회, 회당 5분 구동 | PDR 별첨 p.19 | 7 |
| CREST 데이터량 | 최대 50 MB/day | PDR 별첨 p.17 (CRE00900) | 2 |
| 한 비트스트림 성능 (150 MHz) | BUS 62.58 / PD 70.88 / GRU 63.76 GOPS | shared: `docs/results/p1-summary-draft-2026-10-17.md` §2 (10/18 정식 세션) | 10, 12, 14, 15 |
| 모델별 단독 최대 | BUS 109.62 GOPS, PD 90.23 GOPS (각각 별도 비트스트림, 133.333 MHz) | shared: 같은 문서 §2 | 12, A5 |
| 자원 (150 MHz 정식 판, 배선 후) | LUT 61,022 (77.6 %) · FF 87,721 (55.8 %) · BRAM36 180.5 (68.1 %) · DSP 242 (60.5 %). 장치 총량 LUT 78,600 · FF 157,200 · BRAM36 265 · DSP 400 | shared: `docs/results/te0715-trustzone-t1-2026-10-17.md` | 15, A5 |
| 타이밍 (150 MHz 정식 판) | WNS +0.018 ns, WHS +0.023 ns (슬라이드에는 "timing met"만) | 같은 문서 | A5 |
| PD 엔진 매핑 근거 | FFN pointwise 쌍: FINN 3,822 ms vs 공유 엔진 119.8 ms (133.333 MHz 판 측정) | 엔진 매핑 receipt 결과 문서 (10/16) | 10, A7 |
| PD 전 corpus 일치 | 보드 출력 16,692개 digest 모두 정수 기준과 일치 | 같은 receipt 문서, 150 MHz 판 재확인 (10/17) | 10, A3 |
| BUS/PD 프레임 일치 | 1,391 / 1,391 프레임 byte 일치, 결함 0 | shared: `docs/results/te0715-one-bitstream-50gops-2026-10-11.md` | A3 |
| 상시 운용 soak | 3시간 10,260 batch byte 일치 | shared: `docs/results/p1-summary-draft-2026-10-17.md` §3 | A4 |
| 가중치 재적재 | BUS 3,368 words + PD LN 10,773 words, A/B 모두 byte 일치. COMP도 새 비트스트림 없이 교체 확인 (사용자 확인 10/18, 발표 "verified on all 3 models") | shared: `docs/results/te0715-weight-swap-2026-10-11.md` | 12, A6 |
| GRU 정수화 | 압축률 변화 −0.002 % (31일 합계 3.2463× → 3.2462×), 31일 모두 완전 복원 | shared: `docs/results/gru-model3-integer-2026-10-12.md` §2 | 13 |
| GRU 보드 step | 중앙값 33.6 µs | shared: `docs/results/gru-lossless-encoder-2026-10-17.md` | 14 |
| GRU 인코더 | ARM C 인코더 출력이 기준과 byte 일치, PC에서 정확히 복원 | 같은 문서 | 14 |
| 자원 여유 (150 MHz 정식 판) | LUT 22.4 % · FF 44.2 % · BRAM36 31.9 % · DSP 39.5 % 남음 (100 % − 사용률) | 위 자원 행에서 계산 | 20 |
| 배치 사용률 (150 MHz 판, 배치도) | 논리 사이트(SLICE·BRAM·DSP) 20,315개 중 빈 사이트 368개 → 98 %에 논리 배치. 격자 1,326칸(2열 × 5행) 중 절반 이상 빈 칸 25개 | 배치 export `sites.csv` (`tools/chip_map.py` 출력) | 18 |
| FM 검증 범위 | 설계 23개 항목 중 22개 보드 증거, 1개 시뮬레이션 | shared: `docs/results/p1-summary-draft-2026-10-17.md` §3 | 15, A4 |
| 온도 (방열판, 150 MHz) | 최고 74.7 °C | 보드 세션 기록 (10/17) | A5 |

## 2. 기준이 맞지 않는 수치 (확인 필요)

- PDR 정확도와 FPGA 정수 모델 평가는 데이터 분할, 판정 단위, 채널 구성이 서로 다름 → 한 표에 섞으면 안 됨

| 모델 | PDR 값 (float, PDR p.45–48) | 정수 모델 평가 값 | 차이 원인 |
|---|---|---|---|
| PD | F1 0.611, CSI 0.440 (기준선 CRATERRE F1 0.575, CSI 0.403) | 검증셋 F1 0.7570 (float 0.7563, 임계값 0.6) | 평가셋과 임계값 다름 |
| BUS | F1 0.889, 경보 12/14, 오경보 1 (기준선 EWMA F1 0.833, 10/14, 0) | EWMA 판정 F1 0.6301 (float 0.6205), 창 단위 F1 0.5199 (float 0.5267) | 사건 단위 vs 창 단위 판정 |
| GRU | CR 11.841, RMSE 0.58 counts, 최대 오차 ≤1.5 (기준선 NASA log CR 4, RMSE 1.89) | CR 3.2463× (28채널, 2022-01, **무손실**) | 데이터, 채널 수, 손실/무손실 방식 다름 |

- 제안 (기본안)
  - 7·9·11번: PDR 기준선 비교(float) + "float → 정수 변화량"을 따로 표시
  - 가능하면 정수 모델을 PDR 시험 방식 그대로 재평가해 한 표로 통일 (모델 담당과 확인)
- GRU는 현재 구현이 무손실. PDR의 RMSE/최대 오차 표현과 맞지 않음 → 11번 문구 결정 필요

## 3. 초록(제출본) 수치

- II-2-7 초록: PD F1 0.611, CSI 0.440 · BUS F1 0.889 · GRU CR 11.841, RMSE 0.58 count
- PDR 수치와 같음 → 정확도 판 A, Appendix Abstract에 사용

## 4. GRU 모델 구성 차이 (확인 필요)

- PDR 별첨 p.89: GRU 1층, hidden 32, 출력 C채널 (POES/MetOp 6채널)
- 현재 정수 구현: hidden 64(게이트 출력 192 = 3 × 64), 출력 28채널, 2022-01 데이터 31일
- 슬라이드 13번은 hidden 크기와 채널 수를 적지 않음. 판 확정 때 함께 정리

## 5. 아직 없는 수치

| 항목 | 상태 | 반영 판 |
|---|---|---|
| 추론 시 보드 전력 (12 V 입력, INA226) | 측정 전. 모니터 자체 전원과 별도 전원 팬은 제외해 보고 | beta-1 |
| 미세조정 후 가중치 업로드 크기 | 목표: 일부 계층 가중치만, 세 모델 합 ≤ 1 MB (사용자 계획). 현재 전체 패키지 14.94 MB, PD 부분 패키지도 BUS golden만 빠진 크기 | 공유 저장소 `docs/results/te0715-weight-partial-update-2026-10-17.md` §3 |
| Vivado 전력 추정치 | **사용하지 않음** (10/18 사용자 결정). 측정값만 표시 | — |
| 모델별 지연(ms/추론) | 상시 운용 평균 BUS 0.17 s, GRU 0.28 s, PD 1.26 s (FM 포함 batch 지연). 슬라이드용 정의 확정 필요 | alpha-1 |
