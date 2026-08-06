# 在其他机器运行 GIST-1M 1-bit 对比

本文说明如何在一台新的 Linux 机器上，从零运行：

- Zvec `HNSW_RABITQ total_bits=1`
- Elasticsearch 8.18.8 `bbq_hnsw`
- Elasticsearch 查询不传 `rescore_vector`
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

将 `gist-960-euclidean.hdf5` 放到本机任意位置。路径不要求与原测试机相同，例如：

```bash
export ZVEC_BENCH_DATASET=/mnt/ann-data/gist-960-euclidean.hdf5
export ZVEC_BENCH_WORK_DIR=/mnt/ann-results/gist-one-bit
```

结果目录应位于空间充足的本地磁盘。数据和结果路径包含空格也可以，脚本内部会
按独立参数传递。

先检查数据：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh inspect
```

预期关键信息：

```text
base_count: 1000000
query_count: 1000
dimension: 960
distance: euclidean
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

smoke 会自动使用 `${ZVEC_BENCH_WORK_DIR}/smoke` 和索引名
`gist-hnsw-bbq-smoke`，不会污染后续完整 1M 实验。

脚本创建的容器带有 image、端口、volume、CPU 和 JVM 配置签名。再次使用同名
容器时签名必须一致；如果修改了这些参数，请同时指定新的 `--es-container` 和
`--es-volume`，避免静默复用旧实验环境。

只想查看完整命令而不执行时：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh smoke \
  --overwrite --dry-run
```

## 6. 运行完整 GIST-1M 1-bit 对比

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh all \
  --overwrite
```

默认关键参数：

| 参数 | 默认值 |
|---|---|
| Zvec 量化 | `total_bits=1`, `num_clusters=16` |
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

## 8. 数据换路径或复制到另一台机器

全新实验只需要修改：

```bash
export ZVEC_BENCH_DATASET=/new/path/gist-960-euclidean.hdf5
export ZVEC_BENCH_WORK_DIR=/new/path/gist-one-bit-results
```

如果只在同一台机器移动 HDF5，而 work-dir 和 ES Docker volume 都还在，search
时使用：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh search \
  --allow-dataset-relocation
```

跨机器时需要区分两种索引的存储位置：

- Zvec collection 位于 work-dir。复制 HDF5 和整个 work-dir 后，可以直接运行：

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

该选项只忽略 manifest 中数据文件的绝对路径和 mtime 差异，仍严格检查：

- 完整文件 SHA-256
- 文件大小
- 距离类型
- base/query 数量
- 向量维度
- ground-truth 宽度

SHA-256 会写入新 manifest；旧 manifest 没有该字段时，relocation 会拒绝继续，
需要重新构建一次。每次启动 benchmark 都会顺序读取 HDF5 计算哈希，这也会让数据
进入 page cache，哈希时间不计入引擎构建或检索 QPS。

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
  --dataset /data/gist.hdf5 \
  --work-dir /data/results/gist-one-bit \
  --cpus 0,2,4,6,8,10,12,14 \
  --build-threads 8 \
  --search-threads 1,4,8 \
  --es-port 29200 \
  --es-container gist-es-29200 \
  --es-volume gist-es-data-29200 \
  --overwrite
```

切换到原来的高 Recall 生产配置：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh all \
  --zvec-total-bits 7 \
  --es-rescore-oversample 3 \
  --overwrite
```

完整参数：

```bash
bash tools/benchmark/hnsw_rabitq_vs_es/run_comparison.sh --help
```

## 11. 输出与核对

`ZVEC_BENCH_WORK_DIR` 下生成：

- `manifest.json`：数据签名、环境、构建配置和构建结果
- `search-results.jsonl`：每个引擎、线程数、搜索参数的 Recall/QPS 和原始重复值
- `zvec/`：Zvec collection

ES 索引保存在配置的 Docker volume 中。正式汇总前检查：

```bash
jq . "$ZVEC_BENCH_WORK_DIR/manifest.json"
jq -s 'group_by(.engine) | map({engine: .[0].engine, points: length})' \
  "$ZVEC_BENCH_WORK_DIR/search-results.jsonl"
```

完整默认实验应得到每个引擎 24 个检索点。
