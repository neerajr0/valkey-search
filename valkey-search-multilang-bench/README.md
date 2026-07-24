# valkey-search-multilang-bench

Standalone benchmarking harness for evaluating candidate tokenizer libraries for
[valkey-search](https://github.com/valkey-io/valkey-search) multi-language
full-text search support.

## Purpose

This repo answers one question per language: **which tokenizer produces the best
word boundaries for information retrieval?** It evaluates candidates in isolation
(no valkey-search dependency) so we can make confident library choices before
committing to integration.

## Architecture

The harness is split into three independent layers:

| Layer | What it Measures | Language | Status |
|-------|-----------------|----------|--------|
| 1 — Tokenizer Quality | Segmentation F1, OOV Recall, throughput | C++ | **In progress (Chinese)** |
| 2 — IR Retrieval Quality | MAP, NDCG@K, Recall@K, MRR | Python | Planned |
| 3 — Cross-System Performance | Latency, QPS, memory | Go/Python | Planned |

## Current Focus: Chinese

Candidate libraries:
- **cppjieba** (MIT, header-only) — MMSEG + HMM
- **Friso** (MIT, C) — MMSEG (Redis Search baseline)
- **ICU** (Unicode License) — BreakIterator + CjkBreakEngine

Comparison targets:
- OpenSearch SmartChinese (HMM + Viterbi)
- Redis Search (Friso)

## Quick Start

### Prerequisites

- CMake 3.16+
- C++20 compiler (GCC 11+ or Clang 14+)
- ICU 74+ development libraries (system or bundled)
- Internet access for dataset download

### Build

```bash
# Download datasets
./layer1/data/download_sighan.sh

# Fetch third-party dependencies
git submodule update --init --recursive

# Configure and build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j8

# Run Chinese benchmarks (all tokenizers)
./layer1/tokenizer_bench --tokenizer=all --language=zh \
    --corpus=../layer1/data/sighan2005/pku/raw.txt \
    --gold=../layer1/data/sighan2005/pku/gold.txt

# Run a single tokenizer
./layer1/tokenizer_bench --tokenizer=cppjieba --language=zh \
    --corpus=../layer1/data/sighan2005/pku/raw.txt \
    --gold=../layer1/data/sighan2005/pku/gold.txt
```

### Output

Results are written to `results/zh/` as JSON:
```json
{
  "tokenizer": "cppjieba",
  "corpus": "sighan2005_pku",
  "metrics": {
    "precision": 0.946,
    "recall": 0.938,
    "f1": 0.942,
    "oov_recall": 0.721,
    "tokens_per_sec": 1250000
  }
}
```

## Directory Structure

```
valkey-search-multilang-bench/
├── CMakeLists.txt              # Top-level build
├── layer1/                     # C++ Tokenizer Quality Harness
│   ├── CMakeLists.txt
│   ├── include/
│   │   └── tokenizer_interface.h
│   ├── tokenizers/
│   │   ├── icu_tokenizer.h/cc
│   │   ├── cppjieba_tokenizer.h/cc
│   │   └── friso_tokenizer.h/cc
│   ├── scoring/
│   │   ├── segmentation_scorer.h/cc
│   │   └── throughput_scorer.h/cc
│   ├── data/
│   │   ├── download_sighan.sh
│   │   └── README.md
│   └── main.cc
├── layer2/                     # Python IR Retrieval (future)
│   ├── eval/
│   │   ├── scorer.py
│   │   └── dataset_loader.py
│   ├── backends/
│   │   ├── standalone.py
│   │   └── opensearch.py
│   └── run.py
├── results/
│   └── zh/
├── scripts/
│   └── run_benchmark.sh
└── third_party/                # Git submodules
    ├── cppjieba/
    └── friso/
```

## Datasets

| Language | Dataset | Source | Size |
|----------|---------|--------|------|
| Chinese | SIGHAN 2005 Bakeoff (PKU + MSR) | [icwb2-data](https://github.com/yuikns/icwb2-data) | ~1.1M words |

## Evaluation Methodology

Each dataset provides human-annotated word boundaries. The harness feeds raw
(unsegmented) text to each candidate library, then compares predicted word
boundaries against gold boundaries at the character-offset level:

- **Precision**: correct_boundaries / predicted_boundaries
- **Recall**: correct_boundaries / gold_boundaries
- **F1**: harmonic mean of precision and recall
- **OOV Recall**: fraction of out-of-vocabulary words correctly segmented
- **Throughput**: tokens/sec on 100K-token batches

## License

BSD-3-Clause
