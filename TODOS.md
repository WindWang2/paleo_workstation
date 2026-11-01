# TODOS — 未完成事项

2026-10-27 整理：已关闭条目、交付流水和旧环境故障移至
[清理前快照](docs/ledger-archive/backlog-before-cleanup-20261027.md)。
这里保留未完成工作；旧报告中的状态须按其日期和提交读取，不能视为当前验证结果。
架构与实施规则见 [AGENTS.md](AGENTS.md)、[文档索引](docs/README.md)。

## P1 — 时深口径确认的 UI 接线与基准成对确认（2026-11-01）

服务侧 `confirmTimeDepthSemantics` 与宿主 `ProjectDataFacade` 通道已就绪
（见 [进度](docs/progress/td-petrel-confirm-20261101.md)），缺操作面：

- 井震标定工作台/数据页复核队列的「确认时深口径」入口
  （ms/s、TWT/OWT、时间基准、符号取反四要素 + DT_ALL 同族来源批量）；
- 地震体 `timeReference` 与井时深基准的成对确认向导（未对齐时缝隙如实
  报「时间基准未确认一致」，不静默串基准）。

## P1 — Data_service 类型化数据与版本事务统一（2026-10-28）

已接入目录四表、新建工程、五类井数据的导入模型快照和主要读取门面；
标准化及深度补算井表也建立独立模型版本。后续按
[接入方案](docs/DATA_SERVICE_INTEGRATION.md) 推进：

- 将交互编辑、曲线计算、通用派生注册、版本回滚和外链重定位接入同一模型
  发布协议；保留 RAW、quality、共享撤销与失败回滚，不能原位覆盖已发布模型。
- 给旧工程提供显式、事务性的模型回填；AlreadyStored 不算回填成功。
- 迁移岩性、样品、地震及附件的专用读口；共享外部路径应通过版本 ID 消歧。
- 地质编辑、约束、版本发布通过专用模型扩展接入；QGIS 继续管理空间文件。

## P2 — 完整编图套件的界面断言排查（2026-10-24）

本机 `tst_mappingworkbench` 完整运行为96通过、4失败、1跳过。失败项为
`hierarchyPanelShowsAndPreservesTheTree`（异步树尚为空）、
`evidencePanelAddsViewsAndPreservesOtherSelections`（旧横排断言与当前两列布局）、
`wellReadinessAbortedByCatalogRebindRecoversPanel`（重绑后摘要状态）、
`panelSelectionAndBusyFollowActualState`（跨层结果列表）。需按DESIGN与异步刷新契约
核实实现/断言，保留有意义的交互验收，不靠跳过用例转绿。测井协议、准备检查、结果落库及
合成来源证据链的专项运行通过；详见 `docs/progress/well-facies-data-support-20261024.md`。

## 整体审查后续：任务与操作模型（2026-10-23）

界面减负、草稿/定位/快捷键和局部刷新修复见 `docs/progress/ui-simplification-20261023.md`。
剩余工作不计为本批已完成：

- **P1 / R04：已完成（2026-10-28，`docs/progress/impact-background-and-navtree-perf-20261028.md`）。** ImpactJob/CompareJob 三段式落地（worker 独立开层、commit 核对世代/修订/输入版本）；取消/切工程/修订前进不发布过期结果；GUI 响应性有确定性同步点证据（tst_impactrecompute 10/10）。
- **P2 / R05、R09：统一旧新工作流与保存作用域。** 先列兼容工具能力清单，再迁移到同一命令/状态模型；区分保存工程、编辑、成果版本、发布和独立 Web 工程，保留各自撤销和版本契约。
- **P2 / R07：预测前置条件。** 默认入口已改测井；Mock 所需的有效测井预测、层位资料等在点击前显示缺失项和准备入口，与功能层同一规则。
- **P2 / R14：数据操作编排下沉。** “设主文件”已接既有共享撤销栈；将 SetPrimaryCmd 等数据操作从 ui/pages/dataops 迁入功能层，视图只发意图。
- **P2：Windows 万条资产基准分阶段测量。** `tst_assetpaging::scaleRenderingFilteringSortingSelection` 在旧全量基准循环触发默认 5 分钟超时，采样为 `resolvedVersionPath → canonicalFilePath → GetFileAttributesExW`。拆分夹具、旧基准、刷新/过滤/定位计时，复核路径规范化成本与测试预算；取得完整结果前不报告提速倍数，不用单纯提高超时掩盖耗时。证据见 `docs/progress/ui-state-regressions-20261023.md`。

## P3 — QGIS CRS 缓存生命周期修复后恢复缓存（2026-10-22）

- **What:** 跟进 QGIS CRS 的 PROJ 主对象跨线程生命周期：创建线程退出后，共享实例仍有效且可投影，之后恢复 `QgisRuntime` 的 CRS 缓存。当前已关闭 CRS 实例缓存，坐标转换缓存保留。
- **验收:** `tst_projectsetup::nativeProjectionAfterWorkerExit` 在启用 CRS 缓存时通过，异步打开工程后勾选相图及新建工程均正常；对比大工程打开与交互耗时。
- **证据:** `docs/progress/native-crs-lifetime.md`。不可仅检查 `isValid()` 或以单线程创建成功代替线程退出验证。

## P3 — 井点因子实际值标注（备份分支需求提取，2026-10-18）

- **What:** 井号与 `factor_value` 双行标注，预览和编图共用规则；保留比值原口径，
  四舍五入到两位小数（整数不带小数），空值显示“无数据”，禁止以 fid/行号代替实际值。
- **Context:** 旧草稿只有设计与 `applySamplePoints` 声明，没有实现和接线；完整需求及
  提取依据见 `docs/progress/wip-salvage-20261018.md`。实施须覆盖数值、空值、舍入及历史结果，
  不改原始数据。该条为待实现需求，不表示当前已支持。

## P2 — 新 worktree 首 configure 漏 include 的根因修复（from #484 P2, 2026-10-17）✅ 已收口（goal/perf-maintenance-20261027）

**What:** 新 worktree 首次 configure 会漏 glm/saribbon/sbm/segyio 的 include 路径，再跑
  同一条 configure 即恢复（症状与「再 configure 一次」操作手册见 BUILDING.md
  「构建与测试」段）。#484 指出：坑已写成手册 12 天，根因修复始终没有独立
  TODOS 条目跟踪——本条补上。根因方向：这些 vendored include 路径靠
  configure 期探测产物（superbuild/prefix 或 cache 变量）在**第二次**
  configure 才命中——候选修法是把探测移到 bootstrap/fetch-only 产出后立即
  写入 cache，或在 configure 期对「目录存在但 cache 未命中」直接
  FATAL_ERROR 早失败（与 #498 磁盘水位闸同「早失败并报明确原因」口径），
  替代「红一轮再重跑」的隐式恢复。
- **Why:** 首 configure 红一轮对新手是断头路；「再跑一次就好」的隐式约定
  无法被 CI 捕获（CI 每次都是全新 worktree 形态）。
- **Context:** BUILDING.md「独立 worktree 开发」段；#484 P2-1。
- **Effort:** human: M / CC: M
- **Priority:** P2
- **收口（2026-10-27，Linux 验收）**：原「漏 4 条 vendor include、二跑恢复」
  症状在当前 master 三条路线（-D superbuild / env / 系统兜底）全新构建目录
  均不复现（compile_commands 四条 include 全量在位，CI 全新 checkout 亦绿；
  历史症状出自 2026-10-03 Windows 本机记录，本任务无法复验 Windows）。
  实测确认的同族真实缺陷：显式 QGIS_PREFIX 悬空时 find_* **静默回落系统栈**
  ——链接系统库同时把 PALEO_QGIS_PREFIX_DEFAULT 编译成悬空路径（链接/
  运行资源栈分裂）。已按本条候选修法落地早失败：悬空显式 prefix →
  configure 期 FATAL_ERROR（报文含 BUILDING.md/bootstrap 修复入口）；prefix
  存在但 QGIS 头命中在 prefix 外 → FATAL_ERROR；无显式 prefix → STATUS 明示
  命中路线。护栏 `ctest -R first_configure`（cmake/verify-first-configure.cmake，
  UNIX）覆盖依赖存在（stub prefix 首配 + 四条 vendor include 断言）/ 缺失
  （显式+env 两路）/ 幂等复用三情形，反向验证（还原修复后）9 断言红。
  详见 docs/progress/perf-maintenance-20261027.md。

## P3 — 快捷键与帮助面

- 解决图层树 Delete 与数据树、顶点工具的键位遮蔽，保留真实交互覆盖。
- 数据页快捷键表可并入 F1 总表；地图画布原生键位需明确是否仅作展示登记。
- 证据：[快捷键账本](docs/ledger-archive/goal-loop-shortcuts-help.md)。

## P3 — 高频修改模块的进一步拆分

候选为 `src/workflow/mappingworkbench.cpp`、`src/app/appcontext.cpp` 与
`src/ui/pages/datalist_tree.cpp`。拆分前重新核对职责边界，不沿用旧行数排名。
AppContext 的工程打开编排涉及锁、journal、manifest、工区规则和服务重绑，需保持接管顺序。
已完成的 constraintpage/datapreviewtabs 拆分见 [历史账本](docs/ledger-archive/README.md)。

## P3 — AI 工具上下文补绑

`src/app/aiwiring.cpp::bindChatToolRunner` 尚未绑定 `traceFetch` 与 `faciesInput`。
补齐地震体道窗与井曲线输入，执行器继续如实报告缺失上下文。
已有工具调用闭环、会话及图形化配置不再列为待实现。

## P3 — 窄面布局

- 评估资产表类型/关联列的宽度限制，保证未决关联和确认控件可用。
- 综合柱状图工具条动作分组仍需评估；已有横向滚动不能代替完整的窄屏交互验收。
- 设计和验证依据：[DESIGN.md](DESIGN.md)、[历史视觉账本](docs/ledger-archive/goal-loop-ui-visual-polish.md)。

## P3 — 连井剖面的剩余能力

- 接入置信度曲线及独立岩性文件导入器。
- fence 剖面跟随主剖面的解释来源选择；多份解释的合并算法需单独定义。
- 已交付的 TVD 换算、真实井距、解释选择及岩屑录井不再作为缺口。
- 证据：[连井剖面账本](docs/ledger-archive/goal-loop-wellsection-deep.md)。

## P3 — 断面 Y 型 / 分叉与自动生长（from goal/fault-surface, 2026-10-02）

- **What:** 成面只接受沿走向单调的单支断棒条带。同一剖面上 trace 与纵向
  同时重叠判为分叉并拒绝。走向转折超过 75° 拒绝。单剖面断层拒绝，
  不向上下外推。不从断层多边形反插断面。
- **Why:** Y 型、多分支和断面自动生长需要另一套拓扑，不是条带三角剖分。
- **Pros:** 失败显式，不会把分叉画成一张假面；**Cons:** 分叉断层仍只有棒。
- **Context:** docs/progress/fault-surface.md「递延」。
- **Effort:** human: L / CC: L
- **Priority:** P3
- **Depends on:** goal/fault-surface 的条带成面核与 SurveyFrame

## P3 — 单因素的数据与屏障语义

- 净毛比/有效厚度需专家明确有效层判据，不能用砂厚代替净厚。
- 测井孔渗统计需定义曲线助记名、层段加权和单位后再接入。
- SGS 逐线屏障语义与协变量栅格跨 CRS 采样仍需实现。
- 依据：[单因素工作流](docs/workflows/SINGLE_FACTOR.md)。

## P3 — 逐断块变差拟合与打印排版

逐硬隔断分量拟合需明确小断块井数不足时的退化策略；当前全场汇聚的隔断过滤
不等同于逐分量拟合。交会结果的打印排版沿 Mapbook 入口实现。
已完成的 SFPKG、协克里金及方向线/软边界消费见 [单因素账本](docs/ledger-archive/goal-loop-single-factor-native.md)。
原“进程内 Python GUI 嵌入”不再作为递延实现路线，执行 [C++ 主线边界](docs/CPP_PRIMARY_BOUNDARY.md)。

## P3 — 多文件井曲线的剩余读口

- LIS 快道/多维通道展开、TVD→MD 的测斜反推仍需按格式与深度契约推进。
- BE 格式等待公开规范或样件，不以猜测实现。
- 多文件读口向尚按单资产工作的消费面扩展前，逐入口核对并集、主文件与重采样口径。
- DLIS/LIS、显式主关联及已有 MD/TVD 对齐见 [多格式交付说明](docs/progress/welllog-multiformat.md)。

## P3 — 地层格架与属性建模

保留 Y 型断层分叉成面、heave 水平错动、深度变化断距、pillar 网格、带内变差拟合、
河道/点坝对象耦合及多对象谱系、多 realization 的逐条流式序列化。
已落地的断块 z 向错位、SGS/相控/对象模型和斜井消费见 [属性建模说明](docs/progress/property-modeling.md)。

## P3 — 自动追踪的多事件歧义

事件丢失后仍可能滑落至邻近同相轴，需评估事件唯一性与振幅极值复核。
不规则测网的落格/跳过策略需按真实测网验收。
3D 滑窗传播服务、画布入口和 `dipHistoryPicks` 显式倾角引导已交付，见
[3D 层位账本](docs/ledger-archive/goal-loop-horizon-3d.md)，不再作为待接入项。

## P2 — 结构侧卡 SHA 复验（2026-10-03 去重审查）

- **What:** `constraintfactorjobs.cpp::generateStructuralFactor` 发布记录
  `structural_sha256`，`prepareAnalysisContourJob` 读取 structural_path 并检存在，
  `qgis/factorcontour.cpp::generateStructuralContours` 检 JSON/FieldContourSurface/尺寸，
  尚未比较记录的侧卡 SHA。不要把这些结构检查称为字节完整性复验。
- **Why:** 内容被改写但格式/尺寸仍合法时，侧卡可影响结构等值线；当前 raster SHA
  守卫只能保护栅格字节。本次保持行为，后续以独立修复加入 prepare/publish 复验。
- **Context:** `tests/tst_singlefactor_fieldcontours.cpp` / `tst_factorworkflow.cpp`；
  建议补合法但被改写的侧卡拒绝及临时产物清理用例，不放宽原阈值。
- **Effort:** human: S / CC: S；**Depends on:** 三段作业侧卡快照契约。

## P2 — 历史性能与视觉红项复核

旧基线的 `tst_cache_las`、`tst_wellcomposite_visual`、`tst_singlefactor_perf` 红项需用
同一依赖链和可比环境重新复核。2026-10-03 的次数、耗时及代码行号仅保留在
[清理前快照](docs/ledger-archive/backlog-before-cleanup-20261027.md)，不作为当前失败结论。
Windows 环境豁免以 `tools/env_redset.json` 及 CI 现行规则为准。

## P3 — 交会分类与跨域采样

- 远程多特征逐点推理需扩展当前 tile 输入契约，并核对井段相名的 catalog 映射。
- 时间/深度域交会需单位、基准、速度模型和不确定性契约。
- 4D/时移与交会打印排版各自立项。
- 监督分类、SOM、分块 GMM 和置信度伴生输出已交付，见 [交会说明](docs/progress/crossplot-facies.md)。

## P3 — 导出图签的单位名字段

编制、审核、日期图签已交付。单位名栏目仍需业务确认字段来源后接入
`src/qgis/layoutexport.cpp` 同一出口。

## P3 — 表 K.1 十二类探井符号地质评审（from wave/deepen-perf C3, 2026-09-30）

- **What:** C3 按 prompt 指名 6 类 + resources/geology/catalog.json 语义色补全 6 类的原生矢量符号做一轮图式 fidelity 评审（对照 Q/HS 1011—2016 表 K.1 正式图式）。
- **Why:** 符号已数据字段驱动落地并可扩展（`wellCategoryDefinitions()`），但 12 类的笔画细节未经地质专家核对。
- **Pros:** 图面规范合规；评审只需对符号表不动代码结构。
- **Cons:** 占用地质专家时间；工区数据暂无类别字段（生产层无 well_class 列，接线已按「有字段才启用」探测）。
- **Context:** `QgisStyleService::applyWellCategoryStyle` + appcontext 类别字段探测；触发条件：真工区井头数据带类别字段上图。
- **Effort:** human: S / CC: S
- **Priority:** P3
- **Depends on:** 井头数据带类别字段

## P2 — 相界逐弧段语义

四类要素级相界、样式、编辑规则及 QA 已交付。剩余为 boundary-graph 线层、共享弧
归属、弧段编辑回写；需真实弧段级编辑需求后实现。
证据：[相界类型账本](docs/ledger-archive/goal-loop-boundary-kinds.md)。

## P3 — project_area 计划递延（from /autoplan Eng + DX review, 2026-09-26）

以下项在各阶段的 NOT in scope 清单里，按 autoplan 的授权路径收集到此，未在门控前逐个询问。

- **SEG-Y 字节表编辑器**：当某个工区的道头 inline 字（偏移 188）不恒为 0 时，需要手改字节映射。本工区 200P_seismic.sgy 的 inline 恒为 0、CDP 在偏移 20，按道号索引即可。触发条件：遇到 inline 字可变的体。Effort: M / Priority: P3 / Depends on: SEG-Y 道索引落地（T1）。
- **IDW power 界面控件**：现在 power 固定为 2、权重与 `paleo:paleo_constraint_idw` 相同。给地质人员可调会改变等厚面形状，需先有用例。触发条件：用户要求调参。Effort: S / Priority: P3 / Depends on: 厚度链落地（T10）。
- **10 ms 残差阈值界面控件**：阈值是筛选值，印在问题上，不是地质标准。做成可改的设置前先看验证页实际使用。触发条件：用户要求改阈值。Effort: S / Priority: P3 / Depends on: 残差表落地（T8）。
- **ONNX 结果重采样**：预测张量挤成二维后不是 411×641 就失败、不写栅格，界面写实际行列数。补一个重采样路径前，先确认模型输出规格。触发条件：接入输出非 411×641 的模型。Effort: M / Priority: P3 / Depends on: 预测页落地。
- **公开文档站**：本产品是内部工作站，README 命令已够。不预设文档 URL。触发条件：对外分发。Effort: L / Priority: P4 / Depends on: 无。
- **TTHW 看板**：首次绿色测试的目标是 vendor 引导后 2–5 分钟；持续跟踪 TTHW 的看板属于度量产品建设，本轮不做。触发条件：入门时长收到投诉。Effort: M / Priority: P4 / Depends on: 无。

## P3 — 真工区人工验收

- 文件夹导入确认对话框与 Web 服务 dock 的完整人工流程。
- SEG-Y 缓存发布修复后的真实数据复验。
- 新工区按 [工区参数说明](docs/AREA_PARAMETERS.md) 完成端到端验收，同时补齐层序命名约定。

## P3 — 分层与图层平台

- include 排序规范化；`DataImportService` 兼容类型别名的调用点迁移。
- include 护栏的调用级补强，以及大缓冲分配的预算审计。
- 图层/资产 schema 级显式关联按实际需求评估；保留现有自定义属性与目录反链语义。
- 核对“删除选中”的摘树/注销图层行为，再按用户需求调整。
- 现行各层已拆为静态库，`paleo_core` 是 INTERFACE 兼容伞，见 [AGENTS.md](AGENTS.md)。

## P3 — 预测与底层扩展

- `PredictionWorkflow::confidenceCompanionAvailable()` 对当前预测入口仍返回 false。
  该入口有真实置信度输出后再声明伴生层，不影响其他已交付算法的置信度产物。
- native processing provider、SBM Mesh/Model/HorizonTextReader 及 C ABI 按新功能需求评估。
- 网络盘慢路径超时与 NFS 降级等待真实工区证据，不沿用历史本地盘耗时作验收。

## P3 — 历史任务与交付的结构化映射

94 份历史任务书与执行账本尚无完整的“编号→交付”元数据映射；不能凭文件名相似度判缺件。
后续需要机器校验时，先补显式映射，再扩展文档漂移护栏。
历史记录入口：[账本索引](docs/ledger-archive/README.md)。

## P3 — 目录读取的线程门

`versions()`、`downstreamClosure()`、`mutationSeq()`、`catalogPath()` 等接口的 live 读取
线程门覆盖需继续核对；staging 副本保留独立语义。补齐时同步 liveReads 验收，来源为 #476 后续。

## P3 — 综合柱状图与解释联动

- `wellcompositestore` 的会话/sidecar 读写迁入 services 门面；多井深度 warp 需渲染契约。
- 测井相逐地层组预测的并发/批量接口、停止时是否保存已完成组，需服务契约确认。
- 预测按指定地层范围取优势相、井相柱装配进编图证据、手工修改井段后重算优势相表。
- 真实 GUI 中“相面标注不可见”的未复现报告继续人工复验。
- 依据：[测井相预测](docs/WELL_FACIES_PREDICTION.md)、[井属性](docs/WELL_VECTOR_ATTRIBUTES.md)。

## P3 — 编图证据深化

- 共享工程的身份认证/RBAC，以及解释历史的哈希链或签名。
- 受控重算多条目队列、截断索引的断点续扫。
- CRS 迁移、层位特殊字符转义的复核，以及跨项目/会话反查恢复的验证。
- `acceptsLocalGridCrs` 收紧为工程网格精确匹配。
- 依据：[编图证据契约](docs/progress/mapping-evidence-implementation.md)。

## P2 — env_redset STALE strict 档的到期开启计划（#764，2026-10-28）

`PALEO_FLAKY_STRICT=1` 已随 #764 在 workflow 级 env 开启（flaky 台账跨 run 持久化后，
同一测试被重跑洗满 3 次即硬红）。`PALEO_REDSET_STALE=strict` **暂不默认开启**：它把
「豁免项本轮通过 → 名单可收缩」从 ::warning 升为 ::error，直接依赖
`tools/env_redset.json` 现有 16 条（13 fixed + 3 exempt/localdeps）在 vendored 车道的
实际通过面——翻转即可能在必过 linux 车道引入本地无法复现的新红。

到期开启计划（满足任一条件即可在 ci.yml 删掉本节的「暂不」并在 workflow 级 env 加
`PALEO_REDSET_STALE: strict`）：

1. **证据条件**：连续 3 个 linux run 的 `tools/check_env_redset.py --strict` 输出里
   STALE 条目集合稳定（同一批豁免项连续通过），且 `tools/env_redset.json` 已按该集合
   删完可删条目——此时 STALE 只剩「新僵化」信号，升红不会误伤。
2. **时间条件**：2027-01-28（与 `docs/ci/known-reds.json` 的
   `ts-stale-lupdate-backlog` 复审期对齐）前未满足证据条件，则在本节补一轮复跑证据
   并续期，不许无限期停在过渡档。
3. 开启后首个 run 若因 STALE 红，按「先删名单条目、再放行」处理，不得回调
   `PALEO_REDSET_STALE` 值。
