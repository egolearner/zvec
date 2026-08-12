#!/bin/bash
# Cover ES bbq_hnsw Recall@10 0.80-0.86 on Cohere: rescore with small num_candidates.
# At num_candidates=32 even oversample=1.1 already reaches 0.858, so the remaining
# gap must be closed by lowering num_candidates (>= ceil(topk*oversample)).
set -x
cd /home/jiliang.ljl/project/zvec || exit 1

export PYTHONPATH=/tmp/zvec-pypath
export ZVEC_BENCH_PYTHON=/home/jiliang.ljl/.conda/envs/venv/bin/python
export ZVEC_BENCH_CPUS=0,2,4,6,8,10,12,14
export ZVEC_BENCH_BUILD_THREADS=8
export ZVEC_BENCH_SEARCH_THREADS=1,4,8,16
export ZVEC_BENCH_DURATION_SECONDS=10
export ZVEC_BENCH_REPEATS=3
export ZVEC_BENCH_WARMUP_QUERIES=1000
export ZVEC_BENCH_DATASET=/data/cohere_medium_1m
export ZVEC_BENCH_ES_INDEX=cohere-medium-1m-hnsw-bbq
export ZVEC_BENCH_WORK_DIR=/home/jiliang.ljl/benchmark-results/2026-08-11/hnsw_rabitq/cohere/one-bit

R=tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh

bash $R search --engines elasticsearch \
  --es-rescore-oversample 1.1 --search-values 12,16,20,24 || exit 1
echo "SMALLNC_DONE_ov1.1"

bash $R search --engines elasticsearch \
  --es-rescore-oversample 1.5 --search-values 16,20,24 || exit 1
echo "SMALLNC_DONE_ov1.5"

echo "SMALLNC_ALL_DONE"
