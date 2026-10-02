# Goal Loop ledger — well-logset

- 日期：2026-10-02；分支 `goal/well-logset-20261002`，从 `origin/master` `eaaf46d` 创建；worktree `.worktrees/well-logset`。
- 七项 Oracle：并集核、重名/canonical、导入序、消费迁移、性能、诚实跳过、文档。未跑不标通过。

## 轮0 — 接口定案

勘察（本分支工作区，不再另开调查）：

- `EntityAssetLink`（`src/catalog/datacatalog.h`）有 `role` / `isPrimary` / `ordinal` / `unresolved`。`addLink` 在「已决且 isPrimary」时把同 (entityType, entityId, role) 的其他主关联降为非主。不变量保持：不多主。
- `linksForEntity` 对全部角色做 ordinal 稳定排序（同 ordinal 保持入库序）。空 entityId 返回空集。`well_log` 在 `RoleRegistry::defaults()` 中 `ordered=true`，`maxCount=0`（不限成员）。词表只描述、不拒收。
- `LasParser::parseHeader(path, LasHeaderInfo, error)` 只扫到 ~A 段头，`curveNames[0]` 为深度索引。失败返回 false 并写 error。不读数据体。
- 导入写路径是 staging `DataCatalog *cat`（`dataimportservice.cpp` 约 815 行）。LAS 已决分支目前无条件 `link.isPrimary=true`（约 1107 行），因而触发降级。未决分支不设 entityId。`attachLink` 仍会把被挂链接升主并降级旧主——本方向不改。
- `next*Id` 纪律：在 owner 线程上按当前状态取 max+1，值写进随后的 `addLink` 参数，journal 重放同一条 link。ordinal 不另开序列、不单独 `++mutationSeq`（`addLink` 自己记账）。
- 读面放 `src/services/welllogset.h`（数据层），不塞进 `ProjectDataFacade::assetFilePathFor`：该函数按角色返回一条主文件路径，tops / time_depth 仍是单主文件。

锁定签名：`WellLogSet::wellLogFiles` / `wellCurveIndex`，告警走 `WellLogWarnings`。冲突口径见头文件注释。

证据：读 `datacatalog.h/.cpp`、`lasparser.h`、`roleregistry.cpp`、`dataimportservice.cpp`、`crossplotsources.cpp`、`projectdata.cpp`、`petrophyscomputeservice.*`、`propertymodelworkflow.cpp`、`sectionworkbench.cpp`、`entitypanel.cpp`、`datalist.cpp`、`datapreviewtabs.cpp`。

## 轮1 — 并集读面

`wellLogFiles` / `wellCurveIndex`：只 `parseHeader`。排序 ordinal、versionNumber、versionId。
跨文件重名：主文件有该列则原名 + canonical，其余 `@completeBaseName`；主文件没有该列则全部加别名且无人 canonical。同文件重名保留，第二条起 `#列号` 并警告。缺文件/坏头跳过。

证据（cdb `-g -G -c g;q`，PATH 以 `paleo-qgis-deps\Library\bin` 为首，Qt 6.11.2）：

```
PASS   : TestWellLogSet::oracle1_ordersFilesAndUnionsCurvesWithoutDepth()
PASS   : TestWellLogSet::oracle2_renamesCrossFileDuplicatesAndFollowsPrimary()
PASS   : TestWellLogSet::oracle6_skipsMissingUnparsedAndUnresolved()
PASS   : TestWellLogSet::ordersByOrdinalVersionNumberThenVersionId()
PASS   : TestWellLogSet::doesNotElectPrimaryWhenMarkedFileIsMissing()
Totals: 10 passed, 0 failed, 0 skipped, 0 blacklisted, 892ms
```

`ordersByOrdinalVersionNumberThenVersionId` 的第四份版本号用 1。`currentVersion` 的缺省 versionNumber 是 1，存 0 选不中。

## 轮2 — 导入序

连续两份已决 LAS：第一份保持主、ordinal 0，第二份非主、ordinal 1。catalog 里没有的井仍走未决，ordinal 0。同内容新 SHA 版本仍降旧主（原回归）。文件夹导入里 `0A1.Las` 保持主，后到的 `zzA1b.las` 非主。

```
PASS   : TestImport::consecutiveResolvedLasKeepsFirstPrimary()
PASS   : TestImport::consecutiveUnknownLasStayUnresolved()
PASS   : TestImport::newShaVersionDemotesPreviousPrimaryLink()
PASS   : TestImport::folderImportWellHeadsFirstAndIsolatesFailures()
Totals: 6 passed, 0 failed, 0 skipped, 0 blacklisted, 2749ms
```

## 轮3 — 消费迁移

岩石物理批处理能读到只在非主文件上的 NPHI。属性建模 `requestFromCatalog` 的曲线集是并集，RT 只在文件 B 上仍命中。剖面井轨画出非主文件上的 SP。

```
Totals: 5 passed, 0 failed, 0 skipped, 0 blacklisted, 253ms
```

（`tst_welllog_consumers`：init + 三条消费断言 + cleanup。）

## 轮4 — 交会、综合图、设主文件

交会清单对重名 GR 给出原名与 `@基名` 两条。实体面板「设为主文件」把对应链接升主（`setLinkPrimary` 用 `links()` 下标）。`tst_panels` 角色行：12 行，well_log 按 ordinal 排成主文件、未决、成员、成员。

```
Totals: 4 passed, 0 failed, 0 skipped, 0 blacklisted, 399ms
```

（`tst_welllog_ui`。）

```
PASS   : TestPanels::dataPage_entityViewRoleSlotsRender()
PASS   : TestPanels::dataPage_entityViewRefetchesAfterCatalogChange()
Totals: 4 passed, 0 failed, 0 skipped, 0 blacklisted, 463ms
```

## 轮5 — 性能与真工区

```
QINFO  : TestWellLogSetPerf::wellCurveIndex_100x3_under500ms_and_linearRatio() BASELINE wellCurveIndex_100x3_ms = 172.863
QINFO  : TestWellLogSetPerf::wellCurveIndex_100x3_under500ms_and_linearRatio() BASELINE wellCurveIndex_ratio_100_over_10 = 11.095
PASS   : TestWellLogSetPerf::wellCurveIndex_100x3_under500ms_and_linearRatio()
SKIP   : TestWellLogSetPerf::realArea_headerIndexOrSkip() PALEO_REAL_PROJECT_AREA not set — real-area well logset skipped
Totals: 3 passed, 0 failed, 1 skipped, 0 blacklisted, 513ms
```

本机 `PALEO_REAL_PROJECT_AREA` 未设置。跳过是实测结果，不记成真工区通过。说明见 `docs/progress/well-logset.md`。

层检查：`python tools/check_layering.py --strict` → 检查通过：无层违规。
`python tools/check_i18n.py` → 文案入口无裸字面量。

## 修正 — 岩石物理别名（`09729c8`）

并集显示名（`RHOB@file`、`RHOB#列`）按词干送进 `LasAliasMap`。精确词干优先于家族别名，`DEN` 不再抢先占住后出现的真 `RHOB`。表达式里唯一词干命中时，用那份文件做深度网格。没有真 `RHOB` 时，`DEN` 仍可写成 `RHOB`。

cdb，Qt 6.11.2 运行库在 PATH 最前，`tst_welllog_consumers`：

```
PASS   : TestWellLogConsumers::petrophys_readsRtFromNonPrimaryFile()
PASS   : TestWellLogConsumers::petrophys_phiDensityUsesAliasedRhob()
PASS   : TestWellLogConsumers::petrophys_expressionPrefersRealRhobOverDen()
PASS   : TestWellLogConsumers::petrophys_expressionDrivesOnAliasedRhob()
PASS   : TestWellLogConsumers::propertymodel_requestUsesUnion()
PASS   : TestWellLogConsumers::sectionworkbench_showsNonPrimaryCurve()
Totals: 8 passed, 0 failed, 0 skipped, 0 blacklisted, 546ms
```

## 收尾构建

`cmake --build build --parallel 8`（vcvars64，`INCLUDE` 前插 `paleo-qgis-deps/Library/include` 以找到 `sqlite3.h`）退出码 0（约 59s，主体是重链接）。紧接着再跑一次得到 `ninja: no work to do.`。这次日志里没有新的 `warning C` / `error C`。`tst_panels` 重编译时出现的 `dataopsmodel.h` C4804 是原有告警。本地 `CMAKE_CXX_FLAGS` 里的 qscintilla / QGIS ui 头路径只在这台机器的构建缓存里，不进提交。

构建之后用 cdb（Qt 6.11.2 运行库在 PATH 最前）复跑：

```
tst_welllogset        Totals: 10 passed, 0 failed, 0 skipped, 809ms
tst_welllog_consumers Totals: 8 passed, 0 failed, 0 skipped, 555ms
tst_welllog_ui        Totals: 4 passed, 0 failed, 0 skipped, 371ms
tst_welllogset_perf   BASELINE wellCurveIndex_100x3_ms = 163.127
                      BASELINE wellCurveIndex_ratio_100_over_10 = 8.195
                      SKIP real area（PALEO_REAL_PROJECT_AREA 未设置）
                      Totals: 3 passed, 0 failed, 1 skipped, 510ms
```

`tst_panels` 里 `dataPage_setPrimaryLink` 原先按 `setPrimaryButton` 查找。资产表对 `well_log` 用的是 `setWellLogPrimaryButton`（文案「设为主文件」）。测试改为找这个按钮并仍断言点击后主链接对调。复跑 `dataPage_setPrimaryLink`、`dataPage_entityViewRoleSlotsRender`、`dataPage_entityViewRefetchesAfterCatalogChange`：Totals 5 passed，386ms。

`tst_import` 全表在 cdb 下 50 passed / 1 failed / 2 skipped。失败的是既有压力测试 `manyConcurrentImportsDoNotDeadlock`：`assets().size()` 已等于 12，只有 `imported` 信号计数是 10。脱离调试器单跑三次，一次 12/12 通过，两次信号计数 10。本方向的连续 LAS 导入用例在全表里是通过的。不把这个信号计数波动记成井曲线并集失败。

全量 `ctest -j1` 在这台机器上不能整表绿灯：`tst_runtime` 断言 `C:\Users\wangj.KEVIN\paleo-qgis-prefix/resources/srs.db` 不存在；若干链接 QGIS 的测试在进入 `main` 之前加载 `iconv.dll` / `qgis_gui` 时以 `0xc0000139` 退出（脱离 ctest 直接跑 `tst_layerservice.exe` 同样如此）。上一轮串行 ctest 在约 31/178 处被中止。这两项是本机 QGIS 前缀和 DLL 装载问题，不是本分支的断言失败。真工区 SKIP 见轮 5。

