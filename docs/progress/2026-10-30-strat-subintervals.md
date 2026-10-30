# 进度：统一四子层段体系（2026-10-30 Asia/Shanghai）

## 已完成
1. **审计**：`docs/STRATIGRAPHIC_SUBINTERVALS.md` §2——界面当单元的现有路径与处理策略。
2. **Data Service**（`vendor/Data_service` → `ce4e1db`，分支 `feat/stratigraphic-subintervals-20261030`，上游 PR https://github.com/changyanyanchang/Data_service/pull/5）：
   - 模型/派生/校验/地震掩膜/影响/历史标签；服务 API + 懒建表；单元测试与文档。
3. **宿主 domain/services**：`UnitSelection`、标签分类、草稿方案、二级选择、井样点掩膜、单因素均值、厚度加权综合、缓存键/图层令牌。
4. **测试**：`tst_stratigraphic_subintervals`（标签分类 + 端到端合成）；Data_service `stratigraphic_framework_tests`。
5. **构建与全量 ctest**：worktree 全量构建成功；新测试通过；相对 master 基线无新增回归（`tst_startup_trace::injectedDegradationMustTripGate` 在基线同机复跑亦失败，属 envdebt_perf-host 波动）。
6. **自审 ≥2 + 独立审**：见 PR 正文；High/Medium 已清（测试 `cacheKey` 字段误写、`makeSelection` 半成品）；Low 列于 PR。

## 未完成（明确限制，见设计文档 §8）
- Ribbon 二级选择器 UI 接入；单因素/编图/地震相等调用点从 `horizon` 字符串迁到 `UnitSelection`。

## 构建命令
```
flock -x /workspace/audit/fix4/.git/paleo-shared-build-j4.lock \
  /workspace/audit/strat-logs/build.sh /workspace/audit/strat-subinterval /workspace/audit/strat-build
/workspace/audit/strat-logs/ctest.sh /workspace/audit/strat-subinterval /workspace/audit/strat-build
```

## 续：二级选择器与 UnitSelection 迁移（Asia/Shanghai 2026-10-30 16:xx）

### 完成
- UI：`SubIntervalSelectorBar`（父单元→S1–S4/综合、展开状态、可读缺界面提示）；与 `HorizonChipBar` 并列；切换只写 `activeUnit`。
- 联动：`SelectionContext::activeUnit` / 状态栏「分析层段」；whatsThis + PaleoIcons（subInterval*）。
- 桥接：`linkage/unitselectionbridge`（`bindAnalysis` / `matchesActiveAnalysis` / `AnalysisBinding::enrich`）。
- 调用点：约束单因素/井因子、编图相面派生、厚度链、证据匹配；`factorResultHorizonKey`/`factorConstraintHorizonKey` 分离成果键与界面键。
- 兼容：旧单界面标签仍 legacy；不批量映射。
- 闸门：layering / file_budget 本轮引入清零；`tst_stratigraphic_subintervals` 通过；ts 缺失 643→595（既有红，未扩大 baseline 数）。

### 未完成
- Ribbon 各业务页内二级选择器与页内 horizonCombo 彻底去字符串语义。
- 测井相井综合面板 / 地震相掩膜提取 UI 全路径自动 bind DS。
- 多方案与修订版本选择器；子层段时深/解释就绪红绿灯接 DS。

## 续：井相 / 地震相 / 砂地比 → DS → 编图证据（Asia/Shanghai 2026-10-30 16:xx）

### 完成
- domain：`subintervalfacies`（砂地比分类、provisional 窗、地震 Slice|Interval + weighted_mode、证据行）。
- services：`subintervalfactors`（砂厚累计、DS bind single_factor/well_facies/seismic_facies、证据列表）。
- UnitSelection.subIndex；约束井因子按子层段深度窗；预测 `bindAnalysis` + tip/whatsThis/图标；证据导航稳定 ID。
- 测试：`tst_stratigraphic_subintervals::sandRatioWellFaciesAndEvidence` 通过。
- 闸门：layering / file_budget / ui_size / ui_invariants / i18n 本轮引入 0；ts 缺失维持 595（既有红）。

### 未完成（本续轮已清空强制项；见下节）
- （已迁到「生产路径落地」完成清单。）

## 续：生产路径落地（Asia/Shanghai 2026-10-30 18:xx）

### 完成（必须项 1–3）
1. **砂地比提取后自动 bind**：`autoBindSandRatioExtraction` + `openProjectDataService`；约束井因子 `sandratio` 发布成功后对 contributing 行写 DS `single_factor`/`well_facies`。
2. **测井相井综合面板**：`panelfacieschrome` 去掉层位 Combo，改为只读分析层段（`SelectionContext::activeUnit`）；`main.cpp` 注入 provider；中文 tip/whatsThis + 图标齐全。
3. **地震相掩膜 UI + 切片→层段发布**：`predictpage_seismic`（Slice|Interval 语义条、投票、覆盖率、发布按钮）；`paleomainwindow_attach_predict_seismic`（`publishSeismicSliceAggregate` / 切片后 bind；无 qgs 直连）；预测页 horizonCombo 标 `[legacy]`，分析路径走 UnitSelection。
4. **测试**：`autoBindAndSeismicPublishPath` 通过；聚焦闸门 layering / file_budget / qgis_spread / qgs_audit / docs_drift / ui_size / ui_invariants / mappingpages / shortcuts_shell 绿；`check_ts_stale` 既有红 595（未扩大）。
5. **file_budget**：`panelsetup::setupUi` 基线 411→339（只缩）。

### 仍待（明确限制）
- （已迁到「多方案/修订 + occurrence 红绿灯」完成清单。）

## 续：多方案/修订 + occurrence 红绿灯（Asia/Shanghai 2026-10-30 19:xx）

### 完成
1. **多方案/修订 UI**：`SubIntervalSelectorBar` 增加方案/修订下拉；`reloadFromProject` 经 `loadProjectSchemeFromDir` 读 DS；切换后 `makeSelection` 写入 `UnitSelection.schemeId/revision` → `SelectionContext::setActiveUnit`；状态行可读「方案名 / 修订」。
2. **红绿灯接 occurrence**：`summarizeSubIntervalOccurrences` 汇总井出现（complete/affected/pending）+ bindings（可用资料/受影响）；展开「状态」显示 `[绿]/[黄]/[红]/[灰]` 行。
3. **其余页 horizon 语义**：编图页厚度链/批导出 tip 标 `[legacy]`；whatsThis 增 ConstraintPage horizonCombo + 方案/修订条目；图标 `subIntervalSchemeCombo`/`subIntervalRevisionCombo`。
4. **测试**：`occurrenceSummaryAndSchemeLoad`；`check_ts_stale` 维持缺失 595（未扩大）。
5. **闸门**：layering / file_budget / ui_size / qgs-audit / qgis-spread / docs_drift / ui_invariants 绿。

### 仍待（明确限制）
- 业务页内 horizonCombo 彻底移除字符串驱动的分析路径（预测/约束已 legacy；分析只走 UnitSelection）。
- 编图发布行挂稳定 SubIntervalId（证据导航已按稳定 ID）。
- 方案下拉在超多修订时的分页/搜索（当前全量列表）。
