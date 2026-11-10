# FPGA (XC7Z030)

## FINN 결과 (`finn/`)

| 폴더 | 대상 | 내용 |
|---|---|---|
| `bus-ai2/` | AI #2 BUS HK 이상 탐지: 모든 층이 FINN 데이터플로 | `rtl/` FINN 생성 RTL, `stitch.json`, `ooc-150/` 단독 합성 보고서 (150 MHz) |
| `pd-layernorm-ai1/` | AI #1 입자 위험 탐지의 LayerNorm (나머지 연산은 공유 엔진) | `rtl/`, `stitch.json`, `connected-rtl.json` |
| `gru-tail-ai3/` | AI #3 Comp GRU 꼬리 | `rtl/`, `stitch.json`, `connected-rtl.json`, `ooc-125/` 보고서 (125 MHz) |
| `finn-rtllib/` | FINN 라이브러리 RTL (FIFO 등, BSD-3-Clause) | |

- 가중치·임계값 초기화 파일(`.mem`)과 ONNX 그래프는 넣지 않았습니다. RTL이 이 파일을 읽는 자리는 비어 있습니다.
- 공유 엔진 RTL은 넣지 않았습니다. 엔진은 아래 비트스트림에 들어 있습니다.

## 비트스트림 (`bitstream/r36v3n/`)

현재 채택된 설계입니다. 세 모델과 Fault Management를 비트스트림 하나에 넣었습니다.

| 항목 | 값 |
|---|---|
| 파일 | `accelerator.bit`, `accelerator.bin` (압축, 4.30 MB) |
| 대상 | xc7z030sbg485-1, Vivado 2020.2 |
| 클럭 | PS FCLK0 100 MHz, FCLK1 125 MHz (FINN·DMA·FM), 공유 엔진 150 MHz (MMCM) |
| 타이밍 | WNS +0.057 ns, WHS +0.033 ns |
| 자원 (칩 전체 대비) | LUT 56,674 (72.1 %), FF 69,441 (44.2 %), Slice 17,355 (88.3 %), BRAM 206 (77.7 %), DSP 360 (90.0 %) |
| 배치 여유 | 완전히 빈 칸 169 / 1,159 (14.6 %) |
| 그림 | `placement.svg` 배치도, `routing.png` 배선도 |

- 보드 검증: 출력이 소프트웨어 모델과 바이트 단위로 같고, Fault Management 고장 주입 46회가 모두 통과했습니다.
- `route-power.rpt`는 Vivado 추정값(무벡터)이며 측정값이 아닙니다. 측정 전력은 발표 슬라이드에 있습니다.
- 보고서 안의 로컬 경로는 `<local>/파일이름`으로 바꿨습니다.

모든 파일의 SHA-256은 `SHA256SUMS`에 있습니다.
