# well-logset — 多文件井曲线并集读面（goal/well-logset-20261002）

分支 `goal/well-logset-20261002`（自 origin/master `eaaf46d` 起）。迭代账本
`.goal-loop-ledger-well-logset.md`。一口井可以挂多份已决 `well_log`；本方向补的是
读面和导入序，不改 catalog 磁盘格式，也不把曲线体写进 catalog。

## 交付一览

| 块 | 内容 |
|----|------|
| 读面 | `WellLogSet::wellLogFiles` / `wellCurveIndex`（`src/services/welllogset.*`）。只调 `LasParser::parseHeader`，遇 `~A` 即停 |
| 导入 | 已有已决主 `well_log` 时，新 LAS 为非主，`ordinal = max+1`。第一份已决文件仍自动为主、ordinal 0。未决分支不改 |
| 消费 | 岩石物理批处理、属性建模 `requestFromCatalog`、剖面井轨、交会图清单、测井综合图按并集取曲线 |
| 界面 | 实体面板按 ordinal 列出全部 `well_log`，非主文件按钮「设为主文件」只发信号；数据页接 `setLinkPrimary` |

`ProjectDataFacade::assetFilePathFor` 仍是单主文件路径（分层、时深还是一份文件），不是并集 API。
`addLink` 的「同角色只有一个已决主」不变量不改。`attachLink` 仍强制 `isPrimary=true`，本方向不改。

## 仲裁

文件顺序：`ordinal` 升序，其次 `versionNumber`，再次 `versionId`。只取该资产的
`currentVersion`。`CatalogVersion::versionNumber` 缺省是 1，存成 0 的版本对
`currentVersion` 不可见。

跳过（计入 `WellLogWarnings`，不发明曲线）：

- 路径空、文件不存在、头解析失败：这条链接不进结果，告警「文件不存在」或解析错误。
- `catalog` 空或未打开：空结果，一条「catalog 未打开」。
- `wellId` 空，或没有任何已决 `well_log`：空结果，不因此加告警。未决链接忽略。

导入槽（`assignResolvedWellLogSlot`，在 `addLink` 之前写上 link，journal 重放同一条）：

- 该井没有已决 `well_log`：本条主文件，ordinal 0。
- 有已决成员但没有主：本条补成主，ordinal = max+1。
- 已有主：本条非主，ordinal = max+1。不改既有链接，不另开序号，不单独 bump `mutationSeq`。

## canonical

并集不含深度列（`curveNames[0]`，`column == 0`）。

- 跨文件同名，且主文件上有这一列：主文件列保持原名，`canonical=true`。其余列名为
  `<mnemonic>@<completeBaseName>`，`canonical=false`。
- 主文件没有这一列，或这口井没有主关联：重名列全部加 `@` 别名，没有 `canonical=true`
  （不挑一份文件升主）。
- 别名仍然撞车：再追加 `#<versionId>`。
- 同一文件里同名：全部保留。第一条用原名，其后为 `<名>#<列号>`，并记警告，不静默丢掉。
- 只出现在非主文件、且没有重名的曲线保持原名，`canonical=false`。
- `setLinkPrimary` 之后，canonical 跟着新的主文件走。

交会图 `load` 把 `GR@基名` / `GR#列号` 映回该文件 `~C` 里的原列名再取样。

## 消费迁移

| 调用方 | 行为 |
|--------|------|
| `PetroPhysTaskService` | 每井一份 `WellRef`。进工作线程前用并集选驱动文件：Vsh*→GR，PhiDensity→RHOB，PhiNeutron→NPHI，PhiSonic→DT，SwArchie→RT。表达式按词干匹配（`RHOB` 命中 `RHOB@file`）；恰好一条助记符命中才用那份文件，否则主文件。公式查找和表达式绑定都走词干：精确词干优先于家族别名，所以 `DEN` 不会占住后出现的真 `RHOB`；没有真 `RHOB` 时 `DEN` 仍可写成 `RHOB`。其它文件线性重采样到驱动深度网格（严格递增、有限深度；网格外 NaN，不外推）。曲线名用并集名。`sourceVersionId` 留在驱动文件。工作线程不碰 `DataCatalog`。`neutronInPercent == false` 时 NPHI 原样返回 |
| `PropertyModelWorkflow::requestFromCatalog` | 助记符大小写不敏感精确匹配；只解析命中的那一份 LAS；深度是第 0 列 |
| `SectionWorkbench` 井轨 | 每个源文件最多一条曲线，优先 GR，否则第一条数据曲线；显示并集名。深度单位不是 M/FT 时只跳过该文件。`coordinateStatus` 为 ok 或 untransformed 的井都纳入 |
| `CrossplotSources::inventory` | 每条 `WellCurveRef` 一个 `SourceSpec`，`id = wellId\|versionId\|mnemonic` |
| 测井综合图 | `wellCurveIndex` 只拿头。单道仍看当前文件。兄弟 LAS 的数据体在同一次后台 `PaleoTask` 里解析（`requestLas` 的 `siblingPaths`）；当前文件失败则整次失败，兄弟失败则跳过 |

## 性能与真工区

合成门（`tst_welllogset_perf`，本机 cdb + Qt 6.11.2 运行库在 PATH 最前）：

| 指标 | 值 |
|------|----|
| `wellCurveIndex` 100 井 × 3 文件 | 172.863 ms；收尾复跑 163.127 ms（门 < 500 ms） |
| 100 井 / 前 10 井 | 11.095；收尾复跑 8.195（门 < 20） |

`PALEO_REAL_PROJECT_AREA` 在本机未设置。`realArea_headerIndexOrSkip` 为
SKIP：`PALEO_REAL_PROJECT_AREA not set — real-area well logset skipped`。
未跑的真工区不记成通过。设置该变量且目录下有 `井曲线/*.las` 时，测试只按头里的
WELL 分组、把外部路径登记进临时 catalog（`managed=false`），再计 `wellCurveIndex`。

## 递延

- DLIS / LIS / BE 不读。地震读路径不改。
- 相关对比、成图工作台仍按「有没有 well_log」或单资产预览，不走并集。
- 非驱动文件重采样只做线性、不外推，不做 MD/TVD 深度对齐。
- 综合图单道仍是当前文件；多文件只进综合道。
- `attachLink` 继续把新挂上的链接升成主文件。已有多文件的井若走挂接而不是导入，主标记会换。
