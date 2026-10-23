# 审查 A — 数据面（d699694c..HEAD）

审查窗：`d699694c..5abc061a`。只读。未改源码、未构建。

## Findings

### A-1 / P1 / VERIFIED / io+catalog 提交

- File:line：`src/io/projectcoordinates.cpp:104-113` `projectSegyFile`；`src/io/projectcoordinates.cpp:71-84` `projectRasterFile`；`src/io/dataimport_coordinates.cpp:26-30` `prepareSeismicCoordinates`；`src/io/dataimport_geodata.cpp:210-215` / `:505-511`；`src/io/ingestplan.cpp:914-926` `rollbackFailedRow`；`src/io/dataimport_session.cpp:307-324` `moveStagedFiles`
- 函数：`projectSegyFile` / `projectRasterFile` / `rollbackFailedRow` / `moveStagedFiles`
- 根因：坐标转换在 `addVersion` 之前把成品路径上的文件写出来，失败时不删除；行回滚只收集 journal 里已 `AddVersion` 的受管版本目录。这些文件名不是 `.partial`，批量提交会把它们搬进工程目录，且没有版本行。
- 触发条件：工程 `georeference.kind==crs`，文件夹/工程引导批量导入地震或层位；道头转换中途失败，或栅格 warp 失败 / `remove` 成功但 `rename` 失败。单文件 `importProjectFile` 在 journal 非空且 `fileResult==Failed` 时会 `discardStaging`，不走这条发布链。
- Observed：`projectSegyFile` 先 `QFile::copy` 到受管路径，道循环失败只返回「地震坐标转换失败；未发布半成品」，不删输出。`projectRasterFile` 失败时可能留下 `path.projected.tif`，或把已 `writeHorizonGeoTiff` 的暂存 tif 改过投影后返回 false。`moveStagedFiles` 只跳过 `commit-intent.json` 和 `*.partial`，其余 `artifacts/` 文件全部 `rename` 进工程。
- Expected：失败不发布半成品；未登记文件不得进入工程目录。现成约定是失败产物用 `.partial`，或先登记再由 `rollbackFailedRow` 删目录。
- 调用链：`produceFromPlan` → `executeIngestPlan` → `produceItem`/`importOneFile` → `prepareSeismicCoordinates` 或 `importHorizonFamily`/`dedupHorizonRederive` → 转换失败返回 → `rollbackFailedRow`（看不见未登记文件）→ `commitImport` → `moveStagedFiles`。
- 排除比对：不是 #588/#595 的「已登记版本失败不回滚」。那条回滚只覆盖 journal 内 `AddVersion`。本缺陷是新转换函数写在登记之前，又没用 `.partial`，绕过已有回滚。单文件失败路径已丢弃暂存，不在此列。
- 修复策略：转换写到 `*.partial`，成功后再改名；失败分支删除输出和 `.projected.tif`。`rollbackFailedRow` 不能看见的文件，不要先写到最终受管路径。
- 回归测试：批量导入中故意让第二口地震道头转换失败，断言工程 `artifacts/` 下没有该派生 SEG-Y，catalog 无对应版本；栅格 warp 失败同断言。

### A-2 / P1 / VERIFIED / io 井点坐标

- File:line：`src/io/welltableprojection.cpp:11` `projectWellCoordinates`；`src/io/dataimport_file.cpp:229-231`；`src/io/welltable.cpp:201-207` `readWellTable`；`src/io/dataimport_wells.cpp:173-185` `importWellHeadFamily`
- 函数：`projectWellCoordinates` / `readWellTable` / `importWellHeadFamily`
- 根因：源 CRS 为空时 `projectWellCoordinates` 直接返回 true，不转换、不报错。导入侧只在 `kind==crs` 时写入 `projectCrs`，不要求源 CRS。随后 `applyGeoreference` 把未转换的 X/Y 当成工程米制坐标反算经纬度，并把 `coordinateStatus` 写成 `ok`。
- 触发条件：已有 `kind=crs` 的工程里导入 xlsx/csv/shp 井表；`tableMapping` 无 `crs`，表头/表名也推不出 EPSG、UTM 或「经度」。向导对「指定了井点文件且源 CRS 仍空」会报错（`projectsetupscan.cpp:173`），但文件夹导入和确认表重导不走这道闸。
- Observed：井点原样入库，状态为已配准。地图和 WGS84 包围盒按错误 CRS 解释。
- Expected：源 CRS 未知且工程要求转换时拒绝导入，或保持 `untransformed`，不得标 `ok`。
- 调用链：`importOneFile` 插入 `projectCrs` → `readImportWellInput` → `readWellTable` → `projectWellCoordinates`（空源即成功）→ `importWellHeadFamily` → `applyGeoreference` → `addEntity`。
- 排除比对：不是 #645 文案漂移。向导扫描有声明源 CRS 的提示，导入执行路径没有同一闸门。
- 修复策略：`projectCrs` 非空且源 CRS 为空时返回错误。推断失败不得静默当成同一坐标系。
- 回归测试：工程 CRS 为 EPSG:32649，导入无 CRS 提示的井表（列名 X/Y），断言失败或 `coordinateStatus!=ok`，且 X/Y 未被当成 32649 米。

### A-3 / P2 / VERIFIED / io+catalog 井口必填列

- File:line：`src/io/wellfileparsers.cpp:173-174` `feedWellHeadLine`；`src/io/welltable.cpp:129-146` `parseSheet`；`src/io/welltableimport.cpp:23-27` `readImportWellInput`；`src/io/dataimport_wells.cpp:173-177`；`src/catalog/catalogstore_crud.cpp:77-82` `upsertEntity`；`src/domain/wellsection.cpp:197` `datumOffset`
- 函数：`feedWellHeadLine` / `parseSheet` / `importWellHeadFamily` / `upsertEntity` / `datumOffset`
- 根因：本窗把 KB、TD 从必填改成可选。缺值写成 NaN 后仍 `acceptedRows++`，井实体照建。坐标列仍然拒收。海拔基准对非有限 KB 回退 0.0，与新代码「不要把缺失编成高程零」的注释相反。sqlite 数值列也不能存 NaN，缺失被写成 0.0，只在 extra 里放 `kbMissing`/`tdMissing`。
- 触发条件：井口行 X/Y 合法，KB 非数值或 TD 为 `-99999`。`.dat` 走 `readWellTable`（`welltableimport.cpp:23-25`），其它文本走 `parseWellHeadDevice`。两条都放行。
- Observed：`rejectsInvalidWellHeadRequiredColumns` 的 `bad_kb`、`missing_td` 会进入 `heads` 并建成井。`missing_x` / `missing_y` / `inf` 仍拒收。海拔对齐模式把缺 KB 显示成 0。
- Expected：对照 `tests/tst_projectparsers.cpp:148-158`，缺 KB 或缺坐标的行应拒收，不得进目录。
- 调用链：`importOneFile` → `readImportWellInput` → `readWellTable` 或 `parseWellHeadDevice` → `importWellHeadFamily` `addEntity` → `upsertEntity`。剖面：`projectdata` 拷贝 `e.kb` → `datumOffset`。
- 排除比对：比 #645「井表文案」更具体。`tst_projectparsers` 失败是这条口径变化的症状，不是断言文案过期。坐标并未放宽，不能把整条测试失败都算测试过期。
- 修复策略：恢复 KB/TD 必填，或若产品确定可选，则海拔/时深消费者在 NaN 时拒绝海拔基准，不要代入 0；并改测试契约。不要只改测试。
- 回归测试：缺 KB、缺 TD、缺 X 三行；断言只有完整行进 `parseWellHeadText` 和 `readWellTable`，catalog 无缺 KB 井；若保留可选，断言海拔模式不把该井画在 0。

### A-4 / P2 / VERIFIED / io 井表类型

- File:line：`src/io/welltable.cpp:24-34` `column`；`src/io/welltable.cpp:103-108` `parseSheet`；`src/io/welltable.cpp:196-199` `readWellTable`；`src/io/importclassification.cpp:14-18` `classifyImportFile`
- 函数：`column` / `parseSheet` / `readWellTable` / `classifyImportFile`
- 根因：同时有井名+X+Y 和层位+深度时，类型固定为 `well_head`，分层行被丢掉。多表工作簿一旦先识别成井口，后续分层表被 `out.type==well_head` 跳过。列匹配第二轮用 `contains`：`MD` 长度大于 1，会命中 `BASE_MD`/`BOTTOM_MD`，把底深当成顶深。
- 触发条件：一张表同时有井号、横坐标、纵坐标、层位、顶深（分层表常重复井口坐标）；或只有 `BASE_MD`/`BOTTOM_MD`、没有独立顶深列。自动分类和 `readWellTable` 都走这里。若用户强制 `well_stratification`，`table.type!=type` 会整文件失败（`welltableimport.cpp:26`），而不是导入分层。
- Observed：重复井名变成「井口重名」未决链接，层位深度不入库。仅有底深列时，顶深等于底深，层段写不出来。
- Expected：分层表即使带井口坐标也应保留层位；底深列不得占用顶深角色。
- 调用链：`classifyImportFile` → `readWellTable` → `parseSheet`；确认后 `readImportWellInput` → `importWellHeadFamily`。底深误配再进 `writeWellLayerTable`（`welltable.cpp:224-231`，要求 `base>top` 才写入）。
- 排除比对：不是 #645 文案或版本计数。新井表分类器引入。
- 修复策略：井口与分层分列判定；两者都有时按分层导入或要求用户选表，不要静默丢层。`contains` 只允许词边界，或底深别名优先于子串 `MD`。
- 回归测试：列「井号,X,Y,层位,顶深」多行同井，断言类型为分层且层位数等于行数；仅 `BASE_MD` 时不得把底深记成顶深。

### A-5 / P2 / VERIFIED / io 栅格 CRS 元数据

- File:line：`src/io/projectcoordinates.cpp:46-49` `coordinateWkt`；`src/io/projectcoordinates.cpp:81-85` `projectRasterFile`；`src/qgis/factorcontour.cpp:147-148`（消费方，本窗未改，但是活调用链）
- 函数：`coordinateWkt` / `projectRasterFile`
- 根因：warp 成功后 `GDALSetMetadataItem(..., coordinateWkt(target), ...)` 的返回值被忽略。`coordinateWkt` 在 `exportToWkt` 失败时返回空串。函数仍 `remove`+`rename` 并返回 true。调用方随后 `addVersion`。`factorcontour` 只要 `PALEO_CRS_WKT` 指针非空就 `OSRNewSpatialReference` 并替换数据集 SRS；空串也会被当成已保存的 CRS。
- 触发条件：工程 CRS 转换层位/古地理栅格成功，但 WKT 导出或元数据写入失败。源 CRS 声明路径（`:57-59`）会检查返回值，目标 CRS 这条不检查。
- Observed：导入报成功，派生 tif 已替换。后续等值线优先使用空的或写失败后残留的 `PALEO_CRS_WKT`，盖掉 GDALWarp `-t_srs` 写入的投影。
- Expected：元数据写失败或 WKT 为空应返回错误，不得把该栅格当转换成功发布。
- 调用链：`importHorizonFamily` / `dedupHorizonRederive` → `projectRasterFile` 返回 true → `addVersion` → 以后 `factorcontour` 读 `PALEO_CRS_WKT`。
- 排除比对：不是 A-1 的未登记孤儿。这里版本已登记，错误被当成成功。也不是 #634「worker 退出后 CRS 对象失效」；这是文件元数据在错误时仍提交。
- 修复策略：`coordinateWkt` 为空或 `GDALSetMetadataItem!=CE_None` 时删除 warp 结果并返回 false。消费方忽略空 WKT。
- 回归测试：模拟 `exportToWkt` 失败或元数据写入失败，断言 `projectRasterFile` 为 false，且 catalog 无该派生版本。

## #645 归属（不占 Finding 编号）

### 岩屑「没有可用井段」：测试夹具过期，不是词表回归

`tst_import::wellAttachmentLinksCoreCuttingsLabAndReference` 把 `A1岩屑录井数据.xlsx` 写成 JPEG 魔数加序号（`tests/tst_import.cpp:3000-3006`），再 `importProjectFile`。本窗 `importAuxReferenceFamily`（`dataimport_geodata.cpp:363-366`）对岩屑目录下的 xlsx/csv/txt 调用 `storeImportedCuttings`。`readCuttingsFile` 打不开这份假 xlsx，`table.ok==false`，于是 `cuttingsimport.cpp:17-18` 返回「岩屑录井表没有可用井段」。

本机真文件 `/home/kevin/projects/paleo_data/2.沉积相分析-第9届/2.4岩屑录井数据/A1岩屑录井数据.xlsx`（43 734 字节，单表 Sheet1，927 行）表头是 `顶深(m)`、`底深(m)`、`岩石定名`、`颜色`、`岩性描述`。`cuttingsdoc.cpp:47-56` 词表含这些列，`normalizeCuttingsHeader` 会去掉 `(m)`。颜色正则只改显示词，不拒行。真表不会因列映射被拒。#633 起非表附件不能再只挂链接，测试夹具未跟上。不是解析器拒掉合法井段。

### 版本 17→21：设计内新增 4 个 DERIVED，不是重复登记

mini 仍是 17 个导入项 / 17 个资产（`generate.py:277-295`，`tst_mkprojectfixture.cpp:33`）。多出的版本：

| 来源 | 条数 | 代码 |
|---|---|---|
| `well/井位坐标.xml` `convert=well_head` → `convN.dat` | +1 | `readImportWellInput` 对 `.dat` 井口走 `readWellTable`，`normalized` 非空，`storeNormalizedWellTable` 再登记 `well_data.dat` |
| `2.4岩屑录井数据/A{1,2,3}_岩屑录井.csv` | +3 | `storeImportedCuttings` 各写一份 `cuttings.csv` DERIVED |

合计 21。mini 的 georeference 是 `similarity2d`，不是 `crs`，不会为地震再派生版本。mini 无层位文件，没有层位 tif。`resolveImportedWellDepths` 只处理带 `wellTableParsed` 且缺 MD、有 TVD 的分层；mini 分层文本自带 MD，不会再加版本。岩性道来自岩屑派生表，不是另建资产，也不是同一字节登记两次。测试「每资产恰一个 RAW」过期。应改期望为 17 资产 + 4 个派生版本，并断言文件名 `well_data.dat`×1、`cuttings.csv`×3。

### 井表文案

`readImportWellInput`（`welltableimport.cpp:43`）仍在 `acceptedRows==0` 时返回「井表没有可用数据行」。这句没改。#645 里同文案失败若仍出现，不是本窗把句子删了，而是上游拒收原因变了（例如假岩屑、或井表类型不匹配走了另一句）。不要把 A-3 的必填列放宽和这句文案混成同一根因。

## 必查项结论

1. 井表必填列：缺 X/Y 或哨兵坐标仍拒收。缺 KB/TD 会进目录。见 A-3。
2. 岩屑：夹具过期，不是词表回归。见上节。
3. 版本 17→21：1 个井口规范化 + 3 个岩屑派生。设计内新增。
4. 角色往返：`reference` 链接角色字符串会进 sqlite/JSON（`catalogstore_json.cpp` 读写 `role`）。`boundary` 挂在 `study_area` 上，与 `roleregistry.cpp:103-104` 一致，`addLink` 不会因类型不符写诊断。未发现角色名在 JSON 往返中丢失。`kbMissing`/`tdMissing` 只存在于 sqlite extra，`loadTables`（`catalogstore_json.cpp:207-209`）读完即从内存 extra 剥掉；`toJson` 写的是剥掉之后的 NaN kb（Qt 变成 null），`fromJson:440` 用 `toDouble(qQNaN())` 还原。这是内部标记，不是角色标记丢失。
   - 未升格为 Finding：`roleregistry` 只把 `reference` 登在 `well`（`:57-60`），`catalogroles.cpp:44` 仍把 `reference` 放在 misc。辅助实体参考链接会得到「角色与实体类型不符」，note 会持久化。链接不丢、角色名不丢。#643 只补了井附件那一支。
   - 线程：新 `addVersion`/`addEntity` 都走 `DataCatalog`，自带 `checkWriteThread`。扫描在 `QtConcurrent` 里只读文件，不写 catalog。未见新的线程外写。
5. 坐标失败：半成品会被批量提交发布，见 A-1。目标 CRS 的 WKT 写失败仍当成功，见 A-5。`nativereference.cpp` 的 `validNativeCrs`/`nativeToGeographic` 失败返回 false/空，不写文件。`paleoprojectfile.cpp:128-132` 在 `kind=crs` 且 `isComplete()` 失败时返回 false，不把坏 CRS 当成已读入。`projectboundary.cpp:28-50` 转换失败在 `addEntity` 之前返回，不留半个测区实体。
6. 路径：`managedPath`/`resolvedVersionPath`/`isSafePathSegment` 拒绝 `..`、绝对路径和反斜杠。新派生文件名是常量或经 `managedPath`。`moveStagedFiles:317-318` 拒绝相对路径里的 `..`。`projectsetupscan.cpp` 的 `collect` 用 `NoDotAndDotDot` 且跳过符号链接。未发现 `..` 或绝对路径逃出工程目录的拼接。

## 排除的疑点

- 地理图 `mapCrs` 把地理 CRS 记成 `EPSG:3857`（`projectsetupmaps.cpp:22`）。注释写明这是米制计算网格，图层仍用自己的 CRS。只在没有井点/地震/层位时填 `sourceCrs`（`projectsetupscan.cpp:160-161`）。古地理导入另读 GDAL，不把这个字符串写进图层。不是「WKT 失败仍成功」。
- `nativeHorizonGrid` 在头里没有 `UTM`/`EPSG:` 时返回 true，是跳过拟合，不是把失败当成功。拟合失败会让 `binHorizon` 失败。
- `projectWellCoordinates` 中途失败会改已转换的井点，但 `readWellTable` 带 error 返回，导入检查 `table.error`。`ingestplan` 只用井名，不用这份半转换坐标。
- `vectorOrText` 忽略 `exportToWkt` 返回值；失败时 `wkt` 先置空，`QString::fromUtf8(nullptr)` 为空，不会把垃圾 WKT 当 CRS。
- 井单因素：缺列、重复井层、砂厚与砂地比不一致会拒整文件（`wellfactorfile.cpp:36-62`）。空单元格跳过一致性检查，不构成静默入库坏比值。
- 岩屑颜色正则、首个 `ok` 但无井段的工作表：真 A1 只有一张有数据的表，不是本次失败原因。首表只有表头、数据在后续表时会选空表并让 `storeImportedCuttings` 失败；当前夹具和真 A1 都不是这个形态，标 SUSPECTED，不编号。

## 覆盖清单

逐行读了变更函数及直接调用方：

- `src/io/cuttingsdoc.cpp`（变更段 + `readCuttingsFile`/`parseCuttingsSheet`）
- `src/io/cuttingsimport.cpp` `cuttingsimport.h`
- `src/io/dataimport_coordinates.cpp`
- `src/io/dataimport_file.cpp`（井表/地震坐标/边界分支）
- `src/io/dataimport_folder.cpp`（`resolveImportedWellDepths` 接线、行回滚）
- `src/io/dataimport_geodata.cpp`（层位投影、岩屑挂井、去重再派生）
- `src/io/dataimport_internal.h` `dataimportservice.h`（变更字段）
- `src/io/dataimport_log.cpp` `dataimport_log.h`（`name:` 目标井）
- `src/io/dataimport_wells.cpp` `importWellHeadFamily` 与分层建井分支
- `src/io/horizonbinner.cpp` `horizonbinner.h`（native grid 接线）
- `src/io/horizonnativegrid.cpp` `horizonnativegrid.h`
- `src/io/importclassification.cpp`
- `src/io/ingestplan.cpp`（井表预览、shapefile 展开、`rollbackFailedRow`）
- `src/io/projectboundary.cpp` `projectboundary.h`
- `src/io/projectcoordinates.cpp` `projectcoordinates.h`
- `src/io/projectsetupmaps.cpp` `projectsetupscan.cpp` `projectsetupscan.h`
- `src/io/wellfactorfile.cpp` `wellfactorfile.h`
- `src/io/wellfileparsers.cpp`（井口/分层/写出变更段）
- `src/io/welllayerdepthimport.cpp`
- `src/io/welltable.cpp` `welltable.h` `welltableimport.cpp` `welltableprojection.cpp` `welltabletext.cpp` `welltabletext.h`
- `src/domain/projectclassifier.cpp` `projectsetup.h` `sidecarvocab.h`
- `src/catalog/catalogroles.cpp` `catalogstore_crud.cpp` `catalogstore_json.cpp`（实体 kb 与链接 JSON）`datacatalog_export.cpp` `roleregistry.cpp`
- `src/metadata/nativereference.cpp` `nativereference.h` `paleoprojectfile.cpp` `paleoprojectfile.h`（crs 分支）

接口级：`dataimport_session.cpp` 的 `moveStagedFiles`/`commitImport`/`failSession`；`datacatalog_versions.cpp` 的 `managedPath`/`resolvedVersionPath`；`factorcontour.cpp` 的 `PALEO_CRS_WKT` 读法；`wellsection.cpp` `datumOffset`；`workflow/projectsetup.cpp` 的 mappings 下传；`app/mkproject.cpp` 的 convert 路径；真 A1 xlsx 表头（zip/xml，未跑解析器）。

## 未核查区

- `dataimport_file.cpp` 测井快照/去重后半段未逐行重读，只核对了井表与地震坐标插入点。
- `catalogstore_json.cpp` 资产/版本/链接的 fromJson 后半，只核对了实体 kb 与链接 role 字段仍在 toJson。
- `writeSurveyGeoJson` 的 `boundaryGeometry` 经 `QVariantMap` 再写出，未用损坏几何做往返实验。
- 未跑 ctest，未在进程内调用 GDAL。真岩屑结论来自表头对照词表，不是 `readCuttingsFile` 的运行结果。
- Windows 0xC0000409、#646 串行超时不在本域。
