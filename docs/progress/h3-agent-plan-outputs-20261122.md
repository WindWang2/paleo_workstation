# H3｜版本化 DAG 的显式多成果与耐久交付（2026-11-22）

- 分支：`codex/h3-agent-plan-outputs`（worktree `../paleo-h3-agent-plan-outputs`）
- 基底：`origin/master = 8eca4644db31b4dcfd5ff11bebf39f0bbcfaf3a5`（开工 fetch，0 开放 PR）
- 范围：对有意保留的 v1 单终端边界增加 v2 显式多成果；不改 v1 行为、
  不扩科学算法面、不依赖 H1/H2/H4/H5 的新 API。

## 环境与构建（真实记录）

- 主机：Linux x86_64（Cachyos，glibc ≥ 2.43）。主 checkout 旧 superbuild 前缀
  早于 master 新锁/凭据机制（#1046/#1052），改走 deb 闭包路线：
  `vendor/fetch-deps.sh` 以主 checkout `vendor/cache/debs`（拷贝，非共享写）离线
  重建本 worktree 的 `vendor/prefix`，stamp `90a7bf14…` 与当前
  `vendor/deb-closure.lock` 匹配，configure 无 Office 警告。
- 主机偏差记录：无 dpkg（`dpkg-deb -x` 由 `/tmp/h3-tools/dpkg-deb` 的
  ar+bsdtar shim 等价实现，仅本机构建用）；Arch GCC `-print-multiarch` 输出
  为空，configure 显式 `-DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`；
  构建经 `./paleo-dev build -j4`（vendored Qt 工具需 deb 前缀 LD_LIBRARY_PATH；
  多 Agent 主机级互斥锁 `paleo-parallel-20261122-agent/heavy-build.lock`）。
- 测试：`./paleo-dev test`（自动注入 QGIS_PREFIX_PATH 与 XDG 沙箱）。

## 实现（工作包 ↔ 落点）

| 工作包 | 落点 | 说明 |
|---|---|---|
| W1 值类型/编码 | `src/workflow/capabilityplan_outputs.{h,cpp}` | `PlanExport/PlanExports`；声明顺序即持久顺序；JSON 严格编解码（未知字段/版本拒绝）；alias 必须 `isSafePathSegment`（节点 ID 不拼路径） |
| W2 闭包预检 | 同上 `validatePlanV2` | 复用 `validatePlanGraph`（capabilityplan.cpp 拆出的共享整图预检，v1 另加单终端检查，行为不变）；exports 上界 8 个/别名 128B；终端全覆盖 + 逆向可达全贡献 |
| W3 类型化执行 | `src/workflow/capabilityplanexecution.{h,cpp}` | 按 nodeId/portId 收集真实工件；空 path 不当成功端口（`missing_result_port`）；节点前取消（`cancelled`）保留有界前缀；子请求身份与 v1 完全同式 |
| W4 adapter/元数据 | `compositePlanSpec` | 组合父 spec：alias 为端口 ID，kind/unit/axis/datum/geometry/约束复制自真实算法端口；中间 raster 可额外导出 |
| W5 Host 冻结 | `capabilityhost.{h}` 声明 + `capabilityhost_plan.cpp`（共享 `preparePlanCommon`）+ `capabilityhost_plan_v2.cpp` | `preparePlanV2` 票据语义与 v1 相同；exports 增删/别名/顺序变化进意图摘要 → `idempotency_conflict` |
| W6 成组发布 | `capabilitynativeplan_v2.cpp` | 所有 exports 进父 targets/manifest；成员组内依赖 `parentPortIds`（未导出上游留描述性 `planUpstream`）；compute-only→重启→source publish 复用既有 `freezeSourcePublication`/copySource 链 |
| W7 预算/窗口 | executor + recovery 既有链 | 沿用 256KiB/1GiB/64 工件/16KiB 摘要预算；节点间取消/迟到取消/提交后保留 receipt；**共享缝一处**：`capabilityrecovery_worker.cpp` 失败分支在有界摘要非空时也持久化（v1 单能力 gateway 侧同语义，16KiB 截断保护不变） |
| W8 结果消费 | 无新 API | 父 `artifactManifest` 含全部别名端口；`operation_results`/`operation_read` 按原 API 列出/认证读取；子步骤仍是 v1 记录，不冒充可恢复 v2 对象 |
| W9 验收 | `tests/tst_h3_plan_outputs.cpp`、`tests/tst_h3_plan_publication.cpp` | 见下 |

冻结描述符版本语义：缺版本 = v1（按原样恢复）；`version=2` + exports = v2；
未知版本/带 exports 无版本 = 拒绝（`unsupported_plan_version`/`missing_plan_version`）。
`restoreNativePlan`（capabilitynativeplan.cpp）按描述符分派，v1 路径零改动。

CMake：`cmake/extra-h3-agent-plan-outputs.cmake`（源码进 `paleo_workflow`，
两个新测试最小链接集），根 `_paleo_extra_manifest` 仅追加本任务条目。

## 验收矩阵与证据（本地实测，2026-11-23）

环境：Linux x86_64（Cachyos，glibc 2.43+），deb 前缀 QGIS 4.2.3，RelWithDebInfo。
命令形态：`./paleo-dev build --target <t> -j4`；二进制直跑
（ctest 宿主与 deb 前缀 expat 冲突——G2 账本同款已知问题，直接跑二进制等价口径，
每测试独立 XDG 沙箱变量照常注入）。`ctest -N` 注册核对：#658 tst_h3_plan_outputs、
#659 tst_h3_plan_publication。

| 场景 | 结果 |
|---|---|
| v1 兼容（单终端规则/旧冻结 JSON/父图身份/重放） | `tst_capability_plan` 49/49、`tst_capability_recovery` 37/37（checkpoint/crash 窗口）、`tst_capability_recovery_native` 10/10（含 parentGraphIdentityIsCheckedBeforeAnyNewChild）、`tst_algorithm_output_set` 15/15 |
| v2 预检失败（缺 exports/未知引用/重复别名/重复源/无贡献/未覆盖终端/非法别名/超上界） | `tst_h3_plan_outputs::v2ValidationClosureFailures` 9 组全部按预期 code 拒绝；`exportsCodecRejectsUnknownVersionsAndFields` 未知版本/字段/无版本 exports 全拒 |
| IDW→contour A/B 三成果（导出 raster+A+B） | `hostPublishesThreeRealArtifactsAndReadsBackEveryPort`：3 个 catalog 版本、3 个真实文件（TIFF II/MM 头、GPKG SQLite 头）、shaA≠shaB、父 computeCount=1、子步骤私有目录隔离（主 journal 无子记录）、`operation_results` 列出 3 个别名端口（kind=raster/vector/vector）、逐端口 `operation_read` 认证读取成功 |
| 收据 string/非首批节点拒绝 | `firstBatchScopeRejectsNonFileAndNonPlanAdapters`：CONSTRAINT_RECEIPTS 引用、seismic、depth 均拒 |
| 变更 exports 后复用请求键 | `hostIdempotencyDistinguishesExportChanges`：删项/改名/重序均 `idempotency_conflict`；等价重放 replay 返回原 operation；v1 入口同 key 冲突 |
| prepare 后 epoch 改变 | `v2TicketsRespectEpochAndExpiry`：epoch++ 后 start → stale_session |
| prepared 后破坏字节 | `tamperedPreparedOutputFailsTheWholeSet`：checkpoint("prepared") 注入篡改 → 整组拒绝，catalog 0 版本 |
| compute-only→重启→source publish | `computeOnlyThenRestartedSourcePublishDoesNotRecompute`：重建 Host 后 source publish 3 成员一次整组 receipt；父/子 computeCount 均 1（copySource 不重算）；成员版本携带同一 manifest |
| 混合来源拒绝 | `mixedSourcePublicationRequestIsRejected`：带参数/额外输入 → mixed_source_operation_inputs |
| 节点间取消 / 提交后迟到取消 | `executorStopsBetweenNodesOnCancel`（后续节点不启动、前缀有界保留）；`lateCancelAfterCommitKeepsReceiptAndArtifacts`（cancel_after_commit，3 版本保留） |
| exports 预算上界 | `exportsRespectTheirOwnUpperBounds`：>8 项 plan_too_large、非法别名 invalid_export_alias，journal 无记录 |
| 共享缝回归（recovery worker 失败摘要缝 + host prepare 缝） | capability/python/pi/g1/g5 全族 22 个测试直接二进制全绿（tst_capability_host 12、tst_g1_modeltransport 65、tst_g2_piterminal 11 等，见上文命令） |

Source gates（`ctest -R "layering|ui_invariants|ui_size|ui_tokens|i18n|docs_drift|cpp_mainline|qgis_versions|ci_known_reds|qgis_spread|file_budget"`）：30/30 通过；
脚本直跑核对：`check_qgis_spread.py` 散布 169=baseline（新增 0）、`check_file_budget.py --strict`
新增 0、`check_ui_size.py` clean、`check_layering.py --transitive` 对基线通过。
`--transitive` 的 func/ui 两指标权威闸在 CI lint 车道（本地按 AGENTS.md 口径未注入 QGIS_PREFIX）。

## 未覆盖 / 已知边界

- Windows 侧未验证（本机为 Linux；v2 无平台分支代码，但 NativeFormat/路径行为未实测）。
- `layering_transitive` 本地未注入 QGIS_PREFIX 的 func/ui 两指标（CI lint 权威闸）。
- v2 worker 中 contour 的外部 raster 引用复用 `verifyNativeRasterInput`（operation_result
  引用或受信 raster_path）；模型自报裸路径仍按既有 untrusted_result_ref 拒绝，未新增
  授权面——与首批范围一致。
- 组内依赖 parentPortIds 仅表达「被导出的直接上游」；更深链路保留为描述性
  planUpstream（未捏造 Catalog 父版本），prompt W6 允许的降级口径。
- 取消窗口覆盖：节点间（executor 层）、prepared 破坏、提交后迟到；「计算前取消」
  由同一路径（首个节点前 cancelled 查询）覆盖未单列。
- 票据 5 分钟到期未单测（无时间注入设施）；epoch 失效已覆盖
  （v2TicketsDieWithEpochChange）。

## 独立复审（general-purpose reviewer，未参与实现，绑定 commit）

- 首轮（66f06c9e）：APPROVE，无阻断项。十三条要求域 12 项核清；
  silent-weakening 扫描（限额/检查/断言/基线）零发现；三条调用链
  （preparePlanV2→accept→worker→commitSet、版本分派、source publish）逐一核对。
- 复审发现已修复（fix commit）：
  1. **组内血缘跨重启丢失**（P2）：artifactManifest 端口行新增 `parents`
     序列化、outputsFromManifest 恢复（capabilitypublication.cpp，向后兼容——
     旧清单无字段按空处理）。这是经复现证明 plan 私有适配无法完成的必要发布
     缝（ledger：artifactManifest/outputsFromManifest 两函数的端口行 hunk）。
     新增断言：两条发布路径的 catalog parentVersionIds 均为
     raster→[]、contour→[raster 版本]。
  2. v1 多 issue 计划的首个 issue code 顺序变化（checkSingleResult 移到
     参数/输入检查后）——接受/拒绝结果不变，v1 全部测试绿；如实记录。
  3. codec 对「version=2 但缺 exports」给专用 missing_export_field。
  4. 清理：未使用 exported 集合与 kPlanDescriptorV1；测试更名
     v2TicketsDieWithEpochChange（原名称夸大覆盖）。
- 修复后复跑：tst_h3_plan_outputs 19/19、tst_h3_plan_publication 7/7、
  algorithm_output_set 15/15、capability_plan 49/49、recovery 37/37、
  recovery_native 10/10；gates layering --strict ✓、file_budget/qgis_spread
  新增 0、27 项 gate ctest 全绿。
