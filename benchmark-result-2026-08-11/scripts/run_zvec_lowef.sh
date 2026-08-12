#!/bin/bash
# Rebuild the deleted Cohere zvec 1-bit HNSW index and add small-ef points so the
# Recall@10 0.80-0.86 band has a zvec counterpart to the new ES rescore curve.
# ef=32 is re-measured as a consistency check against the pre-deletion run
# (1 thread: recall 0.860 / 5361 QPS).
# Runs in a separate work dir so the existing 292-point file is never touched.
set -x
cd /home/jiliang.ljl/project/zvec || exit 1

export PYTHONPATH=/tmp/zvec-pypath
export ZVEC_BENCH_PYTHON=/home/jiliang.ljl/.conda/envs/venv/bin/python
export ZVEC_BENCH_CPUS=0,2,4,6,8,10,12,14
export ZVEC_BENCH_BUILD_THREADS=8
export ZVEC_BENCH_SEARCH_THREADS=1,4,8,16
export ZVEC_BENCH_SEARCH_VALUES=12,16,20,24,32
export ZVEC_BENCH_DURATION_SECONDS=10
export ZVEC_BENCH_REPEATS=3
export ZVEC_BENCH_WARMUP_QUERIES=1000
export ZVEC_BENCH_DATASET=/data/cohere_medium_1m
export ZVEC_BENCH_WORK_DIR=/home/jiliang.ljl/benchmark-results/2026-08-11/hnsw_rabitq/cohere/one-bit-lowef

R=tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh

bash $R build --engines zvec --overwrite || exit 1
echo "LOWEF_BUILD_DONE"

bash $R search --engines zvec || exit 1
echo "LOWEF_DONE_1bit"

bash $R search --engines zvec --zvec-refine || exit 1
echo "LOWEF_DONE_refine"

echo "LOWEF_ALL_DONE"
