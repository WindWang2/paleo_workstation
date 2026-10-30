# 统一「四子层段」地层体系：审计与设计

> 日期：2026-10-30（Asia/Shanghai）。分支 `goal/strat-subinterval-20261030`；Data Service 分支 `feat/stratigraphic-subintervals-20261030`。

## 1. 最高原则

- 地震层位/界面（T70、T71、D61 …）是 **Boundary**，本身不是地层单元。
- 相邻两条有序界面构成 **父单元 Interval**（显示为 `T70–T71`）。
- 每个父单元恰好 4 个有序厚层段 **S1–S4**（自上而下，顺序来自界面 `order`，不从图像坐标推断）。
- 单井相、地震相、单因素、编图与成果都归属稳定的 **SubIntervalId**；父单元结果只能是显式派生综合（aggregate）。
- **Data Service 是唯一数据权威**：方案、修订、井内范围、地震掩膜、成果归属、影响预演都由它提供；宿主不保存副本。

## 2. 审计：把单一界面当单元使用的现有路径

| 位置 | 现状 | 问题 | 本次处理 |
|---|---|---|---|
| `src/domain/mappinghorizons.h` `mappingHorizons()` / `baseHorizonFor()` | `AreaRules::sequenceBoundaries` 有序界面名单；厚度底面 = 下一界面 | 名单本身正确（是界面序），但编图 chip 用单个界面名当“编图单元” | 保留为界面序权威与历史标定；新增 `paleo::strat::adjacentIntervals()` 从同一名单派生父单元，不写死 T70/T71 |
| `AreaRules::targetHorizon`（默认 D61）及 `paleomainwindow.cpp`、`attach_mapping`、`constraintpage_sections.cpp`、`validatepage`、`predictpage`、`aiwiring`、`previewdoc.cpp`、`predictionworkflow`、`validationworkflow`、`workflows_internal.h`、`aiassistworkflow` | 以单一界面名作为当前编图/预测目标 | 单一界面被当成单元 | 保留（历史/标定兼容）；新任务应携带 `UnitSelection`。**本 PR 未改这些调用点**（见 §8） |
| `constraintfactorjobs*.cpp`、`mappingworkbench*.cpp` | 图层 id `constraints.%1`、`cartographic.%1.%2`，`job.horizon` | 结果按界面名键控 | 提供 `layerToken()`/`cacheKey()` 作为新键；调用点迁移列为后续 |
| `dataimport_geodata.cpp:64` | 文件名匹配界面名 | 导入只能判定“界面”，正确 | 不改；导入层位仍是界面，不自动成为单元 |
| `evidenceconflict.cpp:314` | 比较 horizon 字符串 | 同上 | 不改 |
| `wellpredictionservice`（vector_formations 按名）、`stratigraphymodel.cpp`（DS 界面 → `WellTop{horizon,md}`）、`wellsectionworkflow` | 井分层按界面名 | 正确地把它们作为界面深度 | 作为 `draftScheme()` 的井分层输入 |
| Data_service `src/stratigraphy/core/internal.h endpoints()`：E12=T70–T72、E3=T72–T73、E4=T73–T80；`interpretation.cpp moveBoundary`、`quick_workflow.cpp` | 恩平组算法分段 | **E12 跨两个父单元**，不能直接改名为 `T70–T71` 或任何单元 | 保持算法分段不变；框架 `Interval.legacySegment` 仅作引用；`resolveLegacyLabel("T70-T72")` → `legacy_unresolved` |

## 3. 模型（Data Service `paleo_data/stratigraphic_framework.h`）

- 强类型 `StrongId<Tag>`：`BoundaryId`、`IntervalId`、`SubIntervalId`、`InterpretationRevisionId`，相互不可隐式转换；显示名不是身份。
- 稳定 ID：FNV-1a(方案 id + 上/下界面 id [+ 序号])，重派生不变。
- `SubIntervalDivision`：模式 A `initial_equal`（0/.25/.5/.75/1，内部面 `derived`/provisional）；模式 B `interpreted`（方案级比例）。
- `WellIntervalOccurrence`：每井顶/底、3 条内部面深度+状态+锁定；状态 `complete|top_missing|not_penetrated|pinched_out|faulted|pending`。缺失不补造。
- 校验 `validate()`：界面顺序/重名、父单元与界面不一致、井倒置、内部面逆序/越界、相邻父单元重叠（error）与间隙（warning，断层除外）。
- 地震掩膜 `seismicMask()`：由每道顶/底时间按比例生成，半开 `[lo,hi)`（S4 含底），缺失/倒置道 = 2（待复核）；**不是固定时间四等分**。
- 影响 `impactedSubIntervals()`：比较几何指纹（方案比例 + 各井生效顶底），只返回真正变化的子层段。
- 历史标签 `resolveLegacyLabel()`：单一界面名 → `legacy_unresolved`（**绝不批量映射 T70→T70–T71/S1**）；`上–下/Sn` 完整匹配才给 `resolved_candidate`。

## 4. Data Service API 与存储

非破坏性懒建表（`ds_meta.stratigraphic_framework_schema=1`）：`strat_schemes`、`strat_scheme_revisions`（不可变 JSON 快照 + sha256 摘要 + parent）、`strat_result_bindings`。

`commitStratigraphicScheme`（只读拒绝；派生+校验；expected-head 乐观并发；同事务把受影响成果与父单元综合标为 `stale`）、`listStratigraphicSchemes`、`listStratigraphicSchemeRevisions`、`readStratigraphicScheme`、`restoreStratigraphicSchemeRevision`（回退 = 新修订）、`readWellSubIntervalRange`、`bindSubIntervalResult`（新成果必须有有效 SubIntervalId；仅有历史标签 → `legacy_unresolved`）、`listSubIntervalResults`、`previewStratigraphicImpact`。旧工程无表时读接口返回空，不写任何东西。

## 5. 宿主侧

- `src/domain/stratigraphicunit.{h,cpp}`（数据层）：`UnitSelection`、`cacheKey()`（方案+修订+单元+指纹+参数摘要）、`layerToken()`、`classifyLabel()`、`requiresSubInterval()`、`adjacentIntervals()`。
- `src/services/stratigraphicsubintervals.{h,cpp}`（功能层）：`draftScheme()`（工程界面名单 + 井分层 → 方案草稿）、`selectorOptions()`（父单元 → S1–S4 二级选择）、`makeSelection()`、`wellSampleMask()`、`wellSubIntervalMean()`（样点不足 → NaN，不回退父单元）、`aggregateInterval()`（厚度加权，缺任一子层段 → NaN + 缺失列表）。

## 6. 兼容与迁移

- `mappingHorizons()`、`AreaRules::sequenceBoundaries/targetHorizon` 原样保留，继续用于标定和历史；C3/C6/D53/D61 只是默认界面名单，父单元由相邻关系派生。
- 不做任何批量重写；旧成果只能以 `legacy_unresolved` 登记，人工指定后再绑定子层段。回滚 = 恢复旧修订（生成新修订），原始数据不变。

## 7. 测试

- Data_service `tests/stratigraphic_framework_tests.cpp`：派生/顺序/稳定 ID、等比与不等厚、逆序/越界/重叠/间隙/尖灭/未钻穿、地震掩膜无重叠无遗漏、影响范围、历史标签、持久化重开、版本冲突、回退、只读、旧工程。
- 宿主 `tests/tst_stratigraphic_subintervals.cpp`：标签分类 + 端到端（T70/T71/T72 → 2 父单元 → 8 子层段 → 单井相 → 地震 → 单因素 → 综合 → 成果登记 → 重开 → 修改 S2/S3 分界 → 只失效 S2 与综合）。

## 8. 未完成 / 限制

- **本轮已完成**：画布区 `SubIntervalSelectorBar`（父单元→S1–S4/综合）、`SelectionContext::activeUnit` 广播（不重置地图/编辑/布局）、状态栏「分析层段」、whatsThis/图标/tooltip；单因素/井因子/相面派生/厚度链/证据匹配经 `unitselectionbridge` 写入 `UnitSelection` 与 `analysisLayerKey`（成果键）并与 `constraintHorizon`（编辑态界面）分离。
- **仍待**：预测页/地震相/单井相页内 combo 与 Ribbon 命令全面去掉对 `horizonCombo` 字符串的依赖；编图发布/版本行是否改挂稳定 ID；DS 方案选择器（多方案/修订）UI；时深不明/未确认解释的逐子层段红绿灯需接 DS occurrence 状态。
- TVD/TVDSS/OWT/TWT 换算沿用现有时深服务；本框架只记录 `depthType`，不做换算。
