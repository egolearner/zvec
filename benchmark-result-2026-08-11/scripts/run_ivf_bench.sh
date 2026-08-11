#!/bin/bash
# IVF-RaBitQ: zvec vs faiss, GIST + Cohere, total-bits 1 / 7
set -x
cd /home/jiliang.ljl/project/zvec || exit 1

export OMP_NUM_THREADS=16
export OPENBLAS_NUM_THREADS=16

OUT=/home/jiliang.ljl/benchmark-results/2026-08-11/ivf_rabitq
IDX=/tmp/bench/idx
mkdir -p "$OUT" "$IDX"

COMMON="--nlist 1024 --train-size 262144 --niters 20 \
--nprobes 1,2,4,8,16,32,64,128,256 \
--topk 10 --threads 16 \
--repeats 3 --warmup 50 \
--search-threads 1,4,8,16"

run_set() {
  local tag="$1"; shift
  local data="$*"
  for bits in 1 7; do
    for e in "build.gcc12/bin/ivf_rabitq_zvec_bench zvec" \
             "build.ivf-rabitq-compare/ivf_rabitq_faiss_bench faiss"; do
      set -- $e
      echo "=== $tag $2 ${bits}bit $(date +%T) ==="
      ./$1 $data $COMMON --total-bits $bits \
        --index "$IDX/$tag-$2-$bits.ivf" \
        > "$OUT/$tag-$2-${bits}bit.jsonl" 2>"$OUT/$tag-$2-${bits}bit.err"
      echo "exit=$? $(date +%T)"
    done
  done
}

run_set cohere "--dataset-dir /data/cohere_1m"
echo "IVF_COHERE_DONE"

run_set gist "--base /tmp/gist/vecs/base.fvecs --query /tmp/gist/vecs/query.fvecs --groundtruth /tmp/gist/vecs/groundtruth.ivecs"
echo "IVF_GIST_DONE"

# index sizes
{
  for f in "$IDX"/*.ivf; do
    printf "%-32s %10.1f MiB\n" "$(basename "$f")" \
      "$(echo "scale=1; $(stat -c%s "$f")/1048576" | bc)"
  done
} > "$OUT/index_sizes.txt"
cat "$OUT/index_sizes.txt"
echo "IVF_ALL_DONE"
