# 数据模型重设计的依据与裁决（2026-11-06）

本轮根据已合并主仓 `0f63a222`、SDK 固定提交 `f892ed1` 和 HY 实际资料重新设计。
正式产物是[统一文档模型](../architecture/UNIFIED_DOCUMENT_MODEL.md)、
[文件到模型接口](../architecture/FILE_MODEL_IO_CONTRACTS.md)、
[迁移与验收](../architecture/MODEL_MIGRATION_AND_VALIDATION.md)。
它们是待实施的目标设计，本轮没有修改生产代码、SDK、数据库格式或原始资料。

## 1. Swarm 分工与取证范围

三路只读分析分别覆盖来源/导入/文档会话、地质语义/多体系/依赖、大地震/索引/内存。
主代理核对当前实现、重新统计资料清单，统一存储与一致性裁决并编写正式文档。
私有报告、清单、头部解析结果和实验材料留在 git 之外。

| 证据 | 本轮动作 | 能证明/不能证明 |
|---|---|---|
| 文件清单 | 递归 stat 2,051 个文件，0.124 秒一次观测；清单本身不读 payload | 当前文件数/尺寸/修改时间；不是通用扫描性能基准或内容摘要 |
| 两份时深资料 | 只读流读约 8.74 MB，统计井名标记及记录；另查少量表头/首行 | 多来源及原始声明；不是全工区值级 QA、真实井筒身份裁定 |
| 两个 SF3P | 每文件只读一次 4,096B 头，共 8,192B；不读目录/振幅 | 实际轴、布局、完成声明与理论目录尺寸；没有实际分配该目录或测工作集 |
| 历史 XML 结构剖析 | 引用 2026-11-05 已有只读报告 | 56 文件、304 工作表、约 140 万物理行；本轮未重扫全部 XML |
| 源码 | 按主仓/SDK 提交核对读取、发布、状态和缓存路径 | 结构性成本/缺口；不是新接口已经运行的证明 |

## 2. 当前资料规模

| 资料族 | 文件数 | 字节数 | 设计含义 |
|---|---:|---:|---|
| 井坐标 | 2 | 14,195 | 原始声明/身份候选与规范实体分开 |
| 单井 XML | 56 | 1,157,110,578 | 流式语法还须流式输出，不能返回整文件树 |
| 地震层位 | 14 | 823,071,360 | 按面/测线/支持域索引，不反复全表排序 |
| 地震及分页派生 | 3 | 352,262,046,136 | 占总字节 92.3629%，原件外链与目录/载荷分页 |
| 岩心及混合附件 | 1,961 | 27,128,349,497 | 大量图片/Office/专有格式，登记与解析能力分开 |
| 时深 | 2 | 8,736,607 | 一文件多井、多来源同名候选、多用途关系 |
| 相图图片 | 13 | 9,856,642 | 图片与数值空间模型分开，配准为独立解释 |
| **合计** | **2,051** | **381,389,185,015** | **355.196 GiB** |

另有 9 个空文件；旧剖析记录 156 组重复 basename，以及 15 个容器中的 144 条外部关系。
这要求保存空/损坏/未支持状态、包成员及外部关联，不能以文件名作主键或自动递归取全内容。

两份时深资料分别有 12/107 个不同井名标记、11,287/103,912 条数据记录，有 9 组重名。
这只是身份候选证据。已合并 HY 回归在 9 个目录井保留多来源，但不能把旧按名归井提升为
已经确认的真实井筒同一性。时间列名可说明 TWT，仍不足以确认单位、零点和符号。

## 3. 真实 SF3P 目录推导

| 数据 | 实际 IL×XL×sample count | 页布局 | 页数 | 普通 Open 分配的页目录 |
|---|---|---|---:|---:|
| L0 | 3,901×3,241×4,096；平面步长 1 | 8×8×16 | 50,720,768 | 1,623,064,576B，约 1.5116 GiB |
| L1 partial | 976×811×4,096；平面步长 4 | 8×8×16 | 3,185,664 | 101,941,248B，约 97.22 MiB |

现有 chunk 为 8×8×256，每 chunk 16 页。L0 公式为
`ceil(3901/8) × ceil(3241/8) × ceil(4096/256) × 16 × 32B`；另有 bitmap 396,256B。
L1 虽覆盖类似端点，轴 count/step 不同，不能从端点推测稠密轴。

以上是文件头与源码的分配量推导，不是实际进程 RSS 或吞吐测量。
普通打开全部目录的成本已超过一个常见缓存总额度，因此优先改目录访问形状，
单纯降低 LRU 项数、先压缩振幅或增加线程不能解决它。

## 4. 具体缺口与优先级

| 优先级 | 当前实现证据 | 设计决策 |
|---|---|---|
| P1 | `vendor/sbm/src/Engine/PagedWorkspace.cpp` 普通 Open 分配全 bitmap/pageDir | 目录/bitmap 按需分页，也纳入共享内存额度；不重写原 SF3P |
| P1 | 同处 move header 后继续 hash 被移动局部 vector，不能保证包含原 header | 使用实际持有 header/manifest revision，验证同路径同属性不同代次 |
| P1 | `src/workflow/projectsetup_area.cpp` 先 clone、rename、删旧 backup，再投影 Catalog；回调未校验 SDK 基底代次 | 先补 model CAS 和持久恢复；常规导入改 staging+receipt+水位，旧库在恢复/lease 完成前不删 |
| P1 | SDK `query_service.cpp` 的 readCurveSlice/readTable(curves) 先解整组 BLOB；TD 分页也先整读 | describe/window/page 分离，真正数值分块；输出 limit 不当内存保证 |
| P1 | `documentsourceprojection.cpp` 与 `DocumentSession::project/invalidate` 全量重建并清所有观测 | 规范化主选、SQL 索引、delta 订阅和按对象/位置失效 |
| P1 | SDK ensureWell 按 name UNIQUE；同名来源在建立 claim 前合并 | 来源声明/身份裁定分离，保留 legacy alias，不按后缀猜井筒 |
| P1 | SDK horizon/curve 等未覆盖统一修订族；结果目前不能完整追踪同 ID 内容/语义更新 | 所有写路接 facet/outbox，反向依赖包含查询覆盖和负依赖 |
| P2 | SDK horizon 记录页使用 OFFSET，每页还做精确 count；已有 horizon_order(horizon_id,seq) 及隐含 rowid | keyset/一致游标 + 按需 count；先验证既有索引，不无条件新增重复索引 |
| P2 | `areasourceprojection.cpp` 全 source+十类 listData，全版本匹配 | transaction receipt/change pages，首次 metadata bootstrap 后增量 |
| P2 | ParsedFile 仍是整文件对象树，待发布额度为估算 | ChunkSink、分配前 reservation、done 队列背压、受控 spill |
| P2 | 来源 SHA 相同可更新旧目录版本的 areaModelSourceId | 文件版本和 parser/profile import 分离，旧来源映射不可就地重定向 |
| P2 | 固定四子层、井上单元唯一槽位、单数组时深 | 通用 Unit/Occurrence 与分段关系；四分层保留为策略适配器 |

SDK 依据固定在
[query_service.cpp](https://github.com/changyanyanchang/Data_service/blob/f892ed1fc49a843d5f44dd17456d7eaf38135b1f/src/flow/query_service.cpp)、
[project_store.cpp](https://github.com/changyanyanchang/Data_service/blob/f892ed1fc49a843d5f44dd17456d7eaf38135b1f/src/store/project_store.cpp)、
[object_revision_schema.cpp](https://github.com/changyanyanchang/Data_service/blob/f892ed1fc49a843d5f44dd17456d7eaf38135b1f/src/store/object_revision_schema.cpp)。
宿主依据为 [DocumentSession](../../src/workflow/documentsession.cpp)、
[来源投影](../../src/catalog/documentsourceprojection.cpp)、
[工区模型发布](../../src/workflow/projectsetup_area.cpp)、
[SF3P Reader](../../vendor/sbm/src/Engine/PagedWorkspace.cpp)。

## 5. 索引/游标设计可行性实验

独立 Python 3.13.9 / SQLite 3.51.0 进程创建临时合成库，1,000,000 条层位元数据，
两面各 500,000 条，seq=ordinal/2 取整以覆盖重复排序键；索引为 `(horizon_id,seq,id)`。
每页 256 行，对比 OFFSET 与完整 `(seq,id)` 游标。没有打开真实工程数据库或 Qt 连接。

| 查询 | 暖查询 3 次中位数 | VM 步估算（100 指令粒度） |
|---|---:|---:|
| 首页 OFFSET 0 | 0.6780 ms | 约 3,800 |
| 首页 keyset | 0.7209 ms | 约 4,700 |
| 深页 OFFSET 400,000 | 32.1237 ms | 约 1,603,900 |
| 同一深页 keyset | 1.0706 ms | 约 4,700 |

两种深页逐字段/顺序一致；切在重复 seq 之间、另一 horizon 隔离也一致。
EXPLAIN 显示 keyset 使用 horizon+seq 范围索引且无临时排序，OFFSET 仍需跳过前面记录。
VM 统计与时间测试分开，避免 progress handler 干扰计时。临时库约 45.5 MB、该进程峰值
工作集约 50.2 MB，实验后清理临时库。可重跑脚本和完整 JSON 仅存私有分析目录。

这证明拟议复合键分页的正确性与工作量优势；不代表 Qt 驱动、生产磁盘或冷盘性能，
也不能从 3 次查询推导 p95。实现后还须在实际 C++ 查询路径复测。
该实验采用显式三列索引，仅验证游标访问形状；当前 SDK 两列索引带隐含 rowid，
已经可满足同一排序，不能将本实验解读成生产库缺少排序索引或必须新增索引。

## 6. 本轮范围

形成三个互相引用的设计文档，并接入架构入口、数据迁移文档、文档索引与 TODOS。
源码与数据格式没有变化；预算/时延是待验证目标。既有 32 项回归属于已合并批次，
不能用来声称新架构已通过性能验收。

原始数据保护采用本轮前后 metadata inventory 和只读访问方式核对，不对 381 GB 原件
重算完整哈希。私有文件名、路径清单、原始头文本、临时数据库与个人报告不提交仓库。

## 7. 设计复核与本轮验证

交叉复核后补充：完整 Context 身份隔离主选，未裁定来源声明仍可登记，井上出现的证据
可选多对多；固定值、跟随对象头、跟随用途选择分别维护依赖。M1 日志必须保留可重放的
元数据 image/tombstone，不能等 M4 数值历史，也不能把后一次更新冒充前一批投影。
累计读写预算与瞬时驻留额度分开，共享块只计一次，并受统一总额度约束。

本轮已执行的独立设计检查：

- 2,051 个原始文件的相对路径、尺寸和纳秒修改时间与分析前清单完全一致；没有做全量内容哈希。
- 33 处本地文档/代码链接可解析，代码围栏闭合；两个真实 SF3P 页数/目录/bitmap 公式一致。
- 在独立内存 SQLite 中运行文档里的绑定索引：A@r1 与 B@r1 可分别确认主选，
  A@r1 内重复主选被拒绝，候选记录不误占已确认唯一槽位。
- 百万行分页实验见上节；未运行应用 C++ 构建或新架构性能测试，因为本批只改设计文档。

合入前 Source gates 结果随 PR 记录；检查脚本和完整输出保留在私有分析目录。
