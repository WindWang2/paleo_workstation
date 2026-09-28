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

## P3 — UI 分层收口递延（from /autoplan docs/UI_LAYER_PLAN.md, 2026-09-27）

- **clang-tidy include-order CI**：分层检查器只管方向不管序；include 排序规范化递延。触发条件：分层落地后代码风格再收一轮。Effort: S / Priority: P3
- **`DataImportService` using 别名删除**：`FolderPreviewRow`/`FolderRowResult` 解嵌套后保留源码兼容别名一期（保护 tst_import 22 处用点）；二期删除别名、调用点全改 `domain/importrows.h`。触发条件：W2 落地后的下个迭代。Effort: S / Priority: P3 / Depends on: UI_LAYER_PLAN W2
- **大 LAS 同步 `lasAt` 的 UI 线程延迟悬崖**：correlation 侧按路径同步解析保留现状 UX；大文件会阻塞 GUI 线程（现状已存在，分层不恶化）。触发条件：实测大 LAS 连井剖面卡顿。Effort: M / Priority: P3
- **文件夹导入扫描期进度 UX**：本轮只定「忙碌光标 + 状态栏一行」契约；真进度条（文件计数/ETA）递延。触发条件：大文件夹导入实测等待过长。Effort: S / Priority: P3
- **include 级护栏的调用级补强**：单一 `paleo_core` 静态库下 `check_layering.py` 只挡 include 挡不住「不带 include 直接 new」；若要挡需 clang 插件或拆库。触发条件：发现绕过 include 的违规实例。Effort: M / Priority: P4
- **图层平台 · 旧组名词表迁移**：`workflows.cpp` 产层仍用 `01_Prediction`/`02_Constraints`/`03_Predict`/`03_Composite`/`00_Data`，页面档案表（`QgisLayerProfileService`）按 canonical 词表（`02_Prediction`/`03_Constraints`/`05_PaleoMap`/`07_Validation`…）匹配，旧组名层在档案应用时按表外隐藏。触发条件：编图链产出层迁移到 canonical 组。Effort: S / Priority: P2
- **图层平台 · 主题重命名**：QgsMapThemeCollection 无 rename API，档案工具条管理对话框已注记「重命名暂未支持」。触发条件：QGIS 提供 rename 或 `QgisLayerProfileService` 增加记录复制通道。Effort: S / Priority: P3
- **图层平台 · layerId↔assetId 关联面**：`LayerDeclaration` 无 asset 字段，属性对话框业务页关联资产恒「未关联」（按钮禁用+reason tooltip）；`LayerPropertiesDialog::assetIdForLayer()` 预留单点扩展。触发条件：catalog 增图层-资产显式关联。Effort: M / Priority: P2
- **图层平台 · 图层创建时间**：manifest 无时间戳，业务页恒「—」。触发条件：layer_declarations 表加 created_at（schema 变更需评审）。Effort: S / Priority: P3
- **图层平台 · 「删除选中」语义**：QGIS 默认动作只摘树节点不 `removeMapLayer`；若需「删树即删层」，壳侧补工程注销接线。触发条件：用户实测困惑。Effort: S / Priority: P3

## P3 — m2/mapping-pages 递延（wave/mapping-pages, 2026-09-27）

- **置信度伴生栅格**：算法侧无真实置信度输出（ONNX 仅读首个输出张量、paleo:\* 均确定性单输出栅格）——不造假数据；接入点已留（`PredictionWorkflow::confidenceCompanionAvailable()` 恒 false + 声明位）。触发条件：出现带置信度/方差输出的算法。Effort: S / Priority: P3
- **非 IDW 单因素引擎**：welldist（距离变换）/confidence（预测结果直取）/strathick（`paleo:paleo_isopach` 双栅格链）v1 统一走井点 IDW，注册表 algorithm 标签已注「待接入」。触发条件：对应资产链就绪。Effort: M / Priority: P3
- **PaleoEditingToolbar `mEditLayer` 裸指针**：编辑会话开着时切换/新建工程，层被 `QgsProject::clear` 删除后工具条悬空（智能编图自动编辑态使该路径更易触达；真实栈测试复现过 SIGSEGV，测试侧已规避）。edittools/ 属 z3 禁碰区未改。触发条件：编辑会话 × 工程切换实测。Effort: S / Priority: P2
- **ctest -j2 跨二进制 QSettings 竞态**：多测试二进制共享落盘 `paleo/paleo` 配置，`-j2` 下 `lastPage` 读写交错偶发 `tst_ui::windowStateAndExtentPersist` 红、串行全绿。触发条件：并行 ctest 再现。Effort: S / Priority: P4
- **native processing provider 注册**：C++ 嵌入运行时 Processing 注册表仅 `paleo:\*`（`gdal:contour` 属 Python provider）；等值线已走 GDAL C API（gdal:contour 同一底层引擎）交付，native provider 按需引入。Effort: M / Priority: P4
- **SBM Engine 剩余入口**：`QuickOpen` 秒级首屏预览、`ReadTimeSliceTiled` 瓦片渐进发布、渐进 LOD（progressiveLod + `SetActiveLod`）、`ReadVoxelWindow` 三维窗口取数。已接：sdk::Dataset 切片/剖面路由、TranscodeJob（预览页「转码工作区」按钮）。触发条件：工区实测瓶颈或交互预算超限。Effort: M–L / Priority: P2
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
