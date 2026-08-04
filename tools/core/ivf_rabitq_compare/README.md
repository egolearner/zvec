# IVF-RaBitQ C++ benchmark

This benchmark compares native 1-bit IVF-RaBitQ implementations in Zvec,
Faiss (`HR,IVF<nlist>,RaBitQfs`), and RaBitQ-Library.

All three executables load the same dataset, use the same distance, one query
per search call, identical `topk`, `nprobe`, build thread count, and the same
first `train-size` base vectors for training. The default input is
`.fvecs`/`.ivecs` with L2 distance. The Cohere 1M mode reads Zvec's internal
vecs base file and the original text query/ground-truth files. For cosine, the
common loader L2-normalizes base and query vectors once, then all three
implementations use their inner-product path; this is mathematically
equivalent to cosine similarity and keeps preprocessing outside measured
build/search time. They use 20 KMeans iterations, matching Zvec's current fixed
OptKMeans limit, where the implementation exposes that setting.

Zvec's IVF-RaBitQ builder uses its configured builder thread count to create
internal `IndexThreads` for KMeans training, centroid assignment, and cluster
encoding. The benchmark maps `--threads` to that builder parameter.

RaBitQ-Library requires external centroids and assignments. Its executable uses
Faiss KMeans and reports those costs as `train_seconds` and `assign_seconds`;
`encode_seconds` is the library's faster `IVF::construct` path. Search enables
the library's high-accuracy correction. Zvec's native builder does not expose
separate assignment timing or KMeans iteration control, so its assignment is
included in `encode_seconds`. Faiss fixes query quantization at 8 bits (`qb=8`);
all database codes are 1-bit. Before search, every implementation saves and
reloads its index so the QPS comparison does not mix Zvec's persisted-index
path with in-memory indexes from the other implementations.

The FHT/Kac rotator used by Zvec and RaBitQ-Library currently seeds itself from
`random_device` and exposes no seed parameter. Their exact recall can therefore
vary slightly between builds; Faiss HR and coarse KMeans use seed 12345. Run
multiple complete builds when measuring recall variance.

## Build

Clone Zvec with its submodules. Faiss and RaBitQ-Library are pinned under
`thirdparty/`, so no separate checkout or prebuilt vector-index library is
required.

```bash
git clone --recurse-submodules https://github.com/alibaba/zvec.git
git submodule update --init --recursive
```

Build the Zvec executable in the normal Zvec build:

```bash
cmake -B build.release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build.release ivf_rabitq_zvec_bench
```

Build the Faiss and RaBitQ-Library executables directly from the pinned
submodules. The pinned RaBitQ-Library fast-scan implementation requires an
x86-64 machine with AVX2 or AVX-512. The default shared SIMD target is AVX2; set
`-DFAISS_OPT_LEVEL=avx512` or
`-DFAISS_OPT_LEVEL=avx512_spr` only when every benchmark machine supports it.
The build applies the selected target to both Faiss and RaBitQ-Library so a
three-way comparison does not mix different SIMD levels. ARM/SVE is not
supported by the pinned RaBitQ-Library and is rejected at configure time.

```bash
cmake -S tools/core/ivf_rabitq_compare \
  -B build.ivf-rabitq-compare -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAISS_OPT_LEVEL=avx2
ninja -C build.ivf-rabitq-compare \
  ivf_rabitq_faiss_bench ivf_rabitq_library_bench
```

Required host packages are a C++ compiler with OpenMP, CMake 3.26 or newer,
Ninja, and BLAS/LAPACK development libraries.

## Prepare GIST

The C++ executables consume `fvecs`/`ivecs`. Convert an ANN benchmark HDF5 file
without loading the complete GIST base set into memory:

```bash
python -m pip install numpy h5py
python tools/core/ivf_rabitq_compare/hdf5_to_vecs.py \
  /path/to/gist-960-euclidean.hdf5 /data/gist
```

This produces `/data/gist/base.fvecs`, `/data/gist/query.fvecs`, and
`/data/gist/groundtruth.ivecs`.

## Prepare Cohere 1M

The Cohere mode expects these files in one directory:

```text
cohere_train_vector_1m.new.zvec.vecs
cohere_test_vector_1m.1000.new.txt
neighbors.txt
```

The directory is not hard-coded. Select the dataset with
`--dataset cohere-1m` and pass its location with `--dataset-dir`.

## Run

Use the same arguments for each executable:

```bash
THREADS=$(nproc)
export OMP_NUM_THREADS=$THREADS
export OPENBLAS_NUM_THREADS=$THREADS

COMMON_ARGS="--base /data/gist/base.fvecs \
--query /data/gist/query.fvecs \
--groundtruth /data/gist/groundtruth.ivecs \
--nlist 1024 --train-size 262144 --niters 20 \
--nprobes 1,2,4,8,16,32,64,128,256 \
--topk 10 --threads $THREADS --repeats 3 --warmup 20"

build.release/bin/ivf_rabitq_zvec_bench $COMMON_ARGS \
  --index /tmp/zvec-gist.ivf-rabitq
build.ivf-rabitq-compare/ivf_rabitq_faiss_bench $COMMON_ARGS \
  --index /tmp/faiss-gist.ivf-rabitq
build.ivf-rabitq-compare/ivf_rabitq_library_bench $COMMON_ARGS \
  --index /tmp/rabitq-library-gist.ivf-rabitq
```

Run Cohere 1M with cosine using the same configurable data directory on every
machine:

```bash
DATASET_DIR=/path/to/1m_zvec
THREADS=$(nproc)
export OMP_NUM_THREADS=$THREADS
export OPENBLAS_NUM_THREADS=$THREADS

COMMON_ARGS="--dataset cohere-1m --dataset-dir $DATASET_DIR \
--nlist 1024 --train-size 262144 --niters 20 \
--nprobes 1,2,4,8,16,32,64,128,256 \
--topk 10 --threads $THREADS --repeats 3 --warmup 20"

build.release/bin/ivf_rabitq_zvec_bench $COMMON_ARGS \
  --index /tmp/zvec-cohere-1m.ivf-rabitq
build.ivf-rabitq-compare/ivf_rabitq_faiss_bench $COMMON_ARGS \
  --index /tmp/faiss-cohere-1m.ivf-rabitq
build.ivf-rabitq-compare/ivf_rabitq_library_bench $COMMON_ARGS \
  --index /tmp/rabitq-library-cohere-1m.ivf-rabitq
```

Each build record reports `build_seconds` without serialization and
`total_seconds` including `dump_seconds`.

Search reports the best QPS across `--repeats` after `--warmup` queries. It is
therefore a warm-page throughput measurement, not a cold-start latency
measurement. Zvec searches its mmap-backed persisted index, while Faiss and
RaBitQ-Library load their persisted indexes into heap memory; save/reload makes
the lifecycle consistent, but their storage access paths remain different.

For a quick smoke test, append `--max-base 10000 --max-queries 100
--nlist 64 --train-size 10000 --nprobes 1,4,16`. Recall is emitted as `null`
when the base is truncated because the original ground truth is no longer
valid.
