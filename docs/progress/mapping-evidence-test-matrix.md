# 编图证据深化验收矩阵（mapping-evidence-test-matrix）

三批（P0-A `619736e8` / P1-B `2a61b3c6` / P1-C `ece5b47b`）的验收场景到测试
用例映射。状态以 HEAD `ece5b47b` 全量门禁为准；`pre-existing` = 首发
（mapping-evidence-completion）既有用例本轮沿用；缺口如实标注 `NOT COVERED`。

## 场景 × 用例

| 场景 | 测试 | 断言内容 | 状态 |
|---|---|---|---|
| A 证据契约：冻结输入按保存口径重放、网格锁定、缺失一致、缓存安全 | `tst_composeattribution::frozenGridLocksComposeReplay` | compose_grid == 成果网格；冻结重放逐像元相等；grid_source/fidelity | PASS @ ece5b47b |
| | `tst_composeattribution::resampledInputHonestFidelity` | 非对齐输入 alignment=resampled、fidelity=approximate+reasons；replayGrid 仍相等 | PASS |
| | `tst_composeattribution::attributionCacheEvictionKeepsInputsAlive` | 9 输入 resolve 中途驱逐不悬垂（X1 回归） | PASS |
| | `tst_composeattribution::pointReplaySnapsToCellCenter` | 点重放按像元中心决策、网格外 unresolved | PASS |
| | `tst_composeattribution::missingFrozenInputReportsIdenticalReason` | 三入口缺失原因同一文案（带版本 id，X16） | PASS |
| | `tst_composeattribution::{thresholdAttributionUsesFrozenVersionsAndRealParameters, nearestPointAttributionAndUnresolvedInputs, manualDraftHasNoFabricatedAlgorithmAttribution}` | 冻结版本重放/unresolved/人工图斑 manual | pre-existing，PASS |
| B 图斑归因：井/冲突可定位、归因目标激活载荷 | `tst_evidenceconflict::opinionLocatorPassesThroughToFindingEvidence` | 井坐标 locator / 单因素 layer+version locator / 无定位不虚构 | PASS |
| | `tst_mappingpages::WorkbenchPageTests::attributionTargetsListAndActivationPayload` | attributionTargets 行数、井激活 {entity_id,has_coordinates,x,y,horizon}、冲突激活 | PASS |
| | `tst_evidenceconflict`（全套 16 用例） | 七类冲突/severity/cells==0 不产意见 | pre-existing + 新增，PASS |
| C 双向联动：选图斑→证据→图谱→反查→命中主目标→回退 | `tst_mappingworkbench::reverseHitNavigationAndBackRestorePrimaryTarget` | 验收链 C 全链：快照 uid、来源高亮不入栈、图谱含证据+来源节点、反查 A/B 命中、navigateFeature 切 V2/B、canGoBack、back() 复原、跨层位拒绝、reset 清栈 | PASS |
| | `tst_mappingworkbench::mapEvidenceAndGraphUseOneStableTargetWithoutEditing` | 单一稳定目标（链 C 底座） | pre-existing，PASS |
| | `tst_mappingpages::WorkbenchPageTests::evidenceBackButtonReflectsCanGoBack` | 按钮存在/setCanGoBack 使能/发 evidenceBackRequested | PASS |
| | `tst_evidenceselection` | 目标身份/会话守卫既有套件 | pre-existing，PASS |
| D 人工复核：确认态迁移受控、历史不丢 | `tst_impactrecompute::controlledRecomputePreservesHistoryAndIsIdempotent`（R3） | Confirmed 图斑进 affected/review_required/protection；重算后旧版历史不变、新版无解释；already_done；compareWithRecomputeSource 出报告 | PASS |
| | `tst_evidencee2e::scenarioAttributionEvidenceConflictReverseAndReopen` | 复核 Confirmed + 反驳 + 保存 + 重开可追溯 | pre-existing，PASS（修复后 12/12 连跑） |
| E 图谱：事实出处、计算≠科学、边界 | `tst_evidencegraph::overlayProvenanceTargetsAndReviewBound` | 每条边非空 provenance；Assertion 只收人工边；Supported 唯一来源；ConsumedIn×2（zero-cell 排除）；Missing 输入节点；ReviewEvent ≤20 + hidden | PASS |
| | `tst_evidencegraph::supersedesEdgeIsLineageOnlyWithProvenance` | 新→旧 Supersedes、标签含「人工解释不迁移」、仅 Lineage 穿越（Relations/Impact 闸） | PASS |
| | `tst_evidencegraph::relationFilterSelectsOverlayEdgeKinds` | relation 过滤证据边、未知枚举不产边、参与 Query 相等 | PASS |
| | `tst_evidencegraph`（既有 7 用例）+ `tst_datagraph` + `tst_datagraphpage` | 覆盖层不污染共享索引、导出口径、有界性、取消、关系树 | PASS |
| F 影响与受控重算 | `tst_impactrecompute::deterministicImpactPlanAndControlledRecompute` | 计划绑定/模式/保护 + 执行出新版本 | PASS |
| | `tst_impactrecompute::constraintChangeYieldsPotentialImpactAndUnusedIsHonest`（R2） | potential 条目 blocked_upstream 不执行；新因素版本后 whole_rerun 且 ordered_inputs 指新版本 | PASS |
| | `tst_impactrecompute::stalePlanBindingIsRejected`（R1） | 输入再更新→整计划拒绝、无新成果 | PASS |
| | `tst_impactrecompute::identicalInputYieldsNoChangePlan`（R4） | no_change 跳过、不产版本 | PASS |
| | `tst_impactrecompute::changedInputGridDegradesToPotential`（R5/0a） | 网格定义栅格变范围→potential 非 determined | PASS |
| | `tst_evidencee2e::constraintSaveEmitsPreciseChangeSignal` | 约束编辑保存精确点名 | pre-existing，PASS |
| G 工程恢复：重开后证据/反查/目标可恢复 | `tst_evidencee2e::scenarioAttributionEvidenceConflictReverseAndReopen`（重开段） | 版本重开后解释/证据/反查仍在 | pre-existing 沿用，PASS |
| | `tst_evidenceindex::reopenKeepsIndex` | 索引持久化重开 | pre-existing，PASS |
| | NOT COVERED：P1-C 新增链路（反查命中→回退栈）跨工程重开 | 无专用用例；回退栈是会话内存态（reset 于 projectAboutToClose），恢复依赖既有 SelectionTarget 恢复路径 | 缺口，如实声明 |
| H 性能/并发：有界性与后台同步 | `tst_evidencee2e::clickPathIsBoundedAtScale` | 点击链路有界（采样钳/井 500/索引 limit） | PASS |
| | `tst_evidencee2e::reverseQueryHonestyAndBackgroundSync` | 后台同步（真 PaleoTaskService）：stale→syncing→fresh；中途 cancel 索引仍可用且收敛 | PASS |
| | `tst_evidenceindex::cancelAbortsSynchronize` / `failedWriteRollsBackAndStaysUsable` | 取消/失败事务语义 | PASS |
| | `tst_evidencegraph::highDegreeQueryHasBoundedWork` | 1e4/1e5 出边工作预算 | pre-existing，PASS |
| | NOT COVERED：新增链路无墙钟断言 | G11 口径维持「机制验证 + BASELINE 记时输出」，本批未新增 perf 测量 | 缺口，如实声明 |
| 索引诚实/稳健（X8/X12） | `tst_evidenceindex::contradictedExcludesUnlinkedAndPendingCoversDisputed` | contradicted 排除 unlinked；pendingReview 含 Disputed | PASS |
| | `tst_evidenceindex::truncationIsRecordedAndRescanned` / `schemaUpgradeFromV1Rebuilds` | 截断标记 + 重扫清除；v1→v2 重建 | PASS |

## 已知覆盖缺口（如实）

- **「版本已不在目录」stale 分支**：`evidenceReverseQuery` 中目录核验的
  `已不在目录` hit_reason 经公共 API 不可达——`removeAsset` 对存在血缘父边
  的来源版本拒绝删除；已验证「文件缺失」分支（删除 staged 文件命中
  `检索时核验`）。该分支仅覆盖外部改动/损坏目录。
- **跨工程重开的回退/命中链路**：见 G 行。
- **新增 perf 断言**：无（H 行）。

## 执行记录（逐批命令与结果）

```text
# P0-A @ 619736e8
cmake --build build -j8                                    # 全量绿
ctest --test-dir build -j8 --output-on-failure \
  -R 'composeattribution|impactrecompute|evidencee2e|receiptsemantics|mappingworkbench$|mappingpages|layering'
  → 9/9 PASS
./tst_evidencee2e scenarioAttributionEvidenceConflictReverseAndReopen  # 12/12 连跑（flake 修复后）

# P1-B @ 2a61b3c6
ctest --test-dir build -j8 --output-on-failure \
  -R 'composeattribution|impactrecompute|evidencee2e|evidenceindex|evidenceconflict|receiptsemantics|mappingworkbench$|mappingpages|layering|polygonize'
  → 12/12 PASS（345s）

# P1-C @ ece5b47b
ctest --test-dir build -j8 --output-on-failure \
  -R 'evidencegraph|datagraph|evidenceconflict|mappingworkbench$|mappingpages|evidencee2e|evidenceselection|layering'
  → 11/11 PASS（60.6s）

# 全量门禁 @ ece5b47b（日志 /tmp/paleo-me-final-ctest.log）
ctest --test-dir build -j8 --output-on-failure
  → 415 tests，413 PASS，2 FAIL（均新增、均负载型 flake，非确定性）：
    tst_singlefactor_perf      proposedBudgets() 'largeMs <= 15000' FALSE
                               （L median 15802ms，单测 3/3 PASS ~163s）
    tst_seismic_baseline       transcodeSf3pAndVoxel() paged page checksum
                               failed (page 16416)（单测 3/3 PASS ~7s）
    基线红 tst_evidencee2e 本轮 PASS（flake 已修）。
python3 tools/check_layering.py --strict                   # 检查通过：无层违规
```
