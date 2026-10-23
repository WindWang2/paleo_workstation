# 03 审查 D — 增量 UI + QGIS 封装

审查窗：`d699694c..HEAD`（#630–#647）。只读。对照 DESIGN.md 只记偏离事实，不提议新视觉。

结论：取消、对话框内校验失败、离线底图完成后的会话守卫、相分类草稿保存失败，这几条主路径没有「失败仍报成功 / 写到已关闭工程」。下面 3 条是本切片新缺陷。剖面定位没有用过期 id 打开错误地震体。相图图例不会把缺席的模板类画成面，27 vs 7 归 #645。

## Findings

### D-1 / P2 / HIGH / 工程引导

- File:line：`src/ui/dialogs/projectsetupdialog.cpp:191` `scan`；`:196-197` `showPreview`；`:274-280` `finish`；`src/ui/paleomainwindow_projectsetup.cpp:22-24` `runNewProject`；`src/ui/paleomainwindow.cpp:848-863` `maybeSaveProject`
- 函数：`ProjectSetupDialog::finish` / `scan` / `showPreview`；`PaleoMainWindow::runNewProject`
- 根因：「创建空白工程」把 `m_empty` 置真后跳过 `m_busy`/`m_dirty`/资料错误检查并 `accept()`。`scan()` 会把 `m_empty` 清回 false。`runNewProject` 在 `exec()` 返回后、`create()` 之前调用 `maybeSaveProject()`，后者在有未保存编辑或工程脏时进入模态事件循环。过期预览若在这个循环里到达，`showPreview` 看见 `m_scanRevision != m_revision` 会再调 `scan()`，空白意图被改写。随后 `create()` 读到的是非空 `sources()` 和尚未被这次重扫更新的 `m_preview`。
- 触发条件：已打开工程且关窗前会弹保存/放弃（图层编辑脏或 `.qgz` 脏）；新建对话框里一次识别尚未返回，期间又改过路径（`textEdited` 已 `++m_revision`）；用户点「创建空白工程」并在保存对话框里选保存或放弃。无打开工程时 `maybeSaveProject()` 立即返回，这条竞态不发生。
- Observed：用户要的是空白工程。竞态后 `workflow->create()` 可能按表单里的资料路径和上一轮预览建工程并排队导入。若上一轮 `preview.errors` 为空，最新来源的校验被跳过。
- Expected：空白工程只建空工程；识别未完成或修订号已变时不得改写 `m_empty`，也不得用过期 `m_preview` 调用 `create()`。
- 调用链：`创建空白工程` → `m_empty=true; finish(); accept()` → `dialog.exec()` 返回 → `maybeSaveProject()` 模态循环 → `ProjectSetupWorkflow::previewReady` → `showPreview` 修订号不符 → `scan()`（`m_empty=false`）→ 用户关闭保存框 → `workflow->create(dialog.projectPath(), dialog.sources(), dialog.preview())`。
- 排除比对：不是 #556 吞错。取消按钮走 `reject()`，`exec()!=Accepted` 时不会 `create`。非空白路径的 `finish()` 在 `m_busy`、`m_dirty`、`preview.errors`、图件层位不合法时不 `accept()`。`ProjectSetupWorkflow::create` 在 `mkpath` 之前也会再拒 `preview.errors`。缺陷只在空白按钮绕过这些门、再被异步 `scan()` 翻回。
- 修复策略：空白创建在 `accept()` 前固定快照（空 sources + 仅 projectCrs 的 preview），并在 `create()` 前忽略后续 `previewReady`。`scan()` 不要清除已经确认的空白意图。`maybeSaveProject()` 返回后若 `m_busy` 或修订号变了，放弃创建。
- 回归测试思路：打开脏工程，填一个会报错的资料路径并触发识别，识别返回前点「创建空白工程」，在保存框停留到预览回调进入，再确认保存。期望只出现空工程或明确取消，不能导入该资料，也不能把识别错误当成创建失败以外的成功。

### D-2 / P2 / HIGH / 数据导入

- File:line：`src/ui/paleomainwindow_project.cpp:66-122` `runFolderImportAt`；`:139-198` `runFilesImportAt`；对照 `src/ui/paleomainwindow_paleomaps.cpp:72-96`；`src/ui/dialogs/offlinebasemapdialog.cpp:60-62`
- 函数：`PaleoMainWindow::runFolderImportAt` / `runFilesImportAt`
- 根因：来源对话框在 `exec()` 返回后核对了 `sessionId`，但随后的 `previewFolderAsync` / `previewFilesAsync` 回调没有再核对会话、只读或正在打开。回调直接弹出确认表，确认钩子调用 `importFolder` / `importFiles`。扫描期间主窗口不是模态的。
- 触发条件：任一导入按钮（井、测井、地震、边界、文件、工区文件夹）确认来源后，预览任务尚未返回时切换或关闭工程，然后在后弹出的确认表里点确认。
- Observed：确认表仍按扫描开始时的文件清单导入。同路径的古地理图导入和离线底图会在确认前拒绝「工程已变化」。这里没有这道门。
- Expected：预览回调若 `sessionId` 已变、工程只读或正在打开，丢弃预览并提示重新选择，不弹出可确认的导入表。
- 调用链：`DataPage::importRequested` → `ImportSourceDialog::exec`（此处有会话检查）→ `runFolderImportAt`/`runFilesImportAt` → `FolderImportWorkflow::preview*Async` → GUI 回调 `buildFolderConfirmDialog` → `DialogState::beginImport` → `importFolder`/`importFiles`。
- 排除比对：不是勾选被忽略。确认表在 `hooks.importAllWithWells` 存在时用 `rowSelected()` 把未勾选行放进 `skippedPaths`（`folderconfirm_preview.cpp:155-157`，`folderconfirm_results.cpp:69-74`）。`preferredType` 只给边界/地震扩展名设初始类型，用户改下拉后 `collectFolderTypeOverrides` 会按与分类器不同的当前值发 override。单文件向导在 `accept` 后读 `typeCombo`。本条是工程身份，不是类型下拉。
- 修复策略：回调开头比对 `sessionId`、`isProjectReadOnly()`、`isOpening()`；不匹配则 `PaleoNotify::warning` 后 return。确认钩子里再查一次，与古地理图第二段对话框同口径。
- 回归测试思路：打开工程 A，选一个文件开始文件夹预览，预览返回前换成工程 B，再点确认。B 的目录里不应出现 A 的预览文件；A 若已关闭也不应再被写入。

### D-3 / P2 / HIGH / 数据页撤销

- File:line：`src/ui/pages/datapage.cpp:118-130` 排队 `SetPrimaryCmd`；`src/ui/pages/dataops/dataopscommands.h:185-191` `SetPrimaryCmd::undo`
- 函数：`DataPage::wireDataOps` 内 singleShot；`SetPrimaryCmd::undo`
- 根因：#647 把「设为主文件」改成撤销命令，并在执行时记录当时的主文件。`undo()` 在 `m_prevPrimary` 为空时直接 return。`setLinkPrimary` 只会把目标行设为主并降级其他主行，没有对应的「取消主标记」。该角色第一次设主时，命令仍被 `push` 进栈，撤销移动栈索引，主标记保持为真。
- 触发条件：某井某角色尚无主测井，用户点设为主，然后撤销。
- Observed：撤销栈认为命令已撤销。关联仍是主文件。已有旧主文件时撤销能把主标记切回去，所以不是整条命令没入栈。
- Expected：撤销后恢复执行前状态。原先没有主文件时，该行 `isPrimary` 回到 false。
- 调用链：测井关联按钮 → `QTimer::singleShot(0)`（核对 catalog 指针与 `catalogPath`）→ `pushCommand(new SetPrimaryCmd(..., previous))` → 用户撤销 → `SetPrimaryCmd::undo` 因 `previous` 空而返回。
- 排除比对：不是 #590 悬垂 this。singleShot 在发信号按钮拆除后才入栈，并用 catalog 指针和路径丢掉切工程后的旧意图，这半段是对的。也不是栈漏了这条命令，是 undo 对「无前主」是空操作。
- 修复策略：undo 在 `m_prevPrimary` 为空时清除本次提升行的主标记（需要 catalog 有明确的 unset，不能靠再调 `setLinkPrimary`）。redo 仍提升目标行。
- 回归测试思路：无主测井的井上设主，撤销，断言该 role 没有任何 `isPrimary`。再重做，断言只有目标资产为主。已有旧主文件的用例保持现有期望。

## 已确认、不编号

- clang-tidy：`mappingworkbenchpage.h` 的 `buildEvidenceSection` / `buildEvidenceEditor` 参数名是 `group`，`mappingworkbenchpage_evidence.cpp` 定义参数名是 `parent`。HEAD 仍不一致。`readability-inconsistent-declaration-parameter-name`，warnings-as-errors。不单列 Finding。
- #590 悬垂 this / 裸 `QMessageBox`：本切片新对话框走 `PaleoNotify` 或状态标签。离线底图完成回调用 `QPointer`，销毁时 `requestCancel`。没有新的确定 use-after-free 链。
- 相分类草稿保存失败不会报成功。`runWorkbenchLifecycleFamily` 只在 `saveFacies` 返回 true 时 `markFaciesSaved()`；`runWorkbenchCommand` 在 `ok==false` 时显示 `error`，成功文案在 `else if`。`saveFacies` 失败不发 `faciesChanged`。
- 剖面定位（#647 R02）不用过期 id 打开另一份地震。`paleomainwindow_attach_validate.cpp` 用 payload 的 `asset_id` 过滤测线范围，候选不是恰好 1 个就提示并 return，不再回退「第一份地震」。工作台 `show`/`catalog` 同步使用点击时的层 id，失败不显示「已定位」。
- 导入确认表勾选和类型下拉会被执行。见 D-2 排除比对。井/测井按钮不强制类型，确认表显示分类器结果并可改；这是「下一步核对」而不是静默改类型。
- 离线底图完成后不会写到已关闭工程。`PaleoOfflineBasemap::importFile` 在发布前核对 `sessionId`、任务会话、只读、正在打开，以及底图三项设置未变；`projectAboutToClose` 请求取消。`closeProject` 在 `projectAboutToClose` 之后 `++m_sessionId`，`finished` 排队到 GUI，关闭先完成时发布会被丢掉。`configureNativeProjectGrid` 由 `mapreference::configure` 在临时 `QgsProject` 上试算，成功后才写回活工程，不是 worker 退出后贴到旧画布。
- DESIGN.md：本增量把默认折叠、参考图两列写进了规范，代码与该规范一致（`mappingreferencepanel.cpp` 两列网格；工作台样式/来源/分类默认折叠）。`projectsetupdialog` 路径行字面量 `8` 等于 `spacingSm`，像素不偏离。未发现要单列的视觉偏离。

## #645 归属（不占 Finding 编号）

### 27 vs 7，图例/渲染是否把模板层级画进用户图件

不画。`FaciesHierarchy::legend` 只保留 `codes` 里实际出现的编码，空组删除（`facieshierarchy.cpp:91-101`）。`facieshierarchyrenderer.cpp` 的 `apply` 用这份过滤后的图例建分类渲染器，缺席模板类没有符号、也没有要素可画。`paleomapfacies.cpp` 的 `normalizeFacies` 只写出源图已有的面，坐标转换不增加模板面。

27 vs 7 出在分类表大小，不是多画了面。`MappingWorkbench::facies` 对已存分类调用 `FaciesTaxonomy::upgrade`；`FaciesTaxonomy::library()` 是稳定默认集，`templates()` 注释写明不得替换工程分类。升级若把用户的 7 类扩成模板全表，compose 阈值个数仍是 `schema.size()-1`，测试若仍按 7 类期望 6 个阈值，就是期望过期或升级不该扩表，归 #645，不是本切片新的绘面缺陷。`legendTree` 会给已出现类的父级加图例标题行，那是图例层级，不是额外相面。

### 「岩性」vs「岩性道」、assetpaging auxiliary vs tops

本切片没有把岩性道标题改错的新差异。`wellsectionstyle.cpp` 道题注仍返回「岩性」，该文件不在 `d699694c..HEAD`。`datapreviewtabwelllog.cpp` 改的是分层底深来源（`wellTopsForLog` / `wellBases`，末段未知底深拉到曲线最大深度），不是道标题。井道工具条折叠（`panelsetup.cpp`）也不改题注。若 #645 失败句是标题字符串，更像既有文案「岩性」对测试期望「岩性道」，不是本增量写错类型。

assetpaging：本切片没有把 tops 资产改记成 auxiliary。`roleregistry.cpp` 新增的是井域 `reference` 和 `study_area` 的 `boundary`，与 tops 类型无关。`tst_assetpaging.cpp` 仍期望节点 `UserRole+2 == "tops"`、确认表文本 `"tops"`。确认表下拉的显示文本是 `folderTypeLabel`（tops 为「井分层」），data 才是 `"tops"`。若失败点是 `setCurrentText("tops")` 或比对显示文本，这是测试对着内部类型 id、产品对着中文标签，归 #645 的测试期望，不是导入按钮把 tops 收成辅助资料。导航树把挂在 `auxiliary` 实体上的资产收进辅助组（`datalist_tree.cpp`）是既有规则，本增量未改这段。

## 必查项结论

| 项 | 结论 |
|----|------|
| #647 草稿 | 保存失败不提示成功，不 `markFaciesSaved`。脏表按层位留在 `m_faciesDrafts`，工程重绑清空。未发现成功文案与失败返回值不一致。 |
| #647 定位 | 剖面定位拒绝 0 个或多于 1 个候选，不用过期 `asset_id` 打开别的地震体。工作台 show/catalog 同步。证据行带 `project_session`。 |
| #647 撤销 | 设主命令有入栈。第一次设主的 undo 是空操作，见 D-3。有旧主文件时 undo 能切回。 |
| 工程引导取消/校验失败 | 取消和 `finish()` 校验失败不 `create`。空白按钮加在途识别会改写意图，见 D-1。 |
| 导入按钮错类型/忽略勾选 | 确认表勾选和改类型会进入 `skippedPaths` / overrides。没有静默忽略。缺的是预览完成后的工程会话检查，见 D-2。 |
| 离线底图 / native grid 写错工程或画布 | 未发现。发布前对会话和底图设置做了比对；native CRS 先在临时工程试算再发布。 |
| 相图模板层级画进用户图件 | 未发现。归 #645 的是分类表大小/阈值个数。 |
| 新 Qgs 直连 | 本增量没有在 workflow、qgis、视图以外的新文件里新增 `Qgs*` include。`src/app/appcontext_layers.cpp`、`appcontext.cpp`、`main.cpp` 的 Qgs include 是既有的。`portableenvironment.cpp` 新调用的是 GDAL `OSRSetPROJSearchPaths` / `CPLSetConfigOption`，不是 Qgs。 |

## 覆盖清单

逐行（新文件或变更函数全文）：

- `src/ui/dialogs/projectsetupdialog.cpp`、`.h`
- `src/ui/dialogs/importsourcedialog.cpp`、`.h`
- `src/ui/dialogs/offlinebasemapdialog.cpp`、`.h`
- `src/ui/paleomainwindow_projectsetup.cpp`
- `src/app/portableenvironment.cpp`、`.h`
- `src/qgis/projectnativegrid.cpp`、`.h`
- `src/qgis/offlinebasemapvalidation.cpp`、`.h`
- `src/qgis/paleomapfacies.cpp` `normalizeFacies`
- `src/workflow/offlinebasemapimport.cpp`（底图完成路径）
- `src/workflow/projectsetup.cpp` `create` / `applySources`
- `src/ui/pages/mappingworkbenchpage_evidence.cpp`
- `src/ui/pages/mappingworkbenchpage_evidence_detail.cpp`
- `src/ui/pages/mappingworkbenchpage_schema.cpp` `setHorizon` / `wireFaciesUpdates` / `faciesRows` / `buildFaciesSchema` 变更段
- `src/ui/paleomainwindow_workbench_commands.cpp` 三个 family 与 `runWorkbenchCommand` 文案段
- `src/ui/paleomainwindow_project.cpp` `runFolderImportAt` / `runFilesImportAt`
- `src/ui/paleomainwindow_attach_data.cpp` `wireDataImportRequests`
- `src/ui/paleomainwindow_attach_validate.cpp` 剖面定位槽
- `src/ui/pages/datapage.cpp` 设主 singleShot
- `src/ui/pages/dataops/dataopscommands.h` `SetPrimaryCmd`
- `src/qgis/facieshierarchyrenderer.cpp` `legend` / `apply`
- `src/domain/facieshierarchy.cpp` `legend`
- `src/ui/datapreview/datapreviewtabwelllog.cpp` `displayedFormations` 与 `buildWellLogContent` 分层段

只看 diff、并读了变更函数（未逐行读宿主文件其余部分）：

- `constraintpage.cpp` / `.h` / `_defaults.cpp` / `_generate.cpp` / `_interpolation.cpp` / `_wellfactors.cpp`
- `datalist_importbuttons.cpp`、`datalist_batch.cpp`、`datalist.cpp`、`datalist_filter.cpp`、`datalist_select.cpp`
- `mappingeditbasis.cpp`、`mappingreferencepanel.cpp` / `.h`、`mappingworkbenchpage.cpp` / `.h`、`mappingworkbenchpage_references.cpp`
- `folderconfirm.cpp`（仅边界文案）；确认表勾选逻辑读了 `folderconfirm_preview.cpp` / `folderconfirm_results.cpp` 相关函数
- `projectmapsettingsdialog.cpp`、`paleomainwindow_paleomaps.cpp`、`paleomainwindow_workbench_evidence.cpp`
- `paleomainwindow.cpp` / `.h` / `_pages.cpp`（`showStartup`、`onProjectOpened`、`applyPageDockPolicy`）
- `previewmappage.cpp` `showLowResSnapshot`
- `qgisprojectservice_map.cpp`、`projectmapreference.cpp`、`qgisruntime.cpp` / `.h` 变更段
- `src/app/main.cpp`、`appcontext_layers.cpp` 变更段
- `src/ui/wellcomposite/panelsetup.cpp`、`src/catalog/roleregistry.cpp`（#645 文案/角色，只看 diff）

## 未核查区

- 未跑构建、ctest、未打开 #645 的失败日志原文。27 vs 7 和 assetpaging 的「测试过期 vs 产品扩表」只根据代码推断。
- `AppContext` 换工程时是否改绑 `DataImportService::m_catalog` 未逐行追完；D-2 的确定事实是回调不核对会话。
- `src/ui/paleomainwindow_workbench_wells.cpp`、`_editflow.cpp` 不在本增量 diff 里，未逐行重读。
- 主窗口其余附属文件（ribbon、docks、shortcut、webview、seismic 显示条折叠）只看了 diff，没有逐行读未改函数。
- `portableenvironment.cpp` 的 `OSRSetPROJSearchPaths` 是否拷贝字符串，未对 GDAL 源码核实；没有当成 Finding。
