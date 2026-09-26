# TODOS — paleo_workstation

## P3 — catalog.sqlite 查询索引（deferred from /autoplan SELECTIVE EXPANSION, 2026-09-25）

- **What:** 由 `catalog.json` 重建 `catalog.sqlite`，作为资产、版本、关联的查询索引。
- **Why:** ADR 0056 把 sqlite 定义为可重建索引，避免打开工程时扫 JSON。
- **Pros:** 资产变多后列表和校验不用每次解析整份 catalog。
- **Cons:** 20 口井的第一段用 JSON 就够；提前做会多一个必须和 catalog.json 对齐的存储。
- **Context:** `docs/PROJECT_AREA_PLAN.md` 第 3 节。触发条件：资产数量或列表查询变慢。
- **Effort:** human: M / CC: S
- **Priority:** P3
- **Depends on:** catalog.json 受管 RAW 已能往返


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
- **Context:** 文档 §14–15 目前把相界当普通 polygon 拓扑处理。先做单一"相界线"类型跑通，再扩类型。
- **Effort:** human: L / CC: M
- **Priority:** P2
- **Depends on:** P0 矢量编辑落地

## P1 — 工程评审待办（from outside-review findings, 2026-09-25）

以下项已由 CEO 评审确认纳入计划，但**细节规格属工程评审范畴**：

- **保存/发布语义状态机**：`保存版本` / `发布`（result/ 快照）/ `Published` 状态三者关系；发布后能否再编辑；工作副本与已发布快照的发散规则。（§41.7）
- **native:* 算法逐项审计**：§10–12 所列每个算法在 native provider 下是否有 C++ 实现；缺的（raster contour、距井距离/hub 类——注意 `qgis:*` provider 也是 Python）逐一指定 GDAL C++ 替代或直接自研。约 6–10 个 GDAL 封装算法的工作量待评估。
- **算法测试框架自建**：QGIS 官方算法测试基座（AlgorithmsTestBase）是 Python/YAML，C++-only 下不可用；需自写 C++ 测试 harness（§33 修正）。
- **schema 迁移策略**：metadata/project.sqlite + project.gpkg + .qgz + catalog.json 四类文件的版本化与迁移方案——产品核心价值是长生命周期版本数据，此项不可缺。catalog.json 已先落地 `schema_version` 校验 + `.bak` 轮转（autoplan pass 2），迁移方案需一并覆盖；另记：两实例同开一工程的并发写未规定（单机桌面场景，随本项一并评估）。
- **崩溃报告机制**：本地转储 vs 回传，实现期决定（§38）。
- **速度模型 + line-geometry↔CDP 映射**：depth↔TWT 转换与 map↔地震剖面定位的两个新建依赖（§40 SeismicMapLink）。
- **性能测试**：§33 目前无性能用例——画布刷新/工程打开预算（§41.6 NFR）需要可执行测试。
- **渲染对比测试稳定性**：需 vendor 字体 + 钉 Qt 光栅化版本，否则跨平台噪点失败。
- **合成最小 SEG-Y fixture**：golden fixture 工区的地震数据用合成文件，真地震数据太重。
- **多 CRS 假设核查**：§17 MapContext 假设单一 CRS；真实项目可能混合地震工区 CRS 与地图基准，需工程评审确认。

## P1 — Phase 0 spike 交付物（§39, E3）

- vendor superbuild 骨架 + 依赖清单（含裁剪开关 WITH_3D/WITH_MESH/WITH_PDAL 决定 + Qt LGPL 动态链接条目）。
- 目标平台矩阵（OS × 架构）钉定。
- AI 推理运行时选型输出（ONNX Runtime / libtorch / 其他）。
- app-only 功能审计清单（qgis_gui vs src/app 逐能力标注）。

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

- **人工验收清单（非代码）**：真机上点「Web 服务」dock 验证 QWebEngineView GPU/沙箱路径；真数据文件夹导入的确认对话框手测一遍；首个 966MB SEG-Y 预览的实际冻结时长（决定是否排期异步 IO）。触发条件：下次真机启动。Effort: S / Priority: P2
- **第二工区参数化接缝**：分类器中文目录名规则、「8 层序界面」名单、SEG-Y 按道号索引的 base 约定（field-record 字 = 冻结 inlineMin）、411×641 ONNX 门——均为本工区钉死值。第二工区接入前需把「每测区参数」与「全局规则」分开定义（分类器规则数据化）。触发条件：接入第二个工区。Effort: M / Priority: P3
- **「重新定位文件」恢复路径**：外链源文件移动后「找不到源文件」是死胡同——需 re-pick 路径 + 重流式 SHA-256 + 不一致拒解的入口。触发条件：用户遇到外链文件移动或主动要求。Effort: S / Priority: P3
- **Onto 层位/边界文件的命名规范**：文件名不在 8 个层序界面时产未决层位实体、不进编图 chip——属已交付行为；遇新层序命名需扩名单，随第二工区一并处理。触发条件：新层序命名。Effort: S / Priority: P4

## Completed

- **2026-09-26 · project_area 数据底座 + D61 编图链**（p1/p2 双包并入 master）：catalog 实体/资产/版本/显式关联 + SHA-256 受管 RAW；分类器与井口/分层/时深解析；D61 装箱时间栅格；SEG-Y 道索引单测线解码；9 类数据页预览；读侧 facade、D61→D62 厚度→凸包约束 IDW→相多边形；TD 残差验证、三视图联动、PDF 导出、8 层位 chip、版本状态机。`424e185` `f3d9b83` `c994d21`
- **2026-09-26 · 集成接缝 + 清单读错误诚实化**：ComposePage 经 `layerDeclared` 信号跟随新声明；生产路径全改 `tryDeclared`/错误通道（清单损坏不再被当成空清单）。`fdd2f99` `217502a`
- **2026-09-26 · 文档内嵌 PDF 预览**：document 资产原件恒为 RAW 规范源；office 格式首预览时经 soffice headless 懒转受管 DERIVED PDF（父版本=RAW）以 QtPdf 渲染；无转换器/失败如实降级为「用系统程序打开」。`6dbeec7`
- **2026-09-26 · WebEngine 嵌壳组件**：`WebViewPanel` 懒加载孤岛（offscreen/渲染进程终止降级为外部浏览器兜底）+ `AA_ShareOpenGLContexts`。`96ba726`（宿主入口见 goal/webui-host）
- **2026-09-26 · 井文件解析 BOM 剥离**：三个入口统一去 U+FEFF（trimmed() 不去它），plan §3 要求。
- **2026-09-27 · autoplan pass-2 Wave-1（四分支 rebase 至 `ce746b3` 并入 master）**：catalog 耐久性（T17 ver-N/ast-N 序号恢复与拒绝重发、T20 `.bak` 轮转+失败后拒写+`catalogOpenFailed` 信号、T33 `BatchSave` 批次+`unresolvedLinks()`+装载期坏段跳过+TOCTOU canon 复核）；映射/验证（T18 TD `-99999`/非有限过滤、T19 isochron≤0/非有限→nodata、T21 P1/P2/P3 几何校验+`PALEO_INLINE_*`/`PALEO_DT_MS`/`PALEO_T0_MS` 元数据、T24 残差表双击导航、T25 top-XY 采样+`RASTER_MISSING`）；文件夹确认 UI（T22 分类器词表/HZ28 锁/逐行重试/CRS 句/D3 第四计数/D5 生效类型重排序）；T30 退役 `SeismicPreviewPanel`。验证：ctest 53/53 + 真数据 smoke（`PALEO_REAL_PROJECT_AREA`）双绿。`b52114e` `0e4010c` `f4b7335` `1af8d1e` `61cdb0d`
- **2026-09-26 · /autoplan 评审修复 F1–F8（九分支并入 master）**：画布绑定工程 + `m_instances` 悬空清理 + 无基准 ENGCRS 替换 eqc + 打开失败对话框；TimeDepthTool 重写（文件序/不钳制/三原因/MD 回退）；外链 SHA-256 入库复验 + 同 sha 去重 + 路径净化；C 阶段 isochron×IDW²(Vint) 厚度链（凸包裁剪、删错误相化）+ 残差语义（10ms/边界/三类原因行/20井全表）；发布门（pdf_asset_id+sha256+残差完备+OUTPUT 登记+chip 禁用）；预览层（splitter 位置/井下拉/剖面标定/关联列+未决徽标）；文件夹导入后端（两阶段排序/软链跳过/行序对齐）+ 确认表 UI；ONNX 411×641 硬门 + 真服务接线。`5972466` `8a23d01` `0f02fc3` `781914e` `0b8de17` `6735dcf` `e399ddf` `5bb80bc` `4c0f575` `f084e28` `090ffa2`

