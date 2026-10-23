# 审查报告 E — #645 归属 + 增量测试/闸门

审查窗：`d699694c..5abc061a`（#630–#647）。CI 日志：`/tmp/opencode/ci-644/qtest-first-run/`（run 对应 `f3998bd4` / #644）。只读，未构建。

#645 症状不占 Finding 编号。下面「归属」给出测试过期 / 产品缺陷 / 环境判定，并引用产品代码。新缺陷只记闸门与被改写后不再能抓住的不变量。

---

## #645 归属

总判：13 个套件、22 个失败点里，**21 个是测试期望过期**（#637 把默认相库从 7 扩到 27、井表路由与派生版本、文案/标题变了，对应测试没改），**3 个是同一产品缺陷**（单因素页构造时抢先勾选，层位稍后到达不再恢复已保存提取），**1 个是环境/依赖**（在线底图像素；该路径窗内无产品改动）。

`tst_mappingworkbench.cpp` 已用 `saveFacies(..., FaciesCatalog::defaults().mid(0, 7))` 把数值夹具钉在七类上，注释写明原因。下面红的套件没做同样的钉扎，也没把计数改成 27。

### 1. 相分类从 7 扩到 27 — 测试期望过期

#637（`4d944207`）把 `resources/geology/facies-hierarchy.json` 的 `"code"` 从 7 增到 27。`FaciesCatalog::defaults()` 仍是 `FaciesTaxonomy::library()`（`src/domain/faciescatalog.cpp:112`），所以无工程分类时编图/图例读到的就是这 27 条。校验本身按 N−1 工作，没有算错。

| 套件 | 失败 | 判定 | 产品依据 |
|---|---|---|---|
| tst_composeattribution | `thresholdAttributionUsesFrozenVersionsAndRealParameters` `tests/tst_composeattribution.cpp:215` | 测试期望过期 | 夹具传 6 个阈值。`MappingWorkbench::facies()` 无已存 schema 时返回 `FaciesCatalog::defaults()`（`src/workflow/mappingworkbench.cpp:445-455`）。`composeRaster` 要求 `thresholds.size() == codes.size()-1` 且严格递增，否则拒绝（`src/qgis/mappingartifactwriter.cpp:615-621`）。27 类需要 26 个阈值。日志原文就是这条拒绝。 |
| tst_impactrecompute | 6 失败：`tests/tst_impactrecompute.cpp:101,228,321,384,438,517` | 同上 | 同一 `compose("D61", …, thresholds{20,25,30,35,40,45})`。321/384/438 的 `QVERIFY2` 没把 `error` 打进消息，但调用式与 101 相同，拒绝原因同一条。 |
| tst_standardelements | `hierarchicalFaciesLegendFollowsRendererAndLayout` `tests/tst_standardelements.cpp:207` Actual 27 / Expected 7 | 测试期望过期 | `entries = FaciesHierarchyRenderer::apply(layer, FaciesCatalog::defaults(), …)`。条目数等于库大小，库已是 27。后续 headings/symbols 硬编码 3/7 也会跟着过期，被第一处 QCOMPARE 挡住。 |
| tst_standardelements | `rasterLegendAggregatesParentsAndUsesActualSolidColors` `tests/tst_standardelements.cpp:255` | 测试期望过期 | `cells` 按 schema 逐码填充（现 27 个），`raster()` 仍写 `cols=7, rows=1`。`MappingArtifactWriter::raster` 在 `cells.size() != cols*rows` 时返回 false，错误是「预测网格为空或尺寸不匹配」（`src/qgis/mappingartifactwriter.cpp:114-117`）。不是栅格驱动坏了。 |
| tst_wellcomposite | `testWellLegendUsesTheSharedThreeLevelHierarchy` `tests/tst_wellcomposite.cpp:431` Actual 5 / Expected 2 | 测试期望过期 | 图例先灌入整个 `FaciesTaxonomy::library()`，再 `place()` 井上的自定义段（`src/ui/wellcomposite/wellpositionlegendwidget.cpp:368-373`）。库内相级现为三角洲相、湖泊相、近岸水下扇相、浅海相（4）。夹具「河流相/河道亚相/边滩」三级都非空，`place()` 原样保留（`src/domain/faciestaxonomy.cpp:69-75`），成为第 5 个顶层。旧库只有三角洲相 + 自定义河流相，所以期望是 2、叶子 8。 |
| tst_exportpipeline | `optionalTemplatesExportThroughTheRealFaciesRenderer` `tests/tst_exportpipeline.cpp:184` | 测试期望过期 | `FaciesTaxonomy::templates()` = library + `facies-templates-extra.json`（`src/domain/faciestaxonomy.cpp:55-60`）。「水下分流河道」在 hierarchy 是 code 1，在 extra 里又以 code 59/70/81 出现在扇/曲流河/辫状河三角洲下。`resolve()` 对同名不同 code 返回空并要求完整路径（`src/domain/faciestaxonomy.cpp:178-180`）。这是 #637 后的设计口径，不是解析器把唯一名字弄丢。生产代码没有 `resolve(templates(), 短名)` 调用方；本失败只在测试。 |

UI 占位文案已经按新口径写了：「N 个微相需填写 N−1 个递增阈值」（`src/ui/pages/mappingworkbenchpage_schema.cpp:165-166`）。修测试应要么 `saveFacies` 钉七类（workbench 已有先例），要么把计数/阈值改到 27/26，短名解析改走完整路径。不要放宽 `composeRaster` 的 N−1 检查。

### 2. 井口必填列放宽 — 测试期望过期

| 套件 | 失败 | 判定 | 产品依据 |
|---|---|---|---|
| tst_projectparsers | `rejectsInvalidWellHeadRequiredColumns` `tests/tst_projectparsers.cpp:157` Actual 3 / Expected 1 | 测试期望过期 | #630 把 KB、TotalDepth 从拒收改为置空：`src/io/wellfileparsers.cpp:173-174`（`reject=false`，失败则 `qQNaN()`）。X/Y 仍拒收（171-172 行 `reject=true`）。日志与此一致：第 2 行 KB「missing」置空仍接收，第 5 行 TD 哨兵置空仍接收，X/Y 哨兵与 inf 拒收，故 6 行里接收 3。Name/X/Y 仍是必填。测试名仍按旧必填口径。 |

残留（不另开 Finding，随 #645 修测试时一并看）：接收行的 KB/TD 以 NaN 写入井实体（`src/io/dataimport_wells.cpp:145-146,175-176` 直接赋值，无 `isfinite` 门）。这是放宽后的存储形态，不是解析器把非法坐标放进来。

### 3. 空井位文件仍拒收，文案变了 — 测试期望过期

三处断言都要求消息含「没有可用数据行」/「井表没有可用数据行」。文件内容都是只有注释的井位表（`#WellHead File From SMI` + `# no rows`）。

| 套件 | 失败 | 判定 |
|---|---|---|
| tst_import | `folderImportWellHeadsFirstAndIsolatesFailures` `tests/tst_import.cpp:1933` | 测试期望过期 |
| tst_import | `importFolderRowReimportsSingleFile` `tests/tst_import.cpp:2942` | 同上 |
| tst_panels | `folderConfirm_importSummaryAndRowRetry` `tests/tst_panels.cpp:2141`（夹具 `tests/tst_panels.cpp:2014-2018`） | 同上 |

产品链：`.dat` 且类型 `well_head` 不再走 `parseWellHeadDevice`，一律 `readWellTable`（`src/io/welltableimport.cpp:23-26`）。`readWellDelimitedTable` 跳过 `#` 行，表头为空则错误是「井位文本没有可用表头」（`src/io/welltabletext.cpp:32-43`）。`readImportWellInput` 在 `table.error` 非空时立刻返回该错误，到不了第 44 行的「井表没有可用数据行」。

语义没坏：`tst_import.cpp:1931-1932`（类型 well_head、outcome Failed）和 `tst_panels.cpp:2079`（单元格含「失败」）都已通过，只是子串过期。旧解析器对「无数据行」的专用句被新表头识别挡在前面。修测试去匹配「没有可用表头」，或让空注释文件在统一入口仍产出「没有可用数据行」——后一种才是产品改动，当前拒绝本身是诚实的。

### 4. 假 xlsx 岩屑不再静默挂井 — 测试期望过期

`tst_import::wellAttachmentLinksCoreCuttingsLabAndReference` `tests/tst_import.cpp:3012`。

夹具把 JPEG 魔数写进名为 `.xlsx` 的文件（`tests/tst_import.cpp:3000-3006`），本来只为避开 SHA 去重、验证目录角色。#633 起，路径像岩屑表（后缀 xlsx/xml/csv/tsv/txt 且角色/文件名命中）会调用 `storeImportedCuttings`（`src/io/dataimport_geodata.cpp:363-366`）。该函数要求 `readCuttingsFile` 解析出井段，否则整次导入失败（`src/io/cuttingsimport.cpp:16-18`）。`readXlsx` 对非 ZIP 返回「不是 OOXML 工作簿：不是 ZIP 容器」（`src/io/outsourceworkbook.cpp:407-413`）。日志两段原因都来自这条链。

同函数里的 JPG pyramid 警告（`cannot open raster`）是假 JPEG 的副作用，不是本断言的失败点。

这不是「真岩屑表被读坏」。夹具从未是工作簿。若产品仍想「坏表也挂井、岩性派生失败单独降级」，那是另一项政策；当前实现明确 `return *error` 中止导入。本 CI 点按测试期望过期处理：给一份最小可解析 csv/xlsx，不要用假字节再要求 `importProjectFile` 成功。

### 5. mini 夹具版本 21≠17 — 测试期望过期

`tst_import::miniFixtureProjectMeetsImportDomainInvariants` `tests/tst_import.cpp:3152` 与 `tst_mkprojectfixture::importFullChain` `tests/tst_mkprojectfixture.cpp:117`。Actual 21，期望 `kExpectedImports` 17（`tests/tst_mkprojectfixture.cpp:33`）。

资产数断言在上一行（`tests/tst_import.cpp:3151`、`tst_mkprojectfixture.cpp:84`）没有失败，说明没有多登记资产。多出的 4 个版本与派生写入对得上：

- 井口 `well/井位坐标.xml` 经 `convert=well_head` 转成 `.dat` 再导入（`src/app/mkproject.cpp:500-501`）。`.dat` + `well_head` 走 `readWellTable`，非空 `normalized` 由 `storeNormalizedWellTable` 再写一条 DERIVED `well_data.dat`（`src/io/dataimport_file.cpp:363`，`src/io/welltableimport.cpp:47-66`）。+1。
- mini manifest 三份 `2.4岩屑录井数据/A*_岩屑录井.csv`（`tools/reference/mkproject/mini/manifest.json:89-95`）。挂井时 `storeImportedCuttings` 各写一条 DERIVED `cuttings.csv`（`src/io/cuttingsimport.cpp:27-40`）。+3。

分层 `.dat` 不进 `isWellTableContainer`（不含 dat），不产生规范化版本。17+1+3=21。注释「每资产恰一个 RAW 版本」过期；RAW 仍应是 17，多的是 DERIVED。修测试应按 stage/fileName 分开数，不要把 N−1 阈值或拒收口径放宽来凑 17。

### 6. 岩性道标题 — 测试期望过期（根因在窗前）

`tst_datapreview::wellLogPreviewShowsCuttingsLithologyTrack` `tests/tst_datapreview.cpp:322`。Actual「岩性」，Expected「岩性道」。

道已经造出来了（失败点在标题 QCOMPARE，不是 `hasLithology`）。产品新建岩性道一律 `tr("岩性")`：`src/ui/wellcomposite/paneldata.cpp:188`、`src/ui/wellcomposite/defaultlayout.cpp:58`。同标题被 `tst_welllog_defaultview.cpp`、`tst_wellcomposite_visual.cpp` 钉死。标题改动在 #626（`b301f2e8`），早于本增量窗；#644 日志仍红。不是 #630–#647 把岩性道弄丢。

### 7. 批量改型对话框 — 测试期望过期（根因在窗前）

`tst_assetpaging::batchTypeOverrideRefreshesViewsAndKeepsFilter` `tests/tst_assetpaging.cpp:628`。Actual「auxiliary」，Expected「tops」。

`BatchTypeDialog` 用 `folderTypeLabel(type)` 当显示文本、type id 放 item data（`src/ui/pages/dataopspanelops.h:152-153,166`）。`folderTypeLabel("tops")` 是「井分层」（`src/ui/dialogs/folderconfirm.cpp:52`）。测试 `setCurrentText("tops")`（`tests/tst_assetpaging.cpp:611`）匹配不到显示名，下拉停在 `vocab.sort()` 后的第一项。词表含 auxiliary 且按字母排序（`src/ui/pages/datalist_batch.cpp:63-68`），第一项就是 auxiliary，`chosenType()` 读 `currentData()` 得到 auxiliary，表类型列如实显示。

这是 #621（`5a841299`）把下拉从裸 id 改成中文标签之后的驱动方式过期，`dataopspanelops.h` 在本窗无改动。用户选「井分层」仍会写入 tops。不要把产品改回用显示文本反推类型（`folderconfirm.cpp:130` 已写明不靠文本反推）。修测试用 `findData("tops")`。

### 8. 在线底图像素 — 环境/依赖

`tst_onlinebasemap::tileProviderReadsLocalPixels` `tests/tst_onlinebasemap.cpp:258`。图层创建、CRS `EPSG:3857`、`block` 有效都过了；`value(0,0)` 为 nan，不是 120。

`src/qgis/onlinebasemap.cpp` 在 `d699694c..HEAD` 无改动（最后改动 #627）。`createLayer` 只要 wms provider 元数据存在就建 wms/xyz 层，只有该层无效才回退 GDAL TMS（`src/qgis/onlinebasemap.cpp:318-331`，`gdalXyzUri` 在 124-160 行）。wms 层可以 `isValid()` 但不同步解码 `file://` 瓦片，`block()` 给出 nodata（nan），GDAL 回退不会发生。本测试自己 `QgsApplication::initQgis()`（`tests/tst_onlinebasemap.cpp:325-327`），不走 #634 的 `QgisRuntime::initialize` / `invalidateCache`。链接集仍是 `paleo_qgis`（`cmake/paleo-tests.cmake`），窗内没有把 portable CRS 初始化链进这个二进制。

因此不把 nan 判成 #630–#647 的产品回归。若 CI 的 QGIS 带 wms 插件且该插件把本地 xyz URI 标成有效层，这条同步像素断言就会红。修的时候应让 `file://` 模板走 GDAL TMS，或测试直接读 `gdalXyzUri`；那是 #645 的修复方向，不是新单。

### 9. 单因素逐井表不恢复 — 产品缺陷

三处同一根因，不要拆成三张单：

| 失败 | 位置 | 现象 |
|---|---|---|
| `modeSwitchAwayAndBackRestoresSavedExtraction` | `tests/tst_wellattributes.cpp:655` | 勾砂厚后 `wellFactorTable` 行数 0，期望 1 |
| `factorRowSwitchRestoresPerFactorWellTable` | `tests/tst_wellattributes.cpp:716` | 勾选后行数 0，期望 2 |
| `crossFactorSwitchDoesNotLeakContourSuggestion` | `tests/tst_wellattributes.cpp:769` | 等值距仍是构造默认 20，期望 (70−10)/6=10 |

调用链：

1. #647 在构造末尾无条件勾选第一行（`src/ui/pages/constraintpage.cpp:208`）。此时测试尚未 `setHorizons`，层位下拉是空的。
2. 勾选走 `itemChanged` → `restoreWellFactorDisplay()`（`src/ui/pages/constraintpage.cpp:269-275`）。`restoreWellFactorDisplay` 要当前层位文本（`src/ui/pages/constraintpage_wellfactors.cpp:435-448`）。层位为空则恢复不到已保存提取。
3. 之后 `setHorizons` → `currentIndexChanged` 只做 `invalidateWellFactors` + `refreshWellFactorFields` + `refreshFactorResults`（`src/ui/pages/constraintpage_wellfactors.cpp:153-157`），**不**调用 `restoreWellFactorDisplay`。
4. 测试再 `setCheckState(Checked)`。行已经是 Checked，Qt 不发 `itemChanged`，恢复再次不跑。
5. 表保持 0 行。`applySuggestedContourInterval` 无行则不改间距；构造默认是 20（`src/ui/pages/constraintpage_generate.cpp:240`）。所以第三处 Actual 20 / Expected 10。

真实用户路径相同：页先建好并勾上第一因素，壳随后 `setHorizons`。已保存提取不会出现，等值距停在 20，除非用户取消再勾一次。测试期望的契约（选中因素 + 已有层位必须恢复）仍然是对的，不是过期断言。

`tests/tst_geologyux.cpp:60` 现在把「构造后第一行已勾选」钉成绿，它不覆盖恢复。井属性三例仍红，所以这条回归没有被新测试掩盖。修产品：层位变化和构造末尾勾选之后都要走 `restoreWellFactorDisplay`；不要只改测试去接受空表。

---

## Findings

### E-1 / P2 / VERIFIED / catalog + tools

- File:line：`tools/check_role_tables.py:42-47,59-66`；`src/catalog/roleregistry.cpp:57-59`；`src/catalog/catalogroles.cpp:44`；`src/io/dataimport_geodata.cpp:337-338,386-391,430-435`；`src/catalog/datacatalog_links.cpp:24-33,39-46`
- 函数：`check_pair` / `RoleRegistry::builtinTable` / `importAuxReferenceFamily` / `roleDiagnosis`
- 根因：#643 的一致性闸只比角色 **key 集合**。`reference` 两边都有 key，闸门绿。词表侧把 `reference` 只挂在实体类型 `well`；UI 表把它放在 `misc` 组。导入零匹配附件仍建 `auxiliary` 实体并 `link.role = reference`。`roleDiagnosis` 发现实体类型不在 `entityTypes` 里，且 `forEntity("auxiliary")` 没有同名定义，于是写「角色与实体类型不符: reference 于 auxiliary」并 `qWarning`。#644 日志里多个**通过**的用例仍在刷这句（`tst_import`、`tst_panels`、`tst_datapreview`）。闸门声称要挡住的「装载含 reference 链接就刷未知角色」只换了文案，没有消失。
- 触发条件：导入参考资料 / GeoJSON / 未识井附件，走辅助实体 + reference。或以后只在一侧用带空格的 `RoleDef{ QStringLiteral` / `add( QStringLiteral` 登记——正则要求 `add(QStringLiteral` 与 `RoleDef{QStringLiteral` 之间零空白（`check_role_tables.py:46-47`），自检夹具也是零空白，格式化后会漏报。`[a-z_]+` 同样看不见带数字的角色名。同名跨实体（`other` 在 well/seismic_survey/planned 三处）被 set 折成一个 key，planned 是否有 UI 槽也看不见。
- Observed：闸门退出 0；辅助参考链接 note/日志仍是类型不符。
- Expected：要么词表给 `reference` 补上 `auxiliary`（与 `importAuxReferenceFamily` 一致），要么导入改挂井且闸门比较 `(role, entityType)` 而不只是 key；正则至少允许空白，自检要有「空格写法漏登仍红」和「key 相同但挂接域不同仍红」两条。
- 调用链：`importAuxReferenceFamily` → `addLink` → `annotateRoleDiagnostics` → `roleDiagnosis`。闸门不读这条链，只扫两个 cpp 的字符串。
- 排除比对：不是 #645 的套件失败，也不是 #587 开库先写后验。#643 自己引入的闸门漏报。
- 修复策略：补挂接域或改导入；检查器改为抽取 `(role, entityTypes)` / `(role, group)` 并声明允许的跨域复用（interpretation、other 已有注释）。不要把「角色与实体类型不符」从 `qWarning` 里删掉来消音。
- 回归测试：导入一份参考资料 XML，断言 `invalidRoleLinks()` 为空；检查器自检加入空格登记与「key 相同、实体类型不同」。

### E-2 / P3 / VERIFIED / ui/datapreview

- File:line：`src/ui/datapreview/previewmappage.cpp:898-901`；`tests/tst_previewmap_page.cpp:402-409`；调用方 `src/ui/datapreview/datapreviewtabhorizon.cpp:535-540`（image/mapproduct 同型）
- 函数：`PreviewMapPage::showLowResSnapshot` / `TestPreviewMapPage::lowResSnapshotShowsOverlay`
- 根因：窗前实现是 `renderSnapshot(320)` 后立刻 `showPreviewOverlay`（`d699694c` 的 `previewmappage.cpp:898-904`）。现实现只 `primeRenderCache()`，缓存未命中就直接 `refresh()`，不再生成低清帧。测试被改成先 `store()` 一张白图再调用，于是 `overlayVisible()` 仍绿。注释写了「不再同步等待一次低清渲染」，但调用方注释仍是「D6.1 低清整图先上」。
- 触发条件：首次打开层位/图件预览，渲染缓存为空。`showLowResSnapshot` 被调用时 overlay 不会出现，直到正式渲染完成。
- Observed：无缓存时函数不铺占位图；测试因自行灌缓存而通过。
- Expected：若 D6.1 仍有效，无缓存也应铺一帧低清图，测试不得预灌缓存。若 #647 故意取消同步低清，应改调用方注释，并加一条「缓存未命中时 overlay 保持隐藏、refresh 仍被调用」的断言，而不是用灌缓存保住旧断言的外形。
- 调用链：预览页 `singleShot` → `zoomToFullExtent` → `primeRenderCache`（空）→ `showLowResSnapshot` → 再次 `primeRenderCache`（仍空）→ `refresh()`。
- 排除比对：不是 #645 套件（`tst_previewmap_page` 在 #644 日志里未失败）。不是 #590 的 UI 阻塞项；这里是占位契约被测试改写盖住。
- 修复策略：二选一，不要两头都留。保留 D6.1 就把 `renderSnapshot` 加回，并删掉测试里的预灌。取消 D6.1 就改调用方注释，测试分开覆盖命中/未命中。
- 回归测试：不预灌缓存调用 `showLowResSnapshot`，断言与选定契约一致。

### E-3 / P3 / HIGH / tests/support

- File:line：`tests/support/paleo_test_tempdir.h:44-55`
- 函数：`PaleoTempDir::restoreWritePermissions`
- 根因：析构在 `QTemporaryDir` 删除之前，对根下 `QDir::Files` 做 `QFile::setPermissions(... | WriteOwner)`。迭代器默认不跟随目录符号链接，所以不会走进沙箱外的目录树。但文件符号链接会被当成文件列出；`QFileInfo::isWritable()` 与 `QFile::setPermissions` 在 POSIX 上跟随链接（`chmod` 作用于目标）。指向沙箱外、且目标当前不可写的符号链接，会在清理时给目标加上属主写位。
- 删除面：`QTemporaryDir::remove` / `QDir::removeRecursively` 对符号链接是解除链接本身，不递归进目录链接，不删除链接目标。**不会删到沙箱外。**
- 触发条件：夹具在临时目录内放置指向沙箱外只读文件的符号链接，然后 `PaleoTempDir` 析构。现有 `tst_import` 的 `escape.las` 指向刚写好的可写文件，`isWritable()` 为真，这条路径当前不会 chmod 它。
- Observed：清理可以改沙箱外文件的权限位；不会删除沙箱外文件。
- Expected：只改 `root` 之内的真实文件。符号链接跳过，或 `setPermissions` 前用 `canonicalFilePath` 确认仍在 `root` 下。
- 调用链：测试析构 → `restoreWritePermissions` → `QFile::setPermissions` → 目标文件。随后 `m_dir` 析构只删临时目录树。
- 排除比对：不是 #641/#642 的残留目录问题本身；那次清理引入了跟随链接的写权限恢复。不在 #645。
- 修复策略：遇到 `isSymLink()` 直接跳过。不必为了删只读文件去改链接目标。
- 回归测试：临时目录内放一条指向目录外只读文件的符号链接，析构后该文件权限不变、文件仍在。

---

## 闸门核对（非 Finding）

- `tools/file-budget-baseline.txt`：窗内 9 处删除/替换全部是下调或删行（`mappingworkbenchpage.cpp:file` 1637→1200，`fn:MappingWorkbenchPage` 628→395，`fn:renderEvidenceSnapshot` 整行删除）。`renderEvidenceSnapshot` 现为 `src/ui/pages/mappingworkbenchpage.cpp:1114-1200`（87 行），低于 200，删基线正确。没有新增超标条目，没有上调冻结值。这是拆分后下调，不是补登。头注没有为本批单独写归因，但方向符合「只缩不涨」，不记 Finding。
- `tools/ui-size-baseline.txt`：条目数 11→8（头注与实列对齐），各冻结值只降不升（`mappingworkbenchpage.cpp` 1637→1200 等）。没有新超标文件补登。
- `.github/workflows/ci.yml`：增量只有 RelWithDebInfo 改为 `-O2 -g1`，为链接期磁盘。diff 中无 `continue-on-error`、无 `if: false`。没有削弱失败即红。
- `tools/audit_well_facies.py`：手工审计脚本，未接入 `ci.yml`。`catalog.parents[2]` 对 `<project>/artifacts/metadata/catalog.sqlite` 指向工程根，路径深度是对的。短名歧义返回 `None` 后记 issue（code 为空且 defaults 能唯一解析时降为 warning）。不会把「水下分流河道」多归属静默当成通过。不记 Finding。
- `cmake/paleo-test-infra.cmake`：只注册 `role_tables` / `role_tables_selftest`，无 `DISABLED`。漏报在脚本口径（E-1），不在 ctest 注册。
- `cmake/extra-import-ux.cmake` / `paleo-modules-ui.cmake` / `paleo-tests.cmake`：把新 TU 和 `tst_portableenvironment` 等接进既有测试宏。没有把失败套件标成跳过。

增量测试里核对过、不构成掩盖的改动：

- `tst_mappingworkbench.cpp` / `tst_evidencee2e.cpp` 的 `defaults().mid(0, 7)` 是显式七类夹具，注释写了用途；默认 27 类路径仍由未改的 compose/impact/standardelements 抓住（所以那些套件是红的，不是被改绿）。
- `tst_faciescatalog.cpp`、`tst_mappingpages.cpp` 把 7/43 改成 27/91，并断言「浊积水道」短名必须解析为空。这是收紧，不是放宽。
- `tst_shortcuthelp.cpp` 把已知遮蔽清单改成 `pairs.isEmpty()`，对应 `layers.remove` 收到图层树 `WidgetWithChildrenShortcut`（`src/ui/shortcuts/shortcutcatalog.cpp` 窗内 4 行）。变异用例 `mutationDuplicateKeyGoesRed` 仍在。不是把冲突断言改成 contains。
- `tst_import.cpp` / `tst_folderimport.cpp` 的窗内 diff 几乎全是 `QTemporaryDir` → `PaleoTempDir`。失败断言（文案、版本数、假 xlsx）没有被改弱，所以 #644 仍然红。
- `tst_geologyux.cpp` 钉住构造后第一行已勾选、confidence 行不出现。它不断言恢复已保存提取，但也没有把 `tst_wellattributes` 的恢复断言改掉。

---

## 覆盖清单

逐行或失败点精读：

- `tests/tst_assetpaging.cpp`（`batchTypeOverrideRefreshesViewsAndKeepsFilter`）
- `tests/tst_composeattribution.cpp`（阈值用例与 `composedPolygons`）
- `tests/tst_datapreview.cpp`（岩性道用例）
- `tests/tst_exportpipeline.cpp`（`optionalTemplatesExportThroughTheRealFaciesRenderer`）
- `tests/tst_impactrecompute.cpp`（夹具 + 六处 compose 调用）
- `tests/tst_import.cpp`（四处失败 + 窗内 diff）
- `tests/tst_mkprojectfixture.cpp`（版本计数）
- `tests/tst_onlinebasemap.cpp`（`tileProviderReadsLocalPixels`）
- `tests/tst_panels.cpp`（`folderConfirm_importSummaryAndRowRetry`）
- `tests/tst_projectparsers.cpp`（`rejectsInvalidWellHeadRequiredColumns`）
- `tests/tst_standardelements.cpp`（两处图例失败）
- `tests/tst_wellattributes.cpp`（三处失败）
- `tests/tst_wellcomposite.cpp`（图例用例）
- `tests/tst_geologyux.cpp`（全文）
- `tests/tst_previewmap_page.cpp`（`lowResSnapshotShowsOverlay`）
- `tests/tst_shortcuthelp.cpp`（遮蔽用例）
- `tests/support/paleo_test_tempdir.h`（全文）
- `tools/check_role_tables.py`（全文）
- `tools/audit_well_facies.py`（全文）
- `tools/file-budget-baseline.txt` 头注 + 窗内数值 diff
- `tools/ui-size-baseline.txt` 窗内 diff
- `.github/workflows/ci.yml` 窗内 diff
- `cmake/paleo-test-infra.cmake`、`cmake/extra-import-ux.cmake`、`cmake/paleo-modules-ui.cmake`、`cmake/paleo-tests.cmake` 窗内 diff

接口级（diff 与关键断言，未逐行）：

- `tests/tst_evidencee2e.cpp`、`tests/tst_evidenceselection.cpp`、`tests/tst_mappingworkbench.cpp`、`tests/tst_faciescatalog.cpp`、`tests/tst_factorworkflow.cpp`、`tests/tst_import_preview.cpp`、`tests/tst_folderimport.cpp`、`tests/tst_catalogroles.cpp`、`tests/tst_entityview.cpp`、`tests/tst_roles.cpp`、`tests/tst_ui.cpp`、`tests/tst_uxtheme.cpp`、`tests/tst_webviewpanel.cpp`、`tests/tst_stratigraphicweb.cpp`、`tests/tst_portableenvironment.cpp`、`tests/tst_projectcoordinates.cpp`

产品侧为判定读过的调用链：`mappingartifactwriter.cpp` compose/raster、`mappingworkbench.cpp` `facies()`、`faciestaxonomy.cpp` `resolve`/`templates`、`wellfileparsers.cpp` 井口行、`welltable.cpp`/`welltabletext.cpp`/`welltableimport.cpp`、`cuttingsimport.cpp`/`cuttingsdoc.cpp`/`outsourceworkbook.cpp`、`dataimport_file.cpp`/`dataimport_geodata.cpp`/`dataimport_wells.cpp`、`datalist_batch.cpp`/`dataopspanelops.h`/`folderconfirm.cpp`、`paneldata.cpp`/`defaultlayout.cpp`、`wellpositionlegendwidget.cpp`、`constraintpage.cpp`/`constraintpage_wellfactors.cpp`/`constraintpage_generate.cpp`、`onlinebasemap.cpp`、`catalogroles.cpp`/`roleregistry.cpp`/`datacatalog_links.cpp`、`previewmappage.cpp`、`mkproject.cpp` 转换段、`facies-hierarchy.json` / `facies-templates-extra.json` 重复微相名。

## 未核查区

- 未逐行读 `tests/tst_projectsetup.cpp`（+469）、`tests/tst_paleomaplibrary.cpp`（+215）、`tests/tst_offlinebasemap.cpp`（+161）、`tests/tst_panels_dataops.cpp`、`tests/test_prepare_comp_data.py`。diff 抽样未见 `QVERIFY(true)` 或把失败改成更弱 contains；真实数据用例用 `QSKIP` 守环境变量，CI 默认不跑。
- 未跑 `check_role_tables.py` / ctest（简报禁止构建）。角色集合相等是对照源码手工归并的。
- `tst_onlinebasemap` 的 nan 未在本机复现；判定依赖「窗内该文件无 diff + createLayer 的 wms 优先分支」。
- Qt `removeRecursively` 不跟随目录链接是按 Qt 6 实现契约写的，本树没有 Qt 源码可逐行核对，故 E-3 的删除面标 HIGH 而不是对 Qt 内部 VERIFIED。跟随 `chmod` 的写权限面是本文件代码加 POSIX 语义。
- #638 Windows 退出崩溃无本机环境，不臆造。
