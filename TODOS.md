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

## P2 — 单因素原生算法后续（from goal/single-factor-native, 2026-10-02）

- **What:** 克里金体系深化、完整 SFPKG 导入、外委 XML/XLSX 批量读取、
  参考工程全部历史制图策略、时深域转换、监督分类、打印排版、
  完整 Python GUI 嵌入，以及有限断层路径距离 `FaultPathMetric`。
- **Why:** 本次交付只做 C++ 局部方向 IDW、软边界、井群权重、硬屏障栅格连通、
  真实数值等值线和显式制图工作场。上游井数超过 80 等条件下的各向异性路径
  会退成 IDW，不能把 UI 标签当成克里金。
- **Pros:** P0/P1 保持可复算的分析场语义；**Cons:** 路径绕距、历史闭环补接
  和批量外委格式要另立项。
- **Context:** `docs/designs/single-factor-native-integration-plan.md` 第 1 节
  与第 7.3 节。硬屏障模型是 `grid_connectivity_v1`，不是 `FaultPathMetric`。
- **Effort:** human: L / CC: L
- **Priority:** P2
- **Depends on:** 单因素原生 P0 分析场进入目标基线

## P3 — 多文件井曲线其余读口（from goal/well-logset, 2026-10-02）

- **What:** 并集读面已接到岩石物理、属性建模、剖面井轨、交会清单和测井综合图。
  相关对比与成图工作台仍按单资产或「有没有 well_log」。DLIS/LIS/BE 不读。
  非驱动文件只做线性重采样，不做 MD/TVD 对齐。`attachLink` 仍把新挂链接升主。
- **Why:** 本方向锁的是 LAS 已决链接的并集和导入序，不改 catalog 格式，也不改挂接不变量。
- **Pros:** 第二份 LAS 的曲线能进计算和综合图；**Cons:** 走挂接而不是导入时主文件会换。
- **Context:** docs/progress/well-logset.md「递延」。
- **Effort:** human: M / CC: M
- **Priority:** P3
- **Depends on:** goal/well-logset 的 `WellLogSet` 读面

## P3 — 地层格架后续：断块网格 / 随机模拟（from goal/property-modeling, 2026-10-02）

- **What:** V1 只做等比例 IJK 格架 + 井曲线粗化 + 断层竖帘阻断的 IDW。
  未做 pillar/断块错位网格、Y 型断层、沉积相带/对象建模、序贯高斯模拟，
  也未做变差函数克里金。壳层「属性建模」dock 已接（fixes 分支）。
- **Why:** 断块网格和随机模拟是另一立项。斜井测斜表、断层棒投影也未做。
- **Pros:** 等比例格架和 IDW 已能出 DERIVED 属性体；**Cons:** 斜井轨迹、
  断距错位和相控仍要另做。
- **Context:** docs/progress/property-modeling.md「递延」。
- **Effort:** human: L / CC: L
- **Priority:** P3
- **Depends on:** goal/property-modeling 的格架核与属性体容器

## P3 — 层位自动追踪 3D 服务暴露 + 显式倾角引导（from goal/horizon-autotrack, 2026-10-02）

- **What:** 3D 前沿扫掠（algorithms/horizontrack::propagateVolume，数值核 +
  合成断言已落）暴露为 SeismicTaskService 异步任务并接画布层位面叠加；
  追踪核补显式斜率扫描倾角引导（当前隐式：搜索窗中心跟随前一道）。
- **Why:** 2D 剖面闭环已完成；面扩散是解释效率下一档（单种子 → 整层位面）。
  追踪器无事件唯一性校验，近距平行同相轴可滑落——显式倾角先验是现行
  阈值/相干门之外的第三道防线。
- **Pros:** 单种子出整面（分钟级人工拾取 → 秒级）；**Cons:** 体窗内存调度
  （IL 邻域滑窗）+ 画布层位面渲染是新工作量。
- **Context:** docs/progress/horizon-autotrack.md「已知边界/递延」；kernel
  propagateVolume 已含死列不复生/限步长/取消语义（tst_horizontrack 3 例）。
- **Effort:** human: M / CC: M
- **Priority:** P3
- **Depends on:** goal/horizon-autotrack 已落核/2D 闭环/GeoTIFF 上图管线

# TODOS — paleo_workstation

## P3 — 交会分类后续（from goal/crossplot-facies，2026-10-02）

- **有监督分类 / SOM**：样本标注→训练→推理另立项；RemotePredictionRouter
  沿 AI 方向深化。当前 k-means/GMM 只输出未解释簇编号，不自动赋地质相名。
- **时深域交会**：需要单位、基准、速度模型与不确定性契约后再接跨域采样；
  当前 SATR 只配时间层位（ms），深度栅格作为独立特征不能冒充时间。
- **大规模 GMM / 伴生置信度**：工区 N×k 缓冲达到内存预算时改分块 EM；
  分类服务已有真实置信度/距离向量，可按编图消费需要持久化伴生栅格。
  当前 Byte 分类图只写类别与 provenance，井段写平均置信度。
- **4D/时移、交会打印排版**：各自另立项，排版沿 mapbook 方向。
  口径与验收证据见 `docs/progress/crossplot-facies.md`。

## P3 — 地震属性体（时间切片/整体扫描）+ 属性图层入层树（from goal/seismic-attributes, 2026-10-01）

- **What:** 属性计算扩到时间切片/整体属性体（当前仅 IL/XL 剖面属性切片）；
  时间切片属性（IL×XL 地理栅格）入 layermanifest 层树。附带：相干沿剖轴
  道距加权（当前等权）、属性结果同参数去重缓存、SATR 读回器（当前只写）。
- **Why:** 瞬时族需整道谱/时窗族需垂向窗——时间切片（单采样面）不满足输入
  形状，需全测网分块扫描；层树条目需诚实栅格 URI，剖面属性图非地理参考。
- **Pros:** 属性解释工作流完整（平面展布+体透视）；**Cons:** 分块扫描调度 +
  体格式（.sattr 体化）是新工作量。
- **Context:** docs/progress/seismic-attributes.md「已知边界/递延」；SATR 容器
  头已带参数/几何（读回信息齐备）。
- **Effort:** human: L / CC: M
- **Priority:** P3
- **Depends on:** goal/seismic-attributes 已落核函数库/任务编排/SATR（可复用）

## ~~P3 — catalog.sqlite 查询索引（deferred from /autoplan SELECTIVE EXPANSION, 2026-09-25）~~（已落地：goal/catalog-sqlite, 2026-10-02）

- ~~**What:** 由 `catalog.json` 重建 `catalog.sqlite`，作为资产、版本、关联的查询索引。~~
  **2026-10-02 交叉注记**：持久化已迁到独立 `artifacts/metadata/catalog.sqlite`（内存四表仍是查询事实源，不是可重建查询索引）。邻条 mutator 写路径超线性 profiling 保持 2026-10-01 关闭，不重开。
- **Why:** ADR 0056 把 sqlite 定义为可重建索引，避免打开工程时扫 JSON。
- **Pros:** 资产变多后列表和校验不用每次解析整份 catalog。
- **Cons:** 20 口井的第一段用 JSON 就够；提前做会多一个必须和 catalog.json 对齐的存储。
- **Context:** `docs/PROJECT_AREA_PLAN.md` 第 3 节。触发条件：资产数量或列表查询变慢。**2026-09-30 对账（wave/deepen-perf B4）**：10k/100k 夹具实测查询面零劣化（entityById/linksForEntity/列表/计数全 O(1)，打开 10k 136ms / 100k 1408ms 线性）——触发条件未达，记档收工（数据 docs/perf/BASELINE.md §6）；**新发现** mutator 写路径超线性（10k 6.2s→100k 949.7s，疑二次）另立条目。
- **Effort:** human: M / CC: S
- **Priority:** P3
- **Depends on:** catalog.json 受管 RAW 已能往返

## P3 — catalog mutator 写路径超线性 profiling（from wave/deepen-perf B4, 2026-09-30）

- **What:** catalog.json 写路径（BatchSave/导入灌库）规模超线性：10k 资产 6.2s、100k 949.7s（≈153×，疑二次——疑与全量重扫/重序列化次数有关）。
- **Why:** 查询面已 O(1)（邻接索引），写面成为 >50k 资产目录的下一瓶颈。
- **Pros:** 大目录导入/保存不再分钟级卡住。
- **Cons:** 20 井工区（百级资产）远未触达；过早优化挤占域功能。
- **Context:** 夹具 `makeSyntheticCatalogDir`（tst_catalog_scale，`PALEO_CATALOG_SCALE` 门控）。触发条件：资产 >50k 或实测导入超分钟。
- **Effort:** human: M / CC: M
- **Priority:** P3
- **Depends on:** catalog.sqlite 条目（如届时已落，写路径一并设计）
- **2026-10-01 关闭（WP2 goal/data-io-catalog-closure）**：根因=六 mutator 的
  全表快照 COW detach（O(N)/次）+ markStale/nextEntityId/sha/快照查询面线性扫；
  修复=精确 undo 回滚 + 索引化（语义等价由测试钉死）。同机 A/B（RelWithDebInfo）：
  100k 灌库 3,518s→9.28s（379×）、10k 17.1s→1.14s（15.0×）、10k→100k 倍率
  205.5×→8.1×（≤15× 目标达成）、查询面零变化、峰值 RSS ≈1.0GB 无 N 份复制。
  证据 docs/perf/BASELINE.md B7 / docs/progress/data-io-catalog-closure.md。

## ~~P2 — 剖面 dock 取数迁 SeismicTaskService~~（已落地：goal/seismic-runtime-closure, 2026-09-30）

- ~~**What:** 主窗口剖面 dock 的 IL/XL/Time 切换仍裸 QThreadPool 直调——无并发闸、无取消、无 LRU、progressCb 恒 true；迁到 SeismicTaskService 现有通道（闸/取消/LRU/回落齐备）。~~
  **2026-09-30 对账（goal/seismic-runtime-closure 轮 1）**：IL/XL/Time + 卷帘 B 图全部迁
  `startSliceExtraction`（闸/取消/共享 LRU/Auto 回落）；顶替=requestCancel+请求号守卫
  （cancelled≠failed 静默丢弃）；切体/任意线/切片三方互顶替双向取消；顺带修复既有反向缺陷
  （任意线迟到结果覆盖切片显示）。回归：tst_seismic_sectionui 23/23（含 7 个新治理用例）。
- **Why:** 同体数据三条取数路径两条有治理一条裸奔；转码工作区热切换后直调路径吃不到后端红利。
- **Pros:** 三后端一致的空态/取消/回落语义（SECTION §6 对照表已列差值）。
- **Cons:** seismicsection dock 属交互热路径，迁移要过一轮拖动延迟回归。
- **Context:** docs/seismic/SECTION.md §6 记档；D2 生命周期测试已覆盖事件面。触发条件：剖面交互预算超限或后端不一致 bug。
- **Effort:** human: M / CC: M
- **Priority:** P2
- **Depends on:** 无

## P3 — 标准导出流图签块（from wave/deepen-perf D4 评估, 2026-09-30）

- **What:** `buildHorizonMapLayout`（workflow/mapexport.cpp）补编制/审核/日期图签块（QgsLayoutItemLabel，约 30 行）。
- **Why:** D4 评估结论：简化 composer 不建——标准导出流已两步全自动 + 完整设计器已裁剪；四要素唯一缺口是图签。
- **Pros:** 关闭「规范图件四要素」缺口而无需新 UI。
- **Cons:** 图签栏目（编制/审核/日期/单位名）需业务确认字段来源。
- **Context:** docs/progress/deepen-perf.md D4 行；触发条件：发布门要求图签或用户提出。
- **Effort:** human: S / CC: S
- **Priority:** P3
- **Depends on:** 无

## P3 — 表 K.1 十二类探井符号地质评审（from wave/deepen-perf C3, 2026-09-30）

- **What:** C3 按 prompt 指名 6 类 + resources/geology/catalog.json 语义色补全 6 类的原生矢量符号做一轮图式 fidelity 评审（对照 Q/HS 1011—2016 表 K.1 正式图式）。
- **Why:** 符号已数据字段驱动落地并可扩展（`wellCategoryDefinitions()`），但 12 类的笔画细节未经地质专家核对。
- **Pros:** 图面规范合规；评审只需对符号表不动代码结构。
- **Cons:** 占用地质专家时间；工区数据暂无类别字段（生产层无 well_class 列，接线已按「有字段才启用」探测）。
- **Context:** `QgisStyleService::applyWellCategoryStyle` + appcontext 类别字段探测；触发条件：真工区井头数据带类别字段上图。
- **Effort:** human: S / CC: S
- **Priority:** P3
- **Depends on:** 井头数据带类别字段


## P2 — 多 realization / 不确定性支持（deferred from CEO review D6, 2026-09-25)

- **What:** 每层位存 N 个预测 realization，派生置信度面，同一 canvas 切换 realization。
- **Why:** 相对商业软件的研究级差异化能力；井点稀疏区的不确定性可视化。
- **Pros:** 真正的不确定性量化；预测-验证闭环更强。
- **Cons:** 触及数据模型、存储、预测管线、版本、UI — 约使预测子系统翻倍。
- **Context:** 当前文档 §7 只有单个"预测置信度"图层。schema 已预留可空 `realization_id`（见 PALEO_QGIS_PLAN.md NOT-in-scope 决议）。做时先定 realization 与 version 的正交关系。
- **Effort:** human: XL / CC: L
- **Priority:** P2
- **Depends on:** 智能预测管线落地后

## P2 — 相界地质语义类型（deferred from CEO review, 2026-09-25）

- **What:** 相界线不只是 polygon 边，区分整合接触 / 尖灭 / 相变 / 断层切割等类型，影响拓扑编辑与图面表达。
- **Why:** 真实古地理图的边界有地质含义；不同边界类型的编辑行为和符号不同。
- **Pros:** 编图专业正确性；验证模块可按类型核查。
- **Cons:** 数据模型与编辑工具复杂度上升；需要地质专家参与定义。
- **Context:** 文档 §14–15 目前把相界当普通 polygon 拓扑处理。先做单一"相界线"类型跑通，再扩类型。**2026-09-30 进展（wave/deepen-perf C2）**：4 类词面已冻结（`src/workflow/boundarysemantics.h`），单类型断层切割 fault_cut 已跑通（boundary_kind 属性 schema → composepage 下拉 → `applyFaciesBoundaryStyle` 断层红粗边）；kind 落要素级，逐弧段需 boundary-graph 线层；其余三类的差异化编辑行为仍需地质专家定义。
- **Effort:** human: L / CC: M
- **Priority:** P2
- **Depends on:** P0 矢量编辑落地

## P1 — 工程评审待办（from outside-review findings, 2026-09-25）

以下项已由 CEO 评审确认纳入计划。**2026-09-27 对账：全部落地。**

- ~~**保存/发布语义状态机**~~ — 已落地：`docs/VERSION_PUBLISH_STATE_MACHINE.md`（wave-3 derived-publish `72dc070`）。
- ~~**native:* 算法逐项审计**~~ — 已落地：`docs/ALGORITHM_AUDIT.md`（wave-3 model-hardening `82dff27`）。
- ~~**算法测试框架自建**~~ — 已落地：`tests/tst_algorithm_harness` + `tests/algorithmbase.h`（wave-3，`82dff27`）。
- ~~**schema 迁移策略**~~ — 已落地：`docs/SCHEMA_MIGRATION.md` + catalog `schema_version` 校验 + `.bak` 轮转原子落盘；同工程双实例并发写经 `ProjectDirLock`（`AppContext` 打开工程时取锁排他保护）。
- ~~**崩溃报告机制**~~ — 已落地：§38 本地优先（fd 转储 + `.running` 脏退出 + 重启提示），`docs/CRASH_REPORTING.md`（wave-4 `2c1c8e1`）。
- ~~**速度模型 + line-geometry↔CDP 映射**~~ — 已落地（wave-3 derived-publish `72dc070`）。
- ~~**性能测试**~~ — 已落地：`tst_perfbudget`（wave-3 `82dff27`）。
- ~~**渲染对比测试稳定性**~~ — 已落地：vendor 字体注册 + `pinRenderEnvironment` 显式浅色 palette（`fe7f226`）。
- ~~**合成最小 SEG-Y fixture**~~ — 已落地：`tools/make_segy_fixture.py` + `tst_segy_fixture`（wave-3 `82dff27`）。
- ~~**多 CRS 假设核查**~~ — 已落地：`docs/CRS_ASSUMPTION_AUDIT.md`（wave-3 `82dff27`）。

## P1 — Phase 0 spike 交付物（§39, E3）—— 2026-09-27 对账：全部落地

- ~~vendor superbuild 骨架 + 依赖清单~~：`vendor/superbuild/`（裁剪开关 + README，wave-4 `8fc7bb2`）。
- ~~目标平台矩阵~~：`docs/PLATFORM_MATRIX.md`（wave-4 `8fc7bb2`）。
- ~~AI 推理运行时选型~~：ONNX Runtime 已 vendored（`vendor/onnxruntime`，`tst_onnx`/`tst_onnxworkflow` 端到端过）。
- ~~app-only 功能审计~~：`docs/APP_ONLY_AUDIT.md`（零 qgis_app 链接逐能力对账，wave-4 `8fc7bb2`）。

## P3 — 设计评审递延（from /plan-design-review, 2026-09-25）

- **暗色模式**：DESIGN.md token 结构已支持；需重配全部语义色并验证 canvas 符号在暗底可读性。触发条件：V1 完成且用户提出需求。
- **简化版编图 composer**：仅图例/比例尺/指北针/图签的聚焦 UI，替代完整 QgsLayout 设计器（D12 的兜底方案）。触发条件：V1 编图页实测显示完整设计器过载。工作量级：月级（重复 src/app 代码），勿投机先建。

## P3 — project_area 计划递延（from /autoplan Eng + DX review, 2026-09-26）

以下项在各阶段的 NOT in scope 清单里，按 autoplan 的授权路径收集到此，未在门控前逐个询问。

- **SEG-Y 字节表编辑器**：当某个工区的道头 inline 字（偏移 188）不恒为 0 时，需要手改字节映射。本工区 200P_seismic.sgy 的 inline 恒为 0、CDP 在偏移 20，按道号索引即可。触发条件：遇到 inline 字可变的体。Effort: M / Priority: P3 / Depends on: SEG-Y 道索引落地（T1）。
- **IDW power 界面控件**：现在 power 固定为 2、权重与 `paleo:paleo_constraint_idw` 相同。给地质人员可调会改变等厚面形状，需先有用例。触发条件：用户要求调参。Effort: S / Priority: P3 / Depends on: 厚度链落地（T10）。
- **10 ms 残差阈值界面控件**：阈值是筛选值，印在问题上，不是地质标准。做成可改的设置前先看验证页实际使用。触发条件：用户要求改阈值。Effort: S / Priority: P3 / Depends on: 残差表落地（T8）。
- **ONNX 结果重采样**：预测张量挤成二维后不是 411×641 就失败、不写栅格，界面写实际行列数。补一个重采样路径前，先确认模型输出规格。触发条件：接入输出非 411×641 的模型。Effort: M / Priority: P3 / Depends on: 预测页落地。
- **公开文档站**：本产品是内部工作站，README 命令已够。不预设文档 URL。触发条件：对外分发。Effort: L / Priority: P4 / Depends on: 无。
- **TTHW 看板**：首次绿色测试的目标是 vendor 引导后 2–5 分钟；持续跟踪 TTHW 的看板属于度量产品建设，本轮不做。触发条件：入门时长收到投诉。Effort: M / Priority: P4 / Depends on: 无。

## P3 — autoplan pass-2 递延（2026-09-26）

- **人工验收清单（非代码）**：~~下次真机启动~~ 2026-10-01 真机已启动（master `c891dde`，真工区 `~/projects/paleo_project/data/project_area` 打开渲染正常）。**已验**：① `.running` 崩溃旗标生命周期闭环——强杀残留→下次脏检出、干净退出自清 pid 旗标；历史残留 `.running-*` 不自动清、`.running` 兜底常脏（与 issue #41 同族）。② 966MB SEG-Y 取数路径实测：索引缓存命中 open 271–275ms、IL 64ms/热 63ms、时间片 59/30ms、64³ 体窗 46ms、对角剖面 97ms——无冻结级时长，异步 IO 无需追加排期。③ QWebEngineView 真机探针（同 `AA_ShareOpenGLContexts`，xcb/XWayland）：渲染 OK、WebGL OK、QtWebEngineProcess/沙箱正常；GBM 不可用→Chromium 自动回落 Vulkan；外网 TLS 被本机网络拦（非缺陷）。**观测**：966MB 索引首发时 cache publish 两连败（verify-fail → ENOENT）第三次成功自愈——`ensureLegacyGlobalCacheDir` 护栏覆盖不全，记给 audit-issues 归属。**剩余人工项**：文件夹导入确认对话框手测 + Web 服务 dock 点开目检。触发条件：真机有人。Effort: S / Priority: P2
- ~~**「重新定位文件」恢复路径**~~ — 已落地：`relocateVersionSource`（流式 SHA-256 复验、不一致拒解、同 SHA 追加外链版本）+ 预览「重新定位文件…」入口（wave-4 `2c1c8e1`）。
- **第二工区参数化接缝**：~~外置 seam~~ 已落地——`AreaRules`（层序名单/分类器目录规则/SEG-Y 四偏移/ONNX 网格门，经 `project_area.json` 覆盖，默认=本工区值；`docs/AREA_PARAMETERS.md`，wave-4 `8fc7bb2`）。**遗留**：真接第二个工区时按该文档走通一遍验证 seam 完备性。触发条件：接入第二个工区。Effort: M / Priority: P3
- **Onto 层位/边界文件的命名规范**：文件名不在 8 个层序界面时产未决层位实体、不进编图 chip——属已交付行为；名单经 `AreaRules.sequenceBoundaries` 可配（wave-4），命名规范文档化随第二工区处理。触发条件：新层序命名。Effort: S / Priority: P4

## P3 — UI 分层收口递延（from /autoplan docs/UI_LAYER_PLAN.md, 2026-09-27）

- **clang-tidy include-order CI**：分层检查器只管方向不管序；include 排序规范化递延。触发条件：分层落地后代码风格再收一轮。Effort: S / Priority: P3
- **`DataImportService` using 别名删除**：`FolderPreviewRow`/`FolderRowResult` 解嵌套后保留源码兼容别名一期（保护 tst_import 22 处用点）；二期删除别名、调用点全改 `domain/importrows.h`。触发条件：W2 落地后的下个迭代。Effort: S / Priority: P3 / Depends on: UI_LAYER_PLAN W2
- ~~**大 LAS 同步 `lasAt` 的 UI 线程延迟悬崖**~~ — 已落地（wave/deepen-perf B1）：correlation 链路改 PaleoTaskService quiet 异步 + per-well 世代号 + 同井协作取消，59MB 调用点阻塞 442ms→0ms（见 docs/progress/deepen-perf.md）。
- ~~**文件夹导入扫描期进度 UX**~~ — 已落地（wave/deepen-perf B2）：队列整体进度/ETA/全部取消 + `FolderImportQueueAdapter` 生产 runner（GAPS G-2.3 收口）。
- **include 级护栏的调用级补强**：单一 `paleo_core` 静态库下 `check_layering.py` 只挡 include 挡不住「不带 include 直接 new」；若要挡需 clang 插件或拆库。触发条件：发现绕过 include 的违规实例。Effort: M / Priority: P4
- **图层平台 · 旧组名词表迁移**：`workflows.cpp` 产层仍用 `01_Prediction`/`02_Constraints`/`03_Predict`/`03_Composite`/`00_Data`，页面档案表（`QgisLayerProfileService`）按 canonical 词表（`02_Prediction`/`03_Constraints`/`05_PaleoMap`/`07_Validation`…）匹配，旧组名层在档案应用时按表外隐藏。触发条件：编图链产出层迁移到 canonical 组。Effort: S / Priority: P2
- **图层平台 · 主题重命名**：QgsMapThemeCollection 无 rename API，档案工具条管理对话框已注记「重命名暂未支持」。触发条件：QGIS 提供 rename 或 `QgisLayerProfileService` 增加记录复制通道。Effort: S / Priority: P3
- ~~**图层平台 · layerId↔assetId 关联面**~~ — 已落地（wave/deepen-perf C4）：七个产物出口 commit 后盖图层自定义属性 `paleoAssetId`（随 .qgz 持久化）+ catalog extra `manifest_layer_id` 权威反链；`LayerPropertiesDialog::assetIdForLayer()` 业务页开闸。catalog 显式关联表（schema 级）仍按原触发条件递延；首跑盖章缺口（层未实例化则盖不上）由 extra 反链兜底 + 重跑幂等补章。
- ~~**图层平台 · 图层创建时间**~~ — 已落地核实（wave/deepen-perf C4 对账）：`paleoCreatedAt` 盖章链基线已存在，业务页显示正常，无需 schema 变更。
- **图层平台 · 「删除选中」语义**：QGIS 默认动作只摘树节点不 `removeMapLayer`；若需「删树即删层」，壳侧补工程注销接线。触发条件：用户实测困惑。Effort: S / Priority: P3

## P3 — m2/mapping-pages 递延（wave/mapping-pages, 2026-09-27）

- **置信度伴生栅格**：算法侧无真实置信度输出（ONNX 仅读首个输出张量、paleo:\* 均确定性单输出栅格）——不造假数据；接入点已留（`PredictionWorkflow::confidenceCompanionAvailable()` 恒 false + 声明位）。触发条件：出现带置信度/方差输出的算法。Effort: S / Priority: P3
- **非 IDW 单因素引擎**：~~welldist（距离变换）~~ 已落地（wave/deepen-perf C5）：`paleo:paleo_distance_transform` 绕障距离引擎（无屏障=精确欧氏与 paleo_welldist 零容差对拍；break_line 屏障=8 邻接 Dijkstra），注册表标签已翻「绕障距离变换」；confidence 维持冻结拒绝（ONNX 仅读首个输出张量，无置信度通道——记档 docs/ALGORITHM_AUDIT.md §3a）；strathick 核实主线 6 已接（paleo_isopach 双栅格链），无需动作。
- ~~**PaleoEditingToolbar `mEditLayer` 裸指针**~~ — 已落地：`mEditLayer` 与 `mLayers` 均已切为 `QPointer<QgsVectorLayer>`（`src/ui/edittools/editingtoolbar.h:134,137`）。
- ~~**ctest -j2 跨二进制 QSettings 竞态**~~ — 已核实根治（wave/deepen-perf B5）：四轮全量 `ctest -j4`（127 项）历史竞态点全绿——`add_paleo_test` 的 XDG/HOME 沙箱已根治（证据 docs/perf/BASELINE.md §6）；四个测试 main 的 `setPath` /tmp 重定向属历史残留可清理。
- **native processing provider 注册**：C++ 嵌入运行时 Processing 注册表仅 `paleo:\*`（`gdal:contour` 属 Python provider）；等值线已走 GDAL C API（gdal:contour 同一底层引擎）交付，native provider 按需引入。Effort: M / Priority: P4
- ~~**SBM Engine 剩余入口**~~ — 已落地对账关闭（wave/deepen-perf A1）：QuickOpen/ReadTimeSliceTiled/progressiveLod+SetActiveLod 前序 wave 已接，本轮补齐唯一缺口 ReadVoxelWindow 消费侧（3D 16 层堆叠取数 16 请求→1 体窗任务）；四入口消费核账表 docs/seismic/ARCHITECTURE.md §9b。
- **SBM 未 vendor 面**：`Mesh/`（CgalHorizonMeshBuilder，GPL/LGPL 双许可 CGAL 可选）、`Model/`、`Data/HorizonTextReader`（层位面三维渲染/文本导入——现阶段层位走 QGIS 图层，暂不引）。`Engine/SdkC.h` C ABI 已随库编译但未导出消费方。Effort: S / Priority: P3

## Completed

- **2026-09-26 · project_area 数据底座 + D61 编图链**（p1/p2 双包并入 master）：catalog 实体/资产/版本/显式关联 + SHA-256 受管 RAW；分类器与井口/分层/时深解析；D61 装箱时间栅格；SEG-Y 道索引单测线解码；9 类数据页预览；读侧 facade、D61→D62 厚度→凸包约束 IDW→相多边形；TD 残差验证、三视图联动、PDF 导出、8 层位 chip、版本状态机。`424e185` `f3d9b83` `c994d21`
- **2026-09-26 · 集成接缝 + 清单读错误诚实化**：ComposePage 经 `layerDeclared` 信号跟随新声明；生产路径全改 `tryDeclared`/错误通道（清单损坏不再被当成空清单）。`fdd2f99` `217502a`
- **2026-09-26 · 文档内嵌 PDF 预览**：document 资产原件恒为 RAW 规范源；office 格式首预览时经 soffice headless 懒转受管 DERIVED PDF（父版本=RAW）以 QtPdf 渲染；无转换器/失败如实降级为「用系统程序打开」。`6dbeec7`
- **2026-09-26 · WebEngine 嵌壳组件**：`WebViewPanel` 懒加载孤岛（offscreen/渲染进程终止降级为外部浏览器兜底）+ `AA_ShareOpenGLContexts`。`96ba726`（宿主入口见 goal/webui-host）
- **2026-09-26 · 井文件解析 BOM 剥离**：三个入口统一去 U+FEFF（trimmed() 不去它），plan §3 要求。
- **2026-09-27 · autoplan pass-2 Wave-1（四分支 rebase 至 `ce746b3` 并入 master）**：catalog 耐久性（T17 ver-N/ast-N 序号恢复与拒绝重发、T20 `.bak` 轮转+失败后拒写+`catalogOpenFailed` 信号、T33 `BatchSave` 批次+`unresolvedLinks()`+装载期坏段跳过+TOCTOU canon 复核）；映射/验证（T18 TD `-99999`/非有限过滤、T19 isochron≤0/非有限→nodata、T21 P1/P2/P3 几何校验+`PALEO_INLINE_*`/`PALEO_DT_MS`/`PALEO_T0_MS` 元数据、T24 残差表双击导航、T25 top-XY 采样+`RASTER_MISSING`）；文件夹确认 UI（T22 分类器词表/HZ28 锁/逐行重试/CRS 句/D3 第四计数/D5 生效类型重排序）；T30 退役 `SeismicPreviewPanel`。验证：ctest 53/53 + 真数据 smoke（`PALEO_REAL_PROJECT_AREA`）双绿。`b52114e` `0e4010c` `f4b7335` `1af8d1e` `61cdb0d`
- **2026-09-27 · autoplan pass-2 Wave-2（并入 master，`545f1ad`…`8a65531`）**：D2 `PaleoTaskService`+TaskPanel 进度行；D1a/D1d SEG-Y 索引/SHA/单线解码任务池化+每资产缓存；D1b/D1c `catInvoke` marshal+单文件/文件夹导入异步；D6 井上图层+zoom+地图→表联动；D7 预览 60% 预算+最大化钮；D8 厚度触发命名+使能态；D11 GeoJSON 临时配准→DERIVED+水印。ctest 54/54 + 真数据双绿。
- **2026-09-27 · Wave-3 三 PR + codex PR #12 并入 master**：派生产物 catalog 登记/发布链补全/残差下限/ONNX 端到端（`72dc070`）；数据模型加固（剥 uwi/aliases、schema 迁移、SEG-Y 夹具、算法 harness、审计文档、性能预算，`82dff27`）；UX 一致性（胶囊/中文化/链接身份/双向同步/空态/字体/焦点环/undo 跨 reload，`f79945a`+接缝 `175db2a`）；评审修复+CI 基建（`c908e1e`）。ctest 61/61。
- **2026-09-27 · Wave-4 两 PR 并入 master**：崩溃报告+外链重定位（`2c1c8e1`）；AreaRules 参数 seam+Phase-0 vendor 收口四文档（`8fc7bb2`）。ctest 67/67 + 真数据逐字节一致。
- **2026-09-27 · data-fabric 采纳四包 + ribbon 并入 master**（规格 `docs/DATA_FABRIC_ADOPTION.md`）：RoleRegistry 工程词表（`d2bcaf1`）、commit-coord journal 幂等有序提交（`12aa9b1`）、EntityView+ordinal+staleness（`6e34d9d`）、IngestPlan 三段式幂等导入（`6d1f3cb`）、ribbon QGIS 主题图标+自绘补缺（`0615050`）。ctest 67/67。
- **2026-09-26 · /autoplan 评审修复 F1–F8（九分支并入 master）**：画布绑定工程 + `m_instances` 悬空清理 + 无基准 ENGCRS 替换 eqc + 打开失败对话框；TimeDepthTool 重写（文件序/不钳制/三原因/MD 回退）；外链 SHA-256 入库复验 + 同 sha 去重 + 路径净化；C 阶段 isochron×IDW²(Vint) 厚度链（凸包裁剪、删错误相化）+ 残差语义（10ms/边界/三类原因行/20井全表）；发布门（pdf_asset_id+sha256+残差完备+OUTPUT 登记+chip 禁用）；预览层（splitter 位置/井下拉/剖面标定/关联列+未决徽标）；文件夹导入后端（两阶段排序/软链跳过/行序对齐）+ 确认表 UI；ONNX 411×641 硬门 + 真服务接线。`5972466` `8a23d01` `0f02fc3` `781914e` `0b8de17` `6735dcf` `e399ddf` `5bb80bc` `4c0f575` `f084e28` `090ffa2`

- **2026-09-27 · 图层平台（wave/layer-platform）**：三编图页共用底座——LayerTreePanel（工具条/筛选/全量右键/层位灰显+缺源警示 indicator，objectName 兼容）、LayerPropertiesDialog（原生属性壳经 addPropertiesPageFactory 挂 Paleo 业务页 + QgsMapLayerStyleManager 预设/qml）、QgisLayerProfileService（QgsMapThemeCollection 页面档案 page:* + 声明驱动组匹配 + 悬空修剪 + setLayoutMapTheme）、LayerProfileBar（主题下拉/保存/管理）。壳接线 +61 行。ctest 75/75。`6834294`…`17f623e`
- **2026-09-27 · m2/mapping-pages 三编图页升级（wave/mapping-pages）**：基座（三页拆出 predictpage/constraintpage/composepage + panelshared + m1 兜底 qgis/qgislayerprofile + tst_mappingpages 桩，`e022f6b`）；预测页（预测类型/schema 动态表单 algoparamschema/PaleoTaskService 任务化运行/重跑幂等稳定 layerId/历史结果清单）；单因素页（singlefactordef 七因素注册表/生成链 04_SingleFactor/GDAL C API 等值线子组/factorstylewriter 色带 .qml/互斥单选上图/物源-展布-控制点类型化捕获 typedconstraintdrawcontroller/厚度样本折叠区）；智能编图页（融合只列栅格因素/矢量化自动进相界编辑态（派生 gpkg 只读→可写工作副本自愈）/相属性编辑回写 edit buffer/06_Reference 参考图区/导出版面钉 compose 主题 setLayoutMapTheme/布局设计器入口）；壳（showPage + chip 切换重应用页面档案 applyPageProfile + canvas 刷新；PageProfileTests 真实栈）。合并 `8840611` `40acf1e` `a92cab0` `6edeaa0`；ctest 串行 70/70（-j2 偶发 QSettings 竞态见上）。
- **2026-09-28 · 拓扑编辑（同层共边节点联动）**：`PaleoVertexTool` 增加拓扑模式——拖动重合节点同层全联动（含未选中要素）、双击共边向所有共享要素插点、右键删重合节点（逐要素守最小顶点数，任一破坏则整批拒绝）；释放点落在搜索半径内自动焊接到邻居节点（拓扑关系创建路径）。单 edit command 覆盖整批 → 一步原生 undo。开关 = 编辑条/ribbon「要素编辑」组新 checkable「拓扑」动作，镜像 `QgsProject::topologicalEditing`（随 .qgz 持久化），armed 工具实时跟随。**范围**：同层同 CRS；跨层联动与 QgsPointLocator 索引优化未做（当前全要素扫描，工区级数据量无碍）。tst_edittools +6 用例 36/36 绿；mapping-pages 合并带入的 15 个层标记缺失一并补齐（check_layering 绿）。
- **2026-09-28 · SBM 引擎 vendor 化（`vendor/sbm`，pinned `aae56c77`）**：上游 `Seismic-Body-Management` 的 `Data/Sgy`+`Engine`+`Render/ColorMap` 原样入库编成 `paleo_sbm` 静态库（-fno-char8_t 适配其 C++17 `.u8string()`；系统 libzstd + `SEISMIC_HAVE_ZSTD`）；`StorageProfile` 补 POSIX sysfs 分类；`domain/seismic` 16 个 sgy*/colormap 文件改一行转发头、删 15 个重复 .cpp——`sgyvolume`/`sgyindexcache` 的项目增强（TimeGridCache+mmap 并行时间片、`.sgyidx` 伴生文件）以补丁形式回到 vendor 副本（`vendor/sbm/PATCHES.md`）。`SeismicTaskService::startSliceExtraction` 现在优先走 `sdk::Dataset` facade（Backend::Auto，CancelToken↔PaleoTask 桥，条目锁守「单线程独占」契约，8 项 LRU），失败回落 volume 直读。11 套件 119 测试全绿；分层检查绿。
- **2026-09-28 · SBM 引擎入口接线 + 上游 POSIX 崩溃修复**：`startSectionExtraction` 改走 `sdk::Dataset::ReadSection`（useReadPlan 去重+扇区合并读，NearestTrace 模式；插值回落 legacy）；新增 `startWorkspaceTranscode`（engine `TranscodeJob`，可续跑/可取消，Auto 约定 workspaceBase=<sgy路径>，zstd 编码）；数据预览时间片页改走服务异步提取（120ms 防抖+仅贴最新）并挂「转码工作区」按钮——966MB 冻屏路径从 UI 线程同步读转为后台+随机访问后端。修复上游 `SgySequentialScan` POSIX 崩溃（queueDepth>1 时同步 buffer 未分配，读 nullptr；PATCHES P3）。13 套件全绿。
- **2026-09-28 · wave 分支全量并入 master（worktree 收口）**：`feature/seismic-3d-section`（含收尾 WIP `085e88e` 时间片网格缓存/相色预览/剖面色标 + `4fac323` agent-prompts m1/m2 文档）快进并入；`wave/ui-layer-separation` `84fc13b`（datapreviewtabs 冲突：保留 previewdoc 门面、WIP 工区图改 `m_doc->catalog()`）；`wave/layer-platform` `32a9e68`（净合入）；`wave/mapping-pages` `6ea9acc`（pagepanels 维持拆分形态、三页取 m2 实装版+`domain/arearules.h` 路径、qgislayerprofile 取 m1 实装——兜底 API 调用点收口为 `pinLayoutTheme()`、attachWorkflows/attachMapping 的 m2 增量平移入 `paleomainwindow_attach.cpp` 分段、tst_mappingpages 摘除 PageProfileTests〔兜底实现已退役，等价覆盖在 tst_layerplatform〕）。三 worktree（pw-layers/pw-mappages/pw-uilayer）+ 6 个 wave/* 本地分支已删；远程 `origin/wave/*` 引用未动。验证：paleo_core 全量编译 + 21 个相关 ctest 套件绿（tst_correlation_full 性能阈值首跑抖动 3041/3000ms 复跑过）。master 本地领先 origin/master 未推送。

- **2026-09-29 · 数据管理页操作重构（wave/data-page-operations，P3）**：多选框架（ExtendedSelection 三视图 + 选中徽标/信号 + 右键菜单矩阵 + 批量挂接/改型/软删回收/导出/批量预览 + 全选反选 + 刷新选择保持）；多维过滤（十维度条件 + AND/OR + chip 单删取反 + 未决快捷条计数徽标 + 标签 sidecar 体系/标签云/树尾标签分组 + 排序记忆 + 命名预设 + 高亮委托 + 10k<100ms + 空结果态 + paleo:// 状态串）；拖放（资产→井挂接/转移、→标签节点打标、外部文件→导入意图、非法目标红线+原因、多选拖带全部、dragLeave 清反馈）；实体面板（改名/坐标 override、版本时间线卡+两版清单 diff、实体↔资产拓扑图、实体 CRUD（删除=资产处置二选一）、角色编辑（不可撤销确认）、统计段、多选批量概要、会话操作历史）；命令栈（九类命令统一接口、深 50、相邻合并、Ctrl+Z/Y+操作名按钮+状态反馈、会话清栈）；键盘（Ctrl+K 命令面板（动作/资产/实体模糊检索）、命令注册表、快捷键表+冲突检测、Vim 可选导航、焦点强化）；视图形态（树/表/图标/高速（fetchMore 批 256，10k 单批<16ms）/分组四维 + 列配置持久化 + 列头漏斗 + 稳定多键排序）；导入（队列面板逐文件进度/取消/重试状态机、预设、SHA/同名查重处置、目录预估分批确认、摘要报告）；D9 分栏契约（新代码零 setSizes——源码扫描测试钉死 + tst_ui 三条宽度回归；列表最小宽 263px）。全部新码为头文件（纯逻辑 dataops/ + Q_OBJECT 平铺 pages/ 经 moc include 显式编入——零 CMakeLists 改动；moc include 加 __has_include 守卫适配 lint 门 configure-only 场景）。新增 69 测试函数（tst_panels 66 + tst_ui 3），ctest 88/88。递延项与壳接线缺口见 docs/dataops/GAPS.md（catalog 实体/资产/链接更新删除 API、状态栏/导入/队列 runner/预览区拖放宿主接线）。
- **2026-09-29 · 数据预览全面地图化 + 画布交互层（wave/preview-map-canvas，P2）**：全部可地图化资产预览统一到 PreviewMapPage 框架——`src/qgis` 新增五件（previewmapcanvas D1.1 私有层容器/CRS 钉死/视图历史栈/渲染状态/渐进 overlay、previewmaptools D1.2 七件套工具+量测节流、previewrasteranalysis 拉伸/5 色带/统计/直方图/剖面采样/极值、previewidentify 空间索引缓存+栅格双线性、previewrendercache LRU+QTemporaryDir）+ `src/ui/datapreview` 新增六件（组合页/TOC/identify 面板+全表/剖面图/直方图/状态页+会话记忆）。资产覆盖：horizon（等值线 D2.1/拉伸 D2.2/直方图 D2.3/版本切换 D2.9/统计/极值/剖面 D5.1-5.8）、geojson（图例 D2.4/identify D2.5/标注开关 D2.6）、image（world file 探测+配准对搬临时目录 D2.7/大图提示 D2.11）、well_head/DC.dat 井位落图（D2.6/D2.8）、未知类型统一不支持态（D2.12）、同目录叠加（D2.10）；survey 全景框架化（objectName 兼容面全保）。**决策记录**：①工具析构一律不碰橡皮带（canvas scene 先死→悬空 SIGSEGV）；②~PreviewMapCanvas 不调 unsetMapTool（QCursor 在平台拆除后构造即 qFatal）；③工具条弹出按钮不用 QToolBar::addWidget（QWidgetAction 销毁序悬空，改扩展条直挂）；④量测 press/finish 离散帧不节流（首帧被 30Hz 节流误杀是真 bug）；⑤TOC 加层不落记忆、移除单删（增量加层期间整表重写会清掉未加层记忆）；⑥托管副本旁无 world file 时从源目录成对搬临时目录（GDAL 只认数据文件旁的边车）。**递延**：D3.12 触摸板 pinch zoom（offscreen 不可测，QGIS 原生手势路径待真机验证）；等值线生成可改延后（当前同步 235ms 首开含 GDAL 链，可接受）。净增实现 ~5900 行 + 测试 ~2430 行（6 个新测试套件 103 函数）；ctest 94/94 绿 + layering 绿；seismic 分支函数体未动（P5 领地）。

## wave/wellcomposite-deep 决策记录（2026-09-29）

P1 单井综合柱状图深度升级（D1–D8 全量交付）。逐项决策与递延：

- **分层接缝裁决**：ui→io include 被护栏白名单挡死且词表只读 → 派生 XML
  写回（`io::writeComprehensiveWellXml*`）、井斜/时深表解析落 io 层由测试
  直驱全链路；运行时面板发 `derivedDocumentReady(doc, 摘要)` 意图信号，
  壳接 catalog DERIVED 版本落盘——~~递延：壳侧接线~~ 已落地（wave/deepen-perf
  D1：`WellCompositeDerivedSink` + attach 分段 + 组装根 io 注入）。
  sidecar/会话持久化以视图层存储助手
  （`wellcompositestore`，QtCore 文件 IO）落地——**递延**：迁移 services
  门面（届时 ui 白名单只需放行新门面头）。
- **井斜/时深运行时数据路径**：`ComprehensiveWellData`（domain，冻结不动）
  无井斜/时深字段 → io 解析函数产出独立类型；运行时注入走
  `DepthTransform` API（壳从资产解析后喂面板）——~~递延：壳把
  `parseDeviationSurvey/parseTimeDepthTable` 接进装配链~~ 已落地
  （wave/deepen-perf D1：sink `setDepthTableParsers` 装配期注入，
  装载完成自动喂表）。
- **D1.12/D2.9 合并**：单画布内多道天然共享深度轴（标尺道即坐标源）；
  「Y 缩放联动开关」语义落位多画布锁步（MultiWellView::setLinkScroll）。
- **D5.4 datum 校平语义**：各井滚动使同名标志层同屏高（视口 40%），
  深度重映射（warp）未做——correlation 工作流下拉平已够用；真 warp
  需渲染管线深度函数化，**递延**。
- **D4.7 SVG**：QSvgGenerator 可用已交付；SVG 档用固定 8px/m 简化比例
  （矢量无损缩放，比例尺语义由 PDF/PNG 承担）。
- **D4.8 打印对话框**：~~原生 QPrintDialog 接线递延~~ 已落地（wave/deepen-perf D3）：
  `exportToPagedDevice(QPagedPaintDevice&)` 共用管线（QPdfWriter/QPrinter 同源，
  设备 dpi 换算比例尺）+ `nativePrintAvailable()` 探测；无打印环境降级 PDF
  并如实告知（offscreen 测试直驱打印管线）。
- **D7.3 暗色**：柱状图画布保持纸面白底（DESIGN.md 2026-09-29 翻案条的
  wellcomposite 豁免），面板/对话框 chrome 已随主题 token；道内数据符号
  色不跟随（数据符号语义）。
- **测试沙箱坑**：仓库有便携 QSettings 路径
  （`~/.local/share/paleo/profiles/default/…/paleo.ini`）绕过 XDG env——
  直跑测试二进制会跨进程污染会话记忆；ctest 沙箱不受影响。测试内用
  每测独立 projectName 隔离（tst_wellcomposite_visual 各导出用例）。
- **隐藏画布几何坑**：未 show 的画布 body 无真实几何 → D2.8 视口跨度
  钳制以「bodyH ≥ 80px 才可信」守卫，否则 30px 假几何会把缩放因子反压
  到 0.4×（tst_wellcomposite 既有比例尺联动测试由此保绿）。
- **QLatin1String 中文坑**：CJK UTF-8 字面量经 QLatin1String 解释为
  Latin-1 乱码（chronostrat/patterncatalog 曾中招）——中文字面量一律
  QStringLiteral 或 QString::fromUtf8。

- **2026-09-29 · IO/服务层性能与缓存体系（wave/io-perf-cache P4）递延**（2026-09-30 wave/deepen-perf 部分对账）：预算治理只挡 include 层，「不带 include 直接 new」的大缓冲挡不住（与分层护栏同一遗留口径，后续可引入分配钩子审计）；~~Pyramid 消费侧~~ 已落地（B3：导入 Lazy ensure + GDAL .ovr + 预览预热，4096² 读块 14ms→4ms）；D7.8 网络盘超时只有 slow-path 探测设计位（见 docs/perf/BENCHMARKS.md），NFS 自动降级等真实工区再实装；~~catalog.sqlite 触发条件~~ 已评估未达（B4：查询面 10k→100k 零劣化，见上条记档）；~~SEG-Y 坏道跳过~~ 固定步长布局已放宽对齐并行语义（B6），变道长布局保持整索引报错（契约表 docs/perf/INDEX_FORMAT.md §3）。

- **2026-09-29 · P5 地震链路升级（wave/seismic-chain-deep）**：Phase 0–7 全量交付。Phase 0 架构账本+220MB 生产形状体基线实测（`docs/seismic/ARCHITECTURE.md`/`BASELINE.md`，冷索引 146ms/切片 27ms/sf3c 转码 2.3s/fps 15000/峰值 RSS 686MiB）；Phase 1 转码 D1.1–D1.10（分阶段加权进度+ETA、断点续跑 UI 三态探测、vendor P6 并行分片编码池≤4〔修复关队竞态〕、质量报告〔道数/覆盖率/丢弃率/值域/坏道样〕、meta 版本探测与重建、同输出互斥、vendor P8 Auto 只认完整 meta 堵假完成态、sf3p 金字塔层数按体量自适应〔<64MiB 无 LOD/≥16GiB L3〕、PALEO-SEISMIC-TRANSCODE 结构化 JSON 日志）；Phase 2 剖面 D2.1–D2.14（切片纹理 LRU≤4、密度/wiggle/混合三模、阈值+极性、AGC+手动增益曲线、TWT+深度双刻度、LOD 抽稀 2.2ms/帧、纵向拉伸、8 档色标+反转、PNG 导出、相邻线卷帘、240B 道头卡〔`readTraceHeader`〕、书签 QSettings 按体持久化、复制/打印、空数据原因态）；Phase 3 三维 D3.1–D3.12（16 层切片堆叠体渲染〔拖动降 4 层〕、切片面拾取拖拽联动 2D、透明度 uniform+值域 discard、井轨迹+标志层十字、colormap 编辑器〔CPU 重着色路径，修复 rgba 字节级写入〕、相机书签、截图、内存预算提示〔体>RAM/2〕、GL 3s 看门狗→2D 拼接回退件、fps 读数、惯性旋转、多体轮廓）；Phase 4 解释 D4.1–D4.10（画布拾取/断层模式、Pearson 互相关追踪〔修复两个算法 bug：候选窗列跨步、搜索窗钳体积界〕、拾取→DERIVED 层位/断层资产→catalog 登记〔父版本=地震 RAW〕、列表面板〔定位/删除/重命名/CSV〕、QUndoStack undo/redo、IDW 网格化、`<sgy>.seispicks.json` 会话伴生文件自动保存、多解释者名册、置信度红黄绿着色；解释模型落服务层裁决：视图 io/* 白名单仅 lasdoc.h）；Phase 5 井震 D5.1–D5.7（任意线节点表编辑器、服务层任意线 LRU≤4〔51ms→0ms〕、井顶/底独立投影斜井轨迹、AC+DEN→Ricker 合成记录〔缺曲线降级注记〕、沿井分层标注〔既有〕、井旁道 wiggle 小图、最近 N 井过滤）；Phase 6 D6.1–D6.8（切片时延预算入基线〔miss 27ms<500/hit 26ms<50〕、3D LOD ≥15fps、内存治理自建同形接口〔P4 管理器不存在；合并点=统一管理器落地后委托〕、SeismicConcurrencyGate 4 槽信号量并发闸〔全部 12 个 start* 走闸；实测 8 任务最大并发 4；shared_ptr 防析构竞态〕、取消全链路〔短操作补边界检查+排队即取消跳过〕、错误五级分类、会话/转码自动保存点）；Phase 7 文档 6 份（ARCHITECTURE/BASELINE/TRANSCODE/SECTION/3D/INTERPRETATION）+ vendor PATCHES.md 补丁 P6–P9 登记。新增测试 6 套 71 用例（transcode 13/sectionui 16/3dui 8/interpret 11/welltie 7/budgets 9 + baseline 9 实测）；ctest -R 'seismic|datapreview|layering' 18/18 绿。**递延**：catalog 注入 `setInterpretationCatalog` 待 app 层接线（paleomainwindow 非 P5 领地）；IDW 与 ConstraintIDW 合并点；meta 真迁移工具（版本真升级时）；P4 内存管理器合并。

- **2026-09-30 · 域深化 + 性能完善（wave/deepen-perf，四轨并行 + lead 集成）**：A 地震链路——ReadVoxelWindow 消费接线（3D 堆叠取数 16 请求→1）、拖动链路 supersede 取消+脏槽位精化+350ms 自动升层（手势请求 5→3）、时间切片失败原因态+同路径在途取代取消、966MiB 真工区复测刷新（BASELINE §8）；TODOS P2「SBM Engine 剩余入口」关闭。B IO/缓存——大 LAS 解阻（59MB 调用点 442ms→0ms）、导入队列真进度+生产 runner（GAPS G-2.3 收口）、金字塔消费侧（.ovr，4096² 读块 14→4ms）、catalog 100k 评估记档（查询零劣化）+ mutator 超线性发现、QSettings -j2 竞态沙箱根治核实、SEG-Y 坏道跳过放宽。C 编图——ConstraintIDW break_line 屏障/direction_line 各向异性（逐位向后兼容）、相界 fault_cut 单类型跑通（词面 4 类冻结）、表 K.1 十二类探井符号、paleoAssetId 关联激活、paleo_distance_transform 绕障距离引擎（welldist 实装）。D 井综合——WellCompositeDerivedSink 派生登记+深度装配（壳接线+组装根注入）、连井剖面生命周期 9 用例穷举（发现并修 SeismicSectionTool 析构悬空）、打印原生管线（QPagedPaintDevice 共用+降级 PDF）、简化 composer 评估记档不建、D5.4 datum 校平修复。lead 集成 7 处接线 + tst_wellcomposite_visual 钉死渲染环境重生成 golden。新增测试 54 函数（7 套新 + 多套件扩展）；详见 docs/progress/deepen-perf.md（含语义决策与递延清单）。
- **2026-10-01 · 地震属性引擎（goal/seismic-attributes-20261001）**：属性核函数库 `src/algorithms/seismicattr`（自研 radix-2 FFT + 镜像填充频域 Hilbert；瞬时族/时窗振幅族/semblance C2 相干〔IL/XL 半窗分设〕/甜点/瞬时 Q 原型；有限道 Hilbert 折点尾效应定量 ~1/d）；`SeismicTaskService.startAttributeSlice` 任务编排（≤4 并发闸、读/算两段单调进度、三检查点协作取消、逐道 ≤4 线程分片、Time 切片如实拒绝、稀疏测网邻线按轴值表解析）+ SATR 派生资产登记；剖面画布属性叠加层（NaN=透明/几何失配防线）+ SeismicAttrPanel + dock「◈ 属性」闭环；966MB 真机实测全属性 <100ms（包络 36ms/相干 78ms/IL 基线 14ms）。新增测试 4 套 30 用例；详见 docs/progress/seismic-attributes.md。
- **2026-10-02 · 时深转换与速度建模（goal/time-depth-velocity-20261002）**：速度模型核 `src/algorithms/velocitymodel`（层间平均=插值语义锚点位级精确不外推 / V0-k 线性速度函数=TWT 域 2 参数 Gauss-Newton+闭合式可外推；power-2 IDW 空间查询命中即取；JSON 序列化无时间戳幂等；结点二分）；`DepthConversionWorkflow` 编排（catalog tops/time_depth/well_head 关联收集→velocity_model DERIVED 存档→层位时间栅格→depth_raster DERIVED+`depth.<H>` 声明，测网号域透传）；层树右键「转换为深度域…」意图信号+壳接线；剖面左缘深度标尺反投影修正（去常速近似）。真机实测：20 井 9666 结点建模 107ms、层位面 60ms/面（4.3 Mcells/s）、8 面共 489ms、剖面深度轴逐样 298ms。新增测试 4 处 35 用例；详见 docs/progress/time-depth.md。**递延**：TVDSS/KB 基准换算（井位表有 KB 列待确认口径）；V0-k 层段化/三参数与模型对比编辑 UI；时深转换对话框（模型选择/覆盖预览）；地震体整体时深转换（深部重采样）单独立项。

- **已知阻塞：paleo_ui 全量编译缺 QScintilla 头**（2026-10-03 方向20 轮2 撞出，环境问题非代码问题）：`include/qgis/qgscodeeditor.h(30)` 新引入 `#include <Qsci/qsciapis.h>`（该头于 2026-10-02 00:18 随 QGIS 更新加入），而 QScintilla 头只在 `C:/deps/qscintilla-install/include`，`CMakeLists.txt` 与 `cmake/*.cmake` **没有 QScintilla 接线**——`CMAKE_PREFIX_PATH` 含该前缀但不会变成编译期 `-I`。症状是 `layerpropertiesdialog.cpp` 等与 QGIS 代码编辑器无关的 TU 报 C1083。**待定方案**：(a) 给 QGIS 依赖补 QScintilla include/lib 接线；(b) 在不需要代码编辑器的 TU 上断开 `qgscodeeditor.h` 的传递包含。修前 `paleo_ui` 无法全量链接，既有 worktree 靠陈旧 obj 躲过。另：新 worktree **首次** configure 会漏 4 条 `vendor/` include（glm/saribbon/sbm/segyio），**再 configure 一次即恢复**。
