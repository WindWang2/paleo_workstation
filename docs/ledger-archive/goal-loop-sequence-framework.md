# Goal Loop ledger — sequence-framework（方向 28：层序地层格架工作台）

- 日期：2026-10-04；分支 `goal/sequence-framework-20261004`，从 `origin/master` `bd1eb0a` 创建；worktree `.worktrees/sequence-framework`。
- 四项 Oracle（CRUD round-trip / 悬空·空洞·重名全检出 / 建议只出候选 / 全量绿 + 五道门禁）。未跑不标通过。

## 轮0 — 现状锚点与接口定案

勘察（本分支工作区，不再另开调查）：

- `sequence_boundary` 是 catalog 实体类型（`datacatalog.h` 注释 + `dataimportservice.cpp` 约 1269 行：层位文件按 `sb-<NAME>` 建实体，名单外置 `extra["pending"]=true`）。导入期行为一行未改。
- `well_stratification` 是资产类型（`arearules.cpp` 327 行、`projectclassifier.cpp` 108 行），井分层解析出口是 `parseWellTopsText`（`io/wellfileparsers.h`，格式「井名 层名 MD …」）。
- `mappingHorizons()`（`domain/mappinghorizons.h`）= `AreaRules::active().sequenceBoundaries`，序浅→深，默认 8 界面 `C3 C6 D53 D61 D62 D63 D71 D72`。它是层位序的唯一权威——格架只引用它的成员，不改写它。
- 角色词表 `RoleRegistry::defaults()`：well 9 角色 + seismic_survey 7 角色；`tst_roles.cpp` 只钉 `forEntity("well")==9` 与 `forEntity("seismic_survey")==7`，`sequence_boundary` 段此前为空 → 新增 `framework_unit` 不会碰断既有断言（实测 `tst_roles` 全绿）。
- 落库纪律：格架数据走 catalog 资产 + 受管版本（`DataCatalog::managedPath` + `addVersion`），禁止旁路存储；归属关系走 `EntityAssetLink`（`entityType=sequence_boundary`、`role=framework_unit`），关系不由名字推断。

锁定签名：`SequenceFramework::{Framework/FrameworkUnit/MarkerBed}`、`FrameworkStore::save/load`、`FrameworkService::suggest/commitAccepted/diagnose`、`diagnose(DiagnosticInput)`。

## 轮1 — 格架模型与落库（目标 1/2）

`src/domain/sequenceframework.{h,cpp}`：二级格架（层序 → 体系域）+ 标志层，纯值类型 + JSON。
`src/catalog/frameworkstore.{h,cpp}`：资产类型 `sequence_framework`，`stage=OUTPUT`，受管路径 `{stage}/{asset_id}/{version_id}/framework.json`，每次保存涨一个版本并同步 `sb-<NAME>` 归属链接（只补不建——实体不存在时如实跳过）。

关键裁决：

- **双向解析**：单元 → 覆盖层位区间 = `[topBoundary, baseBoundary)`（底界归下一单元）；层位 → 归属单元**只看叶子**（`isLeaf`）——层序一旦被体系域细分，归属由体系域承担，否则「层序与自家体系域」会被判成归属歧义。
- `schema_version` 高于本二进制所知 → `fromJson` 如实报错，不静默截断字段。

## 轮2 — 建议核（目标 3）

`src/algorithms/frameworksuggester.{h,cpp}`。

第一版按「成员顶深均值 + 离散度归一化」做最近邻，实测失败：单元内不同成员本就跨越单元厚度（百米级），而同一层在各井之间的构造起伏只有十米级——按成员离散度归一化会把单元内的正常成员判成超阈值（实测 distance ≈ 9.5 ≫ 1.0，一条候选都出不来）。

改为**区间法**：单元已确认成员在各井上形成深度区间；候选落在区间内 = 距离 0，落在区间外 = 到最近边界的距离（按区间跨度归一化）。并且**优先用本井区间**（该井有该单元成员时），该井没有才退回全工区均值——构造起伏让绝对深度逐井漂移。归一化尺度一律 `max(实测离散度, 相对下限, 1.0)`，绝不除零。

写库纪律：核本身是纯函数（不接 catalog、不接文件）；唯一写口 `FrameworkService::commitAccepted()` 只应用 `accepted==true` 的候选，且全部已存在时幂等不落盘（不空涨 revision）。

## 轮3 — 诊断与导出（目标 4/6）

`src/domain/frameworkdiagnostics.{h,cpp}`：九种诊断（悬空分层 / 悬空单元 / 界面越界 / 归属歧义 / 层位无归属 / 单元无编图数据 / 格架空洞 / 跨单元重名层 / 孤立井分层）。
报告序稳定（kind → 首定位 → 明细），`toText()` / `toMarkdown()` 同输入必同输出（Oracle 4 断言）。数据层的用户可见串走 `QCoreApplication::translate("SequenceFramework", …)`，与 `domain/welllogfacies.cpp` 同款写法。

## 轮4 — 视图（目标 5）

`src/ui/sequenceframeworkcolumn.{h,cpp}`：层级色带柱状图（带高按 thickness，缺厚度等分剩余，保底 12px）。色相按层序序号转、体系域提亮——属数据符号色（DESIGN.md「地图域配色不属 UI token」）；chrome 色一律取 `PaleoTheme::tokens()`。
`src/ui/sequenceframeworkpanel.{h,cpp}`：格架树 + CRUD/排序 + 标志层 / 井间建议 / 一致性诊断三页签 + 报告导出。

分层裁决：护栏禁止 `ui→algorithms`，故建议/诊断的编排放 `src/services/frameworkservice.cpp`（数据层里唯一同时看得见 `paleo_store` 与 `paleo_algorithms` 的位置）；面板只 include `domain` + `services`。CRUD 全部留了不弹对话框的程序化入口，供无头测试直接驱动。

## 轮5 — 本机基线构建修复（非本方向，但挡路）

新 master 在本机依赖布局下有三处编译期红灯，逐一最小修复：

1. `tst_boot` / `tst_polygonize` / `paleo_selfcheck` 只链 `paleo_qgis_iface`，拿不到 `sqlite3.h`（`qgsstyle.h` 直接 `#include <sqlite3.h>`，本机该文件在 vcpkg 依赖根而非 QGIS prefix）。→ 把 `${GDAL_INCLUDE_DIR}` / `${GEOS_INCLUDE_DIR}` 加进 `paleo_qgis_core_deps` 的 INTERFACE。
2. `tests/tst_geostat_kriging.cpp:261` `Sample{..., row + column}` 窄化转换（MSVC C2397）。→ `static_cast<double>`。
3. `tests/tst_inversion_wavelet.cpp` 用 `std::numbers::pi` 但没 include `<numbers>`。→ 补 include。

## 证据

```
PASS   : TestSequenceFramework::crudRoundTripAndHorizonMapping()
PASS   : TestSequenceFramework::savingAppendsNewVersion()
PASS   : TestSequenceFramework::diagnosticsCatchDanglingGapDuplicate()
PASS   : TestSequenceFramework::suggestionsAreCandidatesOnly()
PASS   : TestSequenceFramework::reportIsReproducibleAndRejectsBadSchema()
Totals: 7 passed, 0 failed

PASS   : TestSequenceFrameworkUi::panelCrudAndSorting()
PASS   : TestSequenceFrameworkUi::columnViewTracksFrameworkAndSelection()
PASS   : TestSequenceFrameworkUi::suggestionsRequireConfirmation()
PASS   : TestSequenceFrameworkUi::diagnosticsSurfaceAndExport()
Totals: 6 passed, 0 failed

python tools/check_layering.py --strict       → 检查通过：无层违规
python tools/check_ui_invariants.py --strict  → clean (violations=0, baseline=0)
python tools/check_i18n.py                    → 文案入口无裸字面量
git diff --check                              → 无空白错误

tst_roles / tst_catalog / tst_constraintstore / tst_arearules / tst_workflows 全绿。
```

`tst_roles::forEntityFiltersByEntityType` 原有一条「`forEntity("sequence_boundary")` 为空」断言——
`sequence_boundary` 此前无词表。登记 `framework_unit` 后该断言不再成立，已在**同一 commit** 内改为
钉住新词表段（1 条、role=`framework_unit`、stageDefault=`OUTPUT`），「无词表类型/空类型 → 空集」语义保留。

`tst_datapreview` 在本机报 `0xc0000139`（Qt DLL 顺序，既有环境项；fault-surface ledger 与
`docs/progress/job-framework.md` 已记同一现象，与本方向无关），未改其断言。

## 遗留

- 归属链接只补不删：单元改界后旧链接仍在（`DataCatalog` 无 removeLink 面）。诊断以名字（层位序权威）为准，不受影响；待 catalog 补删除面再收口。
- 体系域只做两级；更深嵌套（准层序组）未建模。
- 建议器只用顶深/厚度，未引入井间距离加权（同井优先已由「本井区间」覆盖）。
