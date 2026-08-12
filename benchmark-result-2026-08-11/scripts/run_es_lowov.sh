#!/bin/bash
# Fill the ES bbq_hnsw Recall@10 0.80-0.95 band on Cohere:
# rescore_vector oversample 1.1 / 1.25 / 1.5 / 1.75
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

for ov in 1.1 1.25 1.5 1.75; do
  bash $R search --engines elasticsearch \
    --es-rescore-oversample "$ov" \
    --search-values 32,64,128,256,512,1000,2000 || exit 1
  echo "LOWOV_DONE_ov${ov}"
done

echo "LOWOV_ALL_DONE"
