# Onboard AI Payload of PADO-SAT: SIREN

한국우주과학회 2026 가을 학술대회 발표 자료입니다. PADO-SAT의 온보드 AI 탑재체 SIREN이 수행하는 고에너지 대전입자 위험 탐지, BUS HK 이상 탐지, 과학데이터 압축과 초기 구현 결과를 소개합니다.

**2026-10-25 공개본: 본편 20장 + 부록 17장.** 아래에서 전체 슬라이드를 볼 수 있습니다.

- [전체 슬라이드 PDF (37장)](https://github.com/juwonde/SIREN_2026_Fall_conf/releases/download/slides-2026-10-25-r2/SIREN-KSS2026-slides-20261025.pdf)
- [현재 릴리스](https://github.com/juwonde/SIREN_2026_Fall_conf/releases/tag/slides-2026-10-25-r2)
- [Figma Slides 원본](https://www.figma.com/slides/TYaGwNEZ2GewJvkSowhTiQ/SIREN)
- [본편](#main-slides) · [부록](#appendix) · [자료 구성](#source-and-data)

PDF와 슬라이드 이미지는 발표자 노트를 포함하지 않습니다. Figma의 숨김 슬라이드는 이 공개본에서 제외했습니다.

## Main slides

### 01. Onboard AI Payload of PADO-SAT: SIREN

![Slide 01: Onboard AI Payload of PADO-SAT: SIREN](assets/slides/01.png)

### 02. Mission overview

![Slide 02: Mission overview](assets/slides/02.png)

### 03. PADO-SAT: mission overview

![Slide 03: PADO-SAT: mission overview](assets/slides/03.png)

### 04. Goal of SIREN

![Slide 04: Goal of SIREN](assets/slides/04.png)

### 05. System budget from SIREN’s perspective

![Slide 05: System budget from SIREN’s perspective](assets/slides/05.png)

### 06. FPGA COTS platform: Z7000-P3

![Slide 06: FPGA COTS platform: Z7000-P3](assets/slides/06.png)

### 07. SIREN-PADO SAT Interfaces

![Slide 07: SIREN-PADO SAT Interfaces](assets/slides/07.png)

### 08. Payload design

![Slide 08: Payload design](assets/slides/08.png)

### 09. Effects of High Energy Charged Particles

![Slide 09: Effects of High Energy Charged Particles](assets/slides/09.png)

### 10. AI #1: Particle hazard detection

![Slide 10: AI #1: Particle hazard detection](assets/slides/10.png)

### 11. Anomalies in Satellite Housekeeping Data

![Slide 11: Anomalies in Satellite Housekeeping Data](assets/slides/11.png)

### 12. AI #2: HK anomaly detection

![Slide 12: AI #2: HK anomaly detection](assets/slides/12.png)

### 13. Satellite Downlink Constraints

![Slide 13: Satellite Downlink Constraints](assets/slides/13.png)

### 14. AI #3: Science data compression

![Slide 14: AI #3: Science data compression](assets/slides/14.png)

### 15. Three AIs on one FPGA SoC

![Slide 15: Three AIs on one FPGA SoC](assets/slides/15.png)

### 16. Performance trade-off per model

![Slide 16: Performance trade-off per model](assets/slides/16.png)

### 17. Download and weekly NN tests

![Slide 17: Download and weekly NN tests](assets/slides/17.png)

### 18. Future Works and Summary

![Slide 18: Future Works and Summary](assets/slides/18.png)

### 19. Summary

![Slide 19: Summary](assets/slides/19.png)

### 20. Thank you

![Slide 20: Thank you](assets/slides/20.png)

## Appendix

### 21. Appendix

![Slide 21: Appendix](assets/slides/21.png)

### 22. Abstract

![Slide 22: Abstract](assets/slides/22.png)

### 23. Onboard AI in Previous Missions

![Slide 23: Onboard AI in Previous Missions](assets/slides/23.png)

### 24. AI Networks in SIREN: ModernTCN and GRU

![Slide 24: AI Networks in SIREN: ModernTCN and GRU](assets/slides/24.png)

### 25. How SIREN is built

![Slide 25: How SIREN is built](assets/slides/25.png)

### 26. Three models on the FPGA

![Slide 26: Three models on the FPGA](assets/slides/26.png)

### 27. How is correctness verified?

![Slide 27: How is correctness verified?](assets/slides/27.png)

### 28. Resources, timing and weight update

![Slide 28: Resources, timing and weight update](assets/slides/28.png)

### 29. Placement, Routing

![Slide 29: Placement, Routing](assets/slides/29.png)

### 30. One orbit, one operating window

![Slide 30: One orbit, one operating window](assets/slides/30.png)

### 31. AI inference, result transfer and shutdown

![Slide 31: AI inference, result transfer and shutdown](assets/slides/31.png)

### 32. Weekly batch versus orbit integration

![Slide 32: Weekly batch versus orbit integration](assets/slides/32.png)

### 33. Fault management as implemented

![Slide 33: Fault management as implemented](assets/slides/33.png)

### 34. Fault management items (1/2): hardware

![Slide 34: Fault management items (1/2): hardware](assets/slides/34.png)

### 35. Fault management items (2/2): service and system

![Slide 35: Fault management items (2/2): service and system](assets/slides/35.png)

### 36. Digital Twin data and retraining

![Slide 36: Digital Twin data and retraining](assets/slides/36.png)

### 37. Abbreviations

![Slide 37: Abbreviations](assets/slides/37.png)

## Source and data

| 경로 | 내용 |
|---|---|
| [docs/slide-index.json](docs/slide-index.json) | 공개 순서, 제목, 이미지 크기와 SHA-256 |
| [assets/slides/](assets/slides/) | README와 PDF에 사용한 전체 슬라이드 이미지 |
| [docs/numbers.md](docs/numbers.md) | 발표 수치와 근거. 상단 갱신 기록을 우선 |
| [docs/power-selection-20261024.md](docs/power-selection-20261024.md) | 대표 전력·운용 시험의 선택과 측정 범위 |
| [docs/mode-integration-appendix-20261024.md](docs/mode-integration-appendix-20261024.md) | 부록 운용모드 통합 검토의 조건과 한계 |
| [assets/](assets/) · [figures/](figures/) | 관측 자료 기반 그림, 모델 시각화, 축약 측정 결과 |
| [tools/](tools/) | 그림 생성과 공개본 검사 도구 |
| [docs/legacy-html-workflow.md](docs/legacy-html-workflow.md) | 이전 HTML 초안의 생성 방법과 이력 |

현행 발표본은 위 Figma 스냅샷과 PDF입니다. 기존 slides/deck.html과 과거 릴리스는 이전 초안으로 보존합니다.

### Rebuild the public snapshot

Python 3와 Pillow가 필요하며 PDF 생성에는 ReportLab이 추가로 필요합니다.

```bash
python tools/publish_slides.py --check
python tools/publish_slides.py --pdf
```

PDF 결과는 dist/SIREN-KSS2026-slides-20261025.pdf이며 GitHub 릴리스에 첨부합니다. 원고, 발표자 노트, 인쇄 연습본과 임시 빌드 결과는 공개본에 넣지 않습니다.
