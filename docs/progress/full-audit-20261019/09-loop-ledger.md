# Goal-Loop 台账（full-audit-20261019）

每轮：Inspect → Hypothesis → Change → Verify → Independent Challenge → Record → Next。

## R0 — 启动与环境（2026-10-19）

- **Inspect**：master=f6963e0b（BASE_SHA）；开放面=PR #568 + issues #553–#561（全部他属，
  排除矩阵见 01）；前两轮审计（#508/#524）已合并，其残留风险清单已读。
- **Change**：建 worktree/分支 codex/full-review-fix-review-fix-20261019；vendor symlink 接线；
  基线构建启动（flock + -j4）。
- **Verify**：构建进行中（2310 目标）。
- **Next**：5 只读 subagent（A 数据/IO、B 算法/服务、C workflow/AI、D qgis/ui、E 构建/测试）
  完成后聚合 → 04-finding-register → 修复计划。

## R1 — 主 Agent 交叉复核（进行中）

- paleomaplibrary/paleomapfiles/paleomapsource（#562 新代码）：copyDataset 孤儿文件/
  QFile::copy 目标冲突/worker 内建 QgsLayer/surveyArea 除 1e6 单位假定——
  **全部落入 PR #568 活跃面（paleomaplibrary.cpp 51 行改动/paleomapsource.*）→
  EXCLUDED_ACTIVE_WORK**，只登记不修。
- #564 新增 qgisdeferreddelete.h / faciespartitionstate.h 复核：契约设计良好，暂无发现。

## R2 — 补充覆盖（主 Agent，等待基线构建期）

- curveexpr.cpp 全文审查：干净（NaN 语义如实、词法/文法正确）；RECORD-ONLY：
  病态深嵌套无递归深度限制（输入面=UI 表达式框，触发面低）。
- faultsurface.cpp 结构级审查（函数清单+buildFaultSurface 逐段）：走向排序/转角
  限制/分叉拒绝/域混用拒绝等防御完备，未见缺陷；深网格段未逐行（诚实标注）。
- B-01/C-01/D-01/C-03/C-06/A-01/C-02/B-06 修复点预研完成（root fix 方案定型）。

## R3 — Fix 1 落地（2026-10-19）

- **Round Findings**: G1-01..08 + A-01/02/03 + B-01/02/06/08 + C-01/02/03/06 + D-01/03/04 + MA-F01 + E-04
- **Changed**: 6 commit（6a942d4f / b1076a62 / eea57e46 / 051ed34d / 0dc5a1a4 / docs）
- **Verify**: 门禁 17/17 绿（原 10 红）；定向套件全绿；tst_ingestplan 基线红→绿；
  tst_ui 仅剩基线既有 legendTitle 红（stash 法证实非本轮引入）；tst_mappingworkbench
  采样点随 B-01 配准校正后全绿
- **Review**: 待 Review 2
- **Remaining**: E-02/B-03/B-04/D-02/A-04/E-05/B-05 未修（P3 低/中，视 R2 意见入 Fix 2 或记录）

## R4 — Review 2 → Fix 2 → 终审（2026-10-19）

- **R2**：R2-1（MUST×1/SHOULD×3）+ R2-2（MUST×1/SHOULD×1）；B-01 双重独立证实；
  E2 golden 跟进全裁合法；E3 基线纪律合规
- **Fix 2**：eb39c31f（MA-F01 假阳性回退）/c632b50f（dedup 在盘复核+warnings 接线）/
  24a8bf4a（D-01 测试升级）；D-01 重构两案破坏既有套件→按实证回退
- **终审**：PASS-WITH-NOTES（FR-1 诚实性 MUST：D-01「收口」声明不实→已更正台账，
  残留 OPEN；FR-2 残留在码；FR-3 transitive 基线与 #578 未来对撞→PR 注明）
- **全量回归**：436/439（99%），基线 17 红→3 红（全部基线既有/已登记：
  layering_transitive #533 挂起、tst_ui legendTitle 基线红、tst_assetpaging 基线
  SEGFAULT）；基线 5 个二进制红转绿（含 A-03 修复的 tst_ingestplan）
- **NOTE 清理**：mapexport 重复注释、测试无用 include、known-reds 滞留引用、
  登记册状态列全量回填
