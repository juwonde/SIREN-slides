# 수치 원장 (alpha-1, 2026-10-18)

- 슬라이드의 모든 수치는 이 표에 먼저 적고 넣음
- 출처 표기
  - `PDR p.N`: 2026 큐브위성 경연대회 PDR 발표자료 본편 N쪽
  - `PDR 별첨 p.N`: 같은 발표 별첨자료 N쪽
  - `shared:`: 공유 저장소 `juwonde/pd-bus-moderntcn-fpga`의 파일
- 시험 보드: TE0715-05-71I33-L SoM + TE0705-04 carrier (XC7Z030, 비행 모델 Z7000-P3와 같은 칩). 모든 보드 결과는 이 시험 보드 기준

## 0. 채택 비트스트림 기준 (10/20, 슬라이드의 성능 수치는 모두 이것)

- 기준: 세 모델 모두 50 GOPS 이상·출력 byte 일치인 판 가운데 전력이 가장 낮은 판 (사용자 10/20). 아래 1절의 150 MHz 판 수치는 이 기준으로 대체됨. 전력은 모두 100 Hz 최대(peak) 기준으로 통일
- 판: `bpg-r27e`, 번들 `e27` (F-ASTRA 6.131.0, 엔진 클럭을 가속기 MMCM에서 받음, MMCM 1개). 대표 측정: 10/20 05:14 UTC 세션 `bpg-r27e-e27-e27-r13` (e26b와 연달아 잰 것). 발표에는 클럭 주파수를 적지 않음

| 항목 | 값 | 출처 | 슬라이드 |
|---|---|---|---|
| GOPS | BUS 62.57 (발표 표기 62.58) / PD 53.04 / COMP 50.82, byte 일치, FM 캠페인 모두 통과 | 세션 `board-models-e2e.json` (F-ASTRA runtime) | 14, 18, 24, 부록 |
| 자원 (배선 후) | LUT 58,372 (74.3 %) · FF 86,309 (54.9 %) · BRAM36 179 (67.5 %) · DSP 244 (61.0 %), WNS +0.092 ns | `bpg-r27e/route-utilization.rpt`, CHANGELOG 6.131.0 | 20, 부록 |
| 자원 여유 | LUT 25.7 · FF 45.1 · BRAM36 32.5 · DSP 39.0 % | 위 행에서 계산 | 20 |
| 추론 전력 (12 V 입력, 평균 / 100 Hz 최대) | BUS 5.02 / 5.46 · PD 5.26 / 5.79 · COMP 4.55 / 5.99 · FM 4.93 / 5.83 W. 같은 판 다른 두 세션의 100 Hz 최대: PD 5.97–6.00, COMP 5.91–6.10 | `runtime/power-tests/pareto-20261019/dnn-e27-r13.out`, CHANGELOG 6.131.0 | 21, 24 |
| 추론 전력량 (대표 세션) | BUS 0.0653 Wh (46.8 s, 1,395,900창) · PD 0.2562 Wh (175.3 s, 1,100회, 1회 0.233 mWh) · COMP 0.0047 Wh (3.7 s, 110블록 × 6,144 step) · 합 0.33 Wh | 같은 `.out`의 phases (seconds × mean_w) + 세션 frame 수 | 21 |
| 전처리 전력 (CAN 수신 + 전처리, PL 리셋, 이미지 r13) | 평균 3.12 W (최대 3.31) | `pareto-20261019/prep-r13.out` | 21 |
| PD 시간-전력 그림 | PD 구간(175.3 s, 17,534 샘플)의 최대 5.79 W 샘플(+137.6 s) 앞뒤 1 s. 계산 수준 ≈ 5.4 W(5.3 W 넘는 샘플 평균 5.41), LayerNorm 구간 ≈ 4.5 W | guard11 `power.csv` 구간만 읽음: `tools/pd_peak_trace.py` → `assets/hw/pd-tpower-e27.{svg,csv}` | 21 |
| trade-off 산점도 | 10/19–10/20 전력 측정 36세션 모두 (모두 byte 일치, COMP 100 Hz 최대는 10/19 오후부터 25세션). 세션별 PD/BUS/COMP 100 Hz 최대와 GOPS | `pareto-20261019/results.jsonl` (`phases.*.max_w`, `models.*.gops`) | 21 |
| PD FFN 엔진 근거 | FINN FFN 3,397 ms (150 MHz xsim 환산) > 보드 PD 한 프레임 전체 158.9 ms의 20배 → "FFN > 20× faster vs FINN" | 엔진 매핑 receipt 문서 §5 + 세션 `mean_frame_ns` | 18 |
| COMP 게이트 엔진 근거 | FINN LUT 20,978 vs 엔진 몫 1,069 (약 20배). 엔진 클럭을 낮춰 속도 이점은 없음 → "8 % faster" 삭제 | 엔진 매핑 receipt 문서 §3 | 18 |

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
