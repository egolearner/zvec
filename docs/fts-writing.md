# FTS writing 去 RocksDB：有界写缓冲与不可变 run 实施设计

状态：待实现的设计与交接规范。日期：2026-11-19。

基线分支：`codex/fts-v1-hardening`；基线提交：`07d2e007b5a80fcc451040f793e9823dd5f0aa24`。本文不表示下述能力已实现，也不包含代码修改。

本文继承 [自定义不可变索引 v1 实施设计](fts-custom-index-v1-implementation.md) 的词项、位置、精确 doclen、评分兼容、双读取模式及发布约束，替换其中“writing 继续使用 RocksDB”的选择。代码现状以该提交和 [实现进度](fts-custom-index-v1-progress.md) 的最新记录为准。外部依据集中在 [Lucene / Elasticsearch / SQLite FTS5 官方机制研究附录](fts-writing-reference-notes.md)。与旧讨论冲突时，本文只覆盖新的 writing 路径；现有 sealed 格式的语义不被默默改变。

## 1. 决策与范围

采用 **有预算的内存倒排缓冲 + 少量不可变磁盘 run + 后台合并 + 上层数据库 WAL**。内存数据直接可查，flush 生成可查 run；seal 流式合成当前自定义 sealed 格式。FTS 不再建立自己的 RocksDB 实例、CF、WriteBatch、merge operator 或 WAL。

范围是 FTS 写入后端。IDMap、标量索引等数据库组件继续使用现有存储；不承诺整个数据库移除 RocksDB。

本期必须满足：

1. 成功返回的写入可被后续查询看到，不引入周期 refresh 才可搜索的延迟。
2. 新增 FTS 写入、flush、merge、查询工作区具有分配前预算；不常驻全量词典、posting、doclen 或文档重映射表。
3. 文件读取服从数据库持久化的 `enable_mmap`：mmap 与 BufferPool 两种模式都支持且语义一致。
4. flush/merge 不改变逻辑父 segment 的 BM25 统计、DocID、分数或排序；删除仍用数据库删除视图。
5. run 落盘不等于数据库 checkpoint；只有完整数据库版本提交后才能回收对应 WAL。
6. 新旧库可明确识别，旧 RocksDB FTS 可迁移，失败保留可恢复来源；新后端不得暗中退回 RocksDB 写入。

FM-index、wildcard、regex 均延期。只保留已有字段身份、词典 generation、canonical ordinal/fingerprint 和可选辅助索引挂接能力，见第 15 节。不实现动态 FM、反向词典或 trigram。

不在本期引入全数据库 MVCC、全局 BM25、评分公式调整、压缩 norm、持久化增量 merge 检查点或新的公共事务 API。

## 2. 已有实现与必须处理的差异

以下路径均相对仓库根目录，符号用于其他机器定位，不能只按本文行号套补丁。

| 基线实现 | 本方案的衔接要求 |
| --- | --- |
| `fts_column_indexer.cc::insert` 生成完整 `vector<Token>`，再建立 `term -> vector<position>`，最后写跨 CF batch | 不能仅替换最后的 WriteBatch；分词、聚合和单文档暂存也必须纳入预算 |
| `fts_indexer.h::kCustomWritesEnabled=false` | 当前生产 writer 仍有默认关闭门禁；新后端需要独立能力门禁和格式检测，不能假定已经默认使用自定义 seal |
| `FtsSegmentReader / FtsReadTerm / FtsReadPosting` | 复用查询抽象，新增一个代表整个 writing 父段的 reader，不逐 run 单独做 Top-K |
| `FtsFieldWriter` 先收 doclen/统计，再收有序词项；4 MiB doc 表缓存加磁盘暂存 | 复用编码，但 run 必须只覆盖自己的局部行域；不能为每个小 run 写整个父段的 doclen |
| `MergeFtsField(kCompact)` 会过滤删除、重编号，并令目标 N 等于目标物理行数 | writing 内部合并不能直接调用这个策略；新增保持父行号和统计贡献的策略 |
| `FtsContainerReader::Open`、`FtsFileOwner`、`FtsRetirementQueue` | 复用双模式范围访问和文件保活；预算必须从“每容器一份”提升为共享父预算 |
| `SegmentImpl::insert` 先 append WAL；`internal_insert` 内还可能触发全段 flush | 前移容量检查、前置 checkpoint 和资源预留，禁止把尚未 apply 的当前 WAL 记录包含进清理范围 |
| `WalFile` 无持久 LSN；恢复按 writing forward block 找 WAL 并从头重放 | 不假定已有 LSN。新增显式 WAL epoch 整文件轮换，覆盖 delete-only checkpoint |
| `CollectionImpl::write_impl` 错误后可能继续批次；查询只在取 segment 列表时短暂持写锁的共享侧 | 新后端首版增加完整查询读屏障和 WAL 后失败的恢复必需状态，见第 8、10 节 |
| sealed DDL 重建仍可临时创建 RocksDB FTS 再转换 | 新后端的创建字段、重建、恢复、seal 均需走同一无 RocksDB builder，不能只改正常 insert |

代码入口目录：`src/db/index/column/fts_column/`；集成入口为 `src/db/index/segment/segment.cc`、`src/db/collection.cc`、`src/db/index/common/{meta.h,version_manager.cc,fts_recovery.cc,manifest/manifest_codec.cc}`。

## 3. 借鉴机制与本项目选择

| 参考对象 | 借鉴 | 本项目作出的选择 |
| --- | --- | --- |
| Lucene | 内存倒排缓冲刷成不可变段；active/flushing 分开计费；reader 生命周期与文件删除解耦 | 一个 collection 的受管总预算；首版单 active、单 frozen；小 run 使用现有自定义 codec |
| Elasticsearch | 搜索可见、操作日志持久化、索引 commit 分离；合并欠账时对写入背压 | 复用数据库 WAL；立即写后可见，不照搬周期 refresh；不新增 FTS translog |
| SQLite FTS5 | pending hash 可作为查询输入，与磁盘段合读；分摊合并工作，段过多时强制推进 | 查询合并 active/frozen/runs；硬限制 run 数；后台工作不足时阻塞或明确资源错误 |

Lucene 的 RAMBufferSizeMB、FTS5 的 pending hash 阈值均不是整个 writer 的分配前硬上限。这里的预算和失败协议是本项目新增的要求，不是通过沿用它们的默认值就能获得的性质。依据及版本边界见 [研究附录第 2–5 节](fts-writing-reference-notes.md)。

## 4. 结构与不变量

```text
Document / Update
    -> PrepareDocument：分词、资源预留、必要的文档级 spill
    -> 数据库 WAL append
    -> 数据库各索引 apply + FTS 文档发布
    -> ActiveBuffer
          | freeze
          v
       FrozenBuffer ---- bounded flush ----> ImmutableRun
                                                |
                             adjacent merge <---+-- RunSet
                                                |
                              seal ------------> Sealed v1 container

Query -> 一个 WritingReadView -> 一个 FtsSegmentReader -> 现有 AST / collector
                   | active + frozen + ordered runs

Collection committed Version -> checkpoint RunSet + next WAL epoch
```

核心不变量：

- 父 segment-local row ID 是 FTS 对外唯一行号；不同已发布输入的行域互不重叠，按时间排列，并完整覆盖已应用的 `[0, row_frontier)`。
- 缺字段的行也占行域；存在但零 token 与缺字段保持区分。输入文档缺少所有 FTS 字段时也必须调用 writer 的行完成接口。
- 更新/upsert 沿用当前“删除旧 global DocID，再追加新物理行”的模型，不原地修改历史 posting。内部 run 合并不清除删除，不重排行号。
- 每个查询固定一份 root、统计和删除/forward 可见状态。每个 term 的 posting 至多来自每个行域一次。
- 一个文件可以属于 live root、durable checkpoint root、构建任务、snapshot、查询中的多个对象；只有所有所有者释放后才能删除。
- run、队列、reader、文件句柄、暂存空间及保留旧版本均有预算或数量限制；到限背压，不能通过排队绕过限制。

术语：`live root` 是当前查询看到的 run 集合；`checkpoint root` 是数据库已提交 manifest 指定、崩溃恢复必须保留的集合。两者可以不同，不能混用其回收条件。

## 5. 内存、磁盘与背压

### 5.1 承诺范围

承诺的是**新增 FTS 子系统的受管内存**，包括自身分配、容器容量、索引工作区和读取 pin；预算与累计文档数、累计词项数无关。schema 大小和查询并发也要受限。

不把现有全数据库 RSS 宣称为常数：调用方的原始文档、既有 `doc_ids_`、删除 bitmap、forward/vector/IDMap、分词模型常驻数据，以及 OS page cache/mmap 驻留都有独立边界。mmap 的 pin 预算不限制内核保留已经触碰的映射页。需要限制进程 RSS 的部署应单独约束这些组件；本期不偷偷改为仅支持 BufferPool，也不声称实现了窗口 mmap。

可用的总量关系为：

```text
M_fts_owned <= B_write + B_read + B_analysis_models
B_write >= active + frozen + document_prepare + flush_workspace
           + merge_workspace + metadata_and_retired_memory
```

其中 `B_read` 与数据库共享 BufferPool cache 的预算分别计量，避免重复计费或漏掉 bypass scratch。allocator 容量、hash bucket、对象头和固定页碎片计入其所有者；不能只按有效 token 字节估算。

建议首轮基准使用以下**可调候选值**，不是性能结论或已经存在的配置：

| 项目 | 初始额度/数量 | 作用域 |
| --- | --- | --- |
| FTS 写预算 | 256 MiB | collection 共享；正常写、恢复、DDL 竞争同一额度 |
| active / frozen | 各最多 64 MiB；各 1 份 | 所有字段合计，排序辅助空间也计入 |
| document prepare | 32 MiB | 所有字段合计；超出走受限 spill，不能增加并发份数 |
| flush 工作区 | 32 MiB；1 个任务 | 含 doc 表缓存、section 缓冲和编码器 |
| merge 工作区 | 48 MiB；1 个任务 | 输入游标、输出编码及排序堆共享 |
| 元数据/retired 记录 | 16 MiB | 分页目录缓存、root、队列、owner；不是每 run 16 MiB |
| 读取工作区 | 64 MiB | collection 共享；各容器、query 子预算可更小 |
| run 数 | soft 8 / hard 16 | 每个 writing root；含即将发布的 run 预留槽位 |
| merge fan-in | 4；资源不足可选 2 | 仅相邻行域 |
| 后台任务 | 1 flush + 1 merge | 不再有无界 pending job 列表 |

以上写预算分项之和为 256 MiB；借用空闲分项也必须保留 flush/merge 的最低前进空间。实际预算最小值由 codec、字段数、页大小校验，配置不足在创建/打开时拒绝，不运行后才发现永远无法 flush。

### 5.2 分配与推进规则

1. arena/页/容器扩容前获取 lease；销毁或所有权转移后再归还。freeze 只是转移归属，不释放内存额度。
2. 单 active 放不下下一文档时冻结它；没有 frozen 槽位或 run 槽位时，等待任务推进。不能再创建第二、第三个 frozen 来缓解压力。
3. 等待预算前释放 collection/segment 发布锁；工作完成后重新加锁并检查版本。后台任务不得等待由当前等待者占有的锁。
4. 所有等待支持关闭、任务错误、超时/取消；不能前进时返回明确 `RESOURCE_EXHAUSTED` / I/O 状态。WAL 前失败可重试，WAL 后失败按第 10 节处理。
5. 长查询保活的资源仍计费。首版写查询互斥本身会造成背压；独立 source/snapshot owner 也不能使 retired 队列无限增长。
6. 不缓存全量全局 term→DF 表。词项查找访问磁盘分页词典；root 只保存有限 run descriptor。

发布后仍被 source/reader 持有的 frozen arena 继续占用原 frozen 的额度和槽位，直到最后一个引用释放；不能转记到 16 MiB 元数据额度后再创建一份 64 MiB frozen。retired 记录预算只覆盖管理对象，底层页的 lease 跟随页的实际生命周期。

查询在取得读屏障前完成主要预算准入；执行中新增范围/游标申请必须立即成功或返回资源错误，不持部分 lease 等待其他查询释放。若需重试，销毁全部查询状态、释放屏障及预算后整体重启，不能释放锁等待后继续使用原 active cursor/统计。前述“等待前释放锁”不是允许查询中途失去快照。

### 5.3 磁盘也有预算

临时文档、section spool、未提交 run、merge 输出、旧 checkpoint、旧 reader 和 WAL 分别计量。每次任务先预留可用空间与文件句柄，输出过程中按块扣减；不足保留旧 root 并失败，不删除恢复所需输入。

checkpoint root 最多 16 个 run，live root 最多 16 个 run，候选任务数量固定，但它们的总字节仍可能很大。必须设置 `max_uncheckpointed_bytes`、`max_retired_bytes` 和临时空间配额，到限触发完整数据库 checkpoint 或暂停写入。不能靠 FTS 单独截断 WAL释放空间。

持久候选文件位于 segment 同文件系统的受控私有目录，便于 rename/fsync。开发测试的独立临时目录在 macOS 使用 `/Users/jiliang.ljl/tmp`。现有 `FtsSpool::Create()` 使用 `std::tmpfile()`；应新增显式 workspace/budget 参数，避免默认临时位置和文件描述符绕过资源治理。

## 6. 文档准备与内存索引

### 6.1 单文档必须先准备，再修改可见状态

新增 `PreparedFtsDocument`：拥有该文档所有 FTS 字段的 presence、精确 doclen、词项/位置数据、资源 lease，以及需要时的私有磁盘来源。准备时不改变 active、父统计或删除视图。

普通文档在受限 arena 内聚合；文档超过 prepare 额度时输出私有排序片段，通过固定 fan-in 外部合并生成一个单文档 run。该 run 在整篇文档成功前不可查询，也不能挂入 checkpoint。单文档仍是原子可见单位，不把一篇文档的半组词项发布出来。

文档级排序片段写入受控追加文件，片段目录也分页落盘；外部归并用固定数量读窗口和交替的输出文件，不能把所有片段名称/offset 先收进内存 vector。排序 key 包含字段、无符号字节序 term、position 及必要的稳定序号，同词位置按原语义保留。每个 merge head 只保留有界词字节窗口，超长词比较分块完成；各 pass 的 I/O 和峰值磁盘计入 prepare 配额。单文档 prepare 并发为 1，等待该准入槽位不占 collection 发布锁。

大文档直接 run 的路径先 flush 旧 active，使行域顺序明确；资源、文件及 row 槽位准备好后才 append WAL。文件可以尚未 fsync，但其内容必须写完、关闭并可有限校验打开。普通文档 commit 只链接已预留页；大文档 commit 只挂接已准备 reader/root。WAL 后不再做可预见的大分配、分词或排序。

单文档多字段总量共享预算。预检 row ID、doclen/TF/position 的完整 uint32 边界和 64 位总量加法；不可通过强制类型转换截断。与当前位置契约相同，保留同位置的多个 token，不擅自去重。

### 6.2 分词接口是必要工作，不是可选优化

新增可返回错误的 `analyze_to(text, TokenSink, AnalysisBudget)`。token/filter 链按 token 或有限窗口输出；sink 同时检查 token 字节、位置及预算。旧 `process()` 可以作为兼容调用的收集适配器，但新 writer 不调用它来生成无界 vector。

- whitespace、standard、ngram 与逐 token filter 实现流式/分块路径，保留原有分词和 position 语义。
- 单词跨输入块时不得截断；过滤器输出也要在分配前计费。超长词可用受预算的分块来源进入磁盘排序；无法处理时在 WAL 前返回资源错误。
- Jieba/外部 analyzer 不得仅在全量 `Cut()` 返回后检查 token 数。为每个模式提供经核验的输入工作单元上限和工作区上界，或实现语义等价的有界输出适配。不能任意切文本改变分词。
- 尚未实现有界适配的配置不得启用新 backend；在 schema 打开/创建阶段报告不支持及具体 analyzer。大输入超出已公布限制时可在 WAL 前拒绝，不能静默丢 token 或回退 RocksDB。
- 分词字典/模型加载与缓存使用单独有限配额，禁止为每次查询或每个 run 复制模型。

“所有现有 analyzer 均通过有界适配与差分测试”是默认启用门槛。首个内部开发切片可以只支持已完成适配的 analyzer，但不得作为完整替代宣布交付。

### 6.3 ActiveBuffer

每个 active 覆盖一个连续父行域。全部字段共享一个预算，字段内保存：

- 受限 hash：term 字节只存一份，entry 保存该 buffer 的 DF、TF 合计和 posting 链首尾。
- 分页 append-only posting：按父行追加 `{local_row, tf, position_ref}`；不为高频词建立不断倍增的大 vector。
- 分块 position 字节流：单文档 TF 可以很大；长度、块目录和数据均分块，不一次 materialize 全部位置。
- 局部 doclen/presence 页：覆盖该 buffer 行域，缺字段补间隙；全无字段行同样受行元数据预算约束。

hash 扩容必须同时预留旧表与新表的峰值，或使用固定容量/分段表。posting 页和 term 对象地址在 buffer 生命周期内稳定。首版并发由第 8 节的读屏障保证，不额外引入 lock-free mutable 链。

freeze 后禁止修改。排序只排序 term 引用，按无符号字节序；引用数组必须提前从同一额度预留，不能 flush 时临时申请 O(全词典) 的第二份内存。字段顺序处理，避免每个字段同时打开一组编码器。

## 7. run 格式与 flush

### 7.1 复用编码，显式区分行号语义

run 复用现有 `terms / term_index / term_meta / doclen / postings / skips / positions / position_index`、容器 framing 和分块 codec。不要另造一套通用 KV 存储。

但当前 v1 明确使用父 segment-local DocID。新 run 必须增加**必需语义特征** `run_local_docids` 与明确的 `document_domain` 记录；未实现者拒绝打开。不能只加可忽略的 optional tag，再让旧 reader 把局部行号当父行号。

建议元数据：

| 数据 | 契约 |
| --- | --- |
| `kind=writing_run`、format/required features | 与 sealed base 显式区分；实际数值在 W0 冻结并写 golden |
| run UUID / generation | 文件不可变身份，不用序号推断是否 committed |
| parent segment / FTS block / schema identity | 防止跨段、跨字段 incarnation 挂错 |
| `parent_row_begin`、`row_count` | 文件内行号 `[0,row_count)`；父行号为 begin+local，检查加法及父域边界 |
| 各字段 `present_count / scoring_document_count / total_tokens` | 保存这个行域的统计贡献，禁止直接用于父查询评分 |
| analyzer/scoring identity、dictionary identity | 与当前规范一致；不重新分词已生成 run |
| section 引用、codec、目录保护 | 复用现有规范；正常打开仍有限校验 |

运行时的 run-local ordinal 只在该 run 的字段词典 generation 内有效。run 文件不得被普通 `OpenCustom` 当作完整 sealed base；应通过受限的 run adapter 暴露 posting 数据。

### 7.2 flush 顺序

1. 固定 frozen 的行域、字段集合和统计，分配 run 身份。
2. 按局部行序输出 doclen/presence 输入，完成 `finish_documents()`；这里只处理本 run，成本不随父段已经写入的行数增长。
3. 按排序后的 term 顺序输出 posting/positions；posting 行号转换为 run-local，保留 TF 和位置原值。
4. 利用现有 spool/IndexPacker 生成连续 section、词典及 metadata；对齐计算仍以主文件绝对偏移为准。
5. 完成文件和有限预打开；在锁内以“frozen 身份仍匹配”为条件替换成 run。准备好所有对象后才做无失败的发布。
6. 旧 frozen 在最后一个 reader/source 释放后归还预算。若发布失败，frozen 仍是合法 live 输入，不能先释放。

正常 flush 可以产生尚未进入 checkpoint 的文件，不执行 collection manifest 提交。其数据恢复依赖 WAL；独立 fsync 不会改变这个事实。

### 7.3 writer 复用限制

现有 `FtsFieldWriter` 校验 `N <= row_count`，run 中应填局部统计贡献，不能填父 N。其持久化 score bound 即使编码合法，也不用于 writing 查询。允许首版按局部统计写出既有 bound 字段以复用 codec，adapter 必须屏蔽它。

对单大文档或非常高 DF 的词，词项输入、raw posting 暂存和位置编码均须流式。需要暂停/取消的工作循环以有界块为单位；若现有 `end_term()` 内部一次扫描整个 raw posting，则增加可续作内部状态或显式承认取消等待至该词结束，不能把“按词暂停”宣传成固定时间片。

双模式仍走 `IndexStorage`。reader 按需打开当前 run，不能在每次创建 WritingReadView 时映射/缓存全部文件内容。container 的 read budget 必须成为 collection 共享预算的子 lease，不能 16 个 run 各自无条件获得独立 64 MiB。

## 8. 查询、评分与并发

### 8.1 一个父段，一个 reader，一棵 AST

`WritingRunSetReader` 实现 `FtsSegmentReader`。查询只构建一棵当前 AST，使用当前 collector；不能“每个 run 各取 Top-K 再合并”。

对词 t，在固定 read view 中：

```text
DF(t) = DF_active(t) + DF_frozen(t) + sum(DF_run_i(t))
N     = sum(各行域的 scoring_document_count)
T     = sum(各行域的 total_tokens)
```

不重叠行域保证 DF 可相加，无需全局去重表。N 不是 live documents，也不直接用当前 row_frontier 替代：缺字段、零 token、迁移来源和 reducer 的兼容统计口径必须保留。删除在内部 run 合并时不扣减统计，直到数据库既有跨 segment compaction 按原规则处理。

term handle 向 AST 提供聚合 DF，因此 `TermDocIterator::cost()`、Conjunction 排序和浮点累加顺序不因 run 切分而变化。当前分支已有批量 OR 路径的浮点累加行为；本任务以当前 HEAD 为基线，不能借格式变更再次调整评分顺序。

### 8.2 cursor 组织

- `lookup/lookup_many` 只查有限 run 的分页词典与 active hash，返回有限个轻量 handle；不读取 posting 全值。
- term posting cursor 按行域顺序拼接，当前只打开一个来源的 posting cursor；`advance(target)` 先定位行域再转 local row。无需为每个 query term 预开全部 run 的 cursor。
- positions 按候选父 row 路由唯一来源；重复 phrase term 保留独立逻辑 cursor。只读所需位置块，错误和 EOF 分开。
- missing term、空字段、空 run 及删除覆盖沿用当前 AST 语义。候选 ID/过滤 ID 的转换必须发生在明确的父行号层。
- 首版 writing reader 设置 `supports_batches=false`，所有写段 term/block bound 返回正无穷，禁用依赖有限 bound 的跳块。保留完整求分，避免使用局部 DF/N 的 bound 漏结果。
- sealed 查询继续使用现有实现。后续可以为固定 WritingReadView 推导保守上界和批量 cursor，但必须单独证明并差分，不作为本期正确性前提。

### 8.3 首版同步策略：完整查询读屏障

首版相关查询从捕获数据视图到执行结束持有 collection `write_mtx_` 的共享侧；写入/更新/删除及逻辑 root 发布持独占侧。这样 active hash/positions、统计、forward 和删除视图在查询期间稳定，无需首版就引入跨组件 MVCC。

锁序固定为：`schema_handle_mtx_ -> write_mtx_ -> segment/FTS publication lock`。后台编码在锁外执行；只在发布时取上述锁。禁止持 FTS 锁回调 collection，禁止持发布锁等待预算或后台任务。

必须覆盖单 query、多 query、binding snapshot；需要与完整文档一致的 fetch/group_by 也遵守屏障。多 query 由入口持锁直到所有 worker 完成，worker 不重复取得同一 collection 锁。取得屏障后调用 `get_all_segments_unsafe`，避免递归 shared lock；审查已有 `delete_by_filter` 等已经持锁的 unsafe 调用链。

代价明确：长查询阻塞写入和 root 发布，持续批量写也可能增加查询等待；需要公平准入/超时，避免写饥饿。后台构建仍可进行。首版不承诺“长查询完全不影响写路径”。如混合负载基准无法接受，应另做固定 epoch、可追加 posting 快照及跨存储可见屏障设计，不能简单提前释放锁来提速。

`shared_ptr<ReaderGeneration>` 继续负责文件/arena 保活，不能代替数据同步。关闭先停止准入、取消或等待后台任务，再释放 owner；不得在持 collection 独占锁时等待需要该锁才能退出/发布的任务。

## 9. 合并策略与开销

### 9.1 有限 run 的相邻合并

首版按近似输出字节选择 2–4 个**相邻行域**、大小接近的 run。达到 soft 阈值提前调度；到 hard 阈值，在 WAL 前暂停新文档并完成可减少 run 数的合并。若没有近似同尺寸候选，可选写放大较小的相邻组，但不得不断重写一个巨大老 run 来容纳微小新 run。

复用已有 `max_doc_count_per_segment` 限制父段增长，增加按 FTS 已写字节/合并层数触发提前 seal 的策略。run hard cap 是绝对值，不是“每层固定、层数无限”。异常大单文档可独占一个 run/父段；其资源准入不能绕过预算。

新增 `kCoalesceRuns` 策略：

- 输入行域必须相邻、无重叠，字段 incarnation/analyzer/statistics policy 一致。
- target begin 等于首 run begin，target rows 为行域长度之和；局部 DocID 按偏移变换，父 DocID 不变。
- doclen/presence 流按行域连接；N、tokens、presence 贡献精确相加；不应用删除位图，不修改 IDMap/forward。
- 词项以固定 fan-in 归并；同词 posting 按行域连接，位置流式重编码。无需全 posting hash、全 docID map 或全词典集合。
- metadata、词典 ordinal/fingerprint 重新生成；只在 codec/相对引用均兼容且已证明无需重定位时复制字节，不承诺 positions 零拷贝。

### 9.2 调度和发布

一个 merge job 持有限输入、有限 cursor/工作区和一个输出，支持按 I/O 块节流；任务队列只保存一个当前计划。首版不持久化中间 merge 进度，崩溃后丢弃未提交输出并重做。

发布时校验输入 run 身份仍在 live root 且 schema 未变；替换指定连续子集，不覆盖期间追加的新 run。任务过时则放弃候选。publish 不推进 checkpoint root，旧 checkpoint 输入仍需保留。

同一 run 不同时被两个任务合并。DDL/seal 排他接管 writer，先停止调度并等待当前任务在安全点退出，再捕获稳定输入；等待时遵循第 8 节锁序。

### 9.3 与 RocksDB 的成本比较口径

设新写逻辑倒排字节为 S，每份数据平均被内部重写 L 次；新 run 落盘约写 S，内部合并累计约读/写 L×S，最终 seal 可能再读/写一次。压缩率、选段策略和暂存会改变常数；这不是精确 I/O 预测。

| 项目 | 当前 RocksDB writing | 新 run writing |
| --- | --- | --- |
| 每 term/doc 更新 | bitmap merge、TF、positions、maxTF 等多个 CF 记录 | 内存 term entry + 追加 posting/position；flush 后共享词项前缀 |
| 后台写放大 | memtable/SST flush、CF compaction、merge operand 合并，然后 seal 转换 | run flush、run merge、最终 seal；仍然有写放大 |
| 统计与恢复 | 部分交给 RocksDB，仍需数据库 WAL 协调 | 必须自行保证统计、epoch、root 发布和错误恢复 |
| 查询放大 | mutable RocksDB seek/MultiGet/解码 | 至多 R 次词典定位；posting 按行域惰性连接 |
| 单热点词 | operand 合并/大 value 可能很重 | 分块 posting/position；合并仍需读写长列表，但内存固定 |
| 写后检索并发 | 依赖既有实现 | 首版读屏障较保守，可能成为回归点，必须实测 |

新方案预期减少重复 key/CF 和通用 KV 路径的开销，但不会消除倒排数据的排序、编码、合并与恢复成本。是否优于 RocksDB，以第 17 节同口径实测为准。

## 10. WAL、checkpoint 与故障协议

### 10.1 先修清楚的写入顺序

单次文档操作依次为：

1. 校验 schema/主键/更新目标，完成需要的原文合成；预留所有 FTS 资源并准备文档。
2. 完成容量导致的 forward checkpoint、FTS flush/merge/父段切换、memory components 初始化；这些均在该文档 WAL append 之前。
3. 取得写排他屏障，重新核对父 segment 身份、row、schema、`pk -> resolved_oldID`、目标删除状态和预留；不满足则回到准备阶段或返回明确冲突。delete-only 不推进 row，因此不能只检查 row frontier。
4. append 数据库 WAL；随后执行删除/IDMap/forward/标量/向量及 FTS apply，完成统计与 row frontier。
5. 所有组件成功后才向调用方报告成功并释放写屏障。批次仍保持现有逐文档状态，不宣称整个 batch 原子。

上述准备需要与现有 collection 批处理锁重组，不能在持独占锁的整批循环中等待一个需要该锁发布的 flush。

预检错误发生在 WAL 前：不产生可见改变，可返回明确失败。WAL append 结果不确定、或 append 后任一 apply 失败：collection 进入 `requires_recovery`，停止批次剩余操作；相关 query/fetch、后续写和 checkpoint 均拒绝，直到重开恢复。不能释放锁后继续提供半更新文档。

该错误表示结果可能在恢复后生效，调用方不能按“确定未写入”直接重试。复用已有状态框架并给出明确错误文本；不要默默继续 batch，也不要以只回滚 FTS 假装完成整个数据库回滚。

### 10.2 采用显式 WAL epoch，不引入虚构的现有 LSN

当前 WAL 路径与 segment/forward block 关联，普通 forward checkpoint 会换 block，而 delete-only 不一定换。新协议增加持久的 `wal_epoch` 和对应文件身份，与 forward block ID 解耦；新格式 manifest 指定唯一恢复 epoch。

首版不需要每条操作的持久全局序号或 FTS 私有 WAL。epoch 内从头重放所有完整记录，checkpoint 必须覆盖该 epoch 的全部已应用操作；不实现独立的 FTS 前缀跳过。

新 epoch 的 WAL record 使用带版本的操作 envelope：`epoch / epoch 内操作序号 / 已解析的操作类型 / schema identity / 原目标 global DocID（可空）/ 新 global DocID 与 parent row（追加时）/ 完整 Doc payload`。操作序号仅用于校验重放顺序，不是全局 LSN。具体宽度、必需字段和 CRC 覆盖在 W0 冻结；保留旧 WAL decoder，不能把旧记录按新布局读取。

在容量处理完成并持写排他屏障后，确定新行身份和 update/upsert 的旧目标，再 append WAL。upsert 在日志中固定为插入或替换，不在 replay 时重新查询可能提前落盘的 IDMap 决定删哪一行；update 的合成后文档也写入 payload。重放使用记录中的身份，验证 row 连续性、epoch/schema 匹配和操作顺序，不重新执行主键存在性推断或随意分配另一 DocID。

FTS 之外各存储仍必须能从同一 checkpoint 视图恢复，再按这些身份确定性 apply。提前落盘的 IDMap/删除/标量状态不能被当作日志起点；沿用或补齐各组件已有的恢复隔离与幂等规则。W4 必须验证同一主键在一个 epoch 中反复 upsert/update/delete、恢复中再次退出的结果，不能仅证明 FTS posting 没有重复就宣布数据库恢复正确。

`WalFile` 读取 API 必须区分 record、正常 EOF、截断尾部和损坏，不能把空 string 或反序列化失败当作可继续成功恢复。损坏完整记录必须报错；尾部处理须与持久化选项和已有 WAL 契约一致，并用崩溃测试固定。默认写入成功是否 fsync 仍由数据库已有持久化模式决定，本方案不凭空升级 ACK 保证。

### 10.3 完整数据库 checkpoint

先暂停前台写准入和新的后台调度，在不持 collection/segment 发布锁时 drain 已有 flush/merge，让它们完成发布或安全取消。然后预留尾部 flush/epoch/manifest 所需资源，取得写排他屏障；屏障内由前台同步刷出剩余 active，不再提交给需要该锁发布的 worker，也不等待其释放预算。确保没有已 append 却未成功 apply 的操作；若有不确定状态，禁止 checkpoint。

1. freeze 并刷完全部 active/frozen，得到精确覆盖 `[0,row_frontier)` 的候选 run root。
2. 同步所有被候选版本引用的 FTS 文件、forward、vector、scalar/IDMap、删除快照及目录，遵循已有 durable checkpoint 协调。
3. 预创建新的空 WAL epoch E+1，写 header、同步文件和目录；完成下一 writer 及候选 Version 的所有分配。
4. 候选 collection manifest 同时包含：新 checkpoint run root、统计、row frontier、其他存储 checkpoint，以及恢复用的 E+1。
5. 使用 `VersionManager` 的唯一持久提交入口发布。成功后仅进行不会失败的内存对象切换，以后只向 E+1 追加。
6. 旧 E 可回收；旧 checkpoint/run 文件按 owner 退休。清理失败进入重试队列，不回滚已提交版本。

delete-only checkpoint 同样轮换 epoch。即使 FTS 内容没有新增，也不能用“行数没变”判断 WAL 已可删除。

### 10.4 崩溃窗口

| 故障时刻 | 重开行为 |
| --- | --- |
| 文档准备中、WAL 前 | 私有暂存不属于任何 root，可清理；文档未生效 |
| WAL append/apply 期间 | 只依据已提交 checkpoint + 其指定 epoch 恢复；不使用进程残留 active/run |
| run 写完或 live root 发布后，checkpoint 前 | 仍选旧 checkpoint；丢弃其后 run，完整重放 epoch，避免 run 与 replay 双计数 |
| merge 输出完成但未提交 checkpoint | 旧 checkpoint 输入必须仍存在；输出不能代替恢复来源 |
| E+1 已同步，manifest 尚未提交 | 恢复旧 E；新 E+1 是未引用候选 |
| manifest 已提交，旧 E 尚未删除 | 只打开 manifest 指定的 E+1；不扫描并重放所有 WAL |
| manifest 提交结果不确定 | 保留新旧文件并停止读写；重开以唯一提交权威裁决 |

重放使用相同受预算 writer，可以多次 spill/merge；不能为了“恢复快”一次加载全部 WAL。重放期间禁用因容量触发的普通 `internal_insert -> flush()` 全局 checkpoint；FTS 可以刷临时 run，forward 等恢复工作区使用未提交的候选 block，不能绕过预算或覆盖原 checkpoint。完成整个 epoch 前不得删除恢复来源。第二次崩溃仍从同一 checkpoint 和 epoch 开始。

## 11. manifest、版本门禁与回收

新增 `FtsWritingDescriptor`，保存 backend/最低 reader 能力、parent/block identity、row frontier、schema identity、统计贡献口径及有限 run 列表。每个 run descriptor 包含 UUID/generation、父行域、文件大小、受保护 metadata 身份；文件名从受控身份生成，不接受任意路径。

manifest 中 FTS 状态是明确互斥的三类：`legacy_rocksdb`、`writing_run_set`、`sealed_custom_base`。不能在 `fts_base_` 内偷偷塞入 run list，也不能缺 tag 时猜测新目录布局。writing descriptor 允许空 root，但必须绑定 schema/epoch/row frontier。

run 列表严格有界，验证排序、无重叠、无间隙、统计和行域覆盖；长度受限后再分配。全无 token 的行仍由局部 doclen/presence 或显式空行域记录覆盖，不能被当作未写数据。

外层 manifest 必须使用旧二进制会拒绝的协议版本/必需能力门禁。只新增一个会被旧 `DecodeBlockMeta` 跳过的 tag 不够。W0 冻结新 envelope/version 和 golden；旧版程序不得把 run-set block 当作 `fts.<block>.rocksdb` 打开或修改。

当前基线的首次 `manifest.current` bootstrap 存在需要专门核验的旧程序窗口：authority 先指向 legacy manifest，后续 gated candidate 写入失败时，旧程序仍可能按编号打开并改写旧 manifest。默认启用前必须完成“失败后由真实旧二进制打开/写入/关闭，再由新二进制恢复”的回归，并修复门禁转换协议。单测只用新 reader 重开不能证明旧二进制安全。

W0 的协议选择是先安装**带门禁的已提交 legacy 基线**，再建立 authority，最后才允许新数据候选：

1. 持数据库排他锁，基于当前完整 legacy checkpoint 准备新版 envelope。它只引用原来已提交的数据，带专用 `bootstrap_baseline` 角色；不得混入候选 run/epoch。
2. 原子替换并同步当前已提交的编号 manifest 为该 gated baseline。旧程序按当前编号打开时必须拒绝；在这一步完成前不创建 `manifest.current` 或更高编号候选。
3. 创建并同步引用该 baseline 新内容摘要的 `manifest.current`，之后使用正常候选提交协议。
4. 新程序在缺少 authority 的升级窗口，只允许恢复这种明确标识、验证完整的 baseline；普通 gated candidate 永远不能走这条例外。不以最高编号猜测新格式 candidate 已提交。

这会使门禁升级本身成为一个不可自动退回旧二进制的节点，即使 FTS 数据暂时仍是 legacy。W0 必须验证真实旧 reader 的“遇最高编号门禁失败”行为及所有 rename/fsync 失败点；若旧 reader 会回退到更早 manifest，必须在启用前进一步完成阻止旧版本写入的迁移协议，不能依赖新增 tag 自觉生效。

扩展 `fts_recovery.cc`、文件匹配、snapshot 与备份遍历：keep 集合来自唯一 committed Version 指定的全部 run 和 WAL epoch，不能只保留一个 base 文件。恢复清理只能操作严格命名空间内、未引用且无 owner 的文件；不得按最大 generation、最高 manifest 编号或目录 mtime 猜提交结果。

## 12. seal、跨段合并与 DDL

### 12.1 seal

停止新写，完成必要 checkpoint，捕获精确 root。使用 `FtsFieldSource` adapter 从 active 已刷成的 runs 提供 doclen、presence、有序词项和 posting；流式生成一个当前 sealed v1 容器，输出父 segment-local DocID。

新策略 `kSealRunSet` 保留父段 N/tokens/DF 与全部行号；不趁 seal 清除删除，不把 N 改为 row_count。合并后重新计算 sealed 统计绑定及 bound，正常 reader 仍遵循当前有限校验和保守运行时上界策略。

发布遵循已有 `prepare -> durable commit -> publish`，完整准备后继 writing 段。失败保留旧 run root 和 WAL；不能先毁掉 writer 再尝试打开 sealed 文件。成功后 runs 按 owner 回收。

跨父 segment compaction 继续使用既有 `kCompact` 语义；可通过统一 source 接入，不改变 reducer 原有删除过滤和目标统计口径。

### 12.2 字段创建、删除、重建

创建/重建从 forward 流扫描，调用同一个受预算 builder；不临时创建 FTS RocksDB。历史缺字段/presence 按完整行域记录，新字段使用新的 incarnation。对已有 sealed 字段纯格式迁移不得重分词；真正重建字段则采用指定 analyzer，明确这是 DDL 语义。

DDL 首版沿用 schema 排他路径，不承诺在线重建。候选数据和 schema/根 descriptor 一起提交；失败旧 schema/reader 不变。删除字段后旧查询继续由 owner 保活，回收纳入预算。

多字段可共用 run 容器，但编码器逐字段使用工作区；禁止每字段独占一个 64 MiB active。schema 最大字段数和 metadata 大小沿用当前限制并计入预算。

### 12.3 snapshot/备份

writing snapshot 先按上述无死锁顺序暂停准入并获得完整 durable checkpoint。在写排他屏障内捕获 Version，为其他 mutable 存储生成它们自己的固定快照，并在备份目标生成与该 checkpoint 匹配的独立空 WAL epoch。之后可释放屏障，复制由 owner 保活的不可变 run/forward 文件；源库随后追加的 WAL 不属于该备份。manifest 中的路径/epoch 必须指向备份自己的完整来源，最后才发布备份有效标记。

不能直接在释放屏障后复制源库当前 WAL：文件 owner 只保证存活，不冻结内容。也不能复制 live root 与另一时刻的 manifest。禁止调用旧 RocksDB checkpoint 分支仅针对 FTS；IDMap/标量等其他组件仍按自身固定快照契约处理。

snapshot 自己的文件 owner 和预算独立，不因源 collection 删除字段或 merge 而提前释放；恢复测试必须在另一目录、两种读取模式下完成。

## 13. 旧库升级与回退

新建库可显式选择新 backend；旧库按 manifest 打开 legacy，不因新代码存在就静默迁移。首次转换需排他准备、已提交来源 checkpoint 和明确的新格式门禁。

迁移旧 writing FTS：先按旧恢复规则完成 WAL replay 与统计恢复，再从 `FtsMutableSnapshot`/source 流生成 run-set（初始可为一个覆盖完整父域的 run），创建新 epoch，连同 manifest 一次提交。提交前继续保留 legacy FTS；提交后旧 RocksDB 只作为待回收来源，后续 FTS 写入必须进入新后端。

旧格式 adapter 仍可能在 RocksDB 内部完整物化大 value，这不是新 writer 的内存保证。迁移须遵循现有限额；超额或来源不一致时明确失败并保留旧库，不声称旧库转换已经解决所有内存峰值。

已有 sealed legacy/custom 段可继续通过 reader adapter 混用。只有进入新 writing backend 的路径保证不建立/写 FTS RocksDB；不要求移除读取历史格式所需的 RocksDB 库依赖。

启用回退：门禁基线尚未替换且新格式未提交前可以撤销候选，继续旧 backend。门禁已持久升级后只能由支持该协议的版本打开，哪怕仍保留 legacy FTS 数据。关闭功能开关不能让旧程序安全读取新库，不自动反向转换。

## 14. 模块接口与代码落点

保留深模块边界，避免把队列、hash、每个 codec 都暴露给 `SegmentImpl`。

| 模块/接口（建议名） | 对外职责 |
| --- | --- |
| `FtsWritingIndex` | `prepare_document / apply_prepared / acquire_reader / prepare_checkpoint / prepare_seal / close`；管理预算、active/frozen/run root、后台任务 |
| `PreparedFtsDocument` | 文档候选及资源所有权；析构丢弃未发布数据；commit 不重新分词或申请大内存 |
| `FtsWritingReadView` | 固定 root/统计，持有读屏障与 reader lifetime；不可只返回裸 arena 指针 |
| `FtsRunSource` | 局部/父行号转换，提供当前 `FtsFieldSource` 与低层 posting 数据，不暴露局部 scorer |
| `WritingRunSetReader` | 为查询层呈现一个父逻辑段，聚合 DF 和流式连接 cursor |
| `FtsResourceBudget / FtsWorkspace` | collection 总额度、子 lease、临时目录与文件/磁盘配额；跨 writer/reader/merge 共享 |

`FtsIndexer` 变成 backend 外观，隔离 legacy context、新 writing 和 sealed custom 的生命周期。`FtsColumnIndexer` 保留查询构建，写入分析/存储交给 writing backend；不能让公共新接口依赖 RocksdbContext。

建议新增文件 `fts_writing_index.*`、`fts_run.*`、`fts_writing_reader.*`、`fts_resource_budget.*`，按职责合并必要内部实现；修改 `fts_field_writer`/`fts_merge` 的预算和行域策略。`create_field_indexer`、`remove_field_indexer`、`recover_statistics`、`create_snapshot` 等旧方法必须按 backend 明确分派。

数据库集成新增显式 WAL epoch、manifest writing descriptor、读屏障、准备/apply 分离和恢复状态。先固定接口与故障状态再平行实现；不能仅新增 codec 后把持久化协议留给调用者猜测。

## 15. FM-index 扩展边界

沿用字段 ID/incarnation、base/run UUID/generation、词典排序与编码版本、canonical ordinal 和 fingerprint。future 辅助索引只能绑定某个不可变词典身份；merge/seal 后重新生成词典身份，不能继续引用旧 ordinal。

可选 auxiliary descriptor 以长度可跳过记录挂接到相应不可变 generation，由同一 owner/版本机制管理；不得通过修改已发布 base/run 的 required_features 表示“FM 就绪”。active 临时 term ID 不作为跨代持久身份。

本期不创建 FM 文件/空 section，不引入 FM 配置、不实现查询扩展，也不给每词/每文档留大块 padding。以后先支持 sealed 还是 run sidecar 属于独立方案，本文件不展开。

## 16. 实施切片与依赖

| 阶段 | 交付内容 | 放行条件 |
| --- | --- | --- |
| W0：冻结契约 | run 行域 feature、manifest/epoch 协议、预算/状态机、锁序和接口；补旧门禁 bootstrap 回归 | 编码 golden、失败窗口和旧二进制行为有明确预期；生产开关保持关闭 |
| W1：单文档与 active | 有界 analyzer、PreparedDocument、分页倒排、共享预算、同步查询读屏障 | 旧/新分词与查询差分；单文档超预算在 WAL 前无副作用 |
| W2：run 与合读 | 双模式 flush、局部行域 adapter、聚合 reader、固定 fanout、文件 owner | 任意 flush 边界不改结果/分数；高 TF/缺字段/重复短语均有界 |
| W3：合并与 seal | 相邻合并、背压、seal source、任务取消、磁盘配额 | 低预算持续写入及查询；merge 前后位级分数/排序一致；无全量列表 |
| W4：持久化集成 | WAL epoch、全库 checkpoint、poison/reopen、恢复清理、跨进程崩溃测试 | 第 10 节矩阵、delete-only、恢复再崩溃全部通过 |
| W5：完整生命周期 | 旧 writing 迁移、create/drop/rebuild、snapshot、混合 compaction、关闭 | 不留隐藏 RocksDB FTS builder；旧库失败保留来源，成功后只写新后端 |
| W6：验收启用 | 全 analyzer、平台、性能/资源曲线、格式兼容及运维说明 | 功能与资源门槛全通过后，独立评审是否默认启用 |

W1–W3 可先用内部测试入口，不对外宣称 durable 可用；W4 不应等到最后才设计。worker 可在 W0 后分别实现文档/active、run/reader、epoch/发布，合入顺序保持可回归。不得因 benchmark 好看跳过恢复及 analyzer 门槛。

## 17. 验证与验收

### 17.1 功能与评分

- 对同一文档/操作序列比较当前 HEAD RocksDB writing、新 writing、seal 后 custom：term、AND/OR/NOT、候选集、过滤、短语、重复词、topk、缺字段、零 token、删除与 update/upsert。
- 固定预算随机安排每文档后 flush、不同相邻 merge 树及 checkpoint；同一 writing 父段布局变化必须保持 DF/N/TF/doclen、FP32 分数和排序。跨段 compaction 按其既有语义单独比较。当前 HEAD 已记录 sealed 批量 OR 与标量路径的浮点累加差异；跨 writing/sealed 对比需分别对照当前各路径基线，不把该已有差异误判为本方案新增回归，也不能放宽内部 run 布局不变性测试。
- 覆盖 DF=1/2/127/128/129、TF=128/129/百万级、超长词、NUL/高位字节、位置重复及 UINT32 边界；不以“小词典英文句子”代替极值。
- 两种读取模式同参数全跑；BufferPool 低额度、跨页/short read、查询资源不足必须传递失败，不返回部分 Top-K。

### 17.2 资源与并发

用固定 B、逐步放大数据集 10×/100×，记录受管 allocation 峰值及各分项；其峰值不得随文档/词典总量线性增长。RSS、BufferPool cache、mmap 驻留另列，解释差异。

覆盖全唯一词、单高频词、单大文档、多个大字段、全空字段、长查询 pin、慢磁盘、停止 merge、反复 DDL/snapshot、close/cancel。检查 run hard cap、队列长度、文件句柄、磁盘配额和等待时长；验证后端不会靠增加 frozen、保留无界 reader 或扩大 std::vector 隐藏欠账。

对读屏障/后台发布/关闭使用线程压力与可用的 TSan；检查锁递归、写饥饿、持锁等待预算、任务取消死锁。读写混合负载的尾延迟必须单列，不能用纯只读 sealed 性能代替。

### 17.3 恢复与兼容

在第 10 节每个文件写/同步/rename/manifest commit/epoch 切换/清理边界注入失败并真实退出进程，再从独立进程打开。额外覆盖：

- 满 forward block 的下一文档、delete-only epoch、update 中途失败、批次某文档 WAL 后失败。
- checkpoint 前后的 run merge、候选已打开未发布、旧 reader/snapshot 仍保活、磁盘满/权限错误。
- 重放中多次 FTS flush 和再次崩溃；截断尾部与中间损坏记录分开；read-only 打开需要 replay 的库必须按契约拒绝。
- 新旧实际二进制的格式门禁；缺失/重复/交叠 run、错误 row frontier、不同 analyzer/incarnation 挂错、未知 required feature。
- 新格式成功发布后旧文件删除失败，不得回滚或遗失可恢复来源。

### 17.4 性能对比

至少比较：A 当前 RocksDB writing + 当前 seal；B 新 writing + 新 seal。相同数据顺序、analyzer、字段、删除率、数据库 WAL 持久化模式、读取模式、总可用内存和查询脚本。

报告写入吞吐与 P50/P95/P99、混合查询延迟、写停顿时间、建库峰值受管内存/RSS、最终索引空间、临时/峰值磁盘、WAL/flush/merge/seal 分项读写字节、CPU、恢复时间。分别测试热/冷读取及不同 buffer 大小/run cap。

现有 bench 可能只比较封存后索引，需增加持续写入、同进程检索、强制 checkpoint 和故障恢复场景。禁止把总磁盘空间减少直接解释为 writing 吞吐提升，或只测关闭 WAL 的模式。

新模块补相应单测，同时回归 `fts_column_indexer_test`、`fts_reader_test`、`fts_field_test`、`fts_generation_test`、`fts_manifest_test`、`version_publication_test`、`fts_segment_publication_test`、`fts_recovery_cleanup_test` 及 FTS SQL/多段用例。按仓库 CMake 规则构建 `unittest.<target>`；新增 target 以实际 CMake 定义为准。

## 18. 其他机器接续说明

1. 从上述分支/完整提交核对基线；先检查本地工作区，不覆盖未提交内容。若 HEAD 已前进，重新核对本文件第 2 节入口及格式/恢复改动。
2. 阅读顺序：本文 → 当前 v1 实施规范 → 当前进度的最新条目 → 官方研究附录 → 具体源码和测试。历史设计只作为背景，不覆盖当前校验策略及本文件新写路径决策。
3. 首个开发任务是 W0：新增协议/状态与测试计划，明确 epoch、预算、锁序；不是直接删除 RocksdbContext 或开启生产门禁。
4. 所有新增字段、feature 值和文件命名先做 golden/兼容测试；按职责逐步提交可独立回归的改动，禁止顺手重写 scoring/AST。
5. 每个阶段记录实现范围、执行过的测试、平台、参数和未过门槛。缺少平台/真实旧二进制/大数据结果时明确未验收，不把本设计当作测试证据。

本次交付仅为设计文档与官方研究附录；未实现新 writer、未开启功能门禁，也未通过执行测试证明新方案。
