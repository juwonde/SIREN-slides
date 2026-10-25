# 요구조건 다음에 처리 장치 선정 설명 배치

## 자원·모드 표 후속 수정

5쪽의 두 열은 6쪽과 같은 남색 머리글과 회색 항목 셀을 사용하는 편집 가능한 표로 바꿨다. 왼쪽은 자원 / 설계 기준값, 오른쪽은 모드·기능 / 운용 배정 / 전력 한도로 구성했다. 에너지 24.80 Wh/orbit, APU 질량 160 g 및 마진 포함 176 g, CREST 데이터 50 MB/day 이하, 수신·전처리 10%·평균 3 W 이하를 유지했다. AI 추론은 사용자의 정정에 따라 **주 1회 이상, 주당 3분**, 최대 입력 5 W 이하로 고쳤다. 추론을 궤도당 2분으로 해석한 문구와 합산 에너지 0.66 Wh/orbit는 폐기했다. 수신·전처리는 약 0.49 Wh/orbit, 추론은 최대 0.25 Wh/week로 운용 주기를 나눠 표시했고, 스크립트도 같은 기준으로 고쳤다. 처리율·정확도·가중치 갱신 목표와 발표 시각은 유지했다.

## 인터페이스와 부록 재사용

FPGA 플랫폼과 외부 연결을 각각 한 페이지로 분리했다. 현재 6쪽은 왼쪽 제품 표, 오른쪽 `Why FPGA?`로 구성한다. 사용자 피드백에 따라 선정 이유는 **FPGA 기술의 충분한 Space Heritage → 높은 전력 효율성 → FPGA 기반 AI의 궤도 실증 이력** 순서로 정리했다. 첫 항목은 특정 COTS 모듈의 비행 검증 완료가 아니라 FPGA 기술 전반의 사용 이력이다. 전력 효율은 AI에 맞춘 병렬 연산, 온칩 데이터 재사용과 불필요한 스위칭 감소의 일반적인 장점으로 설명하며 자체 실험 수치는 넣지 않는다. 마지막 사례는 2021년 OPS-SAT의 FPGA 신경망 구름 판별 실증이다. 하단의 기존 세 항목은 제거했다. 7쪽은 숨김 슬라이드의 편집 가능한 원본 시스템 연결도를 크게 재사용했다. OBC와 SIREN은 CSP over CAN, SIREN과 S-band 통신기는 CSP over UART / RS-422로 연결하고 PDU가 전력을 공급한다. UART는 직렬 통신, RS-422는 전기 인터페이스 표기다. 원본 연결·화살표를 유지하고 통신기 철자와 RS-422 표기를 정리했다. 특정 OS 이식 완료처럼 보이지 않도록 복제 그림의 PS 기능은 Control · Fault Management로 표기했으며 숨김 원본은 유지했다.

숨김 슬라이드 `Onboard AI in Previous Missions`는 초록 다음인 부록 23쪽으로 옮겨 표시했다. OPS-SAT, Φsat-1, CogniSAT-6, BlueBON의 기존 표를 재사용하되 OPS-SAT은 ARM + FPGA, Φsat-1은 Myriad 2 VPU, BlueBON은 제조사 발표에 따라 TetraPLEX (GPU)로 표기했다. 완전한 문헌 조사처럼 보일 수 있는 희소성 주장은 빼고, 선택한 영상 처리 사례와 SIREN의 시계열 처리 초점을 설명한다. 출처 링크는 Figma 발표자 노트에 넣었다. 운용모드 통합 검토 30–32쪽을 추가한 현재 구성은 본편 20장, 부록 17장, 숨김 6장이다. 통합안 범위는 [별도 기록](mode-integration-appendix-20261024.md)을 따른다.

배치·배선 그림과 자원 표도 현재 대표 실행과 같은 10/22 설계로 맞췄다. LUT 60,285 (76.7%), FF 82,077 (52.2%), BRAM36 환산 177 (66.8%), DSP 356 (89.0%)이며, 주 클럭 150 MHz / 엔진 121.875 MHz에서 배선 후 WNS +0.072 ns, WHS +0.035 ns다. 부록 28쪽 자원 표와 29쪽 배치·배선 그림이 같은 설계를 가리킨다. 축약 근거는 `assets/hw/selected-chip-results.json`에 보관한다.

## 제품 표 후속 수정

6쪽 제품 소개는 경연대회 2차 발표자료(PDR) 본편 41쪽의 2열 표를 따라 제품 사진을 표 안에 넣었다. 현재 표는 제조사 Space Inventor, SoC XC7Z030 (ARM + FPGA), 저장장치 64 GiB eMMC, OS FreeRTOS / Linux, SIREN 통신 CAN·RS-422로 구성한다. 제품 전력과 임무 전력 예산을 혼동하지 않도록 5쪽의 전력 요구조건은 별도로 유지한다.

9쪽 해결 문구에는 `short-term risk forecast at t + 5 min`을 명시하고, 국문 스크립트도 t+5분 단기 예보로 맞췄다. 요약의 8:20 문구는 “현재는 5분 단기 예측 모델이지만, 앞으로는 1시간 이상 장기 예측이 가능하도록 발전시키겠습니다”로 바꿨다. 화면 문구도 `Forecasting horizon: 5 min → ≥ 1 h (target)`으로 맞췄다.

FPGA 선정 설명은 요구조건 바로 다음인 6쪽에서 Z7000-P3 제품 소개와 함께 설명하고, 외부 시스템 연결은 7쪽으로 분리했다. Figma 본편은 20장이다.

| 쪽 | 내용 |
|---|---|
| 5 | SIREN 자원·모드·전력 및 처리율 요구조건 |
| 6 | COTS platform: Z7000-P3; 제품 사진, ARM + FPGA 구성, 요구조건에 따른 FPGA 선택 이유 |
| 7 | SIREN system interfaces; 원본 연결도 |
| 8 | Payload design 구분 슬라이드 |
| 9–14 | 세 AI의 문제·해결·결과 |
| 15 | Three AIs on one FPGA SoC; 구체적인 구현 구조 |
| 16–20 | 전력·운용 결과, 향후 계획, 요약, 마무리 |

선정 근거는 FPGA의 충분한 Space Heritage, 높은 전력 효율, FPGA 기반 AI의 궤도 실증 순으로 정리했다. 마지막 근거는 Thales Alenia Space / CIAR가 2021년 OPS-SAT의 FPGA에 신경망을 올려 구름 판별을 수행한 사례다. Nano 비교 문단과 실측 결과는 넣지 않았다.

6쪽 스크립트는 2:30 / 2:40, 7쪽 연결 설명은 2:50으로 나누고 이후 누적 시각은 유지했다. 마지막 누적 시각은 8:50이다. 쪽번호와 `figma-script-ko-20261024.json`도 새 순서에 맞췄다. 기존 부록과 숨김 슬라이드는 유지했다.

## 제품·근거의 범위

- Space Inventor Z7000-P3: 위성용으로 상용 구매하는 COTS AI 처리 장치. 제조사 Preliminary Datasheet v0.1 (May 2020), pp.1–2에서 Zynq-7030 및 dual ARM Cortex-A9 + FPGA 구성을 확인했다. 그림은 기존 `assets/mission/siren-apu.png`와 같은 제품 그림을 재사용했다.
- 제품 소개와 요구조건 충족을 구분한다. 3 W mean / 5 W peak 및 모델별 50 GOPS는 SIREN의 요구조건이며, 제조사 사양만으로 비행 장치가 충족했다고 주장하지 않는다. 보드 시험 결과는 뒤의 슬라이드에 있다.
- AI via FPGA의 실증 근거는 [Thales Alenia Space의 2021년 OPS-SAT AI 실증](https://www.thalesaleniaspace.com/en/news/ai-test-success-ops-sat)이다. 첫 항목의 일반적인 FPGA 우주 사용 이력과 구분하며, XC7Z030 자체의 비행 이력으로 확대하지 않는다.
- 중복 논리는 FPGA에서 구성 가능한 보호 기능이다. 현재 SIREN에 TMR을 구현했다고 주장하지 않는다.

출처: [Space Inventor Z7000-P3 제조사 자료](https://satcatalog.s3.amazonaws.com/components/431/SatCatalog_-_Space_Inventor_-_Z7000-P3_-_Datasheet.pdf?lastmod=20210723042533), [ESA OPS-SAT](https://www.esa.int/Enabling_Support/Operations/OPS-SAT), [FINN-R](https://arxiv.org/abs/1809.04570), [AMD UG907](https://docs.amd.com/r/en-US/ug907-vivado-power-analysis-optimization/Power-Optimization-Feature).
