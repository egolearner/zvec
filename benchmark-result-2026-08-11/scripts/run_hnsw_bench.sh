#!/bin/bash
# HNSW-RaBitQ: zvec (1bit / 1bit+refine / 7bit) vs Elasticsearch bbq_hnsw
# Datasets: cohere_medium_1m (768d cosine), gist-960-euclidean (960d L2)
set -x
cd /home/jiliang.ljl/project/zvec || exit 1

export PYTHONPATH=/tmp/zvec-pypath
export ZVEC_BENCH_PYTHON=/home/jiliang.ljl/.conda/envs/venv/bin/python
export ZVEC_BENCH_CPUS=0,2,4,6,8,10,12,14
export ZVEC_BENCH_BUILD_THREADS=8
export ZVEC_BENCH_SEARCH_THREADS=1,4,8,16
export ZVEC_BENCH_SEARCH_VALUES=32,64,128,256,512,1000
export ZVEC_BENCH_DURATION_SECONDS=10
export ZVEC_BENCH_REPEATS=3
export ZVEC_BENCH_WARMUP_QUERIES=1000

R=tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh
ROOT=/home/jiliang.ljl/benchmark-results/2026-08-11/hnsw_rabitq

run_dataset() {
  local tag="$1" dataset="$2" es_index="$3"
  export ZVEC_BENCH_DATASET="$dataset"
  export ZVEC_BENCH_ES_INDEX="$es_index"
  local base="$ROOT/$tag"

  # A + D: zvec 1-bit and ES build, then search (ES without rescore)
  export ZVEC_BENCH_WORK_DIR="$base/one-bit"
  bash $R build  --overwrite || return 1
  bash $R search || return 1
  echo "GROUP_AD_DONE_$tag"

  # B: reuse A's 1-bit index, query-side raw vector refine
  bash $R search --engines zvec --zvec-refine || return 1
  echo "GROUP_B_DONE_$tag"

  # C: zvec 7-bit, separate work dir
  export ZVEC_BENCH_WORK_DIR="$base/seven-bit"
  bash $R build  --engines zvec --zvec-total-bits 7 --overwrite || return 1
  bash $R search --engines zvec --zvec-total-bits 7 || return 1
  echo "GROUP_C_DONE_$tag"
  echo "DATASET_DONE_$tag"
}

run_dataset cohere /data/cohere_medium_1m cohere-medium-1m-hnsw-bbq
echo "HNSW_COHERE_EXIT=$?"

run_dataset gist /tmp/gist-960-euclidean.hdf5 gist-960-hnsw-bbq
echo "HNSW_GIST_EXIT=$?"

echo "HNSW_ALL_DONE"
