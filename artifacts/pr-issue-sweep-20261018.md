# issue 清扫 PR：#531–#541（2026-10-18 swarm 评审收敛）

## 背景

2026-10-18 的 swarm 评审把 #531–#541 共 11 个 issue 一次性铺开，执行分三路代码修复——导出链封装与批量导出（#531 / #537 / #540 / #541）、相面编辑与证据链（#533 / #535 / #540 / #541）、sidecar 与国际化（#534 / #536 / #540）——外加一路闸门接线（#532 / #533 / #538 / #539 / #540）。结论：11 个 issue 的点名范围均已落地；清扫后的统一运行给出闸门 10/10 exit 0、目标测试 45/45 exit 0。各修复报告成文时受并行期纪律约束（禁止跑 cmake / ninja / ctest 与 `tools/check_*.py`），其「能编过、用例能过」的结论在当时未经实证，由本次统一运行首次背书；超出点名范围的存量残留（`horizonbatchexport` 的 Qgs 直连、`mappingfeatureidentity` 裸键、`importledger` 第四份目录推导等）与部分工具基线配套未做，均在「未能覆盖与如实登记」中逐条登记，不冒充完成。

## 改动

### #531 地质出图导出链 QGIS 封装

把 geologicalexport 对话框链路上的 QGIS 直连下沉到 `src/qgis` 薄壳，功能层与视图层签名不再出现 `Qgs*` 类型：

- `src/qgis/layoutpreview.h`（新增）：`PaleoLayoutPreview` 薄壳小部件（`setCurrentLayout` / `zoomFull` 两个动作），头三行 `// 层：QGIS 封装`；实现落在 `layoutexport.cpp`，未新建 .cpp。
- `src/qgis/layoutexport.h`：新增 `LayoutHandle`（`QgsPrintLayout` 所有权句柄）、`LayoutSnapshot`（工程快照 + 版面 XML + 图面图层 id）与 `captureLayout` / `rebuildLayout` / `validateLayoutLayers` / `saveLayoutTemplate` / `qgisVersion` 六个封装入口。
- `src/qgis/layoutexport.cpp`：上述实现 + `PaleoLayoutPreview` 实现（新增 `qgis.h` / `qgslayoutview.h` / `QVBoxLayout` / `layoutpreview.h` include）；`captureLayout` 收敛原 workflow 里的 `writeXml(QgsReadWriteContext)` 与 `items()` / `QgsLayoutItemMap` 图层 id 收集；`qgisVersion` 取代 `Qgis::version()` 直连。
- `src/qgis/geologicalmaplayout.h` / `.cpp`：新增 `buildHandle()`——`build()` 的句柄化入口，失败返回 `nullptr`，不交出包空版面的句柄；头文件改经 `layoutexport.h` 引入 `LayoutHandle`。
- `src/qgis/dataexportwriter.h` / `.cpp`：新增 `writeLayerById(project, layerId, …)`，按图层 id 写成果数据并补 `qgis_layer_id` / `title` 两个图件元信息键（原 `geologicalexport` 内联逻辑下沉）。
- `src/workflow/geologicalexport.h` / `.cpp`：`start()` 首参 `QgsPrintLayout*` → `PaleoLayoutExport::LayoutHandle*`；删除 `<qgis.h>` `<qgslayoutitemmap.h>` `<qgsprintlayout.h>` `<qgsproject.h>` `<qgsreadwritecontext.h>` 五个外部 include 与全部 QGIS 直连代码（快照 / readXml / saveAsTemplate / mapLayer / 图层 id 收集 / `Qgis::version`），改走薄壳；`Job` 改用 `LayoutSnapshot`；补 `QHash` / `QSet` / `QStringList` / `QVariantList` / `QVector` / `memory` 显式 include。
- `src/ui/dialogs/geologicalexportdialog.h` / `.cpp`：`Builder` 返回值 `QgsPrintLayout*` → `std::unique_ptr<LayoutHandle>`；预览控件改 `new PaleoLayoutPreview`（删 `<qgslayoutview.h>` `<qgsprintlayout.h>` 与 `QgsLayoutView` 前向声明）；`rebuild()` 改用句柄，保持「先绑定替换再退役旧场景」的顺序与注释。
- `src/ui/paleomainwindow_attach_mapping.cpp:289-297`：builder lambda 改为经 `PaleoGeologicalMapLayout::buildHandle` 返回 `LayoutHandle`，不再直接返回 `QgsPrintLayout*`。
- `tests/tst_exportpipeline.cpp`：必要适配（`std::unique_ptr<QgsPrintLayout>` 局部改 `buildHandle()`、裸版面访问走 `layout->layout()`、对话框 `Builder` lambda 返回 `buildHandle()`）——API 改句柄后不改它树就编不过，属最小适配。

口径说明：issue 验收第二句「`Qgs*` 类型不出现在 `src/workflow` 与 `src/ui/dialogs`」只在 issue 点名的 `geologicalexport` / `geologicalexportdialog` 四个文件上做到零命中（grep 自查，四个文件代码里已无 Qgs 标识符，仅注释提及）；`horizonbatchexport.{cpp,h}` 仍大量使用 Qgs 类型，属 #527 之前就冻结在 `tools/qgs-audit-baseline.txt` 与 `tools/qgis-spread-baseline.txt` 的存量残留，收敛它需要新写「批量版面改写器」封装，超出最小聚焦纪律，未动（见「未能覆盖与如实登记」）。

### #532 qgs-audit 闸门接线

- `CMakeLists.txt`：新增 `layering_qgs_audit` ctest 项（`check_layering.py --qgs-audit --strict`）；接线前复跑转绿——116 个直连文件全在册、新增 0、可收缩 0。
- `.github/workflows/ci.yml`：lint 车道 Source gates 步新增 `python3 tools/check_layering.py --qgs-audit --strict`；步名与 env 注释同步 #532 / #533 口径（func/ui 两指标权威闸口在本步）。
- `tools/qgs-audit-baseline.txt`：实测「可收缩 0」（116 个在册文件仍有 qgs* 直连），本轮无删行动作。

### #533 相面捕获工具门面与配置键收敛

- `src/qgis/faciespartitioneditor.h`：新增 `enum class PartitionOperation{Split,Paint}` 与 `createCaptureTool(canvas, cad, layer, operation, targetCode, aborted)` 门面声明（返回普通 `QgsMapTool*`）；include `faciespartitionstate.h` 与 `<functional>`，前向声明 `QgsMapCanvas` / `QgsMapTool` / `QgsAdvancedDigitizingDockWidget`，未新增任何 QGIS 头，视图层传递边不增。
- `src/qgis/faciespartitioneditor.cpp`：`createCaptureTool()` 实现——在此 include `faciespartitiontool.h`，把 `editAborted` 接到调用方取消回调（Qt 6.11 无「字符串式信号 + lambda」重载，连接上下文是工具自身；生命周期已核：`~PaleoEditingToolbar()` 直接 delete、`installTool()` 走 `deleteLater`，工具不会比工具栏活得久）。
- `src/ui/edittools/editingtoolbar.cpp`：删除对 `../../qgis/faciespartitiontool.h` 的 include，相面捕获工具改经门面创建并接线取消回调；`:547` / `:748` / `:903` 三处直读 `customProperty("paleo/requireFaciesTopology")` 全部改走 `FaciesPartitionEditor::isPartition()`（与同文件 `:904` 既有写法一致）。
- `src/ui/edittools/editingtoolbar_partition.cpp`：删除同一条 include（本文件只用 `FaciesPartitionEditor` 门面）。
- `CMakeLists.txt`：`layering_transitive` 上方补口径裁决注释——不注入 `QGIS_PREFIX` 的理由（避免同一条已红指标复制进 linux / windows 两个必过车道，#497 教训）、本地全闸命令、以及「差额收敛后改真同口径」的触发条件；新测试全部并入 `WORKING_DIRECTORY` 属性清单。

### #534 JSON sidecar 数据层门面

- `src/services/jsonsidecar.h` / `.cpp`：新增三类 sidecar 的数据层显式门面（`:95-186`）——`projectDirForCatalog`（catalogPath 纯词法上退三级，把 `datalist_undo` / `dataopsmodel` 两份复刻收成一份）、`undoVaultPath` / `undoVaultLoad` / `undoVaultSave`、`dataopsSidecarPath` / `dataopsSidecarLoad` / `dataopsSidecarSave`、`wellcompositeSidecarLoad` / `wellcompositeSidecarSave`；`readJsonFile` / `writeJsonFile` 契约与警告文案前缀不变；新增 `#include "../catalog/datacatalog.h"`（数据→数据）。顺带 #540-11：`writeJsonFile` 的 `f.write(doc.toJson(format))` 丢弃返回值改为字节数比对（`:78-84`，同 `dataexport.cpp:310` 口径），短写返回 false 并 qWarning 留痕。
- `src/ui/pages/datalist_undo.cpp`：`undoVaultPath` 转发 `paleo::undoVaultPath`；load / save 改调 `paleo::undoVaultLoad` / `paleo::undoVaultSave`（损坏 / 打不开时如实按空返回，语义与原 `readJsonFile().object()` 一致）；随之移除不再使用的 `<QDir>` / `<QSaveFile>`。
- `src/ui/pages/dataops/dataopsmodel.h`：`projectDirFor` / `sidecarPath` / `loadSidecarObject` / `saveSidecarObject` 四个视图侧入口全部转为数据层门面转发（签名不变，`datalist` / `entitypanel` / `datapage` 与既有测试零改动）；移除已无调用点的 `<QSaveFile>`（`QDir` / `QFile` / `QJsonDocument` / `QJsonParseError` 保留，避免动到其他 TU 的传递 include 假设）；头注补 #534 收敛说明。
- `src/ui/wellcomposite/wellcompositestore.cpp` / `.h`：`load()` / `save()` 改调 `paleo::wellcompositeSidecarLoad` / `paleo::wellcompositeSidecarSave`；路径派生 `<源名>.wc.json` 仍留在本组件，装载 / 落盘归数据层；头注同步 #482 / #534 口径。
- 测试：`tst_dataopssidecar`（`dataLayerFacadeOwnsPathsAndRoundTrips` + `undoVaultFacadeRoundTripsHeadlessly`——目录推导自持、load/save 往返、与视图侧 `sidecarPath` 同源、catalog 未开空转不写盘、半截 undo 库留痕失败）；`tst_wellcomposite_multiwell`（`testSidecarFacadeRoundTripsHeadlessly`——门面写出的 `.wc.json` 即 `WellCompositeStore` 读的那份，半截文档 `load()` 与门面 load 都如实失败并留痕）。
- 验收：`grep -rn "writeJsonFile\|QSaveFile" src/ui` 实测零命中。

### #535 相面配置键单一事实源与冻结覆盖范围 sidecar

- `src/qgis/faciespartitionstate.h`（新增）：`// 层：QGIS 封装` 契约头。命名键 `kRequireTopology` / `kSchema` / `kFrozenFootprint` / `kOperation` / `kEvidenceFailure` / `kEvidenceFailures` / `kCommitRejected` / `kWorkbenchBinding`（原散落三层的裸字面量）+ 图层侧 / 观察器侧访问器声明；冻结覆盖范围 sidecar 契约：`footprintSidecarPath()`、`saveFootprint()`（写 sidecar 并返回 extra 条目：路径 + bbox/面积/部件数/sha256 可读摘要）、`loadFootprint()`（先 sidecar、再旧字段 `partition_footprint_wkb` 兜底）。仅 QtCore 依赖，不给视图层新增 QtWidgets 传递边。
- `src/qgis/qgisdeferreddelete.h`（新增）：`paleo::qgis::releaseDeferredLayerHandles()`；#499 的理由注释（`deleteLater` 落地前 OGR/GDAL 句柄未释放 → Windows 删除竞态）从功能层搬到这里。
- `src/qgis/faciespartitioneditor.cpp`：全部裸键改走 `FaciesPartitionState` 访问器（`schemaKey` / `footprintKey` / `evidenceEditFailure(s)` / `partitionOperation` / `requireFaciesTopology` / `workbenchBinding`）；补 `saveFootprint` / `loadFootprint` 实现。
- `src/workflow/mappingworkbench.cpp`：`rememberNativeFailure()`、observer 绑定、`afterRollBack` 清理、`faciesSyncAttached` 的 `requireFaciesTopology` 写入、`watchUndo` 的 `commitRejected` / `evidenceFailure`、`beforeCommitChanges` 的 `evidenceFailure` / `commitRejected` 全部改走访问器；`styleLayer()` 的冻结覆盖范围改经 `FaciesPartitionState::loadFootprint(m_dir, v.extra)` 读取。
- `src/workflow/mappingworkbench_edit.cpp`：删除匿名 namespace 里的 `releaseLayerHandles()`（`QCoreApplication::sendPostedEvents`），4 个调用点（`:508` / `:565` / `:790` / `:803`）改调 `paleo::qgis::releaseDeferredLayerHandles()`，随之移除不再使用的 `<QCoreApplication>`；`copyForEditing()` 不再把 `partition_footprint_wkb` 写进 extra，改 `saveFootprint()` 落 sidecar + extra 记路径与可读摘要；`saveEditingVersion()` 对仍带旧字段的版本做迁移（优先沿用旧字段原值，只有旧字段也缺失才按当前覆盖范围重算）并 remove 旧字段。
- `src/ui/edittools/editingtoolbar.cpp`：三处改走 `isPartition()`（见 #533）。
- 测试：`tst_mappingworkbench:partitionOperationsPreserveEvidenceHierarchyUndoAndVersions` 的 footprint 断言从 `partition_footprint_wkb` 改为 `partition_footprint_path`（文件存在）+ `partition_footprint.sha256`（非空）+ 旧字段为空；`install(&reopened, …)` 改经 `loadFootprint()`；新增 `#include faciespartitionstate.h`。
- 兼容策略（如实记录）：读侧先读 sidecar、读不到回落旧字段，既有项目不会读不出来；写侧只写新契约，旧项目保存一次即完成迁移；sidecar 文件被手删（`artifacts/working/<draft>/`）时回落为空 → `install()` 自行重算并冻结当前覆盖范围，与改造前「extra 字段缺失」时的行为一致，但不再是跨版本硬冻结。sidecar 与工作副本 GPKG 同目录，随 `removeProduct()` 已有的 `QDir(working).removeRecursively()` 一并回收，未新增清理路径。

### #536 旧七列错位 schema 的属性表改编码恢复口径

- `src/workflow/mappingevidencereview.cpp`：匿名 namespace 新增 `layerFaciesSchema(version)`（`:68-75`）——先 `FaciesTaxonomy::normalize()`，被拒（相 / 亚相级旧分类缺归属）退 `FaciesTaxonomy::upgrade()`，仍不成才回原 schema，不新增拒绝理由；`:770` `recordSpatialInterpretationEdit` 的 classification 分支改喂 `layerFaciesSchema(versionForLayer(id))`，与 `place()` 读路径同口径；新增 `#include "../domain/faciestaxonomy.h"`。
- 测试：`tst_mappingworkbench:nativeAttributeTableRecodeRecoversLegacySchemaColumns`——旧七列错位 `extra.facies` 草稿 → 原生属性表改编码 → 断言图层 `microfacies` / `facies_label` / `facies_name` / `subfacies` / `texture` 与 `FaciesTaxonomy::place()` 一致，`errorOccurred` 为 0。断言点放在 `endEditCommand()` 之后（属性表改完即同步的真实时刻），正好避开 `mappingworkbench.cpp:1073` 旁路写路径的覆写，因此能真正检出回归。

### #537 层位批量导出文件名撞名唯一化

- `src/workflow/horizonbatchexport.cpp`：`baseName` 由 `%1_%2_%3` 改 `%1_%2_%3_%4`（拼上层位序号 `index+1`），并加 `usedNames` 占用表；撞名时报「层位 %1 与 %2 导出文件名相同」。
- `src/workflow/horizonbatchexport.h`：文件名规则文档改为 `<工程>_<层位>_<日期>_<序号>.<ext>`。
- 测试：`tst_horizonbatchexport:sanitizedCollisionStillGetsUniqueDeliveries`——`"K1/q"` 与 `"K1 q"` 消毒同形 → 两个层位都成功、交付文件名唯一且带 `_1` / `_2` 序号、`metadata.json` 的 `horizons[].files` 与实际落盘名一致、清单含 `error` / `cancelled` 字段。
- 如实记录：公开 API 下不同层位序号必然不同 → baseName 必然不同，该撞名报错已不可达，是防御性不变量检查（保留以满足 issue 建议的双重要求）；测试只能验证唯一性而非该文案。

### #538 file budget 闸门接线与两个超大函数拆分

- `CMakeLists.txt`：新增 `file_budget` / `file_budget_strict` / `file_budget_selftest` 三个 ctest 项（与 `ui_size` 同族 pattern）；`layering_transitive` 上方补 #533 口径裁决注释，新测试全部并入 `WORKING_DIRECTORY` 属性清单。
- `.github/workflows/ci.yml`：lint 车道 Source gates 步新增 `check_file_budget.py --strict` / `--selftest`。
- `tools/file-budget-baseline.txt`：只缩不涨对账——删 3 行回到阈值内（`paleomainwindow_workbench.cpp:file`、`fn:runWorkbenchCommand`、`fn:attachWorkbenchWells`，后两者经拆分后 <200 行）；下调 5 个冻结值（`fn:attachWorkbench` 1091→316、`datalist.cpp:file` 1016→1009、`fn:DataListPanel` 510→503、`attachConstraintPage` 718→713、`attachShellSurfaces` 580→578）；头注记全部账。可收紧 / 可收缩已清零。
- `src/ui/paleomainwindow_workbench_commands.cpp`：`runWorkbenchCommand` 原 397 行单体拆为入口（参数解析 / 依赖面装配 / 族分派 / 公共消息逻辑）+ 三个文件内自由函数（查询族 / 编辑族 / 生命周期族，逐字平移）。私有成员与私有段回调类型经 `WorkbenchCmdDeps` 结构显式传入、回调展开为等价 `std::function` 特化——不在头文件加声明（本岗只可改三个 workbench .cpp）；分支 early-return 经 `WbCmdStep::Return` 上传，行为守恒。
- `src/ui/paleomainwindow_workbench_wells.cpp`：`attachWorkbenchWells` 原 288 行拆出 `attachWellPanelActions`（logRequested / featureSelected / reviseRequested / saveRequested / undoRequested）与 `attachCanvasFaciesMenu`（画布右键改相菜单）两段接线；拆分后 160 行（<200）。

### #539 闸门基线纪律裁决成文

- `AGENTS.md`：新增「闸门基线纪律」节——四类基线机制对照表（layering-baseline 归零纪律 vs qgs-audit / qgis-spread 初冻基线 vs transitive / file-budget / ui-size 初冻 + 冻结值）；写明 qgs-audit / qgis-spread 按文件计数的初冻基线与 layering-baseline 归零纪律不同期、不改检查器计数语义的理由（增量计数 = 把 149+116 个已册存量文件全判红，闸门失去可行动性）；附三批收敛计划（批 1 导出链已落地删 4 行；批 2 `workflow/` 导出同族；批 3 `ui/` 原生控件族经 whitelist 显式裁决）；补「判红后补登基线转绿视为违约」纪律与 #516 / #474 式上游演进例外的记账要求；构建节补源码闸门 ctest 家族清单。
- `tools/qgis-spread-baseline.txt`：基线收缩（只缩不涨）——删 #523 导出链封装收敛后的 4 行（`src/workflow/geologicalexport.{cpp,h}`、`src/ui/dialogs/geologicalexportdialog.{cpp,h}`，工具实测「可收缩 4」），153→149；头注补 149 的来账、#539 初冻基线裁决指引与条目数。
- 风险（如实记录）：不改计数语义意味着已册文件在收敛计划跑完前仍是「新增才红」的豁免区；以 AGENTS.md 成文纪律（判红后补登基线转绿视为违约）作为兜底。

### #540 清扫子项

各修复报告对 #540 子项的编号口径不一（#540a–d / 子项 1-2 / #540-3…13），按原报告标签登记；已知对应关系：#540b = #540-10（dataexport 暂存 isValid）、#540-11（jsonsidecar 短写比对）、#540c = #540-12（CSV 键一致）、#540d = #540-13（暂存根清扫）。

- #540a（导出对话框防抖，`src/ui/dialogs/geologicalexportdialog.{h,cpp}`）：新增 `QTimer *m_rebuildTimer` 与 `scheduleRebuild()`；13 条输入连接 + 选择目录按钮全部改走 200ms 单发定时器（与 `dataexportdialog.cpp:56` 同口径）；构造时首次 `rebuild()` 仍同步执行。
- #540b / #540-10（`src/services/dataexport.cpp`）：私有暂存 `QTemporaryDir input(.input-XXXXXX)` 补 `isValid()` 校验，失败整批中止（与外层同口径同文案）。
- #540c / #540-12（`src/services/dataexport.cpp`）：CSV 快路径暂存内键由 `safeName(part)` 改 `QFileInfo(part).fileName()`，`safeName` 只作用于最终落盘名。
- #540d / #540-13（暂存根清扫，`src/services/dataexport.{h,cpp}`）：新增 `paleo::exporting::sweepStaleStagingRoots(parentDirectory)`——只认五个导出暂存根字面量 + 6 位字母数字随机段；>24h `removeRecursively` 并 qWarning，未超龄仅 qWarning 留痕；`execute()` 建暂存前清扫目标父目录；`src/workflow/geologicalexport.cpp` 入口与 `src/workflow/horizonbatchexport.cpp` `startAsync` 入口各清扫一次（工程目录与外部目标父目录）。
- #540 子项 1（相面延迟释放）与子项 2（冻结范围 sidecar）：见 #535。
- #540-3（i18n）：`src/ui/dialogs/dataexportdialog.cpp:33` 格式下拉 5 个裸字面量（GeoPackage / GeoJSON / Shapefile / CSV / GeoTIFF）补 `tr()`（同 `folderconfirm.cpp:39` 口径）；`src/ui/paleomainwindow_attach_compose.cpp` 批量出图选项框 PDF / PNG / SVG / TIFF 四个 `addItem` 补 `tr()`。
- #540-5（`src/ui/pages/mappingworkbenchpage.cpp:383`）：`m_featureDisplayState` 补 `setFont(PaleoTheme::bodyFont(PaleoTheme::kLabelPt))`，与同文件 `:100` `m_status` 对齐。
- #540-6（`src/ui/wellcomposite/compositebody.cpp`）：钉注文字旗旗底由 `QColor(27,115,208,215)`（交互主色 #1B73D0 作装饰底，违反 DESIGN.md:116）改为 PaleoTheme 纸面画布墨色徽标（`tokens(Light).text` 底 + `surface` 字，与同文件深度气泡同口径）；#1B73D0 只留在钉线 / 橡皮筋等交互描边。
- #540-7（`docs/ci/known-reds.json`）：新增 5 条带 issue 链接 + owner + `review_due` 2027-01-18 的登记——job-scope「lint」（Source gates 三处红的汇总入口）、test-scope `file_budget` / `file_budget_strict`（#538 残余 5 个回升条目）、test-scope `ui_size` / `ui_size_strict`（本批修复致 2 个巨型文件回升，先于本岗改动即红）。`ci_triage_reds.py --validate` 通过（known-reds 8 条合法）；合成 jobs+logs 模拟分诊 exit 0（9 条已知红降级 warning、0 未知红）。
- #540-8（`tools/transitive-baseline.txt`）：注释与数值矛盾修正——原注释写 heavy_tus「83→86」而数值已是 87、ui_qtwidgets_notes 涨幅只归因 #516；重写为自洽口径：冻结值 func 0 / ui 6 / heavy_edges 52 / heavy_tus 87，实测值 func 0 / ui 7 / 52 / 87，并写明 ui 7>6 的根因（#531 `layoutpreview.h` 薄壳 1 条新边）与 known-reds 登记去向；基线值不放宽。
- #540-9（`tools/make_appimage.sh`）：runtime `sha256sum -c` 从「文件不存在或不可执行」分支内提到 if 之外无条件执行——复用路径（`build/appimage/runtime-x86_64` 已存在且带 x 位）现在也必与新 pin 对账，改 pin 值后旧 runtime 会被抓出。
- #540-11（`src/services/jsonsidecar.cpp`）：短写字节数比对，见 #534。
- 覆盖边界（如实记录）：清扫器只覆盖 issue 列出的四个暂存根的五个字面量；`src/workflow/mapexport.cpp:236` 的 `.map-export-*`、`src/workflow/mappingworkflow.cpp:374` 的 `.thickness-*`、`src/workflow/mappingworkbench_predict.cpp:303` 的 `horizon-input-*` 同样无清扫器，但不在 #540 清单内且文件不归各修复员，未处理。同 DESIGN.md 规则下另有 `compositebody.cpp:169`（橡皮筋选区填充 @30，交互选区性质）与 `compositeheader.cpp:109`（拖拽幽灵预览 @200）两处 #1B73D0 派生填充未在清单内，仅登记未改。钉注旗改墨色徽标后与同文件悬浮深度气泡视觉同形（都成「墨底白字徽标」）——为避免交互色作装饰底并复用文件既有 token 口径的取舍；若设计上希望两者可区分，需设计侧定稿（DESIGN.md 未给注记旗专用 token）。

### #541 三条（测试得出的定性结论）

1. **相面 changed 路径保持 MultiPolygon（#541.1）**：`src/qgis/faciespartitioneditor.cpp` `apply()` 的 changed 路径补 `convertToMultiType()`（与 `child()` 的 add 路径对齐；GEOS difference 的单部件结果是 Polygon）。`tst_faciespartition:changedGeometryStaysMultiTypeForMultiColumns` 的定性结论：绘制同时切过两个原面（走 `apply()` 的 changed 路径）时，产生的 3 个特征 `wkbType()` 均为 `Qgis::WkbType::MultiPolygon`，undo / redo 后不退化成单部件，且 `validate()` 通过。
2. **零要素相面放行（#541.2）**：`src/workflow/mappingworkbench_edit.cpp` 两处对「无传入 footprint 且 `featureCount()==0`」放行、不写冻结范围（选 issue 的「放行」方案而非「报错改成指明治本原因 + 给手绘入口」方案，保持改动最小）。`tst_mappingworkbench:zeroFeaturePartitionStillOpensAnEditingCopy` 的定性结论：零要素面图层能建出编辑副本、extra 无 footprint 字段、install 后无冻结范围且 `validate()` / `isPartition()` 通过；代价是这类副本的 `validate()` footprint 比对在下次带覆盖范围的保存前整段跳过。
3. **导出发布门对齐与清单留痕（#541）**：`src/workflow/horizonbatchexport.cpp` 外部拷贝块不再早退——拷贝失败记入 `job->error` 后仍写出 `metadata.json`（新增 `error` 字段，保留 `cancelled` 字段）；发布门由 `journal.isEmpty() && !error.isEmpty()` 改为 `task->state()==Succeeded` 且未取消（与 `geologicalexport.cpp:214` / `mapexport.cpp:287-288` 对齐），失败 / 取消批次不 rename 外部目录、不登记 journal，已导出层位在结果里留痕。`tst_horizonbatchexport:workerErrorDoesNotPublishExternalDirectory` 的定性结论：空图层集层位使 worker 报错时，外部目录不被创建、`result.directory` 为空、已导出层位 `ok=false` 且带原因、catalog 无 OUTPUT 登记。行为变化（如实披露）：取消批量出图从此不再交付「已导出的层位」（原文档写 Cancellation retains already exported horizons），这是「与 geologicalexport / mapexport 对齐」的必然结果，已同步改 `horizonbatchexport.h` 文档注释；若产品希望取消时保留部分成果，需另立需求。可见性取舍：失败批次的 `metadata.json` 写在随后被 `QTemporaryDir` 析构销毁的暂存目录，「外部目录不被发布」与「清单存在」无法在事后同时观测——失败用例断言不发布 + 结果留痕 + catalog 未登记；「清单存在 + error/cancelled 字段 + 记录实际落盘名」改在成功用例（`sanitizedCollisionStillGetsUniqueDeliveries`）中断言。worker 里 `rebuildProject` / `readXml` 两个更早的早退仍先于清单写出返回，但那一刻尚未拷贝任何文件、且发布门要求 Succeeded，不可能产出无清单半成品。

## 验证

### 闸门退出码

以下为本次清扫统一运行的闸门退出码（全部 exit 0）：

| 闸门 | 退出码 |
|---|---|
| layering_strict | 0 |
| layering_qgs_audit_strict | 0 |
| layering_symbol_audit | 0 |
| layering_transitive_strict | 0 |
| layering_transitive_ci | 0 |
| layering_selftest | 0 |
| qgis_spread_strict | 0 |
| file_budget_strict | 0 |
| ui_invariants_strict | 0 |
| i18n | 0 |

背景（接线期记录，非最终态）：闸门接线报告复跑 26 项中，`check_layering.py --transitive`（带 `QGIS_PREFIX`，CI lint 口径）、`check_file_budget.py --strict` / 非严格、`check_ui_size.py --strict` 曾为 exit 1，处置分别见 #533（CMake 注释裁决 + baseline 对账）、#538 + #540-7（known-reds 登记）、#540-7；`ci_triage_reds.py --validate` 与合成 jobs+logs 分诊模拟 exit 0。最终统一运行中 `file_budget_strict` / `layering_transitive_ci` 已转绿（见上表）；`ui_size` / `ui_size_strict` 未出现在最终闸门表中，其收敛状态未覆盖。

### 构建目标数

未覆盖：素材未提供本 PR 构建的目标数，本机也未重建。本机 `build/` 的 ctest 清单实测 434 项（`ctest --test-dir build -N` → Total Tests: 434），且不含本 PR 依闸门接线报告新增的 4 个 ctest 项（`layering_qgs_audit` 与 `file_budget` / `file_budget_strict` / `file_budget_selftest`），与清扫前配置一致，不能代表本 PR 构建。统一运行实际执行并逐个给出退出码的是下述 45 个目标，非全量套件；全量套件在本 PR 构建上的结果未覆盖。最终闸门表中另出现 `layering_qgs_audit_strict` / `layering_transitive_ci` 等名称，素材未说明其与「新增 4 个 ctest 项」的对应关系，PR 构建的完整 ctest 清单未覆盖。

### 测试分组结果

45/45 exit 0（按素材逐个给出的退出码汇总）：

| 分组 | 用例 | 结果 |
|---|---|---|
| 闸门与静态检查（18） | layering、layering_strict、layering_selftest、layering_symbol_audit、layering_transitive、qgis_spread、qgis_spread_strict、qgis_spread_selftest、file_budget、file_budget_strict、file_budget_selftest、ui_invariants、ui_invariants_strict、i18n、i18n_selftest、ci_known_reds、env_redset_validate、docs_drift | 全 0 |
| 导出链（5） | tst_exportpipeline（#531 适配）、tst_horizonbatchexport（#537 / #541 新增用例）、tst_layoutexport、tst_dataexport（#540b/c/d）、tst_composeworkflow | 全 0 |
| 相面分类（5） | tst_faciespartition（#541.1 新增用例）、tst_faciescatalog、tst_faciesconsistency、tst_faciesmapping_workflow、tst_domain_faciesclassification | 全 0 |
| 工作面（3） | tst_mappingworkbench（#536 新增、#540 子项 2 改断言、#541.2 新增）、tst_mapping、tst_mappingpages | 全 0 |
| 井合成（3） | tst_wellcomposite（#534 门面往返）、tst_wellcomposite_multiwell、tst_wellcomposite_editing | 全 0 |
| 井相（2） | tst_wellfacies、tst_wellfaciesconfig | 全 0 |
| 数据 ops（3） | tst_dataopssidecar（#534 新增）、tst_dataops_incremental、tst_panels_dataops | 全 0 |
| 证据链（4） | tst_evidencee2e、tst_evidencegraph、tst_nativeevidenceguard、tst_constraintevidence | 全 0 |
| 版本与流程（2） | tst_versions、tst_workflows | 全 0 |

### 未能覆盖与如实登记

- **报告期无编译 / 测试实证**：三路修复报告成文时受并行期纪律约束（禁止 cmake / ninja / ctest 与 `tools/check_*.py`），「能编过、用例能过」在当时未经实证，本节闸门 / 测试结果来自清扫后的统一运行，是第一批实证。报告期的具体未验证点——`tst_mappingworkbench.cpp` 语法（缺 vendored SARibbon.h，MSVC /Zs 在 `src/ui/paleomainwindow.h:3` 即 C1083）、`ui_qtwidgets_notes` 8→6 的推导数字（未跑 `check_layering.py --transitive`）——前者已被统一运行的 exit 0 覆盖，后者仍只是推导而非实测。
- **存量残留未收敛（超范围或基线冻结）**：`horizonbatchexport.{cpp,h}` 的 Qgs 直连（#527 前冻结在 `tools/qgs-audit-baseline.txt` / `tools/qgis-spread-baseline.txt`，收敛需另立任务与新的「批量版面改写器」封装）；`src/qgis/mappingfeatureidentity.cpp:316` / `:464` 两处裸键（与访问器同属 QGIS 封装层，跨层契约已收敛但「单一事实源」未 100%）；`src/workflow/importledger.cpp:25-35` 第四份工程目录推导（现本可改调 `paleo::projectDirForCatalog`，但文件不在可改清单）；`mappingevidencereview.cpp:708`（`changeEvidence` 的 link 分支，用于 category_path / category_key，改它会动证据记录语义）与 `mappingworkbench.cpp:1073`（`beforeCommitChanges` 旁路写路径）两处原始 schema 旁路；`renderEvidenceSnapshot`（267 行）与 `attachWorkbench`（316 行）未拆到 <200 行（前者文件不可写，后者拆子过程需在 `paleomainwindow.h` 加成员声明或再引入大依赖面结构，#538 只把冻结值收紧到 316 而非删行——删行即产生「baseline 外新增超标」）。
- **file_budget 残余 5 个回升条目**：全部落在其他修复员改过的文件且不属闸门岗可改范围（`editingtoolbar.cpp:file` 1112>1107、`mappingworkbenchpage.cpp:file` 1671>1668 与 `fn:MappingWorkbenchPage` 662>659、`attach_compose.cpp:fn:attachComposePage` 407>405、`compositebody.cpp:fn:paintEvent` 206>202）。按「只缩不涨」不上调冻结值，按 #540-7 预授权通道登记 known-reds（review_due 2027-01-18）后接线；这些文件瘦身前，相关 required check 信号会保持红。
- **--transitive 口径差**：接线期带 `QGIS_PREFIX`（CI lint 口径）exit 1（`ui_qtwidgets_notes` 7>6，根因 #531 新增薄壳 `layoutpreview.h` 1 条视图层传递边），无前缀 ctest 口径 exit 0；配置侧选了「CMake 注释 + 文档明示、不注入 QGIS_PREFIX」（注入会让同一条已红指标复制进 linux / windows 两个必过车道），代价是本地 ctest 的「假绿」仍在，差额、根因、本地全闸命令与后续动作已写进 CMakeLists 该测试上方注释。最终统一运行报 `layering_transitive_ci` exit 0；两个记录时点 / 口径不同，合并后建议以 CI lint 车道实跑复核该指标。
- **本机环境既有红（不阻断）**：`tst_horizonbatchexport` 在本机稳定失败（GDAL ERROR 6：PNG driver does not support update access），未改动的 master 构建同样失败，属本机 GDAL 环境问题，CI 为权威判定。本写作会话尝试在本机复跑未果（测试进程以 0xc0000135 退出——shell 未配 QGIS/GDAL 运行时 DLL），故该披露以既有记录为准。
- **tools 基线配套**：`tools/qgis-spread-baseline.txt` 的 4 行删除已由闸门岗落地（153→149）；`tools/transitive-baseline.txt` 的指标同步见 #540-8（注释与数值重写为自洽口径，冻结值不放宽）。#531 曾担心的「qgis-spread 口径下 4 个导出链文件变为可收缩 → `--strict` 判红」已随之消除（最终 `qgis_spread_strict` exit 0）。

## 关联

- Closes #531 —— 地质出图导出链 QGIS 封装（LayoutHandle / LayoutSnapshot / PaleoLayoutPreview / writeLayerById，功能层与对话框不再直连 Qgs*）
- Closes #532 —— qgs-audit 闸门接线（ctest `layering_qgs_audit` + CI lint Source gates）
- Closes #533 —— 相面捕获工具门面 `createCaptureTool` 与 `requireFaciesTopology` 配置键收敛
- Closes #534 —— JSON sidecar 数据层门面（undo vault / dataops / wellcomposite）与短写比对
- Closes #535 —— 相面配置键单一事实源、冻结覆盖范围 sidecar、延迟释放句柄归位
- Closes #536 —— 旧七列错位 schema 的属性表改编码恢复口径
- Closes #537 —— 层位批量导出文件名撞名唯一化（序号 + 占用表）
- Closes #538 —— file budget 闸门接线与 `runWorkbenchCommand` / `attachWorkbenchWells` 拆分
- Closes #539 —— 闸门基线纪律裁决成文（AGENTS.md + qgis-spread 基线收缩 153→149）
- Closes #540 —— 清扫子项集合（对话框防抖、暂存 isValid、CSV 键一致、暂存根清扫、tr()、字阶、DESIGN 色板、known-reds 登记、transitive 对账、appimage pin 对账、sidecar 短写）
- Closes #541 —— 相面 changed 路径保持 MultiPolygon、零要素相面放行、导出发布门对齐与清单留痕
