# IVF-RaBitQ Benchmark: zvec vs Faiss vs RaBitQ-Library

GIST-1M（1,000,000 × 960，L2），1-bit 与 7-bit 两种量化位宽，含多线程检索吞吐扫描。

## 1. 测试配置

| 项 | 值 |
|---|---|
| 数据集 | GIST 1M × 960（`gist-960-euclidean.hdf5`），1000 条 query，groundtruth top-100 |
| 索引 | IVF，nlist=1024 |
| 训练 | train-size=262144（=nlist×256，三方一致且均不触发二次采样），niters=20，seed 固定 |
| 量化 | 1-bit（纯符号位，无 rerank）/ 7-bit（1 符号位 + 6 附加位，带 extra-bit rerank） |
| 距离 | L2；topk=10；Faiss query 量化 qb=8 |
| 构建线程 | 16 |
| 检索线程 | 1 / 4 / 8 / 16（并发查询，每线程独立上下文；各库内部 OpenMP 均锁为 1） |
| 计时 | warmup 50，repeats 3 取最优（热页吞吐） |
| 编译 | 统一 gcc-toolset-12 + AVX-512（zvec `RABITQ_ENABLE_AVX512=ON`，外部 `FAISS_OPT_LEVEL=avx512`） |
| 硬件 | 16 核（icelake-server），61 GB 内存 |

zvec 版本：分支 `ivf-rabitq-bench` @ `1196875`（含 1-bit 扫描快速拒绝、group-by 快速拒绝、query context 缓冲复用、query LUT 缓冲复用）。

## 2. 构建性能（16 线程，秒）

### 1-bit
| engine | train | assign | encode | build | dump | **total** |
|---|---|---|---|---|---|---|
| zvec | 108.37 | 0.00 | 11.35 | 119.73 | 0.19 | **119.92** |
| faiss | 42.12 | 0.00 | 9.81 | 51.94 | 0.05 | **51.98** |
| rabitq-library | 36.57 | 6.99 | 0.26 | 43.82 | 0.05 | **43.87** |

### 7-bit
| engine | train | assign | encode | build | dump | **total** |
|---|---|---|---|---|---|---|
| zvec | 107.76 | 0.00 | 11.87 | 119.63 | 1.12 | **120.74** |
| faiss | 42.16 | 0.00 | 41.41 | 83.57 | 0.28 | **83.85** |
| rabitq-library | 36.64 | 7.02 | 0.52 | 44.18 | 0.27 | **44.45** |

要点：
- zvec 构建为最慢，**瓶颈完全在 kmeans train（≈108s，占 total 的 90%）**，是 Faiss 的 2.6×、RaBitQ-Library 的 2.9×。encode 与 dump 均正常。
- 位宽对 zvec/RaBitQ-Library 的 encode 几乎无影响；**Faiss 的 encode 从 9.8s 涨到 41.4s**（多-bit 编码开销大），导致其 7-bit 构建总耗时增加 61%。

## 3. 检索吞吐 QPS

### 1-bit
| 线程 | nprobe | zvec | faiss | rabitq-library | zvec/faiss | zvec/rabitq |
|---|---|---|---|---|---|---|
| 1 | 1 | 6212 | 5733 | 6503 | 1.08× | 0.96× |
| 1 | 4 | 4294 | 4176 | 4720 | 1.03× | 0.91× |
| 1 | 16 | 2222 | 2112 | 2526 | 1.05× | 0.88× |
| 1 | 64 | 793 | 745 | 927 | 1.06× | 0.86× |
| 1 | 256 | 242 | 214 | 277 | 1.13× | 0.87× |
| 4 | 1 | 26192 | 15296 | 27702 | 1.71× | 0.95× |
| 4 | 4 | 17944 | 12424 | 20418 | 1.44× | 0.88× |
| 4 | 16 | 9272 | 7124 | 10750 | 1.30× | 0.86× |
| 4 | 64 | 3234 | 2598 | 3879 | 1.24× | 0.83× |
| 4 | 256 | 981 | 752 | 1154 | 1.30× | 0.85× |
| 8 | 1 | 50486 | 19947 | 53014 | 2.53× | 0.95× |
| 8 | 4 | 35358 | 16420 | 39851 | 2.15× | 0.89× |
| 8 | 16 | 17653 | 10373 | 21233 | 1.70× | 0.83× |
| 8 | 64 | 6288 | 4138 | 7668 | 1.52× | 0.82× |
| 8 | 256 | 1920 | 1182 | 2249 | 1.62× | 0.85× |
| 16 | 1 | 63189 | 21571 | 75761 | 2.93× | 0.83× |
| 16 | 4 | 43823 | 19156 | 53254 | 2.29× | 0.82× |
| 16 | 16 | 20711 | 11997 | 25376 | 1.73× | 0.82× |
| 16 | 64 | 7175 | 4967 | 8824 | 1.44× | 0.81× |
| 16 | 256 | 2166 | 1514 | 2593 | 1.43× | 0.84× |

### 7-bit
| 线程 | nprobe | zvec | faiss | rabitq-library | zvec/faiss | zvec/rabitq |
|---|---|---|---|---|---|---|
| 1 | 1 | 5370 | 3528 | 5570 | 1.52× | 0.96× |
| 1 | 4 | 3599 | 2049 | 4002 | 1.76× | 0.90× |
| 1 | 16 | 1927 | 899 | 2213 | 2.14× | 0.87× |
| 1 | 64 | 755 | 301 | 870 | 2.51× | 0.87× |
| 1 | 256 | 237 | 81 | 273 | 2.93× | 0.87× |
| 4 | 1 | 22803 | 10675 | 23480 | 2.14× | 0.97× |
| 4 | 4 | 14975 | 6771 | 16833 | 2.21× | 0.89× |
| 4 | 16 | 8007 | 3112 | 9198 | 2.57× | 0.87× |
| 4 | 64 | 3097 | 1014 | 3653 | 3.05× | 0.85× |
| 4 | 256 | 954 | 276 | 1139 | 3.46× | 0.84× |
| 8 | 1 | 43836 | 14772 | 45264 | 2.97× | 0.97× |
| 8 | 4 | 28742 | 10192 | 32802 | 2.82× | 0.88× |
| 8 | 16 | 15073 | 5104 | 18159 | 2.95× | 0.83× |
| 8 | 64 | 5877 | 1700 | 7029 | 3.46× | 0.84× |
| 8 | 256 | 1860 | 474 | 2212 | 3.92× | 0.84× |
| 16 | 1 | 57496 | 17063 | 66988 | 3.37× | 0.86× |
| 16 | 4 | 38236 | 11821 | 45828 | 3.23× | 0.83× |
| 16 | 16 | 18995 | 6086 | 22938 | 3.12× | 0.83× |
| 16 | 64 | 6949 | 2119 | 8592 | 3.28× | 0.81× |
| 16 | 256 | 2092 | 808 | 2591 | 2.59× | 0.81× |

## 4. 多线程扩展性（1→16 线程加速比）

以 nprobe=16 为例：

| engine | 1-bit | 7-bit |
|---|---|---|
| zvec | 2222 → 20711（**9.3×**） | 1927 → 18995（**9.9×**） |
| faiss | 2112 → 11997（5.7×） | 899 → 6086（6.8×） |
| rabitq-library | 2526 → 25376（**10.0×**） | 2213 → 22938（**10.4×**） |

zvec 与 RaBitQ-Library 扩展性接近理想（16 核上 ~9-10×），Faiss 明显偏低（~6×）——其单查询 `search(1, ...)` 路径存在较多每次调用开销，并发下放大。

## 5. Recall@10（单线程；不同线程数结果一致）

### 1-bit
| nprobe | zvec | faiss | rabitq-library |
|---|---|---|---|
| 1 | 0.2352 | 0.2250 | 0.2277 |
| 4 | 0.4490 | 0.4452 | 0.4441 |
| 16 | 0.6066 | 0.6164 | 0.6105 |
| 64 | 0.6698 | 0.6767 | 0.6693 |
| 256 | 0.6738 | 0.6818 | 0.6760 |

### 7-bit
| nprobe | zvec | faiss | rabitq-library |
|---|---|---|---|
| 1 | 0.2424 | 0.2388 | 0.2414 |
| 4 | 0.5231 | 0.5147 | 0.5110 |
| 16 | 0.8159 | 0.8003 | 0.7984 |
| 64 | 0.9642 | 0.9642 | 0.9624 |
| 256 | 0.9862 | 0.9877 | 0.9837 |

三方 Recall 一致（差异 ≤0.015，属随机种子与统计噪声），确认口径对齐。1-bit 无 rerank 在 GIST 上约 0.68 见顶；7-bit rerank 可达 ~0.986。

## 6. 结论

**检索（zvec 表现良好）**
- zvec 全面快于 Faiss：1-bit 1.03–2.93×，7-bit 1.52–3.92×；位宽越高、并发越大，领先越明显。
- zvec 略慢于 RaBitQ-Library，稳定在 0.81–0.97×（约低 3–19%）。两者内层扫描复用同一份 `rabitqlib` fast-scan 内核，差距来自外围（top-k 容器、粗排、上下文管理）。
- 多线程扩展性优秀（16 核 ~9-10×），与 RaBitQ-Library 相当，显著优于 Faiss。

**构建（zvec 明显落后）**
- zvec ≈120s，是 Faiss 1-bit 的 2.3×、RaBitQ-Library 的 2.7×；**唯一瓶颈是 kmeans train 的 108s**。
- 该阶段已使用 32×32 寄存器分块 SIMD，与 Faiss 的差距源于后者把分配步骤的 `X·Cᵀ` 交给 BLAS sgemm。若要追平构建速度，需在分配步骤引入 sgemm（预计 108s → ~40s）。

**精度**：三方等价，无回归。

**口径提示**：zvec 使用 mmap 持久化并开启 `memory_warmup` 预热；Faiss 与 RaBitQ-Library reload 后驻留堆内存。三者存储访问路径不同，但均已预热至热页状态。

## 7. 复现命令

```bash
# 数据准备
python3 tools/core/ivf_rabitq_compare/hdf5_to_vecs.py \
  /path/to/gist-960-euclidean.hdf5 /tmp/gist/vecs

# 编译（统一 gcc-12 + AVX-512）
export CC=/opt/rh/gcc-toolset-12/root/usr/bin/gcc
export CXX=/opt/rh/gcc-toolset-12/root/usr/bin/g++
cmake -S . -B build.gcc12 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DRABITQ_ENABLE_AVX512=ON
ninja -C build.gcc12 ivf_rabitq_zvec_bench
cmake -S tools/core/ivf_rabitq_compare -B build.ivf-rabitq-compare -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DFAISS_OPT_LEVEL=avx512
ninja -C build.ivf-rabitq-compare ivf_rabitq_faiss_bench ivf_rabitq_library_bench

# 运行（--total-bits 取 1 或 7）
ARGS="--base /tmp/gist/vecs/base.fvecs --query /tmp/gist/vecs/query.fvecs \
  --groundtruth /tmp/gist/vecs/groundtruth.ivecs --nlist 1024 \
  --train-size 262144 --niters 20 --nprobes 1,4,16,64,256 --topk 10 \
  --threads 16 --repeats 3 --warmup 50 --search-threads 1,4,8,16"

./build.gcc12/bin/ivf_rabitq_zvec_bench $ARGS --total-bits 1 --index /tmp/idx/zvec
./build.ivf-rabitq-compare/ivf_rabitq_faiss_bench $ARGS --total-bits 1 --index /tmp/idx/faiss
./build.ivf-rabitq-compare/ivf_rabitq_library_bench $ARGS --total-bits 1 --index /tmp/idx/rabitq
```

原始输出：`/tmp/gist/final/{zvec,faiss,rabitq}-{1,7}bit.jsonl`（JSON Lines）。
