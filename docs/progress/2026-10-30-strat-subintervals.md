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
- 地震相 / 单井相 / 预测页迁移。
- 多方案与修订版本选择器；子层段时深/解释就绪红绿灯接 DS。
