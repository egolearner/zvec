# 在其他机器运行 GIST-1M / Cohere-1M 1-bit 对比

本文说明如何在一台新的 Linux 机器上，从零运行：

- Zvec `HNSW_RABITQ total_bits=1`
- Elasticsearch 8.18.8 `bbq_hnsw`
- Elasticsearch 查询不传 `rescore_vector`
- 可选的 Zvec raw-vector refine
- 1、4、8、16 个客户端线程的 QPS 和 Recall@10

一键脚本默认就是上述纯 1-bit 配置。所有机器相关路径、CPU、Docker 名称和
Elasticsearch 地址都可以通过参数或环境变量指定。

## 1. 机器要求

建议配置：

- Linux x86-64
- 16 个物理 CPU 核；较少核心也能运行，但需要同步调整线程参数
- 32–64 GiB 以上内存，建议关闭 swap
- 至少 60 GiB 可用磁盘空间，容纳数据、两个索引和 ES force-merge 临时文件
- Docker
- Python 3.11
- CMake、Ninja 和 Zvec 的正常编译依赖

Elasticsearch 建议设置：

```bash
sysctl vm.max_map_count
```

推荐值至少为 `262144`。如果当前值不足，由机器管理员按该机器的运维规范调整。

## 2. 准备 Zvec 和 Python 环境

在 Zvec 仓库根目录执行：

```bash
cmake -B build.release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build.release _zvec

python3.11 -m venv .venv
source .venv/bin/activate
python -m pip install \
  -r tools/benchmark/hnsw_rabitq_vs_es/requirements.txt

export PYTHONPATH="$PWD/python:$PWD/build.release/lib"
```

也可以使用已经安装了当前 Zvec Python package 的 virtualenv，此时不需要设置
本地源码 `PYTHONPATH`。

## 3. 准备数据

数据可以放到本机任意位置，路径不要求与原测试机相同。

GIST-1M：

```bash
export ZVEC_BENCH_DATASET=/mnt/ann-data/gist-960-euclidean.hdf5
export ZVEC_BENCH_WORK_DIR=/mnt/ann-results/gist-one-bit
export ZVEC_BENCH_ES_INDEX=gist-hnsw-bbq
```

Cohere-1M：

```bash
export ZVEC_BENCH_DATASET=/mnt/ann-data/cohere_medium_1m
export ZVEC_BENCH_WORK_DIR=/mnt/ann-results/cohere-one-bit
export ZVEC_BENCH_ES_INDEX=cohere-medium-1m-hnsw-bbq
```

Cohere 路径必须是包含下列文件的目录：

```text
cohere_medium_1m/
├── shuffle_train.parquet
├── test.parquet
└── neighbors.parquet
```

目录中允许存在 `neighbors_head_1p.parquet`、`neighbors_tail_1p.parquet` 等其他
文件，但实验和数据签名只使用上面三个文件。不同数据集必须使用不同 work-dir
和 ES index，避免误复用 manifest 或索引。

结果目录应位于空间充足的本地磁盘。数据和结果路径包含空格也可以，脚本内部会
按独立参数传递。

先检查数据：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh inspect
```

GIST 预期关键信息：

```text
base_count: 1000000
query_count: 1000
dimension: 960
distance: euclidean
```

Cohere 预期为：

```text
base_count: 1000000
query_count: 1000
dimension: 768
groundtruth_k: 1000
distance: cosine
dataset_format: cohere-parquet
```

## 4. 选择 CPU

查看 CPU、物理 core、socket 和 NUMA：

```bash
lscpu -e=CPU,CORE,SOCKET,NODE,ONLINE
```

从同一个 NUMA node 选择每个物理 core 的一个逻辑 CPU，不要同时选择同一 core
的两个 SMT sibling。例如某台机器可能是：

```bash
export ZVEC_BENCH_CPUS=0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30
```

不同机器的编号通常不同，不能直接复制上面的列表。脚本会用同一列表限制：

- Zvec benchmark 进程
- Elasticsearch HTTP client
- Docker 内 Elasticsearch JVM 及 Lucene 线程

若机器不足 16 个物理核，同时降低：

```bash
export ZVEC_BENCH_CPUS=0,2,4,6,8,10,12,14
export ZVEC_BENCH_BUILD_THREADS=8
export ZVEC_BENCH_SEARCH_THREADS=1,4,8
```

只做功能验证、不做公平性能对比时，可以传 `--cpus none` 关闭亲和性。

## 5. 先运行 smoke test

smoke test 使用 10,000 条 base、100 条 query、两档搜索参数：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh smoke \
  --overwrite
```

脚本会：

1. 启动 Elasticsearch 8.18.8 Docker；
2. 分别构建 Zvec 和 ES 索引；
3. 分别执行检索，ES 通过宿主机 HTTP 请求；
4. 停止由脚本启动的 ES 容器；
5. 保留容器、Docker volume、索引和结果目录。

smoke 会自动使用 `${ZVEC_BENCH_WORK_DIR}/smoke` 和
`${ZVEC_BENCH_ES_INDEX}-smoke`，不会污染后续完整 1M 实验。

脚本创建的容器带有 image、端口、volume、CPU 和 JVM 配置签名。再次使用同名
容器时签名必须一致；如果修改了这些参数，请同时指定新的 `--es-container` 和
`--es-volume`，避免静默复用旧实验环境。

只想查看完整命令而不执行时：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh smoke \
  --overwrite --dry-run
```

## 6. 运行完整 1M 1-bit 对比

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh all \
  --overwrite
```

默认关键参数：

| 参数 | 默认值 |
|---|---|
| Zvec 量化 | `total_bits=1`, `num_clusters=16` |
| Zvec refine | 关闭 |
| ES 量化 | `bbq_hnsw` |
| ES 精排 | `none`，请求不含 `rescore_vector` |
| HNSW | `M=16`, `ef_construction=100` |
| 构建线程 | 16 |
| 搜索线程 | `1,4,8,16` |
| 搜索参数 | `32,64,128,256,512,1000` |
| 吞吐测量 | 每点 3 次，每次 10 秒 |
| JVM heap | 4 GiB |
| ES Docker | 8.18.8、1 shard、0 replica、最终 force-merge 到 1 segment |

如果结果目录和索引都是全新的，可以不传 `--overwrite`。显式传入时，脚本只清理
已知的旧 `search-results.jsonl`，并让底层 benchmark 重建对应索引；不会删除
其他任意目录或 Docker volume。

## 7. 分阶段运行

构建和检索可以分开执行：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh build \
  --overwrite

bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh search
```

也可以只跑一个引擎：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh build \
  --engines zvec --overwrite

bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh search \
  --engines zvec
```

默认实验完成后，可以复用同一份 Zvec 1-bit 索引，追加开启 refine 的检索结果：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh search \
  --engines zvec \
  --zvec-refine
```

也可以用环境变量启用：

```bash
export ZVEC_BENCH_ZVEC_REFINE=1
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh search \
  --engines zvec
```

`--zvec-refine` 只改变查询，不需要重建索引。开启后，`ef` 会作为每个底层
index block 的内部 top-k：RaBitQ 粗召回 `ef` 条，再用原始向量精排得到用户的
`topk`，最后合并各 block 结果。实现对 `ef < topk` 的情况使用
`max(topk, ef)`；默认 sweep 中 `ef >= 32`、`topk=10`，所以内部 top-k 就是
`ef`。没有独立的 ES 式 `oversample` 参数，同一个 `ef` 同时控制图遍历和精排
候选窗口。精确距离可能改变多 block 合并后的最终 top-k。结果中的
`details.raw_vector_refine=true` 和
`details.refine_candidate_rule="max(topk, ef)"` 会标识这组数据；结合每行的
`topk` 和 `search_value=ef` 可得到请求候选数。建议先跑默认的无 refine 两引擎
基线，再只追加 Zvec refine，避免重复写入 ES 基线点。

## 8. 数据换路径或复制到另一台机器

全新实验只需要把数据路径指向本机的 HDF5 文件或 Cohere 目录：

```bash
export ZVEC_BENCH_DATASET=/new/path/cohere_medium_1m
export ZVEC_BENCH_WORK_DIR=/new/path/cohere-one-bit-results
export ZVEC_BENCH_ES_INDEX=cohere-medium-1m-hnsw-bbq
```

如果只移动数据文件/目录，而 work-dir 和 ES Docker volume 都还在，search 时使用：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh search \
  --allow-dataset-relocation
```

跨机器时需要区分两种索引的存储位置：

- Zvec collection 位于 work-dir。复制数据和整个 work-dir 后，可以直接运行：

  ```bash
  bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh search \
    --engines zvec \
    --allow-dataset-relocation
  ```

- Elasticsearch index 位于 Docker volume，不在 work-dir。新机器必须额外迁移
  对应 Docker volume/快照，或者在新机器重新执行：

  ```bash
  bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh build \
    --engines elasticsearch \
    --allow-dataset-relocation \
    --overwrite

  bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh search \
    --engines elasticsearch \
    --allow-dataset-relocation
  ```

如果 ES 是共享的远端服务，只需确认目标 index 仍存在，并使用
`--skip-docker --es-url ...`。

该选项只忽略 manifest 中数据的绝对路径和 mtime 差异，仍严格检查：

- 完整数据 SHA-256
- 数据总大小
- 数据格式
- 距离类型
- base/query 数量
- 向量维度
- ground-truth 宽度

SHA-256 会写入新 manifest；旧 manifest 没有该字段时，relocation 会拒绝继续，
需要重新构建一次。每次启动 benchmark 都会顺序读取 HDF5 或 Cohere 的三个
Parquet 文件计算哈希，这也会让数据进入 page cache，哈希时间不计入引擎构建或
检索 QPS。

## 9. 使用已有或远端 Elasticsearch

如果 ES 已经由外部系统启动：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh all \
  --skip-docker \
  --es-url http://es-host.example:9200 \
  --es-index gist-hnsw-bbq \
  --overwrite
```

有认证时使用底层 benchmark 已支持的环境变量：

```bash
export ES_API_KEY=...
```

或者：

```bash
export ES_USERNAME=...
export ES_PASSWORD=...
```

远端 ES 场景中，脚本只能限制本机 Python client 的 CPU；必须在 ES 机器上另外
施加同等 CPU/内存预算。网络延迟也会进入端到端 QPS，不能与同机 HTTP 结果直接
混为一组。

## 10. 其他可覆盖参数

命令行参数优先于环境变量。例如：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh all \
  --dataset /data/cohere_medium_1m \
  --work-dir /data/results/cohere-one-bit \
  --cpus 0,2,4,6,8,10,12,14 \
  --build-threads 8 \
  --search-threads 1,4,8 \
  --es-port 29200 \
  --es-index cohere-medium-1m-hnsw-bbq \
  --es-container cohere-es-29200 \
  --es-volume cohere-es-data-29200 \
  --overwrite
```

同时考察两边各自的 raw-vector 路径：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh search \
  --zvec-refine \
  --es-rescore-oversample 3 \
  --engines both
```

注意 ES `oversample=3` 是独立重排倍数，而 Zvec HNSW-RaBitQ refine 对每个底层
block 的 `max(topk, ef)` 个候选精排后再合并，不能把两者视为相同参数点。应按
各自的 Recall–QPS 曲线比较。

完整参数：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh --help
```

## 11. 输出与核对

`ZVEC_BENCH_WORK_DIR` 下生成：

- `manifest.json`：数据签名、环境、构建配置和构建结果
- `search-results.jsonl`：每个引擎、线程数、搜索参数的 Recall/QPS、refine/rescore
  配置和原始重复值
- `zvec/`：Zvec collection

ES 索引保存在配置的 Docker volume 中。正式汇总前检查：

```bash
jq . "$ZVEC_BENCH_WORK_DIR/manifest.json"
jq -s 'group_by(.engine) | map({engine: .[0].engine, points: length})' \
  "$ZVEC_BENCH_WORK_DIR/search-results.jsonl"
```

完整默认实验应得到每个引擎 24 个检索点。
