# 算法原子化第二轮（2026-11-06）

本页只记本轮接到代码里的行为。`docs/architecture/ATOMIC_CAPABILITIES_AND_AGENT.md` 仍是目标设计。第一轮记录仍是 `algorithm-atomic-20261106.md`。

## 基底

- `origin/master` 仍是 `a80cfb54`，没有 `capabilitygateway.cpp`。
- PR #908 已 squash 进 #906。#906 head 是 `8d2e730209ceb68dd329aff16c54a3865a09fc50`，状态仍是 OPEN。
- 本分支 `feat/grok-algo-atomic-round2` 从该 head 拉出。PR 指向 `codex/document-data-io-20261106`。
- Data_service pin 仍是 `b690f769debc75fc3c7b56f55c1245ab880b9704`。

## 本轮接到的行为

- `constraint.idw` 的空意图或 `prepare` 只返回 `prepared`，`recomputed=false`。`compute-only` 调用 `QgisProcessingService::run("paleo:paleo_constraint_idw")`，写出可回读的 GeoTIFF，不增加 Catalog 版本。
- 等值线能力 `singlefactor.contours` 只接受本进程签发的栅格路径，调用已有 `FactorContourService::generateFixedContours`。
- `executePlan` 校验目标端口存在、种类和单位，并按拓扑把上一节点的 `result_path` 交给下一节点。
- 相干在整幅工作集超过预算、最小窗仍放得进时返回 `split`。`computeCoherenceTiled` 按 XL 内区块执行，并与整幅 `coherenceInterior` 对拍。
- 聊天工具的 `current_head` 不再当作提交事实。发布必须由宿主传入 `authoritativeHead`。对不上冻结头时返回 `stale_revision`。
- `OperationLog::cancel` 会记入进程内取消表。计算循环和 IDW 的 Processing hook 会看到它。
- 业务键改成 JSON 数组，避免 `|` 拼接碰撞。

## 这次 HEAD 上跑过的测试

worktree `paleo_workstation-algo-atomic-r2`，`-j2` 编译，`ctest -j1`，`QT_QPA_PLATFORM=offscreen`，`QGIS_PREFIX_PATH` 指向主仓 superbuild prefix。

- `tst_algorithm_contract`、`tst_depth_batch`、`tst_seisattr_group`、`tst_capability_gateway`、`tst_capability_round2` 通过。
- `check_layering.py --strict` 通过。`check_qgis_spread.py` 新增 0。`git diff --check` 通过。
- 没有测 p95。没有跑线上 CI，没有跑 Windows。
- 没有做独立子代理复审。操作恢复没有跨进程测试。

## 还没接到

- `runConstraintIDW` / `generateFactor` 仍用活图层进入同一个 `QgisProcessingService::run`，没有改成先快照再走 `execute`。
- 地震公开计算仍接收调用方给出的样值数组。分块避免了第二份整幅工作缓冲，但调用方数组本身不在账本里。
- 操作日志仍是目录 JSON，不是跨进程事务。提交后回执丢失的恢复还没有独立子进程测试。
- 聊天里的大计算仍在调用线程里同步跑。取消标志已经接上，但 UI 没有改成后台任务。
