# 算法原子化试点（2026-11-06）

本页记录本轮已接线的行为。`docs/architecture/ATOMIC_CAPABILITIES_AND_AGENT.md` 仍是目标设计，没有整篇改为已实现。

## 基底

- 工作分支 `feat/grok-algorithm-atomic-modular`，叠在未合并的 PR #906 上。
- 固定 #906 head `b92ec0da3b8eed5af0631987b703a38cdadbd22e`。
- SDK pin：`vendor/Data_service` `b690f769debc75fc3c7b56f55c1245ab880b9704`。
- PR #907（OSGeo4W 钉版）不在本分支。
- 多输出发布复用 #906 的 `prepareAlgorithmOutputSet` / `DerivedAssetRegistrar::commitSet`，没有另写一套。

## 已接线

- `AlgorithmSpec` 增加契约版本、实现版本、执行形状、可复现性和端口单位。未知版本拒绝，不改叫最新版。
- `CapabilityRegistry` 只引用算法 id。参数默认值仍只在 `AlgorithmSpec`。`complete` 不等于可调用。
- 时深批转换调用 `VelocityModel::wellDepthForTwt` / `wellDepthForTwtExtended`。不新写 Vint 公式。
- 栅格生产路径仍调用 `convertTimeGridToDepth`。其外推策略名是 `spatial_idw_extended_v1`（至少一口井在范围内时，范围外井参与 IDW）。这不是新策略。
- 旧 TD 文本仍用 `legacyControlDepth`：有 TVD 用 TVD，否则用 MD。这是遗留替用，不是 MD→TVD 换算。公开批转换拒绝把 MD 当 TVD，也拒绝把 OWT 当 TWT，除非显式 `owt_double_to_twt_v1`。
- 地震 RMS / 包络 / 相位 / 相干调用既有 `seismicattr` 核。包络和相位共用一次 `complexTraceAnalysis`。
- `paleo:paleo_constraint_idw` 的数值核仍是 `ConstraintIDWAlgorithm::processAlgorithm`。本轮只统一有效标量参数，并要求公开调用给出点引用和子层段 id。不把方向约束降成普通 IDW。
- 相图输出规格 `paleo:facies_draft_mapping` 进入同一注册表，发布仍走 #906 的成组提交。
- 操作身份按 `(document, callerScope, clientRequestId)` 落在调用方给的目录。同键同摘要回放，同键不同摘要冲突。提交后取消保留回执。
- 新路径使用进程内 `ProcessBudgetLedger`。未迁入的旧读取不在这个额度里。
- 相干只要会执行，就按整幅 `nIl*nXl*nS` 计价，包括和包络、相位或 RMS 一起请求。整幅放不进预算时返回 `budget_exceeded`。执行器还不会沿 XL 切块，所以不返回未执行的 split。样数乘积在相乘前做溢出检查，放不进 `int` 则 `invalid_shape`，并且在分配体缓冲之前拒绝。
- 包络按道批切。一次请求的操作日志按目录互斥，后写不会盖掉另一条 operation。
- operation 回执以 `operations.json` 为准。缓存文件写失败不撤销已经落盘的回执，也不把这次接纳判成 `persist_failed`。提交前先把状态写成 `publishing`。回执文件本身在提交后写失败时，`finish` 返回 `receipt_persist_failed` 且响应里的 `commitState=committed`；磁盘仍停在 `publishing`。同键再入返回 `commit_uncertain`，不再提交。对 `publishing` 调取消也返回 `commit_uncertain`，不把它改写成未提交。

## 未接线

- 全仓旧 Processing / Native 入口仍是 `legacy-adapter` 或 `internal`。
- 测井相预测保持 `unavailable`：不伪造模型，也不改现有 HTTP 工具的端点语义。
- 没有检查点恢复。`resumeSupported` 为假；可以从冻结计划重新执行。
- 约束 IDW 的栅格发布仍在既有 Processing 工作流。公开 `constraint.idw` 的 `publish` 意图返回 `publish_via_processing_workflow`，不假装已经换了核。
- 属性任务的 SEG-Y 读取仍在 `SeismicTaskService` 的任务线程。纯核只吃调用方拥有的缓冲，不把同一个 `SgyVolume` reader 标成任意并行。
- 没有测量 p95 或峰值 RSS。共享计算的证据是：包络和相位同一次 `complexTraceAnalysis`，包络预算下界高于输出窗字节数。

## 本地验证

命令在 worktree `/home/kevin/projects/paleo_workstation-algo-atomic`，`CMAKE_BUILD_PARALLEL_LEVEL=2`，`ctest -j1`，`QT_QPA_PLATFORM=offscreen`，`LD_LIBRARY_PATH` 指向主仓 `vendor/superbuild/prefix/lib`。

- `tst_seisattr_group` 9/9，退出码 0。
- `tst_capability_gateway` 12/12，退出码 0。
- `tst_depth_batch`、`tst_algorithm_contract`、`atomic_kernel_boundary`、`tst_aichat`、`tst_aichatprojectquery`、`tst_aichattoolloop` 经 ctest 通过。
- 回执改动之后用 ctest 重跑 `tst_depthconversion`，通过（5.51s）。直接启动该二进制会因缺少 ctest 的 XDG 沙箱在 `QgsSettingsException` 处中止，这不是本轮回归。
- 更早一轮还通过了 `tst_algorithm_output_set`、`tst_algorithm_spec`、`tst_algorithm_adapters`、`tst_seismicattr`、`tst_velocitymodel`。这几项没有在最后一次回执改动之后重链。
- `python3 tools/check_layering.py --strict` 通过。`git diff --check` 通过。

## 复核

- 第一轮独立只读复核发现数值和操作缺陷，已修并有回归。
- 第二轮独立只读复核仍有相干混合预算、样数溢出和回执落盘问题，已修。`secondReviewFixes` 与 `mixedCoherenceIsPricedAndShapeCannotOverflow` 覆盖这些修复。
- 第三轮数值复核结论：没有新的 P0/P1/P2。
- 第三轮操作复核的子代理因接口余额中断，没有给出结论。作者对照 `persist`、`finish`、`notePublishing` 和已通过的 `secondReviewFixes` 复核了这三项，这一轮操作侧标为非独立评审。
