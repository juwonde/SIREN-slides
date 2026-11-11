# SIREN

PADO-SAT의 온보드 AI 탑재체 SIREN 발표 자료입니다.

[PDF](https://github.com/juwonde/SIREN-slides/releases/latest/download/SIREN-slides.pdf) · [Figma](https://www.figma.com/deck/TYaGwNEZ2GewJvkSowhTiQ/SIREN-%C2%B7-KSSS-2026-Fall-%C2%B7-Slides?node-id=5-2&t=cUETSI8kuK4kP7eD-1) · [모델 코드](research) · [보드 코드](board) · [FPGA·FINN·비트스트림](fpga) · [본편](#main-slides) · [부록](#appendix)

## Main slides

### 01. Onboard AI Payload of PADO-SAT: SIREN

![Slide 01: Onboard AI Payload of PADO-SAT: SIREN](assets/slides/01.png?v=18e7571dd467)

### 02. Outline

![Slide 02: Outline](assets/slides/02.png?v=232609b014de)

### 03. Mission overview

![Slide 03: Mission overview](assets/slides/03.png?v=31d4ad2bb1d4)

### 04. Mission overview of PADO-SAT

![Slide 04: Mission overview of PADO-SAT](assets/slides/04.png?v=44c233ef1df3)

### 05. Goal of SIREN

![Slide 05: Goal of SIREN](assets/slides/05.png?v=13b84bc89837)

### 06. SIREN’s Budget & Requirements

![Slide 06: SIREN’s Budget & Requirements](assets/slides/06.png?v=5677b6cdb5a2)

### 07. FPGA COTS platform: Z7000-P3

![Slide 07: FPGA COTS platform: Z7000-P3](assets/slides/07.png?v=3bf8dbeef668)

### 08. SIREN-PADO SAT Interfaces

![Slide 08: SIREN-PADO SAT Interfaces](assets/slides/08.png?v=27d495914069)

### 09. Development & Validation of AI

![Slide 09: Development & Validation of AI](assets/slides/09.png?v=cf80a4420e58)

### 10. Effects of High Energy Charged Particles

![Slide 10: Effects of High Energy Charged Particles](assets/slides/10.png?v=9e6f7016bda2)

### 11. AI #1: Particle hazard detection

![Slide 11: AI #1: Particle hazard detection](assets/slides/11.png?v=9c973101d66b)

### 12. Anomalies in Satellite Housekeeping Data

![Slide 12: Anomalies in Satellite Housekeeping Data](assets/slides/12.png?v=68753f57d435)

### 13. AI #2: HK anomaly detection

![Slide 13: AI #2: HK anomaly detection](assets/slides/13.png?v=b43134572635)

### 14. Satellite Downlink Constraints

![Slide 14: Satellite Downlink Constraints](assets/slides/14.png?v=59306f772adc)

### 15. Why Compression?

![Slide 15: Why Compression?](assets/slides/15.png?v=247d87ae0271)

### 16. AI #3: Science data compression

![Slide 16: AI #3: Science data compression](assets/slides/16.png?v=18e4c95da863)

### 17. FPGA implementation

![Slide 17: FPGA implementation](assets/slides/17.png?v=5aae63696853)

### 18. Inference Optimization

![Slide 18: Inference Optimization](assets/slides/18.png?v=0da8bf3467bd)

### 19. FPGA Placement and Routing

![Slide 19: FPGA Placement and Routing](assets/slides/19.png?v=b6f79469648f)

### 20. Implementation & Performance

![Slide 20: Implementation & Performance](assets/slides/20.png?v=a45845e4fe7f)

### 21. Future Work and Summary

![Slide 21: Future Work and Summary](assets/slides/21.png?v=a5dc072a96a2)

### 22. Future Work

![Slide 22: Future Work](assets/slides/22.png?v=2405ac9f2ecf)

### 23. Summary

![Slide 23: Summary](assets/slides/23.png?v=e7d88a23ad68)

### 24. Thank you

![Slide 24: Thank you](assets/slides/24.png?v=50fd69558014)

## Appendix

### 25. Appendix

![Slide 25: Appendix](assets/slides/25.png?v=a0714f95ffef)

### 26. Abstract

![Slide 26: Abstract](assets/slides/26.png?v=9930bd63fd59)

### 27. Onboard AI in Previous Missions

![Slide 27: Onboard AI in Previous Missions](assets/slides/27.png?v=527cc8fef5a5)

### 28. Event Labels from Signal Processing

![Slide 28: Event Labels from Signal Processing](assets/slides/28.png?v=e40c8cb6f8ad)

### 29. AI Networks in SIREN: ModernTCN and GRU

![Slide 29: AI Networks in SIREN: ModernTCN and GRU](assets/slides/29.png?v=ac44fc8f6c41)

### 30. How SIREN is built

![Slide 30: How SIREN is built](assets/slides/30.png?v=0dfed1657b59)

### 31. Three models on the FPGA

![Slide 31: Three models on the FPGA](assets/slides/31.png?v=851501aa6dde)

### 32. AI #1 on the FPGA: ModernTCN layer by layer

![Slide 32: AI #1 on the FPGA: ModernTCN layer by layer](assets/slides/32.png?v=7ce2574c95c9)

### 33. AI #3 on the FPGA: one GRU step

![Slide 33: AI #3 on the FPGA: one GRU step](assets/slides/33.png?v=f6d901ac0bbe)

### 34. How is correctness verified?

![Slide 34: How is correctness verified?](assets/slides/34.png?v=f0e9a6581ebc)

### 35. Resources, timing and weight update

![Slide 35: Resources, timing and weight update](assets/slides/35.png?v=2f6a57cb9bd1)

### 36. Performance trade-off per model

![Slide 36: Performance trade-off per model](assets/slides/36.png?v=4648af4b3510)

### 37. Two modes versus one orbit mode

![Slide 37: Two modes versus one orbit mode](assets/slides/37.png?v=1a007ddb662e)

### 38. Fault management as implemented

![Slide 38: Fault management as implemented](assets/slides/38.png?v=cd32835c19d6)

### 39. Fault management items (1/2): hardware

![Slide 39: Fault management items (1/2): hardware](assets/slides/39.png?v=21bf07235487)

### 40. Fault management items (2/2): service and system

![Slide 40: Fault management items (2/2): service and system](assets/slides/40.png?v=cdb5e3a17015)

### 41. Digital Twin data and retraining

![Slide 41: Digital Twin data and retraining](assets/slides/41.png?v=e281d1aa8781)

### 42. COMP on POES/MetOp data

![Slide 42: COMP on POES/MetOp data](assets/slides/42.png?v=bd142fb65ed1)

### 43. COMP on POES by the AI #1 event criterion

![Slide 43: COMP on POES by the AI #1 event criterion](assets/slides/43.png?v=faa1993c4cf4)

### 44. Abbreviations

![Slide 44: Abbreviations](assets/slides/44.png?v=a03bafd9aaf7)
