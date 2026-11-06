# 文件到模型的高性能接口契约

状态：2026-11-06 目标接口；尚未实现。基础模型见
[统一文档模型](UNIFIED_DOCUMENT_MODEL.md)，实施顺序见
[迁移与验收](MODEL_MIGRATION_AND_VALIDATION.md)。本页中的预算数字是待验收初值，
不是当前机器或全部现有接口的实测性能。

## 1. 统一请求与结果

数据接口由后台工作流调用，视图只提交查询或命令。标准 C++ DTO 表达契约，宿主层适配
Qt 的 JobRunner、信号和取消。无需为这组接口新增 HTTP 服务或第三套线程池。

```cpp
// 协议示意，非本批新增公共头文件；ID/Ref 为强类型值包装。
struct RequestWorkBudget {
    uint64_t maxSourceBytes, maxDecodedBytes, maxOutputBytes;
    uint64_t maxRows, maxCells, maxChunks, maxDirectoryBytes;
    uint64_t maxSpillWriteBytes;
    Deadline deadline;
};
struct ResidentLimits {
    uint64_t maxLiveBytes, maxPinnedBytes, maxQueuedBytes, maxOwnedSpillBytes;
};
struct ReadMetrics {
    uint64_t sourceBytes, directoryBytes, decodedBytes, outputBytes;
    uint64_t chunksRead, sqlRows, spillBytes, peakReservedBytes;
    Duration queueTime, ioTime, decodeTime, publishTime;
};
// Outcome<T> 同时携带 status、实际 ReadCut、coverage、issues、receipt、metrics。
```

RequestWorkBudget 限制本请求累计工作量；ResidentLimits 限制同时驻留/占用的资源，
释放缓冲不会减少已经发生的读/解码工作计数。sourceBytes 是总源读量，directoryBytes 是
其中的目录分类子集，不再相加或重复扣总额度；同一缓冲按唯一 allocation 计 live bytes，
持有者用 claim/pin 计生命周期。预算相加检查整数溢出。读前估计用于排队，实际
分配前仍须取得内存配额，不能把 `fileSize * 4` 当硬上限。磁盘 spill 有独立上限和所有权。

结果至少区分：Ok、Empty、NotFound、UnresolvedIdentity、AmbiguousSelection、
MissingSemantics、SourceChanged、StaleRevision、MissingPinnedInput、CursorExpired、
CatchingUp、IndexRequired、UnsupportedCapability、BudgetExceeded、Cancelled、Corrupt。
Empty 只表示在有效、完整查询中确实无记录。默认失败清空数值输出；容许部分结果的预览
必须显式请求，并携带 mask、未完成原因和覆盖范围，不能被计算/发布入口默认复用。

## 2. 文件访问能力

`resolveAccess(FileVersionRef, AccessPolicy, ReadCut) -> SourceAccessDescriptor`，
随后在 worker 内 `openReadLease(descriptor) -> SourceLease`。

Descriptor 可跨线程传值；Lease 及文件/SQL/解码句柄不跨线程。Descriptor 保存文件版本、
位置修订、URI/provider、预期属性与验证等级；历史 originalPath 只是来源证据。
SDK 原有 stored_path 在适配期可为带操作凭据的兼容镜像，不能与文档位置表争夺权威。

Lease 提供 `stat`、`readRanges`、顺序流、能力查询和取消；上层不知道本地路径。
能力显式区分 seekable、range-readable、sequential-only、snapshot-stable、版本化对象等。
第一批实现本地文件；将来网络 range/provider 或包成员由适配器提供，不改变领域查询形状。

同一请求合并相邻 ranges，并记录读放大。验证文件属性在读前/读后进行；属性稳定仍不是
全内容证明。弱证据不能自动合并两个 FileVersion；需要强复现时捕获实际消费字节/数值，
或使用已确认不可变的受管版本。不对可变文件承诺“同一个句柄就一定是字节快照”。

完整核验是独立任务，可在正式顺序导入时边读边计算摘要。默认打开、翻页、选关系和
切方案不全 hash，也不为了得到指纹复制大文件。SEG-Y quick v1 兼容保留其原算法；
后续策略把 sampled-content evidence 与 location freshness 分开，不能静默改旧引用。

## 3. 文件发现到发布的流水线

```mermaid
flowchart LR
    D[发现清单] --> P[有界识别]
    P --> I[导入计划和身份候选]
    I --> R[流式准备 / 分块落盘]
    R --> V[准备结果校验]
    V --> M[领域事务提交和 receipt]
    M --> O[增量目录投影和 watermark]
    O --> E[文档 ChangeSet / 视图投影]
```

| 拟议接口 | 输入/返回 | 关键约束 |
|---|---|---|
| `discover` | root、scan cursor、过滤/排除规则 → FileCard page | 默认只枚举/stat，排除工程输出、暂存与循环链接 |
| `probe` | SourceLease、适配器能力、ProbeBudget → 候选/声明/已读范围 | 不创建井、不发布、不把候选当完整校验 |
| `planImport` | FileVersion、ParserProfile、裁定、目标 store → ImportPlan | 固定适配器/模板/列映射及 import key |
| `prepareImport` | Plan、ChunkSink、预算/取消 → PreparedManifestRef | parser 无 SQL/UI；manifest 不携带全数组 |
| `validatePrepared` | manifest、预期 heads、输入证据 → ValidatedImport | schema、引用、块完整性、范围、大小、当前输入状态 |
| `commitImport` | opId、validated ref、expectedModelHead → CommitReceipt | 单 SDK writer；CAS、幂等、可见性切换和日志同事务 |
| `projectChanges` | store/generation、afterSeq、预算 → ProjectionReceipt | 消费变更，不再读原件；更新控制数据和水位同事务 |
| `resumeOperation` | opId、输入摘要 → 已有或恢复后的 receipt | SDK 已提交后不重解析；输入不符则冲突 |

discovery cursor 描述持久扫描前沿和 scan generation。变化中的目录不伪装成不可变全树快照：
每项重新 stat、按 file/location identity 去重；结束返回 reconciliation token 和扫描期间变更。
任意目录顺序若需确定发布，使用持久 discovery ordinal 或有界外部排序，不先装满百万路径。

## 4. 解析适配器与背压

`ParserDescriptor` 注册 adapterId/version、输出 schema、magic 与后缀提示、probe 能力、
流式准备能力、最大元素尺寸和 checkpoint 支持。后缀只用于候选筛选。适配器版本独立，
无关格式的修复不应要求全部资料重导入。

ChunkSink 接收对象摘要、原始单元格块、数值块、关系声明和 Issue。每块有 source member/
行列或字节范围、file-local object key、ordinal、编码、实际分配预算和摘要。
parser 只产生来源井声明，不调用 ensureWell 按名称合并不同来源的井。

准备期的流式 XML 不仅要求语法流读，还要求输出分块。大 ParsedFile 整树返回只保留为
有明确上限的小数据兼容包装；超限则 spill、只登记或返回 BudgetExceeded，不能单任务豁免。
慢首文件阻挡有序提交时，后续已完成但未发布的块仍计入同一内存/磁盘预算。

纯 parser worker 不操作 SQL。大型准备结果由私有 spill 文件承接，单 SDK writer 可分批
入 staging 表/载荷目录；Preparing 导入不参与正常查询。最后的可见性切换、对象 heads、
提交凭据及 change_log 在短事务里一起发布，不要求最后一次事务搬移全部数值。

resume token 固定文件版本、适配器/profile、输入证据、块序号和 owned staging；不支持
安全恢复的格式明确从头重试。取消必须停止生产、解除背压、join worker，再清本操作产物。
受管块被其他版本引用时仅减少引用，不能直接删除公共文件。

Office/照片/未知格式默认 register-only。压缩包另限成员数、展开字节、嵌套深度和单元素
尺寸；包内/外部关系先登记引用，预览时才按明确请求解析，不递归自动加载所有目标。

## 5. 元数据列表、游标与一致读

接口分开：`describeObjects(keys, fields)`、`queryObjects(filter, fields, cursor)`、
`queryBindings(slot, cursor)`、`queryResults(status, cursor)`。列表不携带数值数组或大 JSON。
批量点查保留输入顺序及逐项状态，避免 N 次打开数据库与 N+1 汇总查询。

cursor 绑定 filter/排序摘要、ReadCut、lastKey 和 lease/版本能力。字段或排序变化、lease
到期、底层代次替换时返回 CursorExpired。对可变当前表仅保存一个 maxSeq 不能恢复历史行；
需要真实不可变版本或有期限的只读快照。普通 UI 翻页不长期占住 WAL 快照。

示意的规范化绑定索引：

```sql
CREATE INDEX doc_binding_slot
  ON doc_binding(document_id, owner_kind, owner_id, role, context_id, context_revision, binding_id);
CREATE UNIQUE INDEX doc_binding_one_primary
  ON doc_binding(document_id, owner_kind, owner_id, role, context_id, context_revision)
  WHERE decision = 'confirmed' AND is_primary = 1;
CREATE INDEX doc_dependency_reverse
  ON doc_dependency(store_id, object_kind, object_id, facet, partition_key, result_id);
CREATE INDEX model_change_items
  ON model_change_item(commit_seq, ordinal);
```

槽位身份使用完整 `context_id + context_revision`；两个 Context 都处于 r1 不代表同一用途。
上述身份列均非空；无专门 Context 的旧绑定迁入显式 legacy/global Context，不能用 SQL NULL
绕开唯一约束。允许多选的角色使用独立能力/成员约束，不强制每种关系都选一个 primary。

示意 keyset 页面：`WHERE horizon_id=? AND (seq,id)>(?,?) ORDER BY seq,id LIMIT ?`。
排序字段非空且完整复合键稳定。空间邻域仍使用既有 `(horizon_id,inline_no,crossline_no)`
索引；记录游标索引不能替代按测线/空间窗口的索引。
SDK 当前已有 `horizon_order(horizon_id,seq)`，`id INTEGER PRIMARY KEY` 是 rowid 别名；
普通索引末尾隐含 rowid，因此可支持此排序。先用实际 Qt/SQLite 的 EXPLAIN 与工作量计数
验证既有索引，不无条件再建 `(horizon_id,seq,id)` 重复索引；结构依据见
[SQLite 多列索引说明](https://www.sqlite.org/queryplanner.html#multi_column_indices)。
旧表允许空 seq：游标适配按显式 null 分区和 id 顺序覆盖这些记录，再进入非空分区；
游标固定阶段/ReadCut，不让行值比较静默遗漏 NULL，也不修改原始排序值掩盖问题。
OFFSET 晚页成本随 offset 增长，SQLite 的
[行值分页说明](https://www.sqlite.org/rowvalue.html#scrolling_window_queries)给出了相应机制。

精确 totalCount、全域统计、每对象相关 count 不默认随每页执行。返回维护过的摘要或
CountUnknown；请求准确统计时单独安排有预算的聚合任务。

## 6. 真正的数值窗口接口

所有数值请求固定 `ObjectValueRef / 当前修订保护`、字段、WindowSpec、显式重采样策略、
预算和取消。结果带真实坐标轴、缺测 mask、coverage、transform refs 和 ConsumptionReceipt。

| 数据 | 请求单位 | 默认访问形状 |
|---|---|---|
| 曲线 | 轴/样点范围、曲线字段、原始/处理视图 | 共享轴块 + 所需数值/质量块，不读整个 LogSet |
| 原表 | 字段、行键范围 | source_cells/row chunks，按字段裁剪 |
| 分层/时深 | occurrence/关系/分段、输入轴范围 | 元数据点查 + 涉及的段/点块；不读同井其他关系 |
| 层位 | survey/体、实际 IL/XL 查询点序、支持半径 | 有界 tile/测线索引；保留重复道、缺道与显式空值 |
| 地震 | 真实道键/空间窗、sample range、LOD、字段 | 相邻页批读、有限解码、输出真实轴和覆盖 |

CurveChunk 目标包含共享 axisRef、firstSample/count、轴覆盖/单调 run、编码/数值类型、
raw/processed/quality/mask 的块引用。原始 float64 不为节省空间静默降成 float32。
处理值可用局部补丁/新块，不因改一个采样点重写全井 raw 数组。

单调轴在每个 run 内查找覆盖起点，再顺序读命中块，目标为 `O(log C + 命中块)`。
非单调轴、重复深度与间断保留原记录，另建 run/范围索引。仅有 min/max 的普通 B-tree
不保证任意重叠查询都是对数成本；无可用索引且请求会超预算时返回 IndexRequired。

初始目标块为 4,096–16,384 样点，依据列宽选择并限制解码后块大小。小块可留 SQLite；
大块使用版本化 sidecar。二者共用 manifest，不先引入新的通用列式运行库。
SQLite 原生 incremental BLOB 仅作为后续备选：须确认由同一驱动/同一 SQLite 实例支持，
不可把 Qt nativeHandle 交给另一套 SQLite 库。其句柄失效限制见
[SQLite incremental BLOB 文档](https://www.sqlite.org/c3ref/blob_open.html)。

原生计算默认读真实样点，不自动减少关系数、改变邻域或降采样。预览可明确请求 envelope/
LOD，返回实际采用的分辨率。支持插值时把相邻支撑点及外扩空间支持域计入预算和依赖；
不跨缺测、断层或显式 gap 做无声明补值。半开/闭区间规则属于请求/策略，不由每个视图猜。

旧曲线 BLOB/TD JSON 的兼容读取先检查可证明的尺寸，再在后台物化分块；若无法在预算内
读取则要求显式迁移。不能把输出页上限当作全 BLOB 解包已受限的证据。

## 7. 地震页清单与未完成数据

PayloadManifest 固定父来源证据、数据/布局修订、采样/几何约定、编码、构建参数、
算法版本、generation、LOD、块目录位置、校验算法以及 coverage/checkpoint。
路径、mtime、文件大小只参与位置/新鲜度证据，不能独自承担缓存代次身份。

SF3P v7 现有页目录可以按固定目录项偏移分段读取，优先实现按需目录块缓存，
不要求第一步重写所有已有 SF3P。目录、bitmap 和索引本身也必须受预算，不能只限制振幅页。
metadataOnly 打开不得接着偷偷分配全部 pageCount×32 目录。所有 offset/length、尺寸乘法
和解压输出都在分配前校验，截断/越界/未知 codec 返回明确错误。

覆盖按构建代次、LOD、空间/采样范围记录。区分 HeaderParsed、DirectoryReadable、
RequestedRangeAvailable、RequestedPagesVerified、BuildComplete、FullPayloadVerified。
complete 头标记不授权未核验的页；页面存在也不证明父来源匹配。
校验字段记录真实算法：当前代码的页校验是 FNV-1a64 式实现，不能称为密码学摘要或 CRC32。
兼容标识建议为 `legacy-sf3p-fnv64-v1`，固定初始 seed=1099511628211、现有逐字节
xor/multiply 规则以及对 storedBytes 的校验范围；不能换成标准库默认 seed 后仍称兼容。

大 manifest 自身也按页/层级索引读取。可采用持久块树/分层目录，使单块编辑只更新受影响
路径和小根摘要；不能每改一个样点就加载或重哈希千万项清单。内容摘要在创建/修改块时
附带计算，读取和切方案不重算整个对象。最终发布的 manifest 不原位改写，构建 checkpoint
单独可变；页覆盖的快照固定 checkpointRevision，避免读取正在写的半页。

partial 默认不能进入要求全覆盖的计算；显式的局部预览可使用已完成且满足校验策略的页，
同时返回 coverage/missing mask。缺页不能补 0，坏页不能用上一代同页号缓存冒充成功。
LOD 可降级仅在请求明确许可时发生，结果记录实际 LOD，不伪装成原分辨率。

缓存至少分成原始字节/道索引、解码振幅块、层位/时深变换投影和最终视图数据。
key 固定 source/value version、payload generation、layout/encoding、精确范围/页键及算法；
解释缓存额外包含实际消费的语义、选择与变换修订。重定位若验证为同版本不必重解码；
改变时深主选只清命中的解释缓存，不清全部振幅页。

## 8. 发布、增量同步与恢复

SDK 每个可见提交产生 CommitReceipt 与完整事务级 change batch。seq 单调但不必连续；
批内 ordinal 可分页。既有审计文本 changes 表不能直接充当覆盖完整的 outbox。
触发器当前逐行计数可以继续防 stale，但公共事件须合并为实际对象/facet 变化。

每个 batch 在领域提交事务内持久保存投影所需的元数据 after-image，或可独立读取的
不可变元数据引用；删除保存 tombstone、旧归属/支持域及必要的 before 信息。
这是 M1 的基础能力，不等 M4 数值历史实现。只有对象 ID/修订计数不够：B1、B2 连续提交
后恢复 B1，不能把当前表的 B2 内容当作 B1 的投影，也不能永久停在 CatchingUp。
变更页按 `(commitSeq,ordinal)` 有界读取；大 metadata image 放版本化分页块，事务引用
已准备且持久化的块，不把曲线/时深点/振幅数组塞进日志。未完整发布的日志不对外可见。

Catalog 投影与 `(storeId,generation,appliedCommitSeq)` 在同一事务落盘，不越过未完整应用
的提交。超大 batch 分页准备投影，最终切换小型可见性指针；水位仅在整个 batch 完成后推进。
after-image 用于恢复元数据投影，不自动提供历史数值。过渡期若无法读取旧数值，跨库落后
期间必须返回 CatchingUp，而不是组装半新半旧结果；投影追上后重新取得当前一致 ReadCut。

操作状态与恢复：

| 边界 | 可见状态与恢复 |
|---|---|
| 准备前/过程中取消 | 未提交，清自身 staging，不动原件和已发布对象 |
| SDK 事务未提交 | 根据 op receipt 判断重试/放弃，不靠文件存在推断 |
| SDK 已提交、Catalog 未完成 | CommittedNeedsProjection；按 receipt/log 继续，不重读原件 |
| Catalog 已完成、日志清理失败 | 校验 op/input/result 后只做清理；原件可离线 |
| 基底模型被另一写者修改 | ModelHeadConflict；保留可复用准备块，不替换较新的整个库 |
| 提交后用户请求取消 | 报已提交/待恢复，不能承诺回滚；撤销须新补偿命令 |
| 快照辅助程序失败 | 不发布目标；保留可诊断的私有残留，不删除其他操作的文件 |

同 opId、同规范输入返回同 receipt；不同输入冲突。成功凭据的去重保留与归档期限显式定义，
不能删掉凭据后仍承诺永远 exactly-once。订阅和投影采用 at-least-once 交付、幂等应用。

现有全库快照仅保留给备份、迁移和隔离试算。替换旧模型的过渡路径必须先校验模型基底，
保存持久意图和旧代次，等文档投影/恢复凭据完成且无 reader lease 后才回收旧目录。
不在 Windows 打开句柄仍使用旧路径时假设可原位 rename/删除；不使用可写 SQLite 硬链接
假装独立快照。新模型代次通过受控引用切换，既有 snapshot worker 继续作为兼容工具。

变更日志保留按最慢持久消费者水位推进；落后超过保留范围返回 RebuildRequired。
日志引用的元数据 image 与恢复凭据一起保留至对应消费者/操作完成，不能先清载荷后留空引用。
重建走固定 ReadCut、受预算的 metadata bootstrap，并接续该切面之后的事件；不能在变化
的当前表上分页后直接宣称追平。重建不重新解析所有源文件。断开或只读消费者不能无限
钉住日志/载荷，须有明确的 lease、重建与保留政策。

## 9. 资源、并发和可测初值

下表是首轮实现候选默认值；调用者可在项目政策允许范围内调整，所有路径都必须有上限。

| 资源/操作 | 候选默认与硬边界 | 必须记录 |
|---|---|---|
| 元数据页面 | 默认 200，最多 1,000 项/2 MiB | 查询计划、SQL 行数、返回字节 |
| discovery | 枚举不读 payload；前沿可持久 spool | entries/stat 次数、payload bytes=0 |
| probe | 通用候选 256 KiB/file；SEG-Y quick 64 KiB；SF3P 头 1 MiB；批次另设总预算 | 实际 ranges 与总字节，不以耗时推断 |
| 解析 | 默认 4 workers，单输出块最多 8 MiB；共享 decoded admission 256 MiB | active + done-unpublished + sink bytes |
| 数值块 | 目标解码后 ≤1 MiB，特殊块仍须明确上限 | 压缩/解码/输出和读放大 |
| 交互数值响应 | 默认输出 ≤8 MiB；超过走分页或后台流 | sample/trace/cell 多维数量 |
| 地震共享缓存 | 初始总额度 512 MiB，目录块目标配额 32 MiB | pinned、LRU、inflight、句柄常驻内存 |
| 元数据快照 lease | 初始 30 秒、最多 4 个长 lease；内存版本引用不等于长 SQL 事务 | lease 数、WAL 积压、过期原因 |
| 写入 | 单 SDK 协调写者，批行从现有 256 起测 | 事务时长、bytes written、可见性切换时长 |
| 取消 | 正常设备下目标 ≤250 ms 到安全边界 | 请求至终态/资源释放时间 |

缓存额度涵盖 page directory、bitmap、解码块、被使用方 pin 住的块和已预留 in-flight 空间。
LRU 项数不限制常驻字节。256 MiB 解析和 512 MiB 地震是子额度，同时受工程配置的
共同总 cap/仲裁约束（可先以 1 GiB 服务受管内存作候选默认），不是各自独占后再相加放行。
进程中的导入、地震、预测、视图缓冲通过共同资源协调器约束；
还须测进程 RSS，因为 Qt/GDAL/QGIS/驱动分配不完全由这张额度表控制。

交互读取优先于后台扫描/LOD；同文件/同块 single-flight 合并，取消一个订阅者只减少其
claim，最后一个使用者退出才取消底层工作。排队任务也有数量/字节上限，不按视图数各建池。
结果移交为不可变且有所有者的缓冲，不交付指向已关闭 mmap/lease 的悬空 span。

检查取消与 deadline 的边界包括块读前后、解压、算法小批次、spill、SQL batch 和提交前。
OS/设备卡死或不可中断驱动调用不保证绝对 250 ms；需要硬隔离的旧调用使用受控辅助进程。
Qt SQL 连接始终由其所属线程使用，不能加一把 mutex 就跨线程共享。

## 10. 从现有入口迁移

| 当前入口/缺口 | 目标适配 | 停用旧路径的前置条件 |
|---|---|---|
| `parseFile -> ParsedFile` | ChunkSink + PreparedManifest | 大 XML/时深的背压、spill、取消和 raw 保留通过 |
| `readCurveSlice` / `readTable(curves)` | versioned curve chunks + window/keyset | 证明小窗只解命中块，不读整组 BLOB |
| 时深 JSON 整表/后分页 | metadata + relation segment/point blocks | exact 对象/语义与历史能力一致，未知不猜 |
| 全量 `registerAreaSources` | commit receipt/change page | durable watermark、重复/崩溃恢复、bootstrap 工具通过 |
| `DocumentSession::project/sources` | indexed cards + delta subscriptions | 主选/候选/旧 scope 兼容，非相关观测不全清 |
| 全库 clone + rename 发布 | append/staging imports + model CAS | 领域提交到文档投影全边界故障验收通过 |
| SF3P 全 pageDir 打开 | metadata-only + directory-range cache | 相同数值/缺测语义，预算覆盖目录和缓存身份 |

API 包装和新实现必须共用领域校验，不能让旧同步分支绕过预算、固定修订或未知语义。
迁移期间记录 legacy 调用计数和字节读放大，达到验收后按消费者逐批收口。

## 11. 原子能力的消费约束

[原子能力与 Agent](ATOMIC_CAPABILITIES_AND_AGENT.md)复用本页的窗口、lease、预算和 receipt。
一次业务操作可组合多个内部算子，按 batch/window 共享读取；工具之间传引用，不通过
JSON 中转大数组。算法描述必须声明 finite-halo/whole-trace/global 等实际执行形状，
把外扩支持域、最小工作集与临时缓冲纳入预算。串联 halo 按支持域合成，不能只取 max。
主结果与必需质量/方差等输出全部准备后才成组提交；只完成 compute 不代表文档发布成功。
