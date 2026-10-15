# S3 版式/输出（地图布局与出图 + 无屏验证）R0 审计

审计基线：worktree `paleo-wt-goal7-qgis-layers`，HEAD `b0520429`。范围文件全部实现级读过；排除指纹（PR #437、Issue #352–#421、TODOS 已递延项）逐条比对过 #411/#415/#419/#376 原文，无重叠认领。

## 链路与现状（调用图/数据流，逐环 file:line 证据）

**导出核心（QGIS 封装层，三格式共用）**
`PaleoLayoutExport::exportLayout()`（`src/qgis/layoutexport.cpp:124-306`）是唯一导出管线：路径补默认扩展名（:56-69）→ 页选解析（0-based，Range 空选报错 :160-172）→ 三格式分支（PNG :187-213 / PDF :214-239 / SVG :240-266）。QGIS 4.2 的 Pdf/Svg settings 无页表，非全选时走 `trimmedClone()`（:96-118，`clone()` + 逆序 `deletePage()`，先删页内容项再删页）；PNG 仅在「从第 0 页起的连续选择」时用原生 `settings.pages`，否则同样走 trimmedClone（:183）。成功后扫描输出目录收集 `_N` 同名兄弟文件进 `outcome.files`（:273-298）。`resultErrorText`（:71-89）把 `ExportResult` 枚举翻成中文错误串并附 `exporter.errorMessage()`。

**两条消费路径**
- 视图路径：`PaleoLayoutExportActions`（`src/ui/layout/layoutexportactions.cpp`）— QAction → 文件对话框 → `ExportSettingsDialog`（dpi 72–1200 默认 300 + 三档预设 :76-100；页选单选组 :107-151）→ 注入任务服务则 `exportLayoutAsync`（:323-404：GUI 线程 `captureProjectSnapshot` + `layout->writeXml`，worker 重建私有工程再导出，结果经任务 finished 回包）→ `reportExportOutcome`（:406-439：状态条 + 缺层警告走 ErrorHub/消息日志，成功可开目录）。
- 功能路径：`mapexport.cpp startMapLayoutExport`（:304-343，compose 页层位图 PDF 用）、`horizonbatchexport.cpp`（复用同一 live 版面逐层位换层集/范围/标题，`Batch::begin/step/finish` :156-301）、`mapbookqueue.cpp`（逐格建版面 → `mapbooklayout::renderLayout` 落盘回读）。

**地图册读回校验**：`renderLayout`（`src/qgis/mapbooklayout.cpp:302-378`）— 导出 → 文件存在 + 大小 >0 → PNG 验 8 字节签名 + `QImage` 可解码（记录像素尺寸）；PDF 只验前 4 字节 `%PDF`；**SVG 无格式校验分支**（现无调用方，Format 只到 Png/Pdf：`mapbookqueue.cpp:114-117`）。

**模板（.qpt）两条线**
- 文件线：`PaleoLayoutTemplates`（`src/ui/layout/layouttemplates.cpp`）— `saveTemplate` 走 `QgsLayout::saveAsTemplate`（:236）；`loadFromFile` 用 `QDomDocument::setContent` 解析（带行号报错 :272-282）→ `loadFromTemplate(clearExisting=true)`（:286）；内置模板解析 docs/templates 下 `.qpt`（:345-361）；图件复合键走 `PaleoStandardElements::populateFigureLayout` 代码骨架（:300-336，保留地图项层集与范围 :317-323）。**无任何 `%{var}`/`[%var%]` 变量替换**——变量替换只存在于 mapbook 的标题/页脚模式（`src/workflow/mapbook.cpp:24-58`，严格语义：未定义/未闭合即报错）与格名模式（`buildGrid` :109、`tilesFromAreas` :144）。
- 库线：`layouttemplatelibrary.cpp` — qpt 字节进 catalog 受管区（partial+rename 原子落位、SHA-256、置只读 :94-129）+ `layout_templates` SQLite 表（`src/metadata/layouttemplatestore.cpp:30-47`）。

**设计器壳**：`layoutdesignershell.cpp` — 版面销毁即关窗（:229-235，#413 不设 WA_DeleteOnClose）；选中 → 面板/树双向同步（:738-750、:794-810）；undo/redo 后重读选中刷新面板（:993-1000）；`templateFinished` 只接 `updatePageNavigator`（:773-774）。

**字体注册链**：vendored 四字体（`resources/fonts/` + `paleo_fonts.qrc`）在 `PaleoTheme::applyTheme`（`src/ui/paleotheme.cpp:446` → `ensureApplicationFonts` :240-257）注册，仅 `src/app/main.cpp:261` 与测试 `pinRenderEnvironment` 触发；注册失败打 qWarning 后靠替换兜底。图件字体单一事实源 `standardelements::figureFontFamily()`（`src/qgis/standardelements.cpp:42-53`：vendored "Noto Sans SC" → 系统 "Noto Sans CJK SC" → 兜底）。

## 可观察现行行为清单

1. **导出失败必有状态条反馈**：三格式失败都落到 `reportExportOutcome` 的 `showMessage(outcome.error, 6000)`（`layoutexportactions.cpp:419-422`），有 `m_statusTarget` 时不静默。验证：`tst_layoutexport.cpp:356-411 errorPathsAndSignal`（不可写父路径、空页选、null layout）。
2. **缺层警告独立于成功状态呈现**：快照层数据源不可读不阻断导出，warning 进 `ExportOutcome.warnings`（`layoutexport.cpp:690-705`）→ 状态条加长提示 + ErrorHub Warning 档 + QgsMessageLog（`layoutexportactions.cpp:415-417, 427-435`）。验证：`tst_layoutexport.cpp:512-560 missingSnapshotLayerSurfacesWarnings`。
3. **异步导出期间动作禁用 + 重复导出即拒**：`m_inFlight` 置灰三动作（:302-310），第二次 `exportLayoutAsync` 立即 `exportFinished(path,false)` 不建任务。验证：`tst_layoutexport.cpp:459-507`。
4. **页选不碰源版面**：range 导出走 clone，源版面页数/内容不变。验证：`tst_layoutexport.cpp:258-259`。
5. **.qpt round-trip 保留内容项与页面毫米规格**，加载前 `layout.clear()` 后 `loadFromTemplate` 恢复项数/尺寸。验证：`tst_layoutexport.cpp:262-315`。
6. **内置模板一键清空有确认门**：版面有内容项时 `PaleoNotify::ask` 确认（`layouttemplates.cpp:388-400`）；程序化 `applyBuiltin` 不确认。
7. **.qgz round-trip 钉住**：8 个内容项 id、页面 297×210、比例尺/图例/插图 overview 回联主图（uuid 重联）、标题文本。验证：`tst_layoutpersistence.cpp:41-117`。
8. **地图册账实一致**：manifest.json 记每格 file/status + failures 明细；索引页失败/变量未定义/格子退化都进 `failures[]`，单版失败不拖死整队。验证：`tst_mapbook.cpp:339-491` 四个槽。

## 发现分组

### A. 新发现（经核实未被 Issue/PR #437/TODOS 占用）

**A1. 模板加载/保存失败在 UI 动作路径完全静默 — S**
现象：设计器「另存为模板/从模板加载」失败（损坏 .qpt、不可写路径、`loadFromTemplate` 拒绝）用户看不到任何提示。证据：`layouttemplates.cpp:122-135` 与 `:139-152` 的动作 lambda 拿到 `TemplateOutcome` 后**直接丢弃**（error 只活在返回值）；`PaleoLayoutTemplates` 无 status target/消息通道；信号 `templateFinished(path, ok)` 不带错误文本；壳里唯一接线是 `layoutdesignershell.cpp:773-774` 的 `updatePageNavigator()`。对照：导出链同文件已有 `reportExportOutcome` 完整反馈，模板链没有对应物。影响面：设计器 File 菜单两个动作 + 内置模板应用（`applyBuiltinAction` :380-402 同样丢弃 error）。验证方式：offscreen 测试里 `setLayoutProvider` 挂真版面 → 程序化触发加载损坏 XML 的 .qpt → 断言有反馈通道（修复后：ErrorHub 入账 + 错误信号）；先写一个「现状静默」断言证明缺口。规模 S（接线 + 一个测试）。

**A2. 地图册版面硬编码系统字体族 "Noto Sans CJK SC"，绕过 vendored 字体 — S**
现象：地图册逐格/索引/蒙太奇页标签用 `labelFormat("Noto Sans CJK SC", ...)`（`mapbooklayout.cpp:38-47` 定义、:71 调用），与 `standardelements.cpp:42-53` 的 `figureFontFamily()`（vendored "Noto Sans SC" 优先）是**两份重复实现且取字不同**。在无 Noto Sans CJK SC 的环境（AppImage/Windows 裸机）地图册页落字体替换，PDF 文本无嵌入保证——正是 `figureFontFamily` 注释（standardelements.cpp:36-41）记录过的已修复问题在 mapbooklayout 的复刻。证据：`mapbooklayout.cpp:71` vs `layoutexport.cpp:361-365`（layoutexport 已改为薄别名调工厂）。影响面：地图册全部文字页 + 蒙太奇。验证方式：`PaleoTheme::pinRenderEnvironment()` 后断言 `buildTileLayout` 产出标签的 `textFormat().font().family() == "Noto Sans SC"`。规模 S（复用工厂，删重复 labelFormat/northArrowSvg）。

**A3. 地图册回读校验覆盖缺口：PDF 只验 4 字节头、PNG 不验尺寸、SVG 零校验 — S**
现象：`renderLayout` 的 Oracle 校验（`mapbooklayout.cpp:338-372`）——PNG 验签名 + 可解码但不比对像素尺寸 vs dpi×页面毫米（300dpi A4 横版应 ~3508×2480，对比 `tst_layoutexport.cpp:120-124` 已有的核心层断言）；PDF 只 `startsWith("%PDF")` 不验 `%%EOF` 尾部，截断文件判成功；SVG 分支完全没有头校验（函数签名允许 Format::Svg，当前无调用方但契约开口）。影响面：地图册/蒙太奇产物「坏文件算成功」的最后一道闸有洞。验证方式：构造截断 PDF（写 `%PDF-1.4` + 垃圾）喂 `renderLayout` → 现状必过（反证测试）；修复后断言失败。规模 S。

**A4. 索引页/manifest 写失败只进 failures 明细、不计入 result.failed — S**
现象：`mapbookqueue.cpp:517-531`：索引页渲染失败、manifest 写失败均 `result.failures.append(...)` 但 `result.failed` 不变。`Result::summary()`（:155-159）的「失败 N」因此与 failures 明细条数不一致。处置：**`src/workflow/mapbookqueue.cpp` 超出本方向白名单 → 接口提案留档，不跨界改动**（见 05-baseline.md §4）。

**A5. `exportLayoutAsync` 任务创建失败分支不 emit `exportFinished` — S**
现象：`layoutexportactions.cpp:389-393`：`m_taskSvc->start` 返回 null 时只 `reportExportOutcome`，不 `emit exportFinished`；而同函数其余分支（busy 拒 :334、回包 :398）与同步路径（:250, :261, :288）都发。头注释契约（`layoutexportactions.h:84-93`）写「true=已受理（完成时 exportFinished）」，此分支返回 false 属「调用方应显示失败原因」——但 `runExportUi` 对该返回值也走统一 failure 构建（:283-290）**多发一次** `exportFinished(path,false)`，行为恰好被调用方兜住；裸调 `exportLayoutAsync` 的宿主则少一次信号。影响面：信号契约不齐，测试壳 spy 计数会漏。验证方式：mock 一个 start 恒 null 的 PaleoTaskService → 断言裸调时 exportFinished 次数。规模 S。

### B. 已占用（只列归属）

- **Issue #376**（预览 grab 每帧落盘）：预览域，本审计未触碰未认领。
- **Issue #419**（src/qgis 五项低危）：逐项核对，五项均在 previewrendercache/qgislayerservice/qgisprocessingservice/qgiscanvascontroller/mappingartifactwriter，与版式域零重叠；`mappingartifactwriter.cpp` 属 PR #437 只读面。
- **Issue #352/#355/#369** 等 #352–#421 其余项：均非版式域；#411/#415 低危清单逐项比对无本域条目。
- **PR #437**：`mapsnapshot`/`mapversionstore`/`mappingartifactwriter`/`qgisprojectservice` 等只读，未作为发现项。
- **TODOS.md:335-336**：图签块已交付（`layoutexport.cpp:538-555`），不重复发现。

### C. 已修复/与文档不符但不构成缺陷

- **字体族收口已完成**：`figureFontFamilyResolvesToInstalledFamily`（`tst_layoutexport.cpp:566-577`）钉死 vendored 优先；唯地图册副本未跟上（见 A2）。
- **缺层静默已修复**：曾「导出成功掩盖缺层」，现 ErrorHub + 状态条双通道，有测试钉。
- **`docs/progress/mapbook-reporting.md` Oracle 6（计时）** 文档自认未执行、留补测口径——非代码缺陷。
- **蒙太奇无生产调用方**：`buildMontageLayout` 仅 `tst_mapbook.cpp:283` 使用，无 workflow/UI 入口；R5 文档写「同版面组合」但接线未做。属「文档声称 vs 代码」差距，记档供 mapbook 方向后续接线时确认是有意递延还是遗漏。
- **compose 同步批量出图的 `QProgressDialog + processEvents`**（`paleomainwindow_attach_compose.cpp:388-395`）：WindowModal 阻断主窗输入，期间用户够不到被 Batch 实时改写的版面；异步路径为主（:379-387），同步为无任务服务兜底，风险可控，不建议单独立项。

### D. 低价值不建议做

- **`QgisLayoutService::exportPdf` 生产零调用**（仅 `tst_services2.cpp:592` 使用）：多形态备用 API，删除收益≈0、动 public 头有回归面，留。
- **A5 修复优先级低于 A1**：裸调缝当前无生产宿主，随 A1 一并理顺即可。
- **「qgz round-trip 未钉的字段」**（字体/dpi/signatureBlock/地图层集——测试用空层集构造）：逐项补钉成本高、QGIS 序列化自有回归测试兜底，不建议本仓重复。

## 建议的非回归验证场景（无缺陷时的守恒验证）

1. **版式全链守恒闸**：`ctest -R 'tst_layoutexport|tst_layoutpersistence|tst_mapbook|tst_layoutdesigner_full|tst_standardelements|tst_layouttemplatelibrary|tst_horizonbatchexport'` ——覆盖三格式头校验、页选 clone 无副作用、.qpt/.qgz round-trip、地图册账实一致、字体族解析，是 S3 域的守恒底线（当前 CI 在跑）。
2. **导出失败反馈守恒**：对 `exportLayout` 三格式分别造不可写目标（父路径为文件），断言 `outcome.ok==false && !outcome.error.isEmpty()` 且 UI 路径 `reportExportOutcome` 产生状态条消息（A1 修复后可并入同一测试壳验证模板链同款反馈）。
