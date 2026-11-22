# 未封存 writing segment 的 FTS 检索性能

191 最新 DU 复测中，run 的 8 MiB BufferPool 达到 **81.63 QPS**，同轮 RocksDB 为 **47.04 QPS**。完整 Collection 一读一写时，run 写 P99 从 **26.25 ms** 降到 **3.55 ms**，同轮 RocksDB 为 **2.40 ms**。本页后半节记录了两处改动、固定进度召回校验及实际磁盘读量。

2026-11-20，在 Mac 和 Linux 191 上对比 RocksDB writing 和 run writing。两者均能在同一可写 Collection 中查询已成功写入的数据。run reader 合并当前 active、frozen 和已发布的磁盘 run，对整个父段计算 BM25。Collection 对 run 后端持有覆盖完整查询的读锁，写入可能等待正在执行的查询结束。

## 直接查询写段

`fts_bench -mode=index -cmd=search-writing` 建立一个新的可写索引，保持 writer 打开，在最终 flush、seal 和 reopen 之前运行查询。计时包含查询分词、构造 OR、取得当前 reader 和返回 Top100。加载语料、写入、加载查询和导出结果均在计时区间外。此项测量单独一个写段上的 FTS 检索。真实数据库的读写锁、SQL 执行和主键物化另外测量。

DU 为 100001 篇、jieba + lowercase、全部 2000 条 dev 查询，每种模式交错执行 3 个独立进程 × 3 遍。Quora 为 522931 篇、standard + lowercase，固定 1000 条查询每模式一个进程三遍，另对全量 10000 条逐条验收。各遍均为单查询线程，保留首遍。所有进程使用相同语料顺序和按文档 WAL。结果文件记录所有查询的有序内部 DocID 和 FP32 原始位模式。

召回验收同时核对排序文件和语料标注。先在同机逐字节比较每遍输出，再检查每个新库的 `document_ids.tsv` 是否与逐行读取 corpus 得到的行号、主键序列一致。每条查询的 Recall@100 等于 Top100 命中的正例数除以该查询全部正例数，使用 `qrels/test.tsv` 中分数大于零的标注，再对查询取算术平均。DU 的标注对应 dev split。各机的原始分数只在同机验收。[召回复核程序](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/zvec-live-recall-20261120.py)

每个进程独立构建写段，以保留刚摄取语料后实际的内存索引和缓存。mmap 和 8 MiB BufferPool 使用相同输入，在各自的 writer 上测量。Mac 这轮利用刚写入文件的 OS 页缓存，未另行预热文件或重置进程内 BufferPool。后台编码可能在查询开始时仍在进行。QPS 和 P50/P95/P99 各自取逐遍指标的中位数。DU 在查询开始前发布了 7 次 flush，Quora 为 2 次。

### Mac 写段基线

| 数据集 | 写段读取方式 | QPS | P50，ms | P95，ms | P99，ms |
| --- | --- | ---: | ---: | ---: | ---: |
| DU | RocksDB | 153.36 | 5.49 | 15.16 | 26.27 |
| DU | run mmap | 535.67 | 1.52 | 4.41 | 6.31 |
| DU | run buffer | 401.62 | 2.12 | 5.35 | 7.57 |
| Quora 固定子集 | RocksDB | 11.96 | 74.43 | 187.11 | 259.95 |
| Quora 固定子集 | run mmap | 65.51 | 14.06 | 29.28 | 41.69 |
| Quora 固定子集 | run buffer | 65.03 | 14.24 | 29.38 | 41.94 |
| Quora 全量 | RocksDB | 12.17 | 73.16 | 169.22 | 244.04 |
| Quora 全量 | run mmap | 66.21 | 13.98 | 28.28 | 38.37 |
| Quora 全量 | run buffer | 65.06 | 14.24 | 28.69 | 38.53 |

Mac 三种模式的 39 份结果通过同机逐字节对照，也与上一轮同机 RocksDB 和 custom 的封存结果一致。DU、Quora 子集、Quora 全量的 Recall@100 分别为 78.5789%、95.9536%、94.8008%。[逐遍记录](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/direct-results.json) · [文件校验值](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/direct-verified.json) · [qrels 核验](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/recall-at-100.json) · [完整命令](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/commands.json)

另取 DU 前 8192 篇，在相同文档和 2000 条查询上改变 run 后端的 active 上限。Mac mmap、单进程三遍的中位数，64 MiB 为 9283 QPS，8 MiB 为 2137 QPS，2 MiB 为 1077 QPS。三次分别发布了 0、6、35 次 flush，2 MiB 组还发布了 7 次 merge。同样数据下 RocksDB 为 281 QPS。四组共 12 份 Top100 输出完全相同。此表外试验改变了写段的内存与磁盘分布，文档数和查询固定。[各组布局及逐遍结果](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/profile-baseline/layouts.json)

### 191 写段基线

| 数据集 | 写段读取方式 | QPS | P50，ms | P95，ms | P99，ms |
| --- | --- | ---: | ---: | ---: | ---: |
| DU | RocksDB | 46.10 | 17.93 | 51.76 | 92.02 |
| DU | run mmap | 189.77 | 4.24 | 12.21 | 17.17 |
| DU | run buffer | 34.87 | 26.83 | 50.13 | 69.42 |
| Quora 固定子集 | RocksDB | 4.70 | 185.04 | 495.32 | 697.46 |
| Quora 固定子集 | run mmap | 24.50 | 38.33 | 80.56 | 108.91 |
| Quora 固定子集 | run buffer | 21.68 | 43.07 | 90.38 | 124.57 |
| Quora 全量 | RocksDB | 4.39 | 197.41 | 490.25 | 733.69 |
| Quora 全量 | run mmap | 24.47 | 38.54 | 76.81 | 104.74 |
| Quora 全量 | run buffer | 21.36 | 43.63 | 87.92 | 121.18 |

191 的 DU buffer 基线吞吐比 RocksDB 低 24.3%，P99 仍低 22.6%。191 的 39 份输出在同一查询集上全部逐字节一致，也与先前同机封存参考一致。DU 和 Quora 固定子集的 Recall@100 与 Mac 相同。Quora 全量在 191 为 94.8050%，Mac 为 94.8008%。逐条核对 qrels，10000 条查询中有 7 条的 Top100 命中正例数在两台机器间不同。[逐遍记录](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/direct-results.json) · [校验值](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/direct-verified.json) · [逐条标注命中](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/quora-full-positive-recall.json) · [同机封存参考](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/formal-a/matrix/results.json)

跨机再取两份 RocksDB 输出，仅比较 Top100 文档集合。DU 有 36/2000 条、Quora 固定子集有 424/1000 条、Quora 全量有 4312/10000 条的集合不同。在 Quora 全量上，每个查询平均换入 1.187 篇文档。只有其中 7 条改变了命中正例数。同机新旧后端始终使用同一份 RocksDB 输出验收。[Mac 全量原始结果](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/quora-full-rocks-r1.tsv.pass0) · [191 全量原始结果](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/quora-full-rocks-r1.tsv.pass0)

## 完整 Collection 的一读一写

使用 Python SDK 的同一个 mmap 可写 Collection，原生查询和写入在释放 Python GIL 后执行。先逐步验证 insert、upsert、delete 返回后，新词可见、旧词和删除行消失。正式负载预装 DU 的前 40000 篇，然后从同一语料继续逐篇写入 10000 篇。一个读线程、一个写线程从屏障同时出发，每个窗口发出 10 次查询和 100 次写入，共 100 个窗口；各方都完成后才进入下一窗口。每轮使用 DU 的前 1000 条 dev 查询、OR、Top100，只取主键和分数。SDK 的内存池设为 256 MiB，两个后端各配置一个原生查询线程。

每个窗口完成时用该窗口第一条查询核对同一已确认写入位置的结果。所有写入完成后，在同一个写段再执行全部 1000 条查询。用这些固定位置的结果比对有序主键和 FP32 分数。并发期间每条查询实际看到的写入进度由线程调度决定。窗口 QPS 用查询总数除以各窗口两方都完成所需的时间，查询和写入延迟单独计量。两种后端各执行三个独立进程，每轮交换顺序。

| Mac 完整 Collection | RocksDB | run mmap |
| --- | ---: | ---: |
| 混合窗口 QPS | 104.0 | 349.6 |
| 查询 P50 / P95 / P99，ms | 6.29 / 28.87 / 53.11 | 2.01 / 4.24 / 5.39 |
| 单次写入 P50 / P95 / P99，ms | 0.176 / 0.405 / 0.936 | 0.056 / 1.959 / 3.762 |
| 写线程结束后同集查询 QPS | 131.4 | 426.2 |

窗口从两线程同时出发计到两方都完成。RocksDB 各轮有 100/1000 次查询与写线程的发请求区间重叠，run 各轮为 996、1000、1000 次。读写都受窗口内另一方的耗时和并发控制影响。这组结果同时表明 run 的混合查询延迟较低，写入 P95/P99 较高。所有固定进度和最终查询在六个进程间逐字节相同。[逐轮结果](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/mixed-results.json) · [进度及最终校验](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/mixed-verified.json) · [命令](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/mixed-commands.json)

| 191 完整 Collection | RocksDB | run mmap |
| --- | ---: | ---: |
| 混合窗口 QPS | 22.1 | 36.9 |
| 查询 P50 / P95 / P99，ms | 35.81 / 94.10 / 175.52 | 25.09 / 31.45 / 34.23 |
| 单次写入 P50 / P95 / P99，ms | 0.510 / 1.140 / 2.485 | 0.144 / 25.089 / 30.316 |
| 写线程结束后同集查询 QPS | 22.5 | 35.5 |

191 每轮 RocksDB 有 104 次查询与写线程发请求区间重叠，run 每轮 1000 次。相同进度下的 100 条查询和最终 1000 条在六个进程间完全一致。run 的写 P95 与查询 P50 接近，这符合写入等待读锁释放的行为；这项测试量到端到端等待，未分别计量锁等待和存储耗时。[逐轮结果](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/mixed-results.json) · [校验](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/mixed-verified.json)

## 优化前后

两次优化分别冻结程序测量。第一步让无过滤的 OR 使用批评分，并复用词频为 1 的位置读取窗口。DU 和 Quora 仍按前文次数跑完整查询，并逐个结果文件对照同机 RocksDB。写入使用相同 WAL、相同输入、全新的目录，优化前后按 AB、BA、AB 各建三次。下表每个单元格按“优化前 → 优化后”列出。

| Mac | run mmap QPS | run buffer 8 MiB QPS | mmap P99，ms | buffer P99，ms |
| --- | ---: | ---: | ---: | ---: |
| DU 全量 | 535.67 → 946.84 | 401.62 → 589.31 | 6.31 → 2.95 | 7.57 → 4.82 |
| Quora 固定子集 | 65.51 → 141.42 | 65.03 → 136.45 | 41.69 → 14.72 | 41.94 → 15.06 |
| Quora 全量 | 66.21 → 140.51 | 65.06 → 137.65 | 38.37 → 14.35 | 38.53 → 14.44 |

Mac 的 26 份优化版写段输出均与同机 RocksDB 基线逐字节一致。DU 的 ingest/flush 中位数从 8.407 s 降至 8.030 s，seal/sync 从 5.442 s 降至 5.267 s。Quora 的 ingest/flush 从 4.458 s 降至 4.436 s，seal/sync 从 2.386 s 降至 1.895 s。两组总构建分别从 13.856 s 降至 13.299 s、6.879 s 降至 6.366 s。[各遍和各次构建](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/tuned/results.json) · [校验](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/tuned/verified.json)

| 191 | run mmap QPS | run buffer 8 MiB QPS | mmap P99，ms | buffer P99，ms |
| --- | ---: | ---: | ---: | ---: |
| DU 全量 | 189.77 → 339.43 | 34.87 → 37.67 | 17.17 → 7.79 | 69.42 → 61.28 |
| Quora 固定子集 | 24.50 → 73.11 | 21.68 → 47.26 | 108.91 → 30.87 | 124.57 → 46.54 |
| Quora 全量 | 24.47 → 73.24 | 21.36 → 57.33 | 104.74 → 29.66 | 121.18 → 38.64 |

191 的 26 份优化版直接查询输出均与同机基线完全一致。DU 8 MiB buffer 经过批评分后吞吐提高 8.0%，仍比 RocksDB 低 18.3%。DU 优化前后配对构建的摄取为 19.465 → 18.343 s，封存 14.604 → 11.920 s，总计 34.109 → 30.315 s。Quora 分别为 4.027 → 3.976 s、6.808 → 4.816 s、10.848 → 8.845 s。[逐遍和构建对照](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/tuned/results.json) · [校验](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/tuned/verified.json)

191 另取 DU 前 256 条查询，在完整写段上只改变 BufferPool 容量，每组一个进程三遍。读取 `/proc/<pid>/fdinfo` 确认两组各有 8 个索引文件描述符启用了 O_DIRECT，从加载完 qrels 到每遍结束分别采样进程的 `read_bytes`。六份输出均与同机全量参考的对应前缀逐字节相等。

| 191 / DU 固定 256 条 | 第一遍 | 第二遍 | 第三遍 |
| --- | ---: | ---: | ---: |
| 8 MiB，QPS | 37.10 | 37.80 | 37.96 |
| 8 MiB，进程存储读取增量，MiB | 311.61 | 312.73 | 312.95 |
| 64 MiB，QPS | 148.05 | 333.26 | 330.17 |
| 64 MiB，进程存储读取增量，MiB | 40.45 | 0 | 0 |

相同查询在 8 MiB 下反复触发存储读取，扩大容量后第二、三遍的工作集留在池内。若要继续优化固定 8 MiB 的表现，应按索引区段统计读页和重读分布，再决定优先调整词典、posting 或文档长度表的布局。该项诊断使用 256 条，前一张正式表使用全部 2000 条。[各遍命令及内核计数](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/tuned/pool-probe/results.json)

第一步在 Mac 混合负载的窗口中位数是 350.0 QPS，基线为 349.6。可见性探针留下了已删除的行，SQL 查询带有删除过滤条件；第一步的批评分只在无过滤查询上生效。第二步对带过滤的 writing OR 单独做成对测试，两个程序使用相同的进度屏障。

| 第二步，Mac 完整 Collection | 仅无过滤 OR 批评分 | 带过滤 OR 也使用批评分 |
| --- | ---: | ---: |
| 混合窗口 QPS | 348.65 | 410.61 |
| 查询 P50 / P95 / P99，ms | 2.053 / 4.279 / 5.399 | 1.641 / 3.274 / 4.145 |
| 写入 P50 / P95 / P99，ms | 0.055 / 1.994 / 3.790 | 0.057 / 1.630 / 2.709 |
| 写完后同集查询 QPS | 424.35 | 552.94 |

第二步按 AB、BA、AB 交错运行两个独立编译的 SDK。Mac 的窗口 QPS 增加 17.8%，写 P99 降低 28.5%，仍高于 RocksDB 的 0.936 ms。相同进度和全部写完时的查询继续与同机 RocksDB 逐字节一致。[逐轮统计、程序参数和校验值](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/filtered/results.json) · [最终扩展指纹](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/filtered/final-sdk.json)

| 第二步，191 完整 Collection | 仅无过滤 OR 批评分 | 带过滤 OR 也使用批评分 |
| --- | ---: | ---: |
| 混合窗口 QPS | 36.91 | 40.17 |
| 查询 P50 / P95 / P99，ms | 24.89 / 31.50 / 35.44 | 23.20 / 27.26 / 29.19 |
| 写入 P50 / P95 / P99，ms | 0.151 / 24.91 / 30.10 | 0.145 / 23.23 / 26.25 |
| 写完后同集查询 QPS | 34.97 | 38.40 |

191 的混合 QPS 增加 8.8%，写 P99 降低 12.8%。与同机 RocksDB 比较，最终 run 的查询 P99 从 175.52 ms 降至 29.19 ms，写 P99 则从 2.485 ms 升至 26.25 ms。这项性能对照使用删除过滤，其他过滤选择率在本轮只做了结果回归。[191 配对记录](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/filtered/results.json)

### 与 RocksDB 交错构建

每台机器分别对 DU、Quora 执行 RocksDB/run、run/RocksDB、RocksDB/run 三轮，每次使用新目录。一个前台线程顺序写入，run 允许后台 flush/merge；两种格式使用相同的 benchmark WAL。摄取包含最后一次 flush，封存包含 sync，总时间还包括发布清理，各列独立取三次中位数。最后再打开第三个 run 所生成的封存索引，用 DU 全部 2000 条和 Quora 全部 10000 条对照同机 RocksDB 写段输出。

| 主机 / 数据集 | 摄取含 flush，RocksDB → run，s | seal/sync，RocksDB → run，s | 总构建，RocksDB → run，s | 总构建加速 |
| --- | ---: | ---: | ---: | ---: |
| Mac / DU | 21.385 → 8.020 | 1.341 → 4.743 | 22.709 → 12.815 | 1.77× |
| Mac / Quora | 12.152 → 4.468 | 0.636 → 1.897 | 12.786 → 6.353 | 2.01× |
| 191 / DU | 50.225 → 18.445 | 3.405 → 11.959 | 53.640 → 30.454 | 1.76× |
| 191 / Quora | 21.834 → 4.028 | 1.333 → 4.782 | 23.173 → 8.826 | 2.63× |

两机各自的 DU、Quora 完整封存结果都与同机 RocksDB 写段逐字节一致。四组 run 的摄取速度为 RocksDB 的 2.67–5.42 倍，总构建的加速为 1.76–2.63 倍。最终封存仍需 RocksDB 的 2.98–3.59 倍时间。[Mac 各轮耗时及封存校验](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/tuned/rocks-compare/results.json) · [191 各轮耗时及封存校验](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/tuned/rocks-compare/results.json) · [Mac 命令](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/tuned/rocks-compare/commands.json)

## 实现与剩余问题

Mac 的 Quora mmap 采样中，OR 游标重排是最频繁的计算栈顶。加入 writing reader 的批读取后，active、frozen 和磁盘 run 均可以一次返回最多 128 条 posting。聚合游标将局部 DocID 加上各 run 的起点，复用子游标的词频、文档长度。现有 OR 执行器按原查询词序累加，BM25 使用父段的 DF、文档数和总词数。最终写段允许无候选 ID 的纯词项 OR 走批评分，用父段 DocID 应用过滤后再更新 TopK。显式候选 ID、布尔组合和短语仍走相应的专用执行路径。另用稀疏和密集排除条件对照 RocksDB，覆盖多次 flush、merge 和 checkpoint 重开。[查询采样](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/profile-baseline/query-stacks.txt) · [聚合游标](../src/db/index/column/fts_column/fts_writing_reader.cc) · [跨布局用例](../tests/db/index/column/fts_column/fts_generation_test.cc)

DU 的封存采样同时出现读取位置、创建 section reader 和释放内存块。对磁盘中词频为 1 的非内联 posting，改为复用当前游标的位置读取窗口。每条解码后仍校验长度、位置个数及编码，并将单个位置交给独立的游标。此改动同时覆盖后台合并和最终封存。[封存采样](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/profile-baseline/seal-stacks.txt) · [实现](../src/db/index/column/fts_column/fts_field_reader.cc)

封存仍是总构建的下一项优化重点。采样里还有词典编码和 SHA-256。词典 writer 每追加一个普通词都会试编码当前块，输出块时又编码一次。可以先试增量维护块大小，逐项验证前缀编码、溢出和大词边界，再对比 DU、Quora 的 seal/sync；当前验收保留了格式和完整性校验。[词典写入](../src/db/index/column/fts_column/fts_dictionary.cc)

### 工程实现审查

- WAL 编码在追加前拒绝超大文档时，原实现仍持有该文档的 FTS 准备对象。若库中已有待刷数据，下次直接 flush 会因存在未提交的文档而失败。此次在 WAL 错误出口立即释放准备对象，并增加“大文档被拒绝、立即 flush、再写小文档”的用例。[写入路径](../src/db/index/segment/segment.cc) · [回归用例](../tests/db/index/segment/fts_segment_publication_test.cc)
- 旧库迁移在首次写入显式提交记录前创建固定的 `wal.epoch.1`。这两步之间硬退出时，仍使用旧清单且尚无显式 authority 的库会跳过孤儿文件回收，迁移重试可能遇到同名文件。需要专门覆盖这个断点。[迁移](../src/db/index/segment/segment.cc) · [恢复](../src/db/index/common/fts_recovery.cc)
- flush/merge 使用 `std::async`。系统拒绝创建工作线程时，该边界尚未把线程创建错误转换为写入 API 的可重试状态。正常创建、恢复、迁移和 DDL 也分别组装写预算和文件生命周期依赖，新增预算项时需要同步检查四个入口。[任务启动](../src/db/index/column/fts_column/fts_writing_index.cc) · [各入口](../src/db/index/segment/segment.cc)

### 方案覆盖审查

- 旧 WAL epoch 退休时未登记文件字节数。若删除持续失败，`max_retired_bytes` 会低估占用的磁盘，后续需要覆盖故障持续期间的多次 checkpoint。[退休路径](../src/db/index/segment/segment.cc) · [字节计量](../src/db/index/column/fts_column/fts_file_lifetime.cc)
- 共享读预算覆盖存储读取范围。一个长 OR 同时持有的词项、跨 run 游标和批评分工作区仍可在这份预算之外增长，每条查询另有词项数量上限。并发长查询下，8 MiB BufferPool 和共享读预算都不能代表查询总内存。[读取接口](../src/db/index/column/fts_column/fts_reader.h) · [批评分](../src/db/index/column/fts_column/iterator/fts_batch_disjunction_iterator.cc)
- 写入完整持有 Collection 独占锁。前台在 run 数达到上限、flush 或 checkpoint 时，可在持锁期间等待编码任务和磁盘 I/O。混合测试包含正常负载下的等待；限速磁盘、硬上限、长查询和关闭取消四种情形仍需单测等待时间。缩小锁范围要先保证 active、主键、删除和 forward 的可见性一致。[方案 §8](fts-writing.md) · [后台任务](../src/db/index/column/fts_column/fts_writing_index.cc)

此前完整 Collection 的 Linux TSan 在两种 writer 上均报告 Arrow 引用计数告警，Windows、iOS 和 Android 也尚待验收。当前使用显式选择的 run writer，默认后端仍为 RocksDB。[既有验收记录](fts-writing-implementation.md)

## 最终召回核验

将两台机器的原始输出取回后，再按查询集逐文件核对每台机器的 70 份结果，包括基线 39 份、批评分优化 26 份、最终可执行程序 3 份和重新打开封存索引 2 份。每份均与同机、同一查询集的 RocksDB 参考逐字节一致。最终程序的 mmap 全量 Quora 复跑为 Mac 141.80 QPS、191 75.58 QPS，各自仍返回相同的 10000 份 Top100。两机各 15 个混合测试进程的 100 个固定进度和 1000 条最终查询也全部匹配。另在 191 检查了缓存诊断的 6 份 256 条查询前缀。[统一验收结果](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/acceptance.json) · [从原始文件复核](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/zvec-live-verification.py)

最终版本在两机各通过 38 项 Python 回归，191 的 51 项发布和恢复用例全部通过。Mac 的同组用例在第一步优化后通过 51 项，最终版通过 47 项，另 4 项需要启动子进程的用例收到信号 9，系统日志记录了 AppleSystemPolicy 拒绝执行。另用最终 Mac 程序对 mmap、BufferPool 各执行一次三进程恢复：写入确认后退出，第二个进程在回放第一条 WAL 后退出，第三个进程打开库后核对主键、替换前后的词项和命中文档，两种模式均通过。[191 回归](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/191/filtered-publication-tests.txt) · [Mac 回归](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/filtered/publication-tests.txt) · [Mac 系统记录](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/filtered/mac-publication-system.txt) · [mmap 恢复](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/filtered/recovery-mmap-recover.txt) · [BufferPool 恢复](/Users/jiliang.ljl/tmp/fts-live.igPRDsHo/filtered/recovery-buffer-recover.txt)

## 191 的 8 MiB 检索与混合负载写入尾延迟

本轮继续使用同一台 191、同一份 DU 语料与查询。纯检索在新建的未封存写段上执行，RocksDB、run mmap 和 run BufferPool 各使用三个独立进程，每个进程三遍，保留首遍。完整 Collection 的混合测试使用 mmap，每种后端三轮，仍为预装 40000 篇后并发执行 1000 次查询和 10000 次单文档写入。

| DU 未封存写段 | QPS | 查询 P50 / P95 / P99，ms |
| --- | ---: | ---: |
| RocksDB | 47.04 | 17.58 / 50.95 / 90.79 |
| run，8 MiB BufferPool | 81.63 | 11.53 / 21.23 / 29.74 |
| run，mmap | 342.95 | 2.65 / 5.49 / 7.65 |

同机 8 MiB 检索达到 RocksDB 的 1.74 倍，是上版 37.67 QPS 的 2.17 倍。三个独立 BufferPool 进程各自的首遍为 77.34、81.11、81.63 QPS。27 份 DU 结果均与同机 RocksDB 逐字节一致。[逐遍 QPS 与分位数](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/formal.txt)

### 写入为什么会等

在优化前的完整混合负载中，对主写线程实际调用的 `pthread_rwlock_wrlock` 计时。该进程（含预装）在同一把锁上出现了 1000 次超过 1 ms 的等待，累计 23.55 秒，这些等待的中位数是 23.47 ms。该轮端到端写 P99 为 26.41 ms。run 查询从取得段列表直至完成评分和正排取行均持有 Collection 读锁，写线程在进入文档存储和 WAL 路径前就要取得对应的写锁。CPU 采样同时显示，进程用户态样本的 29.55% 落在 libc 的内存复制函数。[Collection 查询锁](../src/db/collection.cc) · [锁等待采样](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/diagnosis.txt) · [CPU 采样](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/profile.txt)

查询请求只返回主键和分数。Top100 分布在多个 Arrow 批次时，内存正排原先对全表执行 `Take`，再选择输出列，正文也参与了取行。先投影请求列，再用原有索引取行，保留重复行和排名顺序。仅启用这项改动的一轮中，混合 QPS 为 260.15，写 P99 为 3.65 ms；100 个固定进度点及最终 1000 条查询均与 RocksDB 一致。整个查询继续持有读锁，插入、替换和删除的可见性遵循同一规则。这项正排优化同时用于 RocksDB 和 run。[正排取行](../src/db/index/storage/memory_forward_store.cc) · [单因素混合结果](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/projection-only/metrics.json)

| 一读一写，三进程中位数 | 上版 run | 本版 run | 本版 RocksDB |
| --- | ---: | ---: | ---: |
| 混合窗口 QPS | 40.17 | 264.86 | 41.75 |
| 查询 P50 / P95 / P99，ms | 23.20 / 27.26 / 29.19 | 2.11 / 4.11 / 5.58 | 14.62 / 71.16 / 154.19 |
| 写入 P50 / P95 / P99，ms | 0.145 / 23.23 / 26.25 | 0.129 / 2.13 / 3.55 | 0.496 / 1.11 / 2.40 |
| 1000 条查询中与写线程请求区间重叠的条数 | 1000 | 1000 | 104 |

run 的写 P99 降低 86.5%，仍比本版 RocksDB 高 1.15 ms。查询持有读锁时，前台写入仍会等待。两种后端在相同窗口发出相同数量的请求，线程实际重叠的比例有所不同。六个正式进程的 100 个进度点和最终 1000 条查询均与 191 的 RocksDB 参考一致。

在正式计时之外，用同一采样器复测最终 run。主写线程超过 1 ms 的锁等待从 1000 次变为 953 次，累计从 23.55 秒降到 2.02 秒。这批等待的中位数从 23.47 ms 降到 1.92 ms，第 100 长的等待从 26.09 ms 降到 3.22 ms。当前端到端写尾延迟仍包含查询持锁的时间。[两版写锁等待统计](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/lock-summary.json)

### 固定容量下减少磁盘读取

原词典的二分查找在每个候选分隔点都要读索引记录、定位压缩词块、还原首词。一个有 2048 个词的回归样例中，同一词查询两次，前后都需要 8 次范围读取。现在 BufferPool 的查询 reader 在首次读到分隔点时记住首词前 8 字节。只有前缀能确定顺序时才跳过对应词块，相同前缀会读取完整词。提示按底层叶块编号共享，每个词典的空间上限为 64 KiB 且不超过池容量的 1/64，全部计入同一 BufferPool，不能取得预算时沿用完整查找。超大词典按固定间隔采样，mmap 和顺序合并继续直接读取。索引文件格式保持兼容。[词典实现](../src/db/index/column/fts_column/fts_dictionary.cc) · [前缀冲突、精确命中与预算释放用例](../tests/db/index/column/fts_column/fts_field_test.cc)

以完整 DU 语料新建写段，只取固定前 256 条查询，从加载完 qrels 到每遍结束读取进程的 `/proc/<pid>/io`。两版均有 8 个启用 O_DIRECT 的索引文件描述符。上版三遍实际存储读量为 308.71、312.84、312.28 MiB，本版为 159.19、135.09、133.50 MiB。第三遍读量下降 57.2%，同一份查询在两版的全部输出与 RocksDB 参考前缀相同。[上版内核计数](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/io-baseline.json) · [本版内核计数](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/io-final.json)

### 召回、构建与回归

DU 2000 条 dev 查询的 Recall@100 为 78.5789%，Quora 固定 1000 条为 95.9536%，全量 10000 条为 94.8050%。逐个索引核对了内部 DocID 与原始语料主键的映射。Quora 的 mmap / 8 MiB BufferPool 在固定子集分别为 73.53 / 60.57 QPS，全量为 75.59 / 60.11 QPS。Quora 两种模式都与此前冻结的同机 RocksDB 完整输出比较，DU 还使用本次重跑的三个 RocksDB 写段作对照。混合查询在窗口结束时和全部写完时有共同的可见数据，逐条一致性在这两处验证。[指标、每遍分位数及结果指纹](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/summary.json)

| 新索引，三组交错构建中位数 | 摄取含 flush，s | seal/sync，s | 总构建，s |
| --- | ---: | ---: | ---: |
| DU，RocksDB | 51.456 | 3.423 | 54.849 |
| DU，run | 18.361 | 11.797 | 30.163 |
| Quora，RocksDB | 22.192 | 1.316 | 23.515 |
| Quora，run | 3.981 | 4.735 | 8.733 |

run 总构建在 DU、Quora 分别达到 RocksDB 的 1.82 倍和 2.69 倍，seal/sync 仍分别耗时 11.80 秒和 4.74 秒。第三个新建 run 封存索引重开后，两组全量查询也逐字节匹配。两机的词典、代际、FTS 查询和内存正排共 112 项 C++ 回归均通过，191 另通过 38 项 Python 回归。[构建与重开参数](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/builds/commands.json) · [191 C++ 回归](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/final-build.txt) · [191 Python 回归](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/python-tests.txt)

取回本机后，另以本轮之前保存的 191 RocksDB 原始排名为参考，重新核对 37 份正式写段和封存结果、12 份正式混合结果文件，以及诊断用的 12 份查询前缀和 6 份混合结果文件。另已核对两机参与本次改动的 6 个源码与测试文件指纹。[逐文件验收](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/local-verification.json) · [本轮 191 实际参数](/Users/jiliang.ljl/tmp/fts-du-cache-lock-20261120-J2WHdiyq/commands.json)

## 复跑

下面的命令在工程根目录运行。`DU` 指向 DU 语料目录，`OUT` 指向已创建的输出父目录，使用新的索引、结果前缀和混合测试目录。

```bash
build.release/bin/fts_bench -mode=index -cmd=search-writing \
  -index="$OUT/du-live-run" -corpus="$DU/corpus.jsonl" \
  -query="$DU/queries.jsonl" -qrels="$DU/qrels" \
  -extra_params='{"tokenizer":"jieba"}' -index_writer=runs -index_format=custom \
  -index_wal=true -index_mmap=true -index_cache_mb=8 -threads=1 \
  -topk=100 -default_operator=or -index_passes=3 -result_file="$OUT/du-run.tsv"

build.release/bin/fts_bench -mode=writing -cmd=create -index="$OUT/du-collection" \
  -extra_params='{"tokenizer":"jieba"}' -index_writer=runs -index_mmap=true
python benchmarks/fts/fts_writing_queries.py --collection "$OUT/du-collection" \
  --writer runs --corpus "$DU/corpus.jsonl" --queries "$DU/queries.jsonl" \
  --output "$OUT/du-mixed"
```

直接写段的 RocksDB 组使用 `-index_writer=rocksdb -index_format=rocksdb`，run 的 BufferPool 组使用 `-index_mmap=false`。混合测试的 RocksDB 组在建库、压测两条命令里均选择 `rocksdb`。Python 应加载与建库工具同一份源码构建的扩展。
