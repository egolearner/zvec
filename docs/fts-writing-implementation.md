# FTS writing 实施记录

分支从 `07d2e007` 开始。以下约定供 writer、run reader 和数据库版本发布共同使用。

未封存写段的查询、同机 Top100 与 qrels 对照、实际并发读写和后续优化见[写段检索性能报告](fts-writing-query-performance.md)。

## 持久边界

- run metadata 使用 required feature 位 0（值 1），base mapping 2 表示 run-local。必需 record 4 的 payload 为小端 u64 parent_row_begin。已有 row_count 表示局部行数，二者之和不得超过 UINT32_MAX。sealed base 仍使用 mapping 1 和零 required features。sealed 入口只接受 base。
- 写段的 manifest 使用与 V1 不同的外层 envelope，只有带 bootstrap_baseline 角色、引用旧的完整 checkpoint 的编号 manifest 能在缺少 authority 时恢复。先替换旧编号，再安装 authority，最后才写新候选。
- epoch 内操作序号从 0 开始。已解析操作分别为 insert、replace、delete；insert/replace 记录新 ID 和父行，replace/delete 记录旧 ID。epoch、序号、global ID 为小端 u64，父行为小端 u32。记录绑定 schema 摘要和完整文档，CRC32C 覆盖身份、长度及 payload。只有整个数据库版本提交才可退休旧 epoch。
- WAL 前准备失败丢弃候选，保留原视图。WAL 结果不确定或 apply 失败转为 requires_recovery。该状态下拒绝数据查询、写入及 checkpoint。读锁顺序为 schema、collection write、segment。后台编码只持有不可变输入。前台在 flush、checkpoint、达到 run 上限时会等待独立的编码任务，此时同 collection 的其他操作可能被写锁阻塞。

## 模块边界

active 按字段顺序准备同一行的 optional text，准备和发布分开；满缓冲在 WAL 前 flush 或返回资源错误。active 对外提供局部行号的 FtsSegmentReader 和 FtsFieldSource，source 固定 buffer 生命周期。写段持有 active 和按 parent_row_begin 排序的 run。查询只构造一个父 reader，使用各输入的合计 N、tokens 和 DF。内部合并及 seal 使用所有输入的统计贡献，保留物理行及删除视图。

初始额度采用设计第 5 节的分项。生产启用取决于第 17 节验收，实验入口须显式选择。每次性能测量注明 WAL 路径、语料、flush/seal 边界和读取模式。

## 使用与兼容

普通新建集合沿用 RocksDB。源码中的内部入口 `internal::CreateFtsWritingCollection` 创建 run 格式集合，`internal::MigrateFtsWritingCollection` 迁移已有可写 FTS 段，`internal::SnapshotFtsWritingCollection` 将完整 checkpoint 复制到新的独立目录。新程序通过常规 `Collection::Open` 识别已提交协议。迁移会升级 manifest，升级前需保留可由旧程序打开的备份。

新写入先逐字段准备一行，再记录数据库 WAL 并发布。缺失字段和分词为空的字段都推进父行域。超过 prepare/active 容量的文档在同文件系统外排，只有整行成功才能被查询。父 reader 对 active、frozen 和有序 run 汇总文档数、token 数与 DF，并将位置和命中行转换为父行号。一个 flush worker 和一个 merge worker 可以与单线程写入重叠。单 run 封存可直接复用已编码 section，多 run 封存流式合并。在 FTS 新格式启用的库中，为已有列创建或重建索引也使用 run writer。

## 2026-11-19 的全量索引对照

每台机器、每个数据集交错构建 6 个新索引，顺序为 AB、BA、AB。A 使用 RocksDB 写入和封存，B 使用 run 写入和 custom 封存。两者均单线程摄取、开启相同的按文档数据库 WAL 写入，时间包括相应边界的 flush 或同步。下表为 3 轮中位数，单位为秒。各列独立取中位数，总时间还含发布和清理。此项测试使用 `fts_bench -mode=index`；完整 Collection 的 forward、IDMap、替换、删除和 checkpoint 在另一项试验中测量。

| 机器 | 数据集 | RocksDB 写入+刷盘 | run 写入+刷盘 | 写入吞吐倍率 | RocksDB 封存 | custom 封存 | RocksDB 总时间 | 新总时间 | 总构建倍率 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Mac | DU | 22.419 | 8.899 | 2.52× | 1.364 | 5.596 | 23.789 | 14.504 | 1.64× |
| Mac | Quora | 12.427 | 4.481 | 2.77× | 0.656 | 2.441 | 13.087 | 6.956 | 1.88× |
| 191 | DU | 52.440 | 21.248 | 2.47× | 3.472 | 14.516 | 55.904 | 35.790 | 1.56× |
| 191 | Quora | 22.544 | 4.345 | 5.19× | 1.349 | 6.745 | 23.902 | 11.107 | 2.15× |

Mac 使用 Apple M6、16 GiB 内存，191 为 Linux x86_64、Xeon Platinum 8369B。DU 的 100001 篇文档采用 jieba + lowercase，Quora 的 522931 篇文档采用 standard + lowercase。Mac 三轮的单文档写入 P50/P95/P99，DU 从 169/409/940 µs 降至 46/112/259 µs，Quora 从 17/33/44 µs 降至 7/10/13 µs。DU 三轮最大写停顿的中位数从 14.68 ms 升至 18.81 ms。DU 的新旧峰值 RSS 为 616 和 947 MiB，Quora 为 235 和 960 MiB。RocksDB 报告每个 write buffer 为 128 MiB、block cache 为 8 MiB。新 writer 的 active/frozen 各限 64 MiB。主机可用内存相同，各后端内部预算分别记录。

Mac 上 DU 完整 2000 条查询，每种读取模式独立启动 3 个进程、每进程 3 遍。Quora 全量 10000 条查询逐条验收，固定 1000 条子集每模式运行 1 个进程、每进程 3 遍。两套索引使用相同单线程 OR、Top100。custom mmap 与 8 MiB BufferPool 读取同一文件，buffer 进程启动前逐页 mmap 预热。DU 各模式 9 遍与各自同机冻结结果一致，Quora 全量亦一致，比较包含全部有序 DocID 和原始 FP32 bits。此处保留操作系统页缓存。

| Mac 搜索中位 QPS | RocksDB | custom mmap | custom BufferPool |
| --- | ---: | ---: | ---: |
| DU，全量 | 2019 | 2441 | 2268 |
| Quora，全量，一遍 | 173 | 337 | 319 |
| Quora，固定子集 | 162 | 337 | 317 |

191 上 DU 的 RocksDB / mmap / buffer 九遍中位数为 805 / 1057 / 415 QPS，Quora 固定子集为 66 / 155 / 95 QPS。两个数据集共 39 个结果文件均与该机对应的旧程序逐字节一致。两台机器各自保留了多 run 封存成本。早期 191 低内存试验中，DU 前 3000 篇使用 2 MiB active、8 MiB buffer 时，新格式总构建曾耗时 29.51 秒，RocksDB 为 3.48 秒。为合并游标复用有界 section 读取窗口后，同负载复测变为 2.15 秒对 3.49 秒。[191 逐轮结果](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/formal-a/matrix/results.json) · [191 正确性比对](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/formal-a/query-comparison.json)

### 完整数据库写入与恢复

Mac 另用 NFCorpus 的 3633 行做完整 Collection 对照，两个独立轮次交换运行顺序。每轮执行 3941 次写入或删除、36 次同线程穿插查询、3 次周期性 checkpoint，然后执行最终 flush、optimize 和 close。此项包含 forward、IDMap、删除状态及各自的真实 WAL。数值是两轮的中位数。

| 指标 | RocksDB | run |
| --- | ---: | ---: |
| 混合摄取速度，输入行/秒 | 3485 | 11070 |
| 至最终 checkpoint，ms | 1178.1 | 386.9 |
| 至 optimize + close，ms | 1453.0 | 657.1 |
| 单次写入 P99，µs | 367 | 73 |
| 混合查询 P95，µs | 834 | 190 |
| 混合查询 P99，µs | 1007 | 1079 |

两轮中每轮只有 36 次查询，P99 对应该轮最慢的一次查询。独立进程验证了同一主键的 insert、upsert、delete、再次 upsert。一轮在写入后退出，下一轮在重放第三条操作后退出，第三个进程从原 checkpoint 重新打开，核对最终主键、删除可见性和新旧搜索词后成功提交。[完整数据库对照](/Users/jiliang.ljl/tmp/zvec-fts-wal-test-20261119-1602/nfcorpus-final/results.json) · [重放中途退出验证](/Users/jiliang.ljl/tmp/zvec-fts-wal-test-20261119-1602/small/partial-final.txt)

Mac 构建和查询表使用最终程序，SHA-256 为 `db1400737fc78e7c26029c70d7abb58f1a37bcaa9b81bf8b14fa61abfcf51175`，191 正式三轮为 `58e282fee2ebfaa3b36abb475e852b0e723a6e1da529fde195dd0d0ca7da217a`。两台机器分别选取本次开发中的冻结构建，跨机仅比较各自的 A/B 倍率。Mac 前一版完整矩阵也保留，其 DU 和 Quora 总构建倍率分别是 1.65× 和 1.94×。两轮主机运行条件有波动，各轮独立统计。[Mac 最终命令](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/mac-confirm/commands.json) · [Mac 最终结果](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/mac-confirm/results.json) · [前一版完整结果](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/mac-final/results.json)

### 资源补测

191 用增加 I/O 计数器的程序，在两个数据集上各重新构建一次两种格式。DU 新旧总构建时间为 34.97 / 55.65 秒，Quora 为 10.65 / 23.41 秒。每种格式对同一语料产生相同长度的 WAL。下表的 run/封存字节按参加该阶段的完整文件计数，同一文件参加多个阶段会分别计入。原始记录还提供 spool 实际读写、单任务占盘高水位和 Linux 进程 I/O 采样。

| 数据集 | WAL 字节 | flush 输出 | merge 输入 / 输出 | seal 输入 / 输出 | 新旧峰值 RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| DU | 94913404 | 273697536 | 122291360 / 90472320 | 241878496 / 175364320 | 377 / 1285 MiB |
| Quora | 59086457 | 76671552 | 0 / 0 | 76671552 / 64500320 | 179 / 1147 MiB |

两次新写入的受管额度合计预留高水位均为 192 MiB。它包含 prepare 和 worker 的准入预留。arena 实际占用、独立 BufferPool、进程 RSS、Jieba 模型的输入大小分别计量。[DU 分项](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/counters-b/du-runs-final-build.json) · [Quora 分项](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/counters-b/quora-runs-final-build.json)

Mac 另外按原顺序截取相同 Quora 语料的前缀，固定 active 2 MiB、soft/hard run 上限 3/4、BufferPool 8 MiB，WAL 开启。下表记录各阶段实际触及的最高值。数据量从 1 万增加到 10 万时，active 实际用量和 prepare vector 容量已稳定，后者开始出现后台合并。此项用于验证内存随规模的变化。

| 输入文档数 | flush / merge 次数 | active 实用峰值，字节 | frozen 实用峰值，字节 | prepare vector 峰值，字节 | 进程 RSS 峰值 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1000 | 1 / 0 | 562856 | 583336 | 2320 | 30.8 MiB |
| 10000 | 3 / 0 | 2042760 | 2094728 | 9232 | 41.0 MiB |
| 100000 | 25 / 11 | 2042760 | 2095136 | 9232 | 56.4 MiB |

### 复跑

在工程根目录编译 `ninja -C build.release fts_bench`，使用独立的新输出目录运行。命令对两个格式显式选择 writer，确保 custom 对照走新写入路径。191 用该机相应的语料和冻结结果替换路径。

```bash
python3 benchmarks/fts/fts_writing_benchmark.py \
  --bench "$PWD/build.release/bin/fts_bench" \
  --du /Users/jiliang.ljl/Desktop/zvec-fts-v1/bench-data/du-retrieval \
  --quora /Users/jiliang.ljl/Project/BEIR/quora \
  --quora-hot /Users/jiliang.ljl/Desktop/zvec-fts-v1/bench-data/quora-hot-1000.jsonl \
  --reference-du /Users/jiliang.ljl/Desktop/zvec-fts-v1/mmap-fix-final-20261118-01/results/du-final-mmap-r1.tsv.pass0 \
  --reference-quora /Users/jiliang.ljl/Desktop/zvec-fts-v1/quora-hardening-20261118-02/results/mmap-full.tsv.pass0 \
  --reference-rocksdb-du /Users/jiliang.ljl/Desktop/zvec-fts-v1/bench-results-v6/du-rocksdb-results.tsv.pass0 \
  --reference-rocksdb-quora /Users/jiliang.ljl/Desktop/zvec-fts-v1/bench-results-v6/quora-rocksdb-results.tsv.pass0 \
  --output "$HOME/tmp/fts-writing-repro"
```

运行器也支持 `--plan` 和 `--collect`。本次 Mac 测量用前者生成清单，独立启动各次程序，将标准输出写入对应的 `<label>.txt`，按相同规则预热，再用后者核对结果。系统曾在通过子进程启动时终止压测程序；这些尝试均未产生索引或计时样本。

## 回归和默认启用门槛

- C++ 回归覆盖 active 取消与原子应用、多字段、超过 4 MiB 的单词、重复词短语、按不同顺序分割和合并后与 RocksDB 的有序结果及分数等价、删改、并发读取、DDL、迁移、封存、独立快照恢复。Linux 主回归 22 个测试目标共 465 条全部通过；后续最新源码专项 10 个目标共 171 条全部通过，两批用例有重叠。[Linux 主回归](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/final-tests.txt) · [最新专项回归](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/final-diff-tests.txt)
- Mac 最终 active 和 tokenizer/filter 共 129 条通过。新格式集合相关的查询、checkpoint、迁移、只读快照、DDL、并发替换等在常规 Collection 测试程序中通过。三个 Python FTS/混合检索测试文件在两台机器各通过 38 条。Mac 使用临时 Python 3.9 环境，191 使用指定的 `db-dev` Python 3.11 环境，两者都核对过实际加载的本次构建扩展。[Linux Python 回归](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/python-binding/result.json)
- Linux 的 ThreadSanitizer 在直接并发读写 active 和 run、强制 flush/merge 的两套插桩测试中均无告警。两种读取模式合计核对 16000 次查询。完整 Collection 并发替换时，新旧 writer 都在 Arrow Acero 的 `source_node.cc:136` 和 `exec_plan.cc:1024` 报告同一类引用计数告警。只对部分 SQL 和 Arrow 源文件进行了插桩，完整 Collection 的线程安全验收仍未通过。Mac 的插桩程序在进入测试前被系统终止。[新 writer 告警](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/tsan-sql/test.txt) · [旧 writer 对照](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/tsan-controls/legacy-all-reports.txt) · [直接并发读写，两种读取模式](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/tsan-controls/tsan-sql-DirectActiveAndRunReadersUnderPublicationBarrier.txt)
- 已用实际旧二进制创建库并连续两次写入。首次迁移在 authority 安装前注入一次真实文件系统写失败，再分别测试 authority 安装后、下一候选 manifest 写失败。两种情况下旧程序写打开均被拒绝，已提交的文件摘要保持一致，新程序可读回两条文档。[旧程序与两次失败窗口](/Users/jiliang.ljl/tmp/zvec-fts-wal-test-20261119-1602/actual-upgrade)
- Mac 和 191 的 manifest 专项各通过 25 条测试，包括新加的 16 个进程退出窗口。每次在升级的四次文件替换之一中，于写入、文件同步、改名或目录同步之前退出。重启逐项核对生效的 writer 和版本，再完成一次新提交。[191 的 16 个窗口与专项回归](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/version-crash-matrix.txt)
- 191 另对真实 Collection 的 checkpoint 在九个代码位置终止进程，覆盖 forward block 收尾、下一 WAL epoch 已创建、flush/merge、run 已就绪、manifest 切换和旧文件退休。测试语料含 50 个主键和一个反复替换、删除、重建的探针，在写入中每 6 行 checkpoint。每次终止都从独立副本重开，核对 51 个主键的存在性与完整字段值，并将四条查询的全部有序 DocID、FP32 分数与正常 checkpoint 对照。九组均一致。[九组结果](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/checkpoint-crashes/checked.json) · [复跑脚本](/Users/jiliang.ljl/tmp/fts-writing.nQZ5UNvi/linux-formal-a/checkpoint-crashes/checkpoint_crash_verify.py)
- 磁盘准入分别跟踪未 checkpoint 的 run、WAL、仍被读者持有或暂时删除失败的文件，以及每个任务的输出与临时文件。达到未 checkpoint 阈值时触发全库 checkpoint，已在执行的有界任务可以完成。退休文件超限会在下一次写入 WAL 前返回资源错误。
- 新 run 的编码仍受单任务临时磁盘限额约束。对已经接收的 run 做合并或封存时，另按输入文件、字段行数、词位数和词典块的最大展开量计算重写空间，并以目标文件系统的当前可用空间为上限。输出和临时文件共用计数；同一时刻只打开一个字段的输入。
- 本轮覆盖 macOS arm64 和 Linux x86_64。Windows、iOS、Android 构建与故障恢复、所有 I/O 故障点的进程退出矩阵、写锁外可取消的慢 I/O 等待仍属启用门槛。Jieba 对外部模型按输入规模预检，要求加载期间词典文件稳定。尚无整个进程的硬堆上限或冷盘性能结论。
