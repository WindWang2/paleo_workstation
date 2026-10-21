# 审查报告 D — src/qgis 封装层 + src/ui（P1–P3 分批）

审查人：Agent D（只读深度代码审查）
基线：e6837e14（工作树 /home/kevin/projects/paleo_workstation-full-review-20261021）
日期：2026-10-21
范围：src/qgis/ 全部；src/ui/ 按 P1（paleomainwindow 家族、pages/、dialogs/、wellcomposite/、wellsection/）→ P2（seismicsection/、seismic3d/、datapreview/、layers/、maptools/）→ P3（correlation/、layout/、welltops/、ai/、杂项组件）。

## 总体结论

本轮在给定范围内做了逐文件 + 调用链追踪的深审（覆盖清单见文末），**新发现问题数量少**：
该区域整体工程质量非常高——147 项已知问题库 + 两轮审计 + 三个修复分支覆盖后，
大量典型缺陷模式（QPointer 缺失的异步回调、裸 QMessageBox、渲染器悬垂、
GL 资源不配对、世代号失效缺失）在本次复查中均见到**已修/已防**形态
（如 evidencemapadapter 的世代号+线程哨兵、previewmapcanvas 的三层记账回收、
qgislayerservice 的 #81/#419 destroyed 钩子、horizonmarkers 的 0-timer 延迟提交、
wellsectionscene_images 的 ImageDecodeHost 析构置空等）。

以下 4 项为本轮可论证的新发现（无一项与已知 147 项库或 20261020 已修清单同根因；
排除比对逐条附后）。宁缺毋滥：若干疑点经核实后主动撤回，撤回说明见「未纳入的疑点」节。

---

## Findings

### D-01 删除实体时未决链接产生幽灵回收条目，多角色同资产产生重复撤销命令
- **Severity**: P3 ｜ **Confidence**: VERIFIED ｜ **Module**: ui/pages（数据管理 · 实体 CRUD）
- **File:line**: `src/ui/pages/entitypanel.cpp:712-721`（recycle 分支）、对照 `:725-727`（detach 分支）
- **函数**: `EntityPanel::beginDeleteEntity(const QString &entityId)`
- **根因**: 「资产一并软删」分支遍历 `m_ctx.cat->linksForEntity(entityId)` 时**不过滤 `l.unresolved`**，
  而 13 行之后的「保留资产」分支显式过滤（`if (m_stack && !l.unresolved)`）——两分支口径不对称。
  对未决链接，`cat->assetById(l.assetId)` 返回默认空 `CatalogAsset`，
  于是压入 `SoftDeleteCmd(m_ctx, /*assetId=*/"", /*displayName=*/"", /*type=*/"", true)`。
  `RecycleBin::remove`（`src/ui/pages/dataops/dataopsmodel.h:416-429`）对空 id 照样入册
  （`m_removed.insert("")`、`m_entries.append` 空条目），而 `RecycleBin::load`
  （`dataopsmodel.h:388-393`）在重新装载时**跳过空 assetId 条目**——写进去的幽灵条目
  一次 save/load 往返后静默消失，会话内则以空白行出现在可回收清单
  （`RecycleBinDialog` 消费 `entries()`，displayName 为空）。
- 次生问题（同函数）：同一资产以多角色挂到同一实体时（linksForEntity 返回多条同 assetId 链接），
  recycle 分支对同一资产压入**多条 SoftDeleteCmd**——`RecycleBin::remove` 幂等所以数据无害，
  但一次用户操作在撤销栈里产生 N 条「移入可回收清单」条目，撤销粒度与用户心智不符。
- **触发条件**: 实体存在未决链接（文件缺失/链接未确认）或多角色同资产链接时，走
  「删除当前实体 → 资产一并软删」。
- **Observed vs Expected**: 观察：可回收清单出现空白行且重开后消失；撤销栈出现重复条目。
  期望：未决链接不产生回收条目（或至少过滤空 id）；同资产只软删一次（可先去重）。
- **影响面（调用链证据）**: `beginDeleteEntity` ← `entityDeleteButton`（entitypanel.cpp:302 构建，
  :709 `EntityDeleteDialog` 确认后进入）→ `m_stack->push(new SoftDeleteCmd(...))`
  （dataopsundo.h:51 push 即 redo）→ `RecycleBin::remove/save` → `RecycleBinDialog` 展示。
- **排除比对**: 已知库 1 的 D 系（D-1..D-24）无此条（覆盖 datalist_undo 导出、dataops 面板裸弹窗等）；
  02-excluded-files.txt 含 `dataopspanelops.h/dataopsmodel.h`（B 分支在修）但 entitypanel.cpp 不在清单内，
  且本问题根因（unresolved 过滤缺失）不在已知 147 项任何条目中。上轮已修清单亦无此项。
- **建议修复**: recycle 分支加 `if (l.unresolved) continue;`（与 detach 分支对齐）；循环前按
  `assetId` 去重。回归测试：构造含 unresolved 链接 + 双角色同资产的实体执行删除，断言
  `recycle->entries()` 无空 id 条目且撤销一次即完整恢复。

### D-02 HorizonChipBar::reloadHorizons 的 deleteLater 窗口内 findChildren 命中新旧两代 chip
- **Severity**: P3 ｜ **Confidence**: VERIFIED ｜ **Module**: ui（层位 chip 条 · 六页工作流公共组件）
- **File:line**: `src/ui/horizonchipbar.cpp:109-122`（reloadHorizons）、`:124-133`（chipNames）、
  `:141-145`（applyActive）、`:147-163`（applyAvailability）
- **函数**: `HorizonChipBar::reloadHorizons()`
- **根因**: 旧 chip 经 `layout()->removeWidget(chip); chip->deleteLater();` 摘除，但直到事件循环
  运行前仍是 `this` 的子对象；`buildChips()` 立即建新 chip 后，`applyActive/applyAvailability`
  （reload 尾部同步调用）与任何同步调用的 `chipNames()` 都会经 `findChildren<QToolButton*>()`
  拿到**新旧两代**。后果分两档：
  1. `chipNames()`（`tests/tst_chips.cpp:66/93` 的断言通道）按名字双计数，返回重复层位名；
  2. `applyActive/applyAvailability` 对将死 chip 无谓置 checked/enabled（视觉无害，因已摘出布局）。
- **触发条件**: 工程打开后 `reloadHorizons()` 被调用的同一事件循环步内再同步读 `chipNames()`
  或触发 `applyAvailability`（后者在 reload 尾部自调，必然发生——只是碰巧幂等）。
- **Observed vs Expected**: 观察：同步窗口内 chipNames 重复、状态操作对象含死对象。
  期望：reload 返回后查询即只反映新名单。
- **影响面**: 生产 UI 视觉不受影响（幂等 + 死 chip 已出布局）；实害在测试通道与未来任何
  依赖「reload 后立即查询」的新调用方。属低危一致性缺陷。
- **排除比对**: 已知库无 horizonchipbar 条目（147 项中 L 系无此项）；文件不在
  02-excluded-files.txt。
- **建议修复**: 改为成员 `QList<QToolButton*> m_chips` 显式记账（reload 时同步 delete 旧表），
  或 reload 内先 `delete chip`（此时无用户点击在飞，安全），查询函数遍历成员表。
  回归：tst_chips 在 reloadHorizons() 后立即 QCOMPARE(chipNames(), mappingHorizons())。

### D-03 地震相×测井相一致性报告每次完成都新建无模式对话框，重复运行堆积
- **Severity**: P3 ｜ **Confidence**: HIGH ｜ **Module**: ui/paleomainwindow_workbench（编图链联动）
- **File:line**: `src/ui/paleomainwindow_workbench.cpp:221-247`
- **函数**: `attachWorkbench` 内 `MappingWorkbench::consistencyReady` 处理器
- **根因**: 每次收到 `consistencyReady(report)` 都 `new FaciesConsistencyDialog(this)` +
  `dialog->show()`——无单例/复用/raise 既有窗口逻辑（全仓唯一创建点，已 grep 证实）。
  mappingworkbenchpage 的「地震相 × 测井相一致性报告」按钮可反复触发
  （`startFaciesConsistency` 每次异步完成再发信号）。
- **触发条件**: 用户不关闭第一次报告窗口，再次运行一致性检查。
- **Observed vs Expected**: 观察：第二个独立模式对话框弹出，与第一个并存（均父到主窗口，
  无泄漏，关闭前各自驻留）。期望：按应用其余「文档型结果面」惯例（如图件设计器、
  参考面板）复用同一窗口并刷新内容/raise。
- **影响面**: 弱。父到 `this` 生命周期安全；纯交互冗余。不违反 DESIGN.md 明文
  （DESIGN 未规定结果对话框复用），故仅作 UX 债登记。
- **排除比对**: 已知库无此项；文件不在排除清单（paleomainwindow_workbench.cpp 未被三分支触碰）。
- **建议修复**: 持 QPointer<FaciesConsistencyDialog> 成员，存在则 setReport+raise，否则新建。

### D-04 composeattribution::gridOf 的「首个栅格重建」路径缺 extent 非空校验，退化栅格导致下游除零
- **Severity**: P3 ｜ **Confidence**: SUSPECTED ｜ **Module**: qgis/composeattribution（图斑归因重放）
- **File:line**: `src/qgis/composeattribution.cpp:80-91`（gridOf 首栅格分支，只查
  `r->width()<=0 || r->height()<=0`）；对照 `:62-67`（frozen 路径 `FrozenGrid::valid()` 含
  `!extent.isEmpty()`）与 `:107`（点输入路径显式 `extent.isEmpty()` 拒绝）
- **函数**: `gridOf` → `replayAt`（`:522-525` `cw = extent.width()/cols` 后除法定位像元）/
  `replay`（`:663-672` 同式）
- **根因**: 三条网格来源路径里，唯「按首个栅格镜像重建」不校验 `r->extent()` 非退化。
  一张 provider 合法、width/height 正常、但地理范围宽或高为 0 的栅格（退化 geotransform），
  会得到 `usable=true`、`cw=0` 的网格；`replayAt` 中 `(point.x()-xMin)/cw` 为 inf，
  `int(std::floor(inf))` UB，后续像元判定失真而非如实拒算。
- **触发条件**: 归因目标成果无冻结网格（旧成果）+ 首个声明为栅格的输入是退化范围栅格。
  现实概率低（正常导出链不会产出零宽栅格），且对照面（composeRaster 主路径
  `mappingartifactwriter.cpp:632-638`）同样只查 width/height——一致地漏，属校验口径缺口
  而非可稳定复现故障，故定 P3/SUSPECTED。
- **Observed vs Expected**: 观察：退化输入下重放产生 inf/UB 而非 unresolved。
  期望：与其他两路同口径，`extent.isEmpty()` 时如实报「网格无法重建」。
- **排除比对**: 已知库 C 系/C.14（previewrasteranalysis nodata）不同根因；composeattribution.cpp
  不在 02-excluded-files.txt。
- **验证思路**: 单测构造 1×N 或 geotransform 像素宽为 0 的内存/临时 GTiff 作首输入，
  调 `replayAt` 断言返回 unresolved 而非越界 cell。
- **建议修复**: gridOf 首栅格分支补 `r->extent().isEmpty()` 拒绝（或在 replayAt/replay 入口对
  cw/ch 为 0 早退）。

---

## 未纳入的疑点（经核实撤回，留档防止后续重复排查）

1. `evidencemapadapter.cpp:334` `QgsGeometry geometry; geometry.fromWkb(bytes);` 疑似
   「静态函数返回值被丢弃」——核实 vendored QGIS 头（qgsgeometry.h:403-408）确认
   `void fromWkb(const QByteArray&)` 为**成员就地设置**重载，写法正确，非缺陷。
2. `nativeattributetablehost.cpp:107` 裸 `QMessageBox::question`（保存/放弃/取消）——
   核实 `attributetablepanel.cpp:50` 已注入 `setSaveDiscardAsk` seam 走
   `PaleoNotify::askSaveDiscard`（FD-004），裸调用是文档化的未接线降级路径，非违规。
3. `entitypanel.cpp` 其余 QMessageBox 命中均为 stale include（注释自述），无调用。
4. `attachFaciesDisplayControls`（paleomainwindow_workbench.cpp:584-611）lambda 捕获
   `controls` 原始指针表——核实 FaciesDisplayControl 全仓单点创建（ribbonpanels.cpp:571，
   随壳构建一次）、与连接 receiver `this`（主窗口）同生命周期，无悬挂窗口。
5. `qgisprojectservice_async.cpp` 取消路径下后台 QgsProject 可能由 GUI 线程 lambda 析构
  （线程亲和为 worker）——文件在 02-excluded-files.txt 且 #483 契约注释已自我登记该风险面，
  本轮未找到比注释更强的可证伪证据，不立新条（若修复分支落地后仍存，建议带 QGIS 源码复核）。
6. `HorizonMarkerSet::rebuild` 删除 items 快照中项——核实快照为 QList 值拷贝且 role 项
   互不为父子，无双删；grabber 复用下转换有 role+wellId 双数据键护栏。
7. `wellsectionscene_images` 后台解码回调——`ImageDecodeHost`（weak_ptr + 析构置空 self）
   + 场景守卫 + imageVersion 代际三重护栏齐全，安全。

## 排除比对汇总

- D-01..D-04 均逐条对照 02a-review-from-zero-known-findings.md（147 项 H/A/B/C/D/E/L1-L5）
  与简报第 3/4 条（上轮已修、上轮登记未修）清单：无同根因项。
- 本轮刻意复核的已知项（确认仍在、未重复立条）：correlationpanel D-06 UAF、
  layoutexportactions D-09 双上报（文件均在活跃修复分支清单内）。
- 已知问题在本次深读中被再次独立观察到「防护代码已存在」的位置（抽样）：
  previewmapcanvas FD-003 三层记账、qgislayerservice #81/#419、evidencemapadapter
  线程哨兵、mappingworkbench_impact X13 同步长操作（有台账登记）、
  constrainteditsession F17 单次扫描、composeattribution thresholdsValid 越界防护。

## 覆盖清单

**逐行深读（读全文或 >70% 含调用方追踪）**

- src/qgis/：composeattribution.{cpp,h}、mappingartifactwriter.cpp、qgisstyleservice.cpp、
  qgisprojectservice.cpp、qgisprojectservice_async.cpp、previewmapcanvas.cpp、
  qgislayerservice.cpp、qgislayerorganizer.cpp、qgislayerprofile.cpp、
  qgisconstrainteditsession.cpp、qgiscanvascontroller.cpp、evidencemapadapter.cpp、
  mapcanvaslink.cpp、crossplotmaplink.cpp、previewrendercache.cpp、mappingreferencecanvas.cpp、
  draftautosave.cpp、projectmapreference.cpp、nativeattributetablehost.cpp
- src/ui/：paleomainwindow_pages.cpp、paleomainwindow_project.cpp、paleomainwindow_status.cpp、
  paleomainwindow_workbench.cpp、paleomainwindow_workbench_commands.cpp、paleomainwindow_docks.cpp
  （前 330 行 + 关键连接）、pages/mappingworkbenchpage.cpp（1637 行全读）、
  pages/entitypanel.cpp（1620 行全读）、pages/composepage.cpp（前 400 行 + 网关逻辑）、
  pages/validatepage.cpp（前 240 行）、horizonchipbar.cpp、batchjobpanel.cpp（轮询域）、
  pages/dataopsundo.h、pages/dataops/dataopscommands.h（SoftDeleteCmd/BatchRestoreCmd）、
  pages/dataops/dataopsmodel.h（RecycleBin）、dialogs/depthanchordialog.cpp、
  dialogs/projectmapsettingsdialog.cpp、correlation/horizonmarkers.cpp（全读）、
  wellsection/wellsectionscene_images.cpp、wellsection/wellsectionscene_faults.cpp、
  wellsection/wellsectiondialogs.cpp（前 330 行）、wellcomposite/wellcompositecanvas.cpp（前 380 行）、
  layout/layoutundostack.cpp

**接口级 + 风险模式扫描（grep 命中逐点回读上下文）**

- src/qgis/ 其余全部：qgisprocessingservice、wellattributestore、standardelements、
  previewmaptools、mapbooklayout、geopatterns、faciespartitioneditor/tool、
  mappingfeatureidentity、paleomapfacies、faciesmaplegend、semanticlegend、topologicalindex、
  wellsectionmapband、seismicsectiontool、constraintcrs、manifestprojection、
  qgiseditingservice、qgislayoutservice、factorstylewriter、wellpointnavigation、
  facieshierarchyrenderer、layoutexport/factorcontour/paleomapsource/previewidentify/
  previewrasteranalysis/dataexportwriter（排除清单内文件，只对照已知项）
  ——扫描面：未守卫 first()/last()/at(0)、QTimer::singleShot 无接收者、
  QtConcurrent/QFuture、installEventFilter/removeEventFilter 配对、QMessageBox 直调。
- src/ui/ 其余：paleomainwindow_attach_{compose,constraint,data,mapping,models,predict,python,
  shell,validate}.cpp、paleomainwindow_{errorhub,faults,paleomaps,references,ribbon,sections,
  shortcuts,wellsection,workbench_editflow,workbench_evidence,workbench_wells}.cpp、
  paleodockmanager.cpp、taskpanel.cpp、webviewpanel.cpp、pages/ 其余、datapreview/、
  seismicsection/、seismic3d/、layout/、welltops/、ai/、edittools/、faults/、
  notifications/（PaleoNotify 降级路径）——扫描面同上，另查 findChildren 内 delete、
  this 捕获无接收者、eventFilter 泄漏、导出走 1× render() 的 D-11 同族路径
  （确认其余路径均走 grabCanvasImage(2.0)/exportPng，唯 dockwidget:501 为已知 D-11）。

**未覆盖/浅覆盖区域（如实声明）**

- seismic3d 着色器细节（seismic3dtf/colormap 的 GL 对象生命周期）只做了 Initialize/Cleanup
  配对检查，未逐行审 GL 状态残留（已知 D-13/D-14/D-15 所在文件均在排除清单，由修复分支覆盖）。
- wellsection/wellsectionpanel.cpp、fencewidget.cpp、wellcomposite 的 trackcompound/trackcurves/
  multiwellview/exportengine（已知 D-07~D-10/D-21/D-22/L5.3 所在文件）只做了已知项对照复核，
  未重新全量深读。
- datapreview/ 各 tab（已知 D-16~D-19 所在族）同样只对照复核。
- layout/ 的 layoutitempalette/panel/tree/templates、mapbookcontroller 只做接口级扫描。
- src/ui/pages/datalist*.cpp 家族（大部分在排除清单）未重读。

## 复核方法备注

- 未构建、未修改任何仓库文件（除本报告）。
- 证据均为静态读码 + 调用链追踪（grep 定位调用方/被调方后回读源码上下文）；
  D-04 的触发输入未实际构造（标 SUSPECTED 并给出验证思路）。
- vendored QGIS 头（qgsgeometry.h 等）以仓库外同版本族源码树核对签名。
