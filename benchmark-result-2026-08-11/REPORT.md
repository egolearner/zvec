# Zvec RaBitQ Benchmark 报告（2026-08-11）

对比两组：

- **IVF-RaBitQ**：zvec vs **Faiss**（`IndexIVFRaBitQ`），数据集 GIST-1M、Cohere-1M。
- **HNSW-RaBitQ**：zvec vs **Elasticsearch 8.18.8 `bbq_hnsw`**，同样两个数据集。

RaBitQ-Library 三方对比本次未跑。

原始数据、汇总文本与分析脚本见文末「原始结果存放」。

---

## 1. 测试环境

| 项 | 值 |
|---|---|
| CPU | Intel Xeon 6982P-C @3.8GHz（KVM guest），**8 物理核 / 16 逻辑核（SMT on）**，单 socket，单 NUMA node |
| 指令集 | AVX-512（含 `avx512_vnni` / `avx512_vpopcntdq` / `avx512_fp16`） |
| 内存 | 61 GiB，**swap 关闭** |
| 内核 | 5.10.134-19.2.al8.x86_64 |
| 磁盘 | NVMe，788 GB（测试期间 >580 GB 可用） |
| `vm.max_map_count` | **65530**（低于 ES 官方建议 262144，`single-node` 模式下 1M×768/960 单 segment 实测可用） |
| Docker | 29.7.1，**rootless**，`Cgroup Driver: none` → `--memory`/`--cpuset-cpus` 不可靠，容器改用 `taskset` 绑核，**无硬内存上限** |
| zvec commit | `92ec99e96435823a58627b181ad8182f439edcd4`（rabitq-bench 分支） |
| 编译器 | GCC 12.3.0（`/opt/rh/gcc-toolset-12`），`CMAKE_BUILD_TYPE=Release`，两侧 SIMD 目标一致（Faiss `FAISS_OPT_LEVEL=avx512`） |
| Python | 3.11.15（conda venv），`elasticsearch` client 8.19.3 |
| ES | `docker.elastic.co/elasticsearch/elasticsearch:8.18.8`，JVM heap `-Xms4g -Xmx4g`，1 shard / 0 replica / `_source` 关闭 / force-merge 到 1 segment |
| 绑核 | 两组测试进程与 ES 容器均绑 `0,2,4,6,8,10,12,14`（每物理核一个逻辑 CPU，避开 SMT sibling） |

**16 线程属于超订**（物理核只有 8 个），报告中 8→16 线程几乎不再增长即源于此。

### 数据集

| 数据集 | 路径 | 维度 | 距离 | base / query | SHA-256 |
|---|---|---:|---|---|---|
| GIST-1M | `/tmp/gist-960-euclidean.hdf5`（HNSW 组）<br>`/tmp/gist/vecs/`（IVF 组，同源转 fvecs） | 960 | euclidean | 1,000,000 / 1,000 | `8e95831936bfdbfa0a56086942e2cf98cd703517c67f985914183eb4cdbf026a` |
| Cohere-1M | `/data/cohere_medium_1m`（HNSW 组，`cohere-parquet`）<br>`/data/cohere_1m`（IVF 组，同一数据集的 vecs/txt 版本） | 768 | cosine | 1,000,000 / 1,000 | `4565da2063dd0966fa4ce7fd52fd5981b93bc72479bfb628bc1f7b4465e601d8` |

Cohere medium 1M 与 Cohere 1M 是同一数据集的两种存储格式，未作区分。

---

## 2. IVF-RaBitQ：zvec vs Faiss

### 2.1 口径

`nlist=1024`、`train_size=262144`（= nlist×256，正好卡在 Faiss 内部下采样阈值上，保证两侧训练样本严格一致）、kmeans `niters=20`、`topk=10`、`nprobe ∈ {1,2,4,8,16,32,64,128,256}`、构建线程 16。

每个 `(nprobe, 线程数)` 点：warmup 50 条 → `repeats=3` **取最快一次**；检索线程 1/4/8/16，各库内部 OpenMP 锁为 1 线程，多线程吞吐来自 bench 层并发查询（每线程独立上下文）。zvec 走 mmap 并已 `memory_warmup` 预热，Faiss reload 到堆内存 —— 存储路径不同但都已预热至热页。

### 2.2 构建耗时与索引大小

| 数据集 | bits | engine | train_s | encode_s | total_s | 索引大小 |
|---|---:|---|---:|---:|---:|---:|
| Cohere | 1 | zvec | 78.7 | 9.1 | **87.9** | **122.0 MiB** |
| Cohere | 1 | faiss | 43.5 | 9.9 | **53.4** | 143.4 MiB |
| Cohere | 7 | zvec | 78.6 | 9.5 | **89.0** | **679.0 MiB** |
| Cohere | 7 | faiss | 43.5 | 41.4 | **85.2** | 899.1 MiB |
| GIST | 1 | zvec | 108.1 | 11.8 | **120.2** | 147.0 MiB |
| GIST | 1 | faiss | 41.9 | 9.7 | **51.7** | **143.4 MiB** |
| GIST | 7 | zvec | 108.2 | 12.8 | **122.1** | **841.2 MiB** |
| GIST | 7 | faiss | 41.0 | 38.9 | **80.2** | 899.5 MiB |

- **zvec 构建慢**：kmeans train 占其 total 约 90%，是 Faiss 的 1.7–2.6×。
- **zvec 索引更小**（除 GIST 1-bit 略大 2.5%）：Cohere 1-bit 小 15%、7-bit 小 24%；GIST 7-bit 小 6.5%。
- Faiss 的 `encode` 在 7-bit 下从 ~10s 暴涨到 ~40s，使其 7-bit 构建总时长逼近 zvec。

### 2.3 等 Recall@10 的 QPS 比值（log 插值）

**Cohere 7-bit**（两侧 recall 几乎重合，差 ≤0.013）

| Recall@10 | 1 线程 | 4 线程 | 8 线程 | 16 线程 |
|---:|---:|---:|---:|---:|
| 0.80 | 2.24× | 2.89× | 3.76× | 4.22× |
| 0.90 | 2.61× | 3.39× | 4.06× | 4.29× |
| 0.95 | 3.01× | 3.66× | 4.49× | 4.54× |
| 0.98 | 3.12× | 3.88× | 4.84× | 4.47× |

**GIST 7-bit**

| Recall@10 | 1 线程 | 4 线程 | 8 线程 | 16 线程 |
|---:|---:|---:|---:|---:|
| 0.80 | 2.11× | 2.65× | 3.20× | 3.19× |
| 0.90 | 2.26× | 2.77× | 3.35× | 3.39× |
| 0.95 | 2.32× | 2.93× | 3.46× | 3.28× |
| 0.98 | 2.54× | 3.14× | 3.67× | 3.24× |

**1-bit**（zvec recall 天花板低于 Faiss，是短板）

| 数据集 | Recall@10 | 1 线程 | 4 线程 | 8 线程 | 16 线程 |
|---|---:|---:|---:|---:|---:|
| Cohere | 0.70 | 1.22× | 1.86× | 2.64× | 3.05× |
| Cohere | 0.75 | 1.10× | 1.55× | 2.10× | 2.32× |
| Cohere | **0.80** | **0.59×** | **0.75×** | 0.94× | 0.99× |
| GIST | 0.50 | 1.01× | 1.44× | 2.01× | 2.15× |
| GIST | 0.60 | 1.01× | 1.33× | 1.73× | 1.75× |
| GIST | 0.65 | 0.99× | 1.22× | 1.53× | 1.52× |

1-bit 下 zvec recall 天花板：Cohere **0.8047 vs Faiss 0.8244**、GIST **0.6753 vs 0.6818**。逼近天花板时 zvec 需要显著更大的 nprobe 才能追上同一 recall，等 Recall 优势被吃掉甚至反超（Cohere 0.80 处 0.59–0.99×）。7-bit 下两侧 recall 基本持平（≤0.013），不存在这个问题。

同 nprobe 直比（不等 Recall，仅供参考）：Cohere 1-bit 1.3–4.2×、7-bit 1.9–5.4×；GIST 1-bit 1.0–2.5×、7-bit 1.5–3.5×。

### 2.4 多线程扩展性（nprobe=256，1 线程为基准）

| 数据集 / bits | zvec | faiss |
|---|---|---|
| Cohere 1-bit | 4.5× / 8.6× / 10.7× | 3.5× / 5.7× / 7.4× |
| Cohere 7-bit | 4.5× / 8.7× / 10.9× | 3.6× / 5.8× / 10.0× |
| GIST 1-bit | 4.2× / 8.4× / 10.2× | 3.5× / 5.7× / 7.6× |
| GIST 7-bit | 4.2× / 8.4× / 10.2× | 3.5× / 6.2× / 11.1× |

（列为 4 / 8 / 16 线程）zvec 在 8 线程即接近线性（8.4–8.7×），4→8 线程扩展性稳定优于 Faiss；16 线程是 SMT 超订，两侧都进入收益递减，Faiss 在 7-bit 上因单线程基数低反而出现更大的相对倍数。

---

## 3. HNSW-RaBitQ：zvec vs Elasticsearch `bbq_hnsw`

### 3.1 口径

固定 `M=16`、`ef_construction=100`、`topk=10`；搜索参数 zvec `ef` / ES `num_candidates` = `32,64,128,256,512,1000`（ES 精排组加到 2000）。

每个 `(配置, 参数, 线程数)` 点：1000 条 query 的 accuracy pass → 预热 1000 条 → **10 秒持续压测 × 3 次**，客户端线程 1/4/8/16。**QPS 取 3 次重复的最大值。** 构建线程 8（= 物理核数）。

四组配置：

| 组 | 配置 | 说明 |
|---|---|---|
| A | `zvec-1bit` | 纯 1-bit，不访问原向量 |
| B | `zvec-1bit+refine` | **复用 A 的索引**，查询侧 `ef` 作为内部 top-k 后用原向量精排 |
| C | `zvec-7bit` | 独立索引，高精度量化 |
| D | `es-norescore` | ES 不传 `rescore_vector`，与 A 同口径 |
| 补充 | `es-ov2/3/5` | ES `rescore_vector.oversample=2/3/5`，用原向量精排，用于等 Recall 对比 |

### 3.2 构建耗时与索引大小

| 数据集 | 配置 | ingest_s | finalize_s | total_s | 索引大小 |
|---|---|---:|---:|---:|---:|
| Cohere | zvec-1bit | 51.9 | 293.4 | **345.3** | **3.16 GiB** |
| Cohere | zvec-7bit | 51.8 | 275.0 | **326.8** | 3.73 GiB |
| Cohere | elasticsearch | 283.5 | 516.3 | **799.8** | 4.72 GiB |
| GIST | zvec-1bit | 51.2 | 265.8 | **317.1** | **3.91 GiB** |
| GIST | zvec-7bit | 51.2 | 273.0 | **324.2** | 4.59 GiB |
| GIST | elasticsearch | 325.7 | 637.4 | **963.2** | 4.46 GiB |

zvec 建库约为 ES 的 1/3（Cohere 345s vs 800s，GIST 317s vs 963s；ES 含 force-merge 到 1 segment）。存储口径不同：**ES 的 store 含磁盘保留的原始 FP32**，zvec 目录同样保留原向量（refine 需要），两者都不是"仅量化码"的体积。

### 3.3 Recall@10 天花板

| 数据集 | zvec-1bit | zvec-1bit+refine | zvec-7bit | es-norescore | es-ov5 |
|---|---:|---:|---:|---:|---:|
| Cohere | 0.766 | **0.989** | 0.980 | 0.750 | 0.993 |
| GIST | 0.597 | **0.963** | 0.954 | 0.321 | 0.966 |

**不精排时 ES 明显更差**：GIST 上 ES 只有 0.236–0.321，而 zvec-1bit 是 0.462–0.597 —— 两条曲线**完全不重叠**，无法做等 Recall 对比，这也是本次补跑 ES `rescore_vector` 曲线的原因。

### 3.4 等 Recall@10 的 QPS 比值

zvec 侧最优配置在所有目标 recall 上都是 **`zvec-1bit+refine`**（优于 `zvec-7bit`：同 recall 下 QPS 更高，且索引更小、复用同一份 1-bit 索引）。

**Cohere**（ES 侧为所有 oversample 设置的**最优包络**，即每个目标 recall 上取表现最好的那组）

| Recall@10 | 线程 | zvec QPS | ES QPS | ES 最优配置 | zvec/ES |
|---:|---:|---:|---:|---|---:|
| 0.80 | 1 | 6522 | 846 | ov1.5 | **7.7×** |
| 0.80 | 4 | 22285 | 1478 | ov1.5 | **15.1×** |
| 0.80 | 8 | 23299 | 1488 | ov1.5 | **15.7×** |
| 0.80 | 16 | 23515 | 1507 | ov1.5 | **15.6×** |
| 0.85 | 1 | 5565 | 819 | ov1.5 | **6.8×** |
| 0.85 | 4 | 19939 | 1456 | ov1.5 | **13.7×** |
| 0.85 | 8 | 23721 | 1485 | ov1.5 | **16.0×** |
| 0.85 | 16 | 23539 | 1495 | ov1.5 | **15.7×** |
| 0.90 | 1 | 4109 | 722 | ov1.25 | **5.7×** |
| 0.90 | 4 | 15500 | 1414 | ov1.25 | **11.0×** |
| 0.90 | 8 | 22825 | 1477 | ov1.25 | **15.5×** |
| 0.90 | 16 | 22376 | 1491 | ov1.25 | **15.0×** |
| 0.95 | 1 | 2161 | 474 | ov3 | **4.6×** |
| 0.95 | 4 | 8576 | 1249 | ov1.75 | **6.9×** |
| 0.95 | 8 | 15728 | 1450 | ov1.25 | **10.8×** |
| 0.95 | 16 | 15830 | 1483 | ov2 | **10.7×** |
| 0.98 | 1 | 801 | 168 | ov3 | **4.8×** |
| 0.98 | 4 | 3252 | 646 | ov1.25 | **5.0×** |
| 0.98 | 8 | 6326 | 1080 | ov1.25 | **5.9×** |
| 0.98 | 16 | 6327 | 1195 | ov5 | **5.3×** |

zvec 侧所有档位的最优配置都是 `zvec-1bit+refine`。该表由 `hnsw_rabitq/cohere/iso_recall_cohere.py` 生成，其中 zvec 的 `ef=12/16/20/24` 点来自重建索引后的补测（见 4 节说明），`ef>=32` 沿用首轮索引。

**ES 侧关键发现：低 oversample 才是 0.80–0.95 区间的最优解。** 首轮只跑了 oversample 2/3/5，导致 recall 0.90 一档无法插值（ov2 的最低点 `num_candidates=32` 已是 0.902）。补跑后可见：

- 固定 `num_candidates` 时，**oversample 的取值对 recall 影响很小**，真正的跃变来自「是否开启精排」：`num_candidates=32` 上 no-rescore 0.677 → ov1.1 0.858 → ov2 0.902 → ov5（无该点）；oversample 从 1.1 加到 2.0 只换来 +0.044 recall，却损失 11% QPS。
- 因此要压到 recall 0.80–0.86，正确做法是**开精排 + 调小 `num_candidates`**（受 `num_candidates >= ceil(topk*oversample)` 约束），而不是调小 oversample。本次补测了 `num_candidates=12/16/20/24`。
- 0.80–0.90 区间 ES 的最优配置是 **ov1.25–1.5**，比首轮最低的 ov2 提升约 5–8% QPS；0.95 以上 ov1.25–3 互有胜负。

**GIST**

| Recall@10 | 线程 | zvec QPS | ES QPS | ES 配置 | zvec/ES |
|---:|---:|---:|---:|---|---:|
| 0.80 | 1 | 2141 | 236 | ov5 | **9.1×** |
| 0.80 | 4 | 8467 | 815 | ov5 | **10.4×** |
| 0.80 | 8 | 15647 | 1244 | ov5 | **12.6×** |
| 0.80 | 16 | 15731 | 1350 | ov5 | **11.7×** |
| 0.90 | 1 | 1001 | 102 | ov5 | **9.8×** |
| 0.90 | 4 | 4042 | 409 | ov5 | **9.9×** |
| 0.90 | 8 | 7842 | 711 | ov5 | **11.0×** |
| 0.90 | 16 | 7823 | 792 | ov5 | **9.9×** |
| 0.95 | 1 | 516 | 48 | ov5 | **10.8×** |
| 0.95 | 4 | 2080 | 194 | ov5 | **10.7×** |
| 0.95 | 8 | 4091 | 355 | ov5 | **11.5×** |
| 0.95 | 16 | 4049 | 394 | ov5 | **10.3×** |

同口径不精排（zvec-1bit vs es-norescore）在 Cohere 重叠带（recall 0.72–0.74）上 zvec 为 **7.7–15.9×**；GIST 无重叠带。

### 3.5 Recall–QPS 扫描（1 线程，`参数:Recall/QPS`）

**Cohere**

```
es-norescore       32:0.677/805  64:0.712/732  128:0.726/612  256:0.738/476  512:0.746/336  1000:0.750/220
es-ov1.1           12:0.678/852  16:0.737/843  20:0.780/820  24:0.818/809  32:0.858/771  64:0.907/676  128:0.935/547  256:0.955/404  512:0.971/282  1000:0.979/177  2000:0.986/104
es-ov1.25          32:0.868/807  64:0.912/693  128:0.938/555  256:0.959/398  512:0.973/263  1000:0.981/163  2000:0.987/94
es-ov1.5           16:0.800/847  20:0.834/837  24:0.858/811  32:0.879/756  64:0.924/636  128:0.946/482  256:0.964/343  512:0.975/217  1000:0.982/134  2000:0.988/75
es-ov1.75          32:0.892/727  64:0.929/606  128:0.951/448  256:0.966/310  512:0.976/195  1000:0.984/118  2000:0.989/67
es-ov2             32:0.902/684  64:0.932/575  128:0.953/441  256:0.969/303  512:0.978/191  1000:0.985/115  2000:0.989/65
es-ov3             32:0.924/658  64:0.946/513  128:0.964/363  256:0.975/238  512:0.982/144  1000:0.988/84   2000:0.991/47
es-ov5             64:0.959/381  128:0.973/258 256:0.981/156  512:0.987/90   1000:0.990/52  2000:0.993/28
zvec-1bit          12:0.617/9012 16:0.646/8396 20:0.667/7803  24:0.678/7463  32:0.703/6251  64:0.733/4393  128:0.747/2802  256:0.758/1633  512:0.763/926  1000:0.766/525
zvec-1bit+refine   12:0.660/7907 16:0.742/7293 20:0.790/6687  24:0.818/6245  32:0.860/5361  64:0.918/3639  128:0.950/2189  256:0.971/1243  512:0.982/715  1000:0.989/404
zvec-7bit          32:0.872/4416 64:0.925/2922 128:0.948/1751 256:0.966/994  512:0.975/552  1000:0.980/305
```

`num_candidates` / `ef` = 12–24 的点为补测（ES 见 3.4，zvec 见 4 节重建说明）。ES 与 zvec 的曲线现在都连续覆盖 recall 0.62–0.99，可在整个 0.80–0.95 区间做等 Recall 对比。

**GIST**

```
es-norescore       32:0.236/774  64:0.265/682  128:0.288/541  256:0.304/409  512:0.314/282  1000:0.321/188
es-ov2             32:0.458/617  64:0.583/493  128:0.689/367  256:0.780/249  512:0.849/160  1000:0.899/100  2000:0.935/59
es-ov3             32:0.534/554  64:0.648/423  128:0.746/296  256:0.824/196  512:0.882/123  1000:0.923/74   2000:0.952/44
es-ov5             64:0.720/346  128:0.804/231 256:0.868/146  512:0.913/88   1000:0.946/53  2000:0.966/30
zvec-1bit          32:0.462/6461 64:0.522/4544 128:0.558/2906 256:0.578/1736 512:0.593/1003 1000:0.597/593
zvec-1bit+refine   32:0.572/5200 64:0.705/3460 128:0.807/2063 256:0.883/1200 512:0.934/700  1000:0.963/403
zvec-7bit          32:0.592/4050 64:0.716/2617 128:0.812/1575 256:0.888/921  512:0.931/533  1000:0.954/308
```

4/8/16 线程的完整扫描见 `hnsw_rabitq/{cohere,gist}/summary.txt`。

### 3.6 并发扩展性与 ES 平台期

zvec 从 1→8 线程接近线性（例如 Cohere `zvec-1bit+refine` ef=1000：404 → 1635 → 3216 QPS），8→16 线程基本不变（物理核已用满）。

**ES 在 4 线程后就进入平台期**：Cohere `es-norescore` 1→16 线程为 805 → 1451 → 1464 → 1502 QPS，8/16 线程几乎不涨；GIST 同样（774 → 1388）。这是单 shard 搜索线程池与 HTTP/协调层开销所致，属实测结果。因此高并发下 zvec/ES 的比值被 ES 的平台期放大。

---

## 4. 口径声明与未做项

**口径差异（相同数值不代表相同工作量）**

- ES 的 store 含磁盘保留的原始 FP32；zvec 目录同样保留原向量。
- **ES QPS 含宿主机 HTTP 往返**（客户端与 ES 同机、绑同一组核），zvec 是**进程内调用**。这部分固定开销在低 recall / 高 QPS 区间对 ES 更不利。
- zvec `ef` 与 ES `num_candidates`、zvec `refine` 与 ES `rescore_vector.oversample` 语义不同。
- IVF 组：zvec 走 mmap（已 `memory_warmup`），Faiss reload 到堆内存；两者都已预热至热页。
- IVF 组 QPS 为 repeats 取**最快一次**，HNSW 组为 3 次 10s 压测取**最大值** —— 两组内部各自自洽，但不要跨组比较绝对 QPS。

**未做项**

- 每个索引只构建 **1 次**，构建耗时未做多次取中位数。
- 未采集峰值 RSS / 内存占用。
- Docker rootless + `Cgroup Driver: none` → ES 容器**无硬内存上限**，仅 JVM heap 限制在 4 GiB。
- `vm.max_map_count=65530` 低于官方建议值。
- 16 线程为 SMT 超订（物理核 8 个）。
- RaBitQ-Library 三方对比未跑。
- Cohere 的 IVF 组与 HNSW 组读取的是同一数据集的两种格式文件，未逐字节校验两者内容一致（各自 SHA-256 已记录）。
- **Cohere 的 zvec `ef=12/16/20/24` 点跑在重建后的索引上**：首轮索引在补测前被外部进程删除（见下条），故用相同配置（`M=16`、`ef_construction=100`、`total_bits=1`、`num_clusters=16`、`build_threads=8`）重建后补测。`ef=32` 在两个索引上都测过，结果一致：recall 0.860 → 0.856（refine）/ 0.703 → 0.698（纯 1-bit），QPS 5361 → 5475（+2.1%）/ 6251 → 6671（+6.7%）。recall 差 ≤0.005 说明两份索引等价，QPS 的正偏差属于 run-to-run 波动与页缓存状态差异，等 Recall 表中 `ef>=32` 一律取首轮数值以避免偏向 zvec。
- **结果目录曾被外部进程删除**：`~/benchmark-results/2026-08-11/` 在 23:45–00:41 之间被清空（本次 benchmark 工具链只会在 `--overwrite` 时删除 `search-results.jsonl`，不会删目录）。全部 JSONL / manifest / 报告已从 23:45 的快照 `2026-08-11-results.tgz` 完整恢复，仅 zvec 索引目录（可重建）丢失。此后每完成一轮补测都会刷新该快照。

---

## 5. 结论摘要

1. **IVF-RaBitQ 7-bit：zvec 全面领先 Faiss**，等 Recall QPS 为 Cohere 2.2–4.8×、GIST 2.1–3.7×，且索引小 6–24%。代价是构建慢 1.7–2.6×（瓶颈在 kmeans train，占 ~90%）。
2. **IVF-RaBitQ 1-bit 是 zvec 的短板**：recall 天花板低于 Faiss（Cohere 0.805 vs 0.824、GIST 0.675 vs 0.682），逼近天花板时等 Recall 优势归零甚至反超（Cohere 0.80 处 0.59–0.99×）。中低 recall 区间仍有 1.0–3.9× 优势。
3. **HNSW-RaBitQ：zvec 对 ES `bbq_hnsw` 优势显著**，等 Recall QPS 为 Cohere 4.6–16.0×（recall 0.80–0.98 全区间，ES 取所有 oversample 的最优包络）、GIST 9.1–12.6×（recall 0.80–0.95），建库快 2.3–3.0×。索引大小：1-bit（最佳配置 refine 复用的就是这份）比 ES 小 33%（Cohere）/ 12%（GIST），但 GIST 的 7-bit 索引反而比 ES 大 3%。
4. **zvec 最佳 HNSW 配置是 1-bit + 查询侧 refine**，在所有目标 recall 上都优于 7-bit，且能复用同一份 1-bit 索引。
5. **ES 不精排不可用于高 recall 场景**：Cohere 天花板 0.750、GIST 仅 0.321，必须开 `rescore_vector`。而 **oversample 的具体取值影响很小**（`num_candidates=32` 上 1.1→2.0 只涨 0.044 recall、掉 11% QPS），recall 的跃变全部来自「是否开精排」；要落到 0.80–0.86 应当开精排并调小 `num_candidates`。0.80–0.90 区间 ES 的最优 oversample 是 **1.25–1.5**，比 2.0 快 5–8%。
6. 高并发下差距被放大：zvec 到 8 线程接近线性扩展，**ES 在 4 线程后即进入平台期**。

---

## 6. 原始结果存放

根目录 `~/benchmark-results/2026-08-11/`：

```
ivf_rabitq/
  {cohere,gist}-{zvec,faiss}-{1,7}bit.jsonl   # 8 份原始 JSONL，各 1 build + 36 search 行
  {cohere,gist}-{zvec,faiss}-{1,7}bit.err     # stderr
  index_sizes.txt                             # 8 个索引文件大小
  summary.txt / summarize_ivf.py              # 构建、同 nprobe 扫描、等 Recall 折算
  iso_recall.txt / iso_recall_ivf.py          # 固定 Recall 的 QPS 比值
hnsw_rabitq/
  cohere/one-bit/       search-results.jsonl (292 点)
                        # zvec: 1bit 24 + 1bit+refine 24
                        # ES:   norescore 24 + ov1.1 44 + ov1.25 28 + ov1.5 40
                        #       + ov1.75 28 + ov2 28 + ov3 28 + ov5 24
                        manifest.json
  cohere/one-bit-lowef/ search-results.jsonl (40 点: ef=12/16/20/24/32 × 1bit / refine × 4 线程)
                        manifest.json        # 重建索引，见报告 4 节
  cohere/seven-bit/     search-results.jsonl (C 24 点) + manifest.json
  cohere/iso_recall_cohere.py / iso_recall_cohere.txt   # 3.4 节 Cohere 等 Recall 表
  gist/one-bit/         search-results.jsonl (152 点)
  gist/seven-bit/       search-results.jsonl (24 点)
  {cohere,gist}/summary.txt                   # summarize_results.py 输出
  {cohere,gist}/smoke/                        # smoke test 产物
scripts/                                      # 复现脚本
logs/                                         # 运行日志
REPORT.md                                     # 本文件
```

复现脚本：`scripts/{run_ivf_bench.sh,run_hnsw_bench.sh,run_es_rescore.sh}`；
运行日志：`logs/{ivf_bench.log,hnsw_bench.log,es_rescore.log}`（已过滤逐条 ingest 进度行）。

汇总命令：

```bash
# IVF
python3 ~/benchmark-results/2026-08-11/ivf_rabitq/summarize_ivf.py
python3 ~/benchmark-results/2026-08-11/ivf_rabitq/iso_recall_ivf.py

# HNSW
B=~/benchmark-results/2026-08-11/hnsw_rabitq
python3 tools/benchmark/hnsw_rabitq_vs_es/summarize_results.py \
  "$B/cohere/one-bit" "$B/cohere/seven-bit" \
  --threads 1,4,8,16 --recall-targets 0.80,0.90,0.95,0.98
```

### 环境搭建注意（本次踩到的坑）

1. 系统 `/usr/bin/g++` 是 GCC 10.2，默认 `gnu++14`，**无法编译 `thirdparty/RaBitQ-Library`**（`std::is_integral_v` / `std::optional` 报错）。因此 Python binding 不能用文档里的 `pybuild`（其 cache 指向 gcc-10），本次改为在 `build.gcc12` 里打开 `-DBUILD_PYTHON_BINDINGS=ON` 一起编，需额外传
   `-Dpybind11_DIR=<venv>/lib/python3.11/site-packages/pybind11/share/cmake/pybind11`。
2. 该机器访问 github.com 被阻断（`SSL connect error`），Arrow 的 bundled 依赖（boost/thrift/utf8proc/re2/rapidjson/xsimd/zlib）下载失败或极慢（boost 实测 ~28 KB/s）。解决办法是把 `pybuild` 里已下载好的压缩包复制到 `build.gcc12` 对应的 ExternalProject 下载路径（注意目标**文件名不同**，需按各 `download-*_ep.cmake` 里的期望路径命名）。
3. `BENCHMARK_HOWTO.md` 里的 `-DRABITQ_ENABLE_AVX512=ON` 在当前 CMake 中**已无对应 option**（cache 里是 `UNINITIALIZED`），AVX-512 由编译器能力探测自动开启，该参数可去掉。
