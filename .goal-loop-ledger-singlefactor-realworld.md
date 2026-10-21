# Goal-Loop 账本：单因素图与地质约束插值业务闭环（2026-10-20）

- 基线：origin/master e6837e14；分支 goal/singlefactor-realworld-workflow-20261020；PR #606
- 历史 PR：#457/#496/#510/#511/#512 已合并；#580/#581/#602 仍 OPEN —— 本轮改动压到 singlefactordef + wellfactors/interpolation 最小面。
- 并行：#599（预测）不触碰。

## 里程碑
- [x] M1–M4 约束门控五态 + 提取解耦（见前）
- [x] M5 P0-五 地质语义：FactorGeologicalProfile；默认「推荐地质口径」；摘要「采用…；当前层位 N 口中 M 口有效」；技术字段折叠；负值排除；明确不用 GR 切砂
- [x] M6 P1 轻量：任务导向计算目标 → 真实 method id；QC 摘要沿用已有真实 CV（无造 RMSE）
- [x] M7 受影响 9 测 9/9；layering/i18n/ui_invariants/diff --check 通过
- [x] M8 审查 R3：因素切换强制 direct 覆盖 auto → 已修；QLatin1Char 断行 → 已修；无遗留 High/Medium
- [ ] 延后：P1 一键编排深化 / 约束反馈再深 / P2 编图衔接打磨；场景 G–N 专项（G/H 由版本键已覆盖）；全量串行 ctest

## 场景 I–N 覆盖对照（R4）
| 场景 | 覆盖 |
|---|---|
| I 切换泄漏 | 新增 tst_mappingpages::horizonAndFactorSwitchDoNotLeakState |
| J 取消/失败/中间/发布 | 既有 tst_factorworkflow::unifiedJobCancelSkipsPublish、failurePaths、staleGenerationDropsLocalDirectionPublish；tst_mappingpages::runBusyLifecycleAndCancel |
| K 绘制/编辑/撤销/删除/保存/重开 | 既有 tst_constraintediting（drawnConstraintJoinsUndoStack、deleteUndoAndCancelRestoresSnapshot、vertexRoundTripUndoRedoSave、saveKeepsIntervalLineGeometry…）；新增 receipts::readinessFollowsDeleteAndDisable（计划/就绪一致） |
| L 分析场 vs 制图场 | 既有 tst_factorworkflow::interpretiveContoursStayOffTheAnalysisField、localDirectionPublishesAnalysisAndSeparateWorkField、fixedLevelsKeepAnalysisIdentity |
| M 因素→编图 | 既有 tst_mappingworkbench::advancedSingleFactorsReachMappingWithoutReopeningProject、tst_factorworkflow::composePriorityAndParametersAreFrozen |
| N 等厚/距井 | 既有 tst_factorworkflow::strathickIsopachEngineRunsAndDeclares、welldistLineageEndToEndReopen |

## R4 新增
- 约束行双击 → constraintLocateRequested → 地图选中并 zoomToSelected（复用 layer service 实例化 constraints.<horizon>）；测试 constraintDoubleClickRequestsLocate
- 一键编排：未做——现有 generate 已经走 TaskService 统一作业（unifiedJobRunsAnalysisContourThroughRunner），再包一层会和 #580/#581 冲突，不够干净
- P2 编图衔接：既有“到编图中引用”+ M 场景测试覆盖，本轮不改

## M9 全量串行 ctest（2026-10-21）
- HEAD 4eb18136：390 个，通过 384，失败 5，构建失败 1（详见 /workspace/ctest-sfrw.log）
- 本 PR 引入：tst_wellattributes::modeSwitchAwayAndBackRestoresSavedExtraction（推荐口径默认值把已保存的字段口径提取藏起来）→ 506320e1 修复，16/16
- 既有（master e6837e14 同环境复跑同样失败）：tst_ui、tst_assetpaging、tst_pythonenv、tst_horizontracksvc；tst_workflows 编译失败（预测侧，本 PR 未改该文件）
- 磁盘：build-sfrw 全量带调试信息约 70G，曾把盘写满；改为逐个构建→运行→删二进制
