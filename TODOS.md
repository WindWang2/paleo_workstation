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

以下项已由 CEO 评审确认纳入计划。**2026-09-27 对账：全部落地。**

- ~~**保存/发布语义状态机**~~ — 已落地：`docs/VERSION_PUBLISH_STATE_MACHINE.md`（wave-3 derived-publish `72dc070`）。
- ~~**native:* 算法逐项审计**~~ — 已落地：`docs/ALGORITHM_AUDIT.md`（wave-3 model-hardening `82dff27`）。
- ~~**算法测试框架自建**~~ — 已落地：`tests/tst_algorithm_harness` + `tests/algorithmbase.h`（wave-3，`82dff27`）。
- ~~**schema 迁移策略**~~ — 已落地：`docs/SCHEMA_MIGRATION.md` + catalog `schema_version` 校验 + `.bak` 轮转；同工程双实例并发写经 `ProjectDirLock`（PR #12 `c908e1e`）。
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

- **人工验收清单（非代码）**：真机上点「Web 服务」dock 验证 QWebEngineView GPU/沙箱路径；真数据文件夹导入的确认对话框手测一遍；首个 966MB SEG-Y 预览的实际冻结时长（决定是否排期异步 IO）。触发条件：下次真机启动。Effort: S / Priority: P2
- ~~**「重新定位文件」恢复路径**~~ — 已落地：`relocateVersionSource`（流式 SHA-256 复验、不一致拒解、同 SHA 追加外链版本）+ 预览「重新定位文件…」入口（wave-4 `2c1c8e1`）。
- **第二工区参数化接缝**：~~外置 seam~~ 已落地——`AreaRules`（层序名单/分类器目录规则/SEG-Y 四偏移/ONNX 网格门，经 `project_area.json` 覆盖，默认=本工区值；`docs/AREA_PARAMETERS.md`，wave-4 `8fc7bb2`）。**遗留**：真接第二个工区时按该文档走通一遍验证 seam 完备性。触发条件：接入第二个工区。Effort: M / Priority: P3
- **Onto 层位/边界文件的命名规范**：文件名不在 8 个层序界面时产未决层位实体、不进编图 chip——属已交付行为；名单经 `AreaRules.sequenceBoundaries` 可配（wave-4），命名规范文档化随第二工区处理。触发条件：新层序命名。Effort: S / Priority: P4

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

