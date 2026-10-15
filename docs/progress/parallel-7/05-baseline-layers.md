# S1 图层生命周期审计（任务域 A 统一图层身份/生命周期 + B 跨页面显示一致性）R0 审计

审计基线：worktree `paleo-wt-goal7-qgis-layers`，分支 `goal7/qgis-layers-layout-20261014`，基线 `b0520429`（= origin/master）。全部论断以代码实证为准；文档声称与代码冲突处在 C 组标注。

## 链路与现状（调用图/状态机/数据流，逐环 file:line 证据）

### ① 声明 → 实例化（manifest → QgsProject）

- **声明落库**：`QgisLayerService::declare`（src/qgis/qgislayerservice.cpp:72-110）→ `LayerManifest::upsert`（src/metadata/layermanifest.cpp:103-143，sqlite `layer_declarations` 表，`INSERT OR REPLACE`；只读实例/非主线程未走写队列均如实失败）。失败语义：空 layerId / 清单写失败 → false + error。
- **source 变更替换**：旧实例在编辑中 → 拒（:85-92）；否则先 `removeMapLayer` 再重实例化，**重建失败仅 qWarning 不回滚声明**（:96-107，注释明示语义）。
- **实例化**：`instantiate`（:112-203）：manifest `find` 读失败 ≠「无声明」（:133-147 分开报错）→ 按 type 分派（mbtiles 走 `offlineBasemap`，raster→gdal，其余→ogr）→ `isValid` 失败即失败（:165-171）→ 非 mbtiles 恢复栅格 CRS（:173，`restoreRasterCrs` 在 PR #437 保护文件内）→ `paleoLayerId`/`title` 在 `addMapLayer` 前落（:175-179）→ `paleoCreatedAt` 仅首实例化落（:195-198）→ `trackInstance` 挂 `destroyed` 精准摘除（:346-363，#81 同款保护）。
- **缓存失效**：`cleared` 清空、`layersRemoved` 摘除（:47-59）；`projectOpened` 时 `purgeDanglingInstances` + `adoptProjectLayers`（:64-69, 365-406）收编 .qgz 恢复层（#287）。m_instances 是 QPointer 守卫的裸指针缓存，project 持有所有权。
- **层位物化/释放**：`setActiveHorizon`（:437-488）= 释放其他层位 + 摘孤儿（声明已删但仍实例化）+ 物化目标层位；读失败 → 整体不动（:440-441）。释放时编辑中的矢量层经 `m_editSvc->rollbackEdit` 回滚（busy 标记随会话释放，Issue #27 口径；未注入时降级直连 rollBack）。`instantiateHorizon` 失败逐个收集经 `horizonInstantiationFailed` 上抛（:274-295），UI 在树外以 ErrorHub warning 卡呈现（src/ui/layers/layertreepanel.cpp:120-133）。

### ② 树归位（QgsProject → QgsLayerTree）

- `QgisLayerOrganizer`（src/qgis/qgislayerorganizer.cpp）：`legendLayersAdded`/`layerInstantiated` 双触发 `placeLayer`（:90-107，幂等）；三分区 `SharedData（00_Data 置顶）/HorizonGroup（paleoHorizon 凭据组，按 mappingHorizons 浅→深序）/SharedFlat（平铺区，底图 rank=1000+ 垫底）`，`Unmanaged` 无声明层透明（:114-140, 217-318）。归位 = clone+insert+remove 先插后摘（:64-74，保勾选/属性/引用；同父重排 +1 修正）。自定义绘制顺序生效时 `slotIntoCustomOrder` 按树序找位（:184-213），`reorganize()` 末尾把自定义序镜像回树序自愈坏序（:409-420）。
- 全量规整触发点：`projectOpened` 后 AppContext（src/app/appcontext.cpp:655-658）。接线序：QgisLayerService 构造先于 AppContext 大 lambda（appcontext.cpp:176 vs 405），故收编（adopt）先于规整（reorganize），.qgz 恢复层可被归位。
- 空占位组：有声明的空层位组保留（懒加载留名），无声明且无子节点才剪（`pruneVacantGroups` :433-455）。

### ③ 页面主题（QgsLayerTree → page:* 主题 → canvas）

- `QgisLayerProfileService::applyPageProfile`（src/qgis/qgislayerprofile.cpp:105-149）：主题不存在 → `stageTreeVisibility` 按档案表摆树（声明驱动 `paleoLayerId`→decl.group，旧组名经 `canonicalize` 折算；predict 并入当前层位约束层；同名树组按组内受管层结果同步勾选）→ `createThemeFromCurrentState` 定格 `insert`。主题已存在 → `applyPrunedTheme`：剔除悬空记录 `update` 回写后 `applyTheme`，空记录 = 全隐藏语义（:283-323，修剪行为有 tst_layerplatform.cpp:583-617 实证：不在记录内的层重放后被隐藏）。`00_Data` 共享层在 page:* 应用/定格时被强制可见并回写主题（:218-281）。
- 失败语义：`stageTreeVisibility` 清单读失败 → false，不建主题、不改 `m_currentPage`（:169-177）；未接线 model/project → false。`data` 页只记录页码不碰画布（:118-123）。
- 消费面：切页 `showPage` → `applyCurrentPageProfile`（src/ui/paleomainwindow_pages.cpp:99-101 → paleomainwindow.cpp:218-224，kPageIds 守卫）；换层位后重放有三路排队（paleomainwindow.cpp:143-151 无守卫 + paleomainwindow_docks.cpp:405-421 有守卫去抖 + 切页本身），全部排队到事件循环尾，保证「先换层位（实例化/释放）再应用主题」。版面孔 `setLayoutMapTheme` 钉 `followVisibilityPresetName`（qgislayerprofile.cpp:393-405；壳侧 `pinLayoutTheme` paleomainwindow_attach.cpp:766-776）。

### ④ 保存/重开

- 保存：`PaleoProjectStore::saveAll` → `writeProject`（src/qgis/qgisprojectservice.cpp:321-390，PR #437 保护文件，只读引用）：`declarationProvider` 读清单失败或 `ManifestProjection::embedDeclarations` 失败 → **整体写失败**（:339-354，防静默丢未实例化声明）；temp+原子 rename；`projectWritten` 后壳侧 `setDirty(false)`（paleomainwindow.cpp:163-166）。
- 重开：`openProject` → `read` → `projectOpened`（qgisprojectservice.cpp:198-251）→ AppContext 原地重绑 `LayerManifest`（appcontext.cpp:477-481）；**仅当 meta sqlite 不存在**时从 .qgz 内嵌副本 rehydrate，存在（哪怕被清空）永不覆写（:470-503）→ `refreshBasemaps`/井位层刷新 → `reorganize()`（:655-658）→ 主窗 `onProjectOpened` → `showPage(lastPage)` → 重放当前页档案（paleomainwindow.cpp:389-483）。主题随 .qgz 往返有 roundtrip 测试（tst_layerplatform.cpp:620-674）。
- 清单 sqlite 侧：`removeMany` 事务化（layermanifest.cpp:150-182）；连接名按 路径+线程+读写 隔离（:26-32）；`title` 列按需补（:61-74）。

### ⑤ 层位 chip 切换时序（问题④）

chip 点击（src/ui/horizonchipbar.cpp:88-104）：编辑中拒切（带原因）→ `selection->setActiveHorizon(h)`（先广播）→ `layerSvc->setActiveHorizon(h)`（物化/释放，`horizonReleased` 早于 `m_activeHorizon` 落位）→ chip 高亮。三处层服务消费都靠**排队**读到终态：indicator 刷新（layertreepanel.cpp:108-119）、档案重放（docks 405-421）、页下拉跟随（paleomainwindow.cpp:143-151）。其余两入口同口径双拨：定位器（paleomainwindow_attach_shell.cpp:141-158）、演化播放器（src/ui/evolution/evolutionplayerpanel.cpp:141-143）。**例外见发现 A-1**。

## 可观察现行行为清单

1. 声明与实例化分离：declare 只写清单；`setActiveHorizon`/`instantiateHorizon` 才物化；未实例化层位在树里有占位组。验证：`tst_layerservice::instantiateHorizonMaterializesOnlyTarget`。证据：qgislayerservice.cpp:72-110, 274-295；qgislayerorganizer.cpp:423-431。
2. 实例化失败「树/画布无迹 + 信号上抛」：失败层不进工程，UI 经 `horizonInstantiationFailed` → ErrorHub warning 卡。验证：`tst_layerservice::instantiateHorizonSurfacesFailures`；layertreepanel.cpp:120-133。
3. 档案首建定格、重放修剪：page:* 主题只在首次应用时按档案表摆树；此后应用仅修剪+回放，**不在记录内的层被置隐藏**（含新实例化的层）。验证：`tst_layerplatform::horizonSwitchPrunesDanglingRecords`（:614-616 断言 T2 约束层被隐藏）。
4. `00_Data` 共享层（wells/survey.area 及一切 horizon 空 + 根组 00_Data 声明层）在任何 page:* 应用/定格时被强制可见并回写主题。验证：`tst_layerplatform::sharedDataAlwaysVisibleInPageProfiles`（:698-743）。证据：qgislayerprofile.cpp:218-281。
5. 树位权威是声明而非拖拽：用户拖/移在会话内保持，工程重开 `reorganize()` 收敛回 canonical 序；Unmanaged 层不被移动不被挤位。验证：`tst_layerorganizer::userDragNotReorganized/unmanagedLayerUntouched`。证据：qgislayerorganizer.cpp:217-318, 396-421；docs/progress/structural-enping-layertree.md:138-139（与代码一致）。
6. manifest 是唯一层集权威：.qgz 只持久化实例化层 + 内嵌声明副本；meta sqlite 存在时内嵌副本只读兜底（重hydrate 仅缺 sidecar 场景）。验证：`tst_layerservice::manifestSurvivesReopen`；appcontext.cpp:470-503；qgisprojectservice.cpp:335-354。
7. `page:*` 主题服务层硬拒重命名（版面地图项钉名防悬空）；空名/同名/重名也拒。验证：`tst_layerplatform` rename 拒绝矩阵。证据：qgislayerprofile.cpp:362-378。
8. 复制图层清空 `paleoLayerId` → 副本成 Unmanaged，不回查声明、不进档案。验证：`tst_layertreepanel`。证据：layertreepanel.cpp:534-551。
9. 剪枝守恒：切层位后重放档案，主题记录里无悬空 layer（`r.layer()!=nullptr`）。验证：tst_layerplatform.cpp:603-610。

## 发现分组

### A. 新发现（经核实未被 Issue/PR/TODOS 占用）

**A-1 数据图谱「打开保存版本」只拨 SelectionContext 不拨 QgisLayerService —— 两个「激活层位」真源分叉（M）**

- 现象：`workspaceRequested` 流（数据图谱 → 打开保存版本/对比）只调 `m_selection->setActiveHorizon(declaration.horizon)`（src/ui/paleomainwindow_workbench.cpp:478），不调 `m_layerSvc->setActiveHorizon`。而全库所有其他入口都是双拨（chip：src/ui/horizonchipbar.cpp:99-102；定位器：src/ui/paleomainwindow_attach_shell.cpp:154-157；演化播放器：src/ui/evolution/evolutionplayerpanel.cpp:141-143）。SelectionContext 是纯广播枢纽，不回写 layerSvc（src/linkage/selectioncontext.cpp:41-54）。
- 证据与影响面：分叉后三处读到旧值——① predict 档案「当前层位约束并入」用 `m_layerService->activeHorizon()`（src/qgis/qgislayerprofile.cpp:179-180, 205-210）；② 图层树「未激活」灰显 indicator 同口径（src/ui/layers/layertreepanel.cpp:703-731）；③ 层位懒加载状态本身。可观察后果：chip 条显示 H 已激活，而新打开的 H 层位图层在树里挂「该图层属于层位 H（未激活）」灰显 indicator，predict 页档案并入的是旧层位 H0 的约束层；分叉一直持续到用户再点一次 chip（chip 路径无条件调 `m_layers->setActiveHorizon`，horizonchipbar.cpp:101-102）才愈合。
- 处置：**修复点文件在 PR #437 保护清单内且超出本方向白名单 → 留给 Issue Agent / #437 方向**（见 05-baseline.md §4）。

**A-2 page:* 主题重放会隐藏「主题创建之后才出现的无声明临时图层」，与头注释契约相悖（M）**

- 现象：服务头注释承诺「落在树根、无 declaration 的临时图层（用户手加）不动其可见性」（src/qgis/qgislayerprofile.h:26-29），且 `stageTreeVisibility` 确实跳过它们（src/qgis/qgislayerprofile.cpp:189-193）——但这只在主题**首次创建**时成立。此后任何重放走 `applyPrunedTheme` → QGIS `applyTheme`（:313），对不在记录内的层一律置隐藏（隐藏语义有 in-repo 测试实证：tst_layerplatform.cpp:614-616 断言「不在 predict 主题里 → 隐藏」），`applyPrunedTheme` 只对 00_Data 共享层有豁免（:315-320），无声明层没有任何豁免。
- 影响面：用户在编图页中途手加一个临时参考层（默认勾选可见），切到别的页再切回来（或换层位触发排队重放），临时层被静默取消勾选——与文档承诺相反，且无任何提示。现有测试只覆盖首次创建路径（tst_layerplatform.cpp:448-453 的 tempAtRoot/tempInPrediction 断言发生在主题创建后的同一次 apply 内）。
- 建议验证方式：扩展 `tst_layerplatform`——`applyPageProfile("compose")` → 加无声明临时层（勾选）→ `applyPageProfile("validate")` → 再回 `"compose"` → 按契约断言临时层仍勾选（当前实现下预期红）。修复方向：重放路径对无 `paleoLayerId` 层恢复/保持勾选。
- 估计规模：M（语义决策 + 服务层小改 + 测试）。

**A-3 LayerTreePanel 在逐节点循环里反复全表读清单（sqlite readAll × N）（S，性能）**

- 现象：`refreshIndicators` 对每个图层节点在循环内调 `m_layerService->declared()`（src/ui/layers/layertreepanel.cpp:706-735，特别是 :722），而 `declared()` = `LayerManifest::all()` → `readAll` 一次完整 `SELECT … ORDER BY layer_id`（src/qgis/qgislayerservice.h:36；src/metadata/layermanifest.cpp:197-225）——N 个节点 = N 次全表扫描。同面板 `currentLayerGroup` 在每次选区变化/右键时亦全表读（layertreepanel.cpp:514-521）。触发频率：每次 legendLayersAdded/layersRemoved + 每次层位切换排队刷新（layertreepanel.cpp:108-160）。
- 影响面：图层/声明规模上去后（数百层）树面板交互与层位切换的 SQLite 往返线性放大；纯 UI 性能，无正确性问题。
- 建议验证方式：单次 `refreshIndicators` 的 sqlite 语句计数（测试钩子或 `MetaStore` 连接包装计数），断言 = 1 次 readAll；或先测量 200 声明 × 100 节点的耗时基线再决定。修复方向：循环外取一次 `declared()` 建 QHash（qgislayerorganizer.cpp:228-232 已有同款回查表写法可照搬）。
- 估计规模：S。

### B. 已占用（只列归属）

- `qgislayerservice.cpp:217-243 removeDeclarations` 空 id 守卫 → **Issue #419**（src/qgis 五项低危之一；注意 b0520429 已在 :227-231 落了守卫注释与空 paleoId 跳过，归属仍归 Issue Agent，不在本审计建议修复）。
- 图层树栅格「面运算（等厚/体积）」右键动作 → **Issue #369**（src/ui/layers/layertreepanel.cpp:336-345）。
- 协克里金工作线程调 `QgisLayerService::instantiate` → **Issue #352**。
- 预览图同步 grab 落盘 → **Issue #376**。
- `src/qgis/qgisprojectservice.cpp`（保存/重开链的写路径与收养逻辑）→ **PR #437** 保护文件；`mappingartifactwriter.cpp`（`restoreRasterCrs` 实现侧）、`mappingfeatureidentity.*`、`evidencemapadapter.*`、`qgisconstrainteditsession.*` → **PR #437**。本审计仅引用其接口，未提修复建议。
- `src/workflow/mappingworkbench*.*`、`mapversioncontroller*.*` → **PR #437**。

### C. 已修复/与文档不符但不构成缺陷

- `docs/progress/mapping.md:67-70` 称 renameTheme 为 insert-then-remove 且「QgsMapThemeCollection 无原生 rename API」——已被 `qgislayerprofile.cpp:362-378`（原生 `renameMapTheme` + page:* 硬拒）取代，`TODOS.md:433` 已有对账记录。纯文档历史快照。
- `docs/progress/structural-enping-layertree.md:31` 写「先摘后插」——与代码相反（实际先插后摘：qgislayerorganizer.cpp:64-74；layertreepanel.cpp:419-422 注释亦明确）。文档笔误。
- `purgeDanglingInstances`（qgislayerservice.cpp:365-385）与 `adoptProjectLayers`（:387-406）在 `projectOpened` 上重复收编 .qgz 恢复层（前者已含 isValid 过滤版收编，后者无过滤再收一遍）。冗余但幂等无害。
- mapping.md:88「页面主题被删后 applyPageProfile 按档案表重建（自愈）」与代码一致（qgislayerprofile.cpp:130-140），非问题。

### D. 低价值不建议做

- **chip 切换双排队重放**：`paleomainwindow.cpp:149-151`（无守卫）与 `paleomainwindow_docks.cpp:405-421`（有守卫）每次切层位至多两次 `applyCurrentPageProfile` → 两次幂等 `applyTheme` + 两次 canvas refresh。合并需跨 TU 共享去抖位，收益仅为省一次幂等回放。
- `QgisLayerService::declaration` 吞掉清单读失败（qgislayerservice.cpp:210-215，无 error 出参），与「无声明」不可区分；消费面均为只读展示（layerpropertiesdialog.cpp:72-86），影响低。
- `LayerManifest::forHorizon` 无错误通道（layermanifest.cpp:227-243），读失败静默返回空；主路径 `setActiveHorizon` 前置 `readAll` 已拦截（qgislayerservice.cpp:440-441），仅直连 `instantiateHorizon` 的旁路（如验证侧）理论上可静默归零。
- `ensureSharedDataVisible` 回写的是 `m_currentPage` 的主题（qgislayerprofile.cpp:270-279）：若将来有人 `captureCurrentAsTheme("page:别页")` 会写错对象——现有全部调用点都传 `currentPage()`，无活路径触发。

## 建议的非回归验证场景（无缺陷时的守恒验证）

1. **全链 roundtrip 守恒**：新建工程 → 声明层位 H1/H2 各两层（不同 group/horizon）+ wells/survey.area → 切 H1 → 四页 `applyPageProfile` 各一遍 → 保存 → 重开 → 断言：manifest 行数不变、`.qgz` 内嵌声明副本与 manifest 一致、`page:*` 主题全部在场且记录无悬空、树三分区（置顶共享/层位组序/平铺区）与重开前一致、当前页档案重放后可见集合一致。（现有 `tst_layerservice::manifestSurvivesReopen` + `tst_layerplatform::themesSurviveQgzRoundtrip` 的组合扩展。）
2. **层位切换往返守恒**：chip H1→H2→H1 后断言：实例集合与首次一致（`isInstantiated` 对拍）、全部 `page:*` 主题记录 `r.layer()!=nullptr`、indicator 状态（灰显集合）与首次一致、编辑中三入口（chip/定位器/演化播放器）拒切文案不变。（现有 `activeHorizonSwapsInstances` + `horizonSwitchPrunesDanglingRecords` 的组合扩展。）

---

补充说明（给主 Agent）：① A-2 依赖 QGIS `applyTheme` 对「不在记录内的层置隐藏」的语义——vendored QGIS 源码不在树内，但 in-repo 测试 tst_layerplatform.cpp:614-616 已对该语义在 vendored 4.2 上做了实证断言，据此外推到无声明层只差一步推理，落地前先跑建议的验证用例定生死。② 层契约合规性：本次范围内文件头三行「// 层：」标记齐全；视图层 include 白名单（layermanifest.h 属 metadata 六头之一、qgis/* 不在禁列）未见违规。
