# 方向 05（goal7）R0 基线：QGIS 图层体系、样式制图与既有地图版式能力深化

- 分支：`goal7/qgis-layers-layout-20261014`（worktree `../paleo-wt-goal7-qgis-layers`）
- 基线 SHA：`b0520429c21f858a1c053b876f511680088564a9`（= 执行时 origin/master 实测 HEAD，与任务书核查基线一致）
- 盘点时间：2026-10-14；Open PR 实测仅 #437；Open Issues 实测 #352–#421（70 项）
- 审计分包：S1 图层生命周期 / S2 符号样式 / S3 版式输出 / S4 渲染性能与 QA 基建（四份实现级审计原文见本目录 `05-baseline-{layers,symbols,layout,perf}.md`，所有论断带 file:line 证据）

## 1. 写权限边界（双重约束核定结果）

本方向白名单（任务书）∩ 排除 PR #437 指纹后的**实际可写集**：

| 可写 | 说明 |
|---|---|
| `src/qgis/` 下除以下 5 个保护文件外的实现 | 保护：`qgisprojectservice.cpp`、`qgisconstrainteditsession.{h,cpp}`、`evidencemapadapter.{h,cpp}`、`mappingartifactwriter.cpp`、`mappingfeatureidentity.{h,cpp}`（PR #437） |
| `src/ui/layers/`、`src/ui/edittools/`、`src/ui/maptools/`、`src/ui/layout/`、`src/ui/symbols/` | 全部可写 |
| `resources/geology/` | 全部可写 |
| `tests/tst_layer*.cpp`、`tests/tst_layout*.cpp`、`tests/tst_symbol*.cpp` | 已全部注册进 CMake（无需动 CMakeLists.txt） |

**关键布线结论**：`CMakeLists.txt` 在 PR #437 文件清单内 → 本方向**不新增测试二进制、不注册新 extra-*.cmake**（后者必须改 CMakeLists.txt 的 `_paleo_extra_manifest`，双重禁止）；全部新回归用例落在上述已注册的 tst_ 文件内新增函数。

**只读但可引用**：`src/ui/paleomainwindow*.cpp`（其中 `paleomainwindow.h`/`_attach_mapping`/`_workbench` 为 #437 保护）、`src/workflow/*`（mappingworkbench*/mapversioncontroller* 为 #437 保护）、`src/metadata/*`、`layoutdesignershell.cpp`、`src/ui/datapreview/*`、`src/ui/decorations/*`。

**Issue 指纹排除（只标注不修复）**：#352（协克里金工作线程调 instantiate）、#355（QgisCanvasController 全局 s_guards）、#369（图层树栅格面运算右键菜单）、#376（预览 grab 落盘，基线已修为 300ms settle 去抖）、#419（src/qgis 五项低危，含 qgislayerservice.cpp removeDeclarations 空 id 守卫）、#409/#410（测试覆盖缺口认领）。S4 复核确认 #419/#355/#376 涉及的三处在当前基线已有修复形态，归属仍归 Issue Agent，本方向不重复做。

## 2. 已有能力（代码实证，详见四份分包审计）

- **声明式图层链**：`QgisLayerService::declare/instantiate/setActiveHorizon`（qgislayerservice.cpp:72-488）+ `LayerManifest`（sqlite `layer_declarations`）为层集权威；`.qgz` 只持久化实例化层 + 内嵌声明副本（meta sqlite 存在时内嵌副本只读兜底）；保存时 manifest 读失败或内嵌失败 = 整体写失败（qgisprojectservice.cpp:339-354，#437 保护文件，只读引用）。
- **树归位**：`QgisLayerOrganizer` 三分区（00_Data 置顶共享 / 层位组按浅→深序 / 平铺区底图垫底），clone+insert+remove 先插后摘；用户拖拽会话内保持、重开收敛 canonical（qgislayerorganizer.cpp:64-74,217-318,396-421；tst_layerorganizer 钉死）。
- **页面档案**：`QgisLayerProfileService` 包装 `QgsMapThemeCollection`（page:* 前缀），首建按档案表摆树 + 定格，重放走 `applyPrunedTheme`（修剪悬空记录 + applyTheme）；00_Data 共享层强制可见（qgislayerprofile.cpp:105-323）；renameTheme 走原生 renameMapTheme + page:* 硬拒（:362-378）。
- **样式/符号**：`FactorStyleWriter` 色带 .qml + 等值线双描边（白 0.55mm/墨 0.25mm）+ 注记规格（JetBrains Mono 9pt、白缓冲 0.6mm、60mm 重复、<10mm 不注记、两位小数、level/ELEV 双识别）与 DESIGN.md 零漂移（factorstylewriter.h:39-44, .cpp:158-179；tst_factorworkflow:551-562 钉）；`GeoPatterns` 词表 14 岩性/8 相/8 线型 + qrc 资源对账 0 缺失；井别 16 类符号数据字段驱动（qgisstyleservice.cpp:757-788）；标注 z 序 `labelsWithLayer` 依赖 vendored QGIS 补丁（已知设计）。
- **版式/输出**：`PaleoLayoutExport::exportLayout` 三格式唯一管线（页选 trimmedClone、失败中文错误串、缺层 warnings 独立呈现）；模板 .qpt save/load + 内置五纸型 + 内容模板骨架；地图册逐格/蒙太奇/索引 + 落盘回读校验 + manifest 账实一致；图签块（编制/审核/日期）已交付；.qgz/.qpt round-trip 有测试钉。
- **性能/QA 基建**：无 QgsRenderChecker，golden = 唯一 wellcomposite 抽样对拍（PALEO_UPDATE_GOLDEN 只写构建目录）；`pinRenderEnvironment` 钉字体/palette（16 套件复用）；offscreen dpr=1 基线有断言；perf 标签测试 20+（RUN_SERIAL 统一）。

## 3. 候选工作项（经主 Agent 逐条代码复核；优先级排序）

| # | 域 | 发现 | 位置（全部白名单内） | 规模 |
|---|---|---|---|---|
| W1 | A/B | page:* 主题重放把「主题创建后新增的无声明临时图层」置隐藏，与 qgislayerprofile.h:26-29 头注释契约相悖（stageTreeVisibility 首建路径跳过它们，applyPrunedTheme 重放路径无豁免） | `src/qgis/qgislayerprofile.cpp` + `tests/tst_layerplatform.cpp` | M |
| W2 | F | `LayerTreePanel::refreshIndicators` 对每个图层节点循环内全表 `declared()`（sqlite readAll × N），且 legendLayersAdded/layersAdded 双信号各触发一次全量重建 → 批量实例化 O(N²·D) | `src/ui/layers/layertreepanel.cpp` + `tests/tst_layertreepanel.cpp` | S/M |
| W3 | F | `QgisLayerOrganizer::placeLayer` 每次按值复制 `declared()` 且 `desiredSlot` 再复制一次重建哈希 → 批量挂载 O(N·D) 复制 | `src/qgis/qgislayerorganizer.cpp` + `tests/tst_layerorganizer.cpp` | M |
| W4 | C/D | i18n 上下文错配：`QT_TRANSLATE_NOOP("GeoPatterns"/"QgisStyleService", …)` 登记的词面运行时却用 `QObject::tr()`（上下文 "QObject"）查表 → 翻译永远不命中（30 花纹/线型词面 + 16 井别词面） | `src/qgis/geopatterns.cpp`、`src/qgis/qgisstyleservice.cpp` + `tests/tst_symbolpicker.cpp` | S |
| W5 | E | 模板另存/加载/内置应用失败在 UI 动作路径完全静默（outcome.error 被 lambda 丢弃，templateFinished 不带错误文本，壳只接 updatePageNavigator） | `src/ui/layout/layouttemplates.{h,cpp}` + 既有 tst_layout* 用例文件 | S |
| W6 | E | mapbooklayout 硬编码系统字体族 "Noto Sans CJK SC"，绕过 `standardelements::figureFontFamily()` 的 vendored 优先链（layoutexport.cpp 已收口，mapbooklayout 是遗漏副本） | `src/qgis/mapbooklayout.cpp` + `tests/tst_layoutexport.cpp` | S |
| W7 | E | 地图册回读校验缺口：PDF 只验 4 字节 `%PDF` 头不验 `%%EOF` 尾（截断文件判成功）、PNG 不比对像素尺寸、SVG 分支零校验 | `src/qgis/mapbooklayout.cpp` + `tests/tst_layoutexport.cpp` | S |
| W8 | E | `exportLayoutAsync` 任务创建失败分支不 emit `exportFinished`（头注释契约不齐；其余分支都发） | `src/ui/layout/layoutexportactions.cpp` + `tests/tst_layoutexport.cpp` | S |
| W9 | D | 相花纹词表缺口：catalog.json 标准词面「三角洲」不命中 kFacies 任何桶（只有 delta=三角洲朵体/deltaic=三角洲相），落 all-other | `src/qgis/geopatterns.cpp` + `tests/tst_symbolpicker.cpp` | S |
| W10 | G | 守恒 Oracle 补强：保存/重开后组序+自定义绘制序+page:* 主题等价的组合断言；导出 PNG 非空内容像素回归 | `tests/tst_layerplatform.cpp`、`tests/tst_layoutexport.cpp` | S |

每项开工前按任务书给出 ≥3 条可观察现行行为 + 反例/非回归场景 + 可运行验证（见各轮 ledger 记录）；四份分包审计各含 ≥5 条可观察行为清单与证据。

## 4. 留给其他 Agent / 受保护而不做（发现但不认领）

| 发现 | 归属 |
|---|---|
| 数据图谱「打开保存版本」只拨 SelectionContext 不拨 layerSvc→双「激活层位」真源分叉（S1 A-1） | 修复点 `paleomainwindow_workbench.cpp:478` 在 PR #437 文件清单内 + 超出本方向白名单 → 留给 #437 方向/Issue Agent |
| mapbookqueue 索引页/manifest 写失败不计入 `result.failed`（S3 A4） | `src/workflow/mapbookqueue.cpp` 超出白名单 → 接口提案留档 |
| 预览 TOC 批量可见性变化无合并口（S4 A2）、装饰开关双重整图重渲（S4 A3）、页切换兜底 refresh 叠加（S4 A4）、paleodecorations gDark 全局静态（S4 A7，#355 同族） | `src/ui/datapreview/`、`src/ui/decorations/`、`paleomainwindow.cpp` 均超白名单 → 留档 |
| `previewmapcanvas.h:164` 死友元 `PreviewMapCanvasSelftest`（S4 A6） | 微清理，R7 阶段顺手处理或留档 |
| SemanticLegendBuilder/SymbolPickerPanel 无生产消费方 | 既有自述回收条件（方向 24/25 决策项），不动 |

## 5. 性能测量方案（F 域）

- **探针**：在 tst_layertreepanel / tst_layerorganizer 内新增规模探针用例（100/400 声明 + 层位实例化），qInfo 落墙钟 + 用「declared() 调用次数」级别的复杂度代理（修复后 refreshIndicators 每轮 1 次 readAll；organizer 归位 0 次复制——以可计数行为断言替代抖动墙钟门）。
- **基线先行**：修复前先在同一构建（Debug、vendored superbuild prefix、offscreen、本机 16C/62G、-j4 flock）跑出探针基线数，修复后同条件复测对比；数字进 ledger 与 PR 描述。
- **不设新墙钟硬门**（本机/CI 负载抖动教训：tst_panels/tst_correlation_full/tst_perfbudget 均已 RUN_SERIAL）；复杂度代理断言是机器无关的回归闸。

## 6. 风险

- W1 触及主题重放语义：必须保证既有 `horizonSwitchPrunesDanglingRecords`（已声明但不在主题记录的层重放后隐藏）等断言继续绿——修复只豁免「无 paleoLayerId / 无声明」图层，且只限 page:* 主题（用户命名主题保持 QGIS 原生语义）。
- W2/W3 缓存/预建引入失效语义：声明增删必须即时反映（organizer 的缓存挂 QgisLayerService 信号失效）；先写「声明变更后归位仍正确」的红测再动手。
- W4 改 tr 调用点：中文源串不变、Qt Linguist 上下文名与 .ts 既有条目对齐（GeoPatterns/QgisStyleService 上下文已存在于 paleo_zh_CN.ts），不改翻译文件本体。
- W5 反馈通道走 ErrorHub（与 layoutexportactions 同款 `ErrorHub::global()` 模式），禁用 PaleoNotify::warning 的 QMessageBox 兜底路径（无 hub 的单测会模态阻塞）。
- 全部改动文件先过 `git diff origin/master...HEAD --name-only` 对照 #437 清单与 Issue 指纹。
