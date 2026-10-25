# SIREN research code

| 경로 | 내용 |
|---|---|
| [pd/](pd) | 고에너지 대전입자 위험 탐지: 3-block causal ModernTCN, count·유효성·시간 특징 생성 |
| [bus/](bus) | BUS HK 이상 탐지: ModernTCN, 채널 특징 생성, 이상 점수의 경보 변환 |
| [compression/](compression) | GRU 예측기, raw·delta·예측 잔차의 BZ2 압축, 지상 복원, C 인코더 |
| [preprocessing/](preprocessing) | Python·C의 POES 레코드 해석·유효값 처리·UTC 집계와 모델 입력 전처리 |

모델·알고리즘의 기본 소스입니다. 학습 가중치와 데이터는 별도로 제공해야 하며, 발표 수치의 재현에는 해당 평가 버전의 설정도 필요합니다.

```bash
python -m pip install -r research/requirements.txt
python -m research.check
```

C 코드는 Linux의 C11 컴파일러를 사용합니다. 인코더에는 libbz2 개발 패키지가 필요합니다.

```bash
mkdir -p build/research
cc -O2 -std=c11 research/compression/f_astra_gru_encode.c -lbz2 -o build/research/gru-encode
cc -O2 -std=c11 research/preprocessing/poes-sem-preprocess.c -lm -o build/research/poes-preprocess
cc -O2 -std=c11 research/preprocessing/f_astra_weekly_prep.c -lm -o build/research/mission-preprocess
python -m research.check --encoder build/research/gru-encode --preprocessor build/research/poes-preprocess
```

원본 소스: [PD ModernTCN](https://github.com/juwonde/SIREN-V9-Edge-Student/tree/e613d03cd12f1410695520707a78c9d66be734d6/src/cube_sat_v9), [BUS ModernTCN](https://github.com/juwonde/bus-hk-ad-v2/tree/05be9e5bcae0fba283b7dc706403ba98e1f2c7e6/src/bus_hk_ad_v2), [GRU·codec·C 전처리](https://github.com/juwonde/pd-bus-moderntcn-fpga/tree/a666e1219197503f40b5f5a3e32129ecd2c8b964/plugins/f-astra/f_astra). PD는 모델과 특징 생성 정의를 추출했고, GRU 모듈은 입력 검사 함수의 import만 독립시켰습니다. POES 레코드 전처리는 현재 연구 코드에서 가져왔습니다.
