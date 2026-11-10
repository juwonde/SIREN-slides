# 보드 코드 (C)

TE0715 시험 보드(Zynq-7000 XC7Z030)의 ARM(PS)에서 도는 SIREN 비행 코드입니다. 궤도마다 한 번 켜서 데이터를 다운로드하고, 세 AI를 FPGA에서 실행한 뒤 결과를 업로드하고 끕니다. 모든 단계에서 Fault Management가 함께 돕니다.

| 역할 | 파일 |
|---|---|
| 부팅과 운용 창 | `f_astra_boot.c`, `f_astra_duty_early.c`, `f_astra_daily_run.c`, `f_astra_daily_window.c`, `f_astra_floor.c`, `f_astra_power_profile.c`, `f_astra_flight_profile.c`, `f_astra_link_off.c`, `f_astra_watchdog.c` |
| 데이터 다운로드 (OBC → SIREN, CSP over CAN) | `te0715-csp-v1-node.c`, `f_astra_orbit_prep.c`, `f_astra_pd_zt.c` |
| NN 실행 (FPGA) | `directrtl_models_runner.c`, `directrtl_hybrid_runner.c`, `directrtl_engine_runner.c`, `directrtl_dma_runner.c`, `f_astra_finn_dma_pool.c`, `f_astra_axilite_load.c`, `f_astra_weights.c` |
| 결과 업로드 | `f_astra_decide.c` (AI #1·#2 판정 16 B → OBC, CAN), `f_astra_gru_batch.c`, `f_astra_gru_encode.c` (AI #3 Comp 패킷 → 송수신기, UART) |
| Comp PS 경로 (비교·시험) | `f_astra_gru_ps.c`, `f_astra_gru_sequence.c` |
| Fault Management | `f_astra_fm_service.c`, `f_astra_fm_duty.c`, `f_astra_fm_schedule.c`, `f_astra_fm_core.c`, `monitor/` |
| 무결성과 보호 | `f_astra_image_verify.c`, `f_astra_ed25519.c`, `f_astra_tz.c` |
| 공통 헤더 | `f_astra_pl_io.h`, `f_astra_gt.h`, `f_astra_fm_core.h`, `f_astra_ed25519.h` |

- 출처: F-ASTRA 6.227.1의 커밋된 소스를 그대로 옮겼습니다.
- 빌드: ARM 교차 컴파일러(`arm-linux-gnueabihf-gcc -std=gnu99 -O2`). 파일마다 머리말에 빌드 방법과 입력 형식을 적어 두었습니다.
- 빠진 것: 시험·벤치마크 코드, 모델 가중치와 설정 파일, 키.
