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

## 复核后补上

- 非数字的点值返回 `invalid_point_value`，不再变成 0。
- 冻结栅格使用 `DataCatalog::localGridCrsWkt()`，并传给算法的 `LOCAL_GRID_WKT`。`runConstraintIDW` 也注入同一串。输出栅格的 `PALEO_CRS_WKT` 与它对得上。
- 相干计划为 `split` 时，网关按 XL 块从样值列表取数并写入结果文件，不再先拷一整幅 float。调用方手里的 `QVariantList` 仍不计入账本。
- 签发的结果路径按 `documentId` 隔开。别的文档拿同一路径做等值线会得到 `untrusted_result_ref`。
- IDW 发布前先写 `publishing`。聊天工具说明改为：`current_head` 无效，compute-only 会调用已有核。

## 还没接到

- `runConstraintIDW` / `generateFactor` 的异步作业仍走 `runFactorAlgorithm`，没有改成 `capability::execute`。
- 操作日志仍是目录 JSON。提交后回执丢失还没有跨进程恢复测试。
- 聊天里的大计算仍在调用线程里同步跑。
