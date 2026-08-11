#!/bin/bash
# RUNBOOK 5.1: ES bbq_hnsw with rescore_vector oversample 2/3/5, both datasets
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

R=tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh
ROOT=/home/jiliang.ljl/benchmark-results/2026-08-11/hnsw_rabitq

run_rescore() {
  local tag="$1" dataset="$2" es_index="$3"
  export ZVEC_BENCH_DATASET="$dataset"
  export ZVEC_BENCH_ES_INDEX="$es_index"
  export ZVEC_BENCH_WORK_DIR="$ROOT/$tag/one-bit"
  for ov in 2 3 5; do
    local vals=32,64,128,256,512,1000,2000
    [ "$ov" = 5 ] && vals=64,128,256,512,1000,2000
    bash $R search --engines elasticsearch \
      --es-rescore-oversample "$ov" --search-values "$vals" || return 1
    echo "RESCORE_DONE_${tag}_ov${ov}"
  done
}

run_rescore cohere /data/cohere_medium_1m cohere-medium-1m-hnsw-bbq
echo "RESCORE_COHERE_EXIT=$?"

run_rescore gist /tmp/gist-960-euclidean.hdf5 gist-960-hnsw-bbq
echo "RESCORE_GIST_EXIT=$?"

echo "RESCORE_ALL_DONE"
