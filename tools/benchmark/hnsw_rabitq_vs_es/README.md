# Zvec HNSW-RaBitQ vs Elasticsearch BBQ-HNSW

本目录提供一套可复现的端到端 benchmark，支持以下两种 1M 数据集：

| 数据集 | base / query | 维度 | 距离 | ground truth |
|---|---:|---:|---|---|
| GIST `gist-960-euclidean.hdf5` | 1,000,000 / 1,000 | 960 | Euclidean | HDF5 `neighbors`，宽度 100 |
| Cohere `cohere_medium_1m/` | 1,000,000 / 1,000 | 768 | cosine | `neighbors.parquet`，宽度 1,000 |

GIST 使用 ANN-Benchmarks HDF5 的 `train/test/neighbors`。Cohere 目录必须包含
`shuffle_train.parquet`、`test.parquet` 和 `neighbors.parquet`；训练行中的真实
`id` 会原样写入两个引擎，并用于计算 Recall。脚本分批读取 base vectors，不会
一次性把约 3 GB 数据全部载入内存。

从零开始跑一次完整对比（Zvec 1-bit / 1-bit+refine / 7-bit 对 ES BBQ，
1/4/8/16 线程，构建时长与索引大小）的逐步操作手册见
[`RUNBOOK.md`](RUNBOOK.md)，结果汇总用
[`summarize_results.py`](summarize_results.py)。

## 快速开始：可迁移的一键入口

在其他 Linux 机器上优先使用 `run_comparison.sh`。数据、结果、CPU 和 ES 配置均可
指定，不依赖原测试机目录：

```bash
export ZVEC_BENCH_DATASET=/path/to/gist-960-euclidean.hdf5
export ZVEC_BENCH_WORK_DIR=/path/to/benchmark-results/gist-one-bit
export ZVEC_BENCH_CPUS=0,2,4,6,8,10,12,14

bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh smoke \
  --overwrite
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh all \
  --overwrite
```

运行 Cohere 1M 时只需把数据参数改为目录，并使用独立结果目录和 ES index：

```bash
export ZVEC_BENCH_DATASET=/path/to/cohere_medium_1m
export ZVEC_BENCH_WORK_DIR=/path/to/benchmark-results/cohere-one-bit
export ZVEC_BENCH_ES_INDEX=cohere-medium-1m-hnsw-bbq

bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh smoke --overwrite
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh all --overwrite
```

一键入口默认运行 Zvec `total_bits=1`、关闭 refine，对 ES BBQ 无
`rescore_vector` 的纯 1-bit 实验。已有索引可追加执行
`search --engines zvec --zvec-refine`，得到 Zvec 1-bit + raw-vector refine
结果。完整的新机器准备、CPU 选择、Docker、远端 ES 和数据换路径说明见
[`RUN_ON_ANOTHER_MACHINE.md`](RUN_ON_ANOTHER_MACHINE.md)。

## 1. 实验目标和口径

### 1.1 主实验：纯 1-bit、无原向量精排

| 项 | Zvec | Elasticsearch 8.18+ |
|---|---|---|
| 索引 | `HNSW_RABITQ` | `bbq_hnsw` |
| 距离 | GIST: `L2`；Cohere: `COSINE` | GIST: `l2_norm`；Cohere: `cosine` |
| HNSW 参数 | `M=16`, `ef_construction=100` | `m=16`, `ef_construction=100` |
| 量化/精排 | `total_bits=1`，不访问 raw vector 精排 | 1-bit BBQ，不传 `rescore_vector` |
| 数据布局 | 完整 Zvec collection | 1 primary shard、0 replica、禁用 `_source` |
| 最终段数 | `optimize()` 后持久化段 | `force_merge(max_num_segments=1)` |

这组配置比较两套产品的纯 1-bit 端到端路径。二者的量化、距离估计和 HNSW
实现并不相同，因此不是逐指令的同算法 microbenchmark。Elasticsearch 索引仍会
在磁盘保留原始 FP32 vector，但本实验查询不使用原向量重排。

### 1.2 补充实验：Zvec 1-bit 开启 refine

Zvec HNSW-RaBitQ 可以在查询时传 `is_using_refiner=True`，使用保存的原始向量
重新计算粗排结果的距离。benchmark 的 `--zvec-refine` 会开启该路径；它是查询
参数，不需要重建 `total_bits=1` 索引。

HNSW-RaBitQ 没有独立的 refine oversampling 参数。开启 refine 后，`ef` 会作为
每个底层 index block 的内部 top-k：RaBitQ 粗召回 `ef` 条候选，再用原始向量
精排得到用户请求的 `topk`，最后进行多 block 结果合并。实现对 `ef < topk` 的
情况使用 `max(topk, ef)`；本 benchmark 默认 `topk=10`、`ef=32..1000`，所以
内部 top-k 就是 `ef`，相当于 3.2–100 倍用户 top-k。同一个 `ef` 同时控制 HNSW
遍历和精排候选窗口。精确距离会改变排序以及多 block 合并后的最终 top-k，所以
Recall@1 和 Recall@K 都可能变化。结果文件会记录 `raw_vector_refine=true` 和
`refine_candidate_rule="max(topk, ef)"`；每行已有 `topk` 和 `search_value=ef`，
可据此得到该点的请求候选数。

Elasticsearch 的 `rescore_vector.oversample` 是独立于 `num_candidates` 的重排
倍数，语义并不相同。
建议按下面的独立配置分组比较 Recall–QPS 曲线：

1. Zvec 1-bit 无 refine，对 ES BBQ 无 `rescore_vector`；
2. Zvec 1-bit 开启 refine，对同一个 ES 无 `rescore_vector` 基线；
3. 如需考察两边各自的 raw-vector 路径，再单独增加 ES `oversample=3` 或 `5`
   曲线，不把相同参数数值解释成相同候选工作量。

Elasticsearch 8.18 中不传 `rescore_vector` 即不精排；更新版本可能引入默认
oversampling，所以所有实验都必须固定并记录具体 ES patch 版本。

### 1.3 检索不是比较一个“相同参数点”

Zvec 的 `ef` 与 Elasticsearch 的 `num_candidates` 含义不同，Zvec refine 与
Elasticsearch oversampling 也不等价，数值相同不代表工作量相同。脚本分别扫描
同一组数值，输出 Recall–QPS 点；结论应来自每个线程数下的 Pareto 曲线，或固定
Recall（如 0.90/0.95/0.99）处插值后的 QPS。

每个 `(engine, search_value, threads)` 点执行：

1. 将 1000 条 query 分配到指定数量的客户端线程，跑完整 accuracy pass；
2. 预热查询和 mmap/page cache；
3. 固定时长持续发单 query 请求，重复 3 次并取 QPS 中位数；
4. 输出 `Recall@1`、`Recall@10`、QPS 和每次 QPS 原始值。

默认客户端线程数为 `1,4,8,16`。Elasticsearch 固定单 shard，避免
`num_candidates` 按 shard 放大。

## 2. 安装

创建或使用 Python 3.11 virtualenv：

```bash
python3.11 -m venv .venv
source .venv/bin/activate
python -m pip install \
  -r tools/benchmark/hnsw_rabitq_vs_es/requirements.txt
```

本地源码模式：

```bash
export PYTHONPATH="$PWD/python:$PWD/build.release/lib"
```

Elasticsearch 必须是 8.18.0 或更新版本。建议先固定 8.18 的具体 patch
版本完成基准，再单独测更新版本，不要把不同 Lucene/OSQ 实现混在同一张图里。
GIST-1M 和 Cohere-1M 都约为 3–4 GB FP32 数据，建议单节点至少分配 16 个
物理核和 32–64 GB 内存，并确保 page cache 能容纳待检索文件。

## 3. 启动 Elasticsearch Docker

固定使用官方 Elasticsearch 8.18.8 镜像。先用 `lscpu -e=CPU,CORE,SOCKET,NODE`
选择物理核；不要假设 CPU 0–15 一定代表 16 个物理核。下面使用本次测试机上的
16 个物理核，并从容器 PID 1 开始设置 affinity，所有 Java/Lucene 线程都会
继承。JVM 固定 4 GB heap；向量文件主要依赖 filesystem cache，不要把可用内存
全部分给 JVM heap。

```bash
export BENCH_CPUS=0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30
docker pull docker.elastic.co/elasticsearch/elasticsearch:8.18.8
docker run --detach \
  --name zvec-es-bbq-8-18 \
  --publish 127.0.0.1:19200:9200 \
  --ulimit nofile=65535:65535 \
  --env discovery.type=single-node \
  --env xpack.security.enabled=false \
  --env xpack.security.http.ssl.enabled=false \
  --env xpack.license.self_generated.type=trial \
  --env xpack.ml.enabled=false \
  --env ingest.geoip.downloader.enabled=false \
  --env "ES_JAVA_OPTS=-Xms4g -Xmx4g" \
  --volume zvec-es-bbq-data:/usr/share/elasticsearch/data \
  --entrypoint /usr/bin/taskset \
  docker.elastic.co/elasticsearch/elasticsearch:8.18.8 \
  -c "$BENCH_CPUS" /bin/tini -- \
  /usr/local/bin/docker-entrypoint.sh eswrapper
```

若 Docker 有可用 cgroup，也可改用 `--cpuset-cpus "$BENCH_CPUS"`，并按实验
内存预算增加 `--memory` 与相同值的 `--memory-swap`。rootless Docker 的
cgroup driver 为 `none` 时这些限制无法可靠生效，应使用上面的 `taskset`
方式、确认宿主机无 swap，并记录没有硬内存上限。

benchmark 通过宿主机 TCP/HTTP 请求容器，不在 ES 进程内执行。开始建库前确认版本
和集群状态：

```bash
export ES_URL=http://127.0.0.1:19200
curl --fail --silent --show-error "$ES_URL/"
curl --fail --silent --show-error \
  "$ES_URL/_cluster/health?wait_for_status=yellow&timeout=60s"
```

`docker stats zvec-es-bbq-8-18` 可单独采集容器峰值 CPU/内存。实验结束后用
`docker stop zvec-es-bbq-8-18` 停止节点；只有确定不再需要已构建索引时，才删除
容器和 `zvec-es-bbq-data` volume。

## 4. 数据检查

GIST：

```bash
export ZVEC_BENCH_DATASET=/path/to/gist-960-euclidean.hdf5
export ZVEC_BENCH_WORK_DIR=/path/to/benchmark-results/gist-hnsw

python \
  tools/benchmark/hnsw_rabitq_vs_es/benchmark.py \
  --dataset "$ZVEC_BENCH_DATASET" \
  --mode inspect
```

预期输出：

```json
{
  "dataset": {
    "base_count": 1000000,
    "query_count": 1000,
    "dimension": 960,
    "groundtruth_k": 100,
    "distance": "euclidean"
  }
}
```

Cohere：

```bash
export ZVEC_BENCH_DATASET=/path/to/cohere_medium_1m
export ZVEC_BENCH_WORK_DIR=/path/to/benchmark-results/cohere-hnsw

python tools/benchmark/hnsw_rabitq_vs_es/benchmark.py \
  --dataset "$ZVEC_BENCH_DATASET" \
  --mode inspect
```

预期关键信息为 `base_count=1000000`、`query_count=1000`、
`dimension=768`、`groundtruth_k=1000`、`distance=cosine` 和
`dataset_format=cohere-parquet`。

如果将同一份数据和已构建结果复制到另一台机器，绝对路径和 mtime 会变化。
search/build-retained-index 时可显式传 `--allow-dataset-relocation`，忽略这两项，
同时继续校验完整数据 SHA-256、大小和 ANN 元数据。Cohere 的 SHA-256 按固定
文件名和三个参与实验的 Parquet 文件内容共同计算。旧 manifest 没有 `sha256`
时不能跨路径复用，需要重新构建一次。

## 5. 构建

推荐分别运行两个 engine，以便独立记录进程/容器峰值内存，并避免另一引擎的
常驻内存和后台线程干扰。

Zvec：

```bash
taskset -c "$BENCH_CPUS" /usr/bin/time -v python \
  tools/benchmark/hnsw_rabitq_vs_es/benchmark.py \
  --dataset "$ZVEC_BENCH_DATASET" \
  --work-dir "$ZVEC_BENCH_WORK_DIR" \
  --mode build --engines zvec --overwrite \
  --m 16 --ef-construction 100 --build-threads 16 \
  --zvec-total-bits 1 --zvec-num-clusters 16
```

Elasticsearch：

```bash
export ES_URL=http://127.0.0.1:19200
taskset -c "$BENCH_CPUS" /usr/bin/time -v python \
  tools/benchmark/hnsw_rabitq_vs_es/benchmark.py \
  --dataset "$ZVEC_BENCH_DATASET" \
  --work-dir "$ZVEC_BENCH_WORK_DIR" \
  --mode build --engines elasticsearch --overwrite \
  --m 16 --ef-construction 100 --build-threads 16 \
  --es-url "$ES_URL" --es-index gist-hnsw-bbq
```

`--build-threads 16` 同时映射为 Zvec optimize concurrency 和 ES
`parallel_bulk` 客户端线程数。ES 节点本身也必须限制在同一组 16 个 CPU；
客户端与节点在同机时也放在这个 cpuset 内，确保总 CPU 预算没有超过 Zvec。
若并发 bulk 未让 ES 节点 CPU 饱和，应在正式实验前只调整 bulk chunk/queue，
直到吞吐平台期，并把最终参数固定用于所有构建重复。

构建输出拆成：

- `ingest_seconds`：HDF5/Parquet 解码、客户端对象/JSON 生成、写入和增量建图；
- `finalize_seconds`：Zvec `optimize + flush` 或 ES
  `refresh + force_merge`；
- `total_seconds`、端到端 vectors/s、最终存储字节数。

ES force-merge 返回后，旧 segment 文件可能延迟释放。脚本会等待 primary store
统计连续 30 次不变后再记录 `index_bytes`，并输出
`store_stabilization_seconds`；这段观测等待不计入 `total_seconds`。

两方 API/进程模型不同，无法仅靠客户端 wall clock 完全隔离“纯算法建图时间”。
因此报告中应把端到端构建作为主指标，同时展示各阶段耗时、CPU time、峰值 RSS、
磁盘写入量。每方至少从空索引完整构建 3 次并取中位数。

## 6. 检索

Zvec：

```bash
taskset -c "$BENCH_CPUS" python \
  tools/benchmark/hnsw_rabitq_vs_es/benchmark.py \
  --dataset "$ZVEC_BENCH_DATASET" \
  --work-dir "$ZVEC_BENCH_WORK_DIR" \
  --mode search --engines zvec \
  --m 16 --ef-construction 100 \
  --search-values 32,64,128,256,512,1000 \
  --search-threads 1,4,8,16 \
  --warmup-queries 1000 --duration-seconds 10 --repeats 3 \
  --zvec-total-bits 1
```

在同一份 1-bit Zvec 索引上开启 raw-vector refine：

```bash
taskset -c "$BENCH_CPUS" python \
  tools/benchmark/hnsw_rabitq_vs_es/benchmark.py \
  --dataset "$ZVEC_BENCH_DATASET" \
  --work-dir "$ZVEC_BENCH_WORK_DIR" \
  --mode search --engines zvec \
  --m 16 --ef-construction 100 \
  --search-values 32,64,128,256,512,1000 \
  --search-threads 1,4,8,16 \
  --warmup-queries 1000 --duration-seconds 10 --repeats 3 \
  --zvec-total-bits 1 --zvec-refine
```

若先运行无 refine 的两引擎主实验，再运行上面的 Zvec-only 命令，三组结果会
追加到同一个 `search-results.jsonl`；可按 `engine` 和
`details.raw_vector_refine` 区分。不要再次运行 ES 基线，以免产生重复点。

Elasticsearch：

```bash
taskset -c "$BENCH_CPUS" python \
  tools/benchmark/hnsw_rabitq_vs_es/benchmark.py \
  --dataset "$ZVEC_BENCH_DATASET" \
  --work-dir "$ZVEC_BENCH_WORK_DIR" \
  --mode search --engines elasticsearch \
  --m 16 --ef-construction 100 \
  --search-values 32,64,128,256,512,1000 \
  --search-threads 1,4,8,16 \
  --warmup-queries 1000 --duration-seconds 10 --repeats 3 \
  --es-url "$ES_URL" --es-index gist-hnsw-bbq \
  --es-rescore-oversample none
```

ES 进程本身也必须限制到同一组 16 个 CPU，而不只是限制 Python client。
两方不要同时压测；每次切换 engine 后重新预热。关闭 swap，并记录 CPU 型号、
SMT、NUMA、内核、编译 SIMD、JVM heap、ES patch 版本和 Zvec commit。

## 7. Smoke test

可先用前 10,000 条 base 和 100 条 query 验证全流程。脚本会针对截断 base
按数据集距离重新计算 exact ground truth；Cohere 使用 cosine 并保留 Parquet
中的真实训练 ID：

```bash
python \
  tools/benchmark/hnsw_rabitq_vs_es/benchmark.py \
  --dataset "$ZVEC_BENCH_DATASET" \
  --work-dir /tmp/gist-hnsw-smoke \
  --mode all --engines zvec --overwrite \
  --max-base 10000 --max-queries 100 \
  --search-values 32,64 --search-threads 1,4 \
  --duration-seconds 1 --repeats 1 --warmup-queries 20 \
  --zvec-total-bits 1
```

Elasticsearch Docker 网络链路可用下面的对应 smoke test 验证，它会覆盖
1/4/8/16 个并发客户端线程：

```bash
python \
  tools/benchmark/hnsw_rabitq_vs_es/benchmark.py \
  --dataset "$ZVEC_BENCH_DATASET" \
  --work-dir /tmp/gist-hnsw-es-smoke \
  --mode all --engines elasticsearch --overwrite \
  --max-base 10000 --max-queries 100 \
  --search-values 32,64 --search-threads 1,4,8,16 \
  --duration-seconds 1 --repeats 1 --warmup-queries 20 \
  --es-url "$ES_URL" --es-index gist-hnsw-bbq-smoke \
  --es-rescore-oversample none
```

## 8. 结果文件

`--work-dir` 下生成：

- `manifest.json`：数据集签名、构建配置、环境和构建结果；
- `search-results.jsonl`：每个并发数、搜索参数点的 QPS/Recall；Zvec 结果中的
  `details.raw_vector_refine` 标记是否开启 refine；
- `zvec/`：Zvec collection；Elasticsearch index 保存在 ES data path。

报告至少包含四张 Recall@10–QPS 图（1/4/8/16 线程）、构建时间分解、最终存储、
峰值内存，以及固定 Recall 处的 QPS 表。不要只报告各引擎独立的最高 QPS，
因为那通常对应不同 Recall。

## 9. Elasticsearch 语义依据

- Elasticsearch 官方 Docker 单节点启动方式：
  <https://www.elastic.co/search-labs/tutorials/install-elasticsearch/docker>
- `bbq_hnsw`、`m` 和 `ef_construction`：
  <https://www.elastic.co/docs/reference/elasticsearch/mapping-reference/dense-vector>
- Elasticsearch 8.18 kNN 的 `num_candidates`、oversampling 和原始向量重排：
  <https://www.elastic.co/guide/en/elasticsearch/reference/8.18/knn-search.html>
- 8.18 对 GIST-1M 使用 optimized scalar quantization 的说明：
  <https://www.elastic.co/search-labs/blog/optimized-scalar-quantization-elasticsearch>
