# Goal Loop ledger — opt-04（智能预测、成果管理、人工修订与预测证据链深度完善）

- 分支：`goal/paleo-opt-04-prediction-evidence-20261016`（自 `origin/master` @ `dbb03269`）
- worktree：`../paleo_workstation-opt-04-prediction-evidence`；**主 checkout 全程零修改**
- 边界：本任务只优化既有预测/证据链模块，不新增产品方向、不碰任何 Open Issue / Open PR
- 资源纪律：编译一律 `-j4` 且持跨 worktree 锁（`<git-common-dir>/paleo-shared-build-j4.lockdir`，
  本机 Git Bash 无 `flock`，故用 `mkdir` 原子抢占 + 45min 陈旧回收，见 `build/lock.sh`，**host-local 不入仓**）
- 子代理：≤5，无嵌套派生；第 5 个为非作者独立 reviewer

## Oracle（验收，逐条需证据）

| # | Oracle | 证据 | 状态 |
|---|---|---|---|
| O1 | 缺证据不再与「证据健康」同形 | `predictionevidence::validate({})` 返回 `Warning/no_evidence`；3 个新用例 | ✅ |
| O2 | `probabilities` 语义不再把最不确定像素写成最确定 | `softmaxGrid` 退化向量 → 255/conf 0；classes==1 置信度 = \|p−0.5\|×2；3 个新用例（含 C>1 口径不变的反向保护） | ✅ |
| O3 | 主动取消失败语义收敛为成功语义 | `cancelPrediction` 先摘 `m_request` 再取消；`userCancellationIsNotReportedAsFailure` + 真实失败仍上报的反向守卫 | ✅ |
| O4 | 全无数据不发布「空图」并给出可操作原因 | `definedCells==0 && gridShapeOk` → 不登记 + 明确原因；`allNodataSeismicResultIsNotPublished` | ✅ |
| O5 | 空网格 ≠ 全无数据（原因不串味） | `gridShapeOk` 前置；`emptyGridSeismicResultReportsShapeMismatch` 双断言（含 `不一致`、不含 `全部为无数据`） | ✅ |
| O6 | 真实 API 2.0 协议 E2E：无输出 / 空模型列表 / 非默认层位 | 假端点 `WellApiServer` + `WellApiEnvironment`；3 个新用例 | ✅ |
| O7 | 非默认层位必须落真实证据链字段 | D62 用例断言 `schema/inputs[].version_id/parameter_hash/model.method/model.mock/support` | ✅ |
| O8 | 静态门禁 | `git diff --check` / `check_layering.py --strict` / `check_ui_invariants.py --strict` / `check_i18n.py` 全绿 | ✅ |
| O9 | 既有行为零回归 | 定向 14 项 ctest：11 通过；2 项故障为**基线既存**（见「基线既存故障」）；`tst_mappingworkbench` 45→50 passed，唯一失败项为环境项 | ✅ |
| O10 | 独立 review 后可执行项已修 | 第 5 subagent 报告 8 条 finding；Major 0；采纳 4 条（假绿加固 ×2、原因精度、要素循环空转、`-o` 重复） | ✅ |

## 环境阻塞（host-local 处置，不入 PR）

`vendor/qgis_attribute_table` 在 git 中只有 `.cpp/.h`、缺 `.ui`，`ui_qgsattributetabledialog.h` 等
三个 uic 头无法生成 → `paleo_qgis_attribute_table`（`paleo_ui→paleo_core` 依赖链）本机不可编译，
`tst_mappingworkbench` / `tst_aiwiring` 等伞式测试全部无法构建。处置：

1. 从 QGIS `final-4_2_2`（与该 vendor README 声明同源）取 `src/ui/{qgsattributetabledialog,qgsdelattrdialogbase,qgsfeaturefilterwidget}.ui`；
2. `uic` 生成到 gitignored 的 `build/gen-ui/`；
3. configure 追加 `-DCMAKE_CXX_FLAGS=/I<...>/build/gen-ui`（Git Bash 需 `MSYS_NO_PATHCONV=1`，否则 `/I` 被当路径）；
4. Qt 6.11 把 `QAction` 移到 QtGui，uic 仍吐 `<QtWidgets/QAction>` → 一行 shim `build/gen-ui/QtWidgets/QAction`。

`vendor/onnxruntime` 本机缺失（否则 8 个 ORT 测试静默消失）→ 展开官方 `onnxruntime-win-x64-1.19.2`（gitignored）。

## 基线既存故障（非本任务引入，已在干净基线与本次运行中一致复现）

| 现象 | 判定 | 证据 |
|---|---|---|
| `tst_onnxworkflow` 退出码 `0xc0000409` | 基线既存：逐用例 14/14 PASS，崩在 `QgisRuntime::shutdown()` 之后（teardown） | 改动前基线运行同样的 14/14 且同样 0xc0000409 |
| `tst_interpretationevidence` 被 ctest 标 `Not Run` | 调度项，非用例失败 | 单独重跑（见 R1 记录） |

## 决策账

| # | 决策 | 依据 | 取舍 |
|---|---|---|---|
| D1 | U1 只加 `Warning`，不把缺证据升为 `Blocking` | 全仓 grep `paleo::evidence::validate`：4 个调用点（`derivedassets.cpp:218`、`mapversioncontroller.cpp:152/357`、`mapversioncontroller_async.cpp:48`）**只按 `"Blocking"` 动作**，无按 size/非空的消费者 | 用户可见发布门禁**不变**（诚实记录：本单元只让 API 层可区分，不是行为修复）；把升格留给有发布权的人 |
| D2 | U2 只在「C>1 且 `bestP<=0`」时判无数据 | 合法归一化概率向量 `max≥1/C>0`，不可能触发；C>1 的 `entropy/lnC` 公式一字未改 | 对「以 probabilities 语义吐未归一化全负向量」的模型，旧=类0+置信1.0，新=无数据——更诚实，但属行为变化，已写进 PR 已知影响 |
| D3 | U4 用已有 `id==m_request.id` 闸门，不新增标志位 | `failed`/`progress`/`detailChanged` 三槽均已按 id 短路（`mappingworkbench.cpp:346/363/368`） | 改一行顺序即可，零新状态；真实失败路径逐字不变 |
| D4 | U5 全无数据判据要求 `gridShapeOk` | 否则空数组/格数不符会套用「全部为无数据」这一更具体的原因，把用户引向错误排查方向 | 格数不符仍不发布，原因改报「与请求不一致」 |
| D5 | `classes` 语义的 `label < kMaxTileClasses` **不改**为 `label < classes` | 该语义下 `classes` 恒为 1（`classes == logits.size()/pixels`），改成 `< classes` 会把所有多类标签判无数据 | 主动放弃一个「看起来像缺陷」的候选，避免自伤 |
| D6 | `mappingworkbench_predict.cpp:150` 的提示文案「远端预测未配置，走本地引擎」**不改** | 该串是方向 51 的显式产品决策，被 `remotepredictconfig.cpp:39`、`aiwiring.h:17`、`appcontext.h:77`、`mappingworkbench.h:50`、`mappingworkbenchpage.h:55` 共同引用 | 文案与 `fail()` 并存确有一处口径张力，但改动会重解释已定产品意图 → 记为**待产品确认**，不在本 PR 内改 |

## 轮次记录

| 轮 | 内容 | 结果 |
|---|---|---|
| R0 | 基线冻结：BASE_SHA、排除矩阵（2 Open PR + 26 Open Issue 指纹）、只读基线核实（世代号四层守卫 / 层位顶底裁剪 / `draft.` 修订落盘 / 密钥处理） | 见 `docs/progress/opt-04-prediction-evidence-20261016.md` |
| R1 | 环境打通 + 14 项定向 ctest 基线 | 10/12 通过；`tst_mappingworkbench` 45 passed/1 failed（环境），`tst_onnxworkflow` 逐用例 14/14 但退出崩 |
| R2 | 读码 + 4 路只读 subagent 探查 → 可执行 backlog | 5 个单元（U1/U2/U4/U5/U6），全部先验证「改前为红」 |
| R3 | U1+U2 实现 + oracle + 红→绿 + commit `baac8371` | 3+3 用例 |
| R4 | U4+U5 实现 + oracle + 红→绿 + commit `f69fb2c5` | 45/3 → 47/1 |
| R5 | U6 E2E 补齐（空输出 / 空模型列表 / 非默认层位） | 50 passed/1 failed（环境项） |
| R6 | 第 5 subagent 独立 review → 4 项采纳（假绿加固、原因精度、要素空转、`-o` 重复）+ U5 新 oracle | 见下 |
| R7 | 复跑 + 静态门禁 + PR | 见下；PR [#493](https://github.com/WindWang2/paleo_workstation/pull/493)（OPEN，**未合并**） |

## 独立 Review 采纳账（非作者第 5 subagent）

| Finding | 严重度 | 处置 |
|---|---|---|
| 两条 API E2E 可「因错误原因通过」（未断言真的打到服务端） | Minor（真缺陷） | **采纳**：分别补 `server.bodies.size()>=1` / `server.modelRequests>=1` 判别式断言 + 说明注释 |
| 全 NoData 判据会吞掉「格数不符/空网格」的更准确原因 | Minor | **采纳**：`gridShapeOk` 前置 + 新 oracle `emptyGridSeismicResultReportsShapeMismatch` |
| D62 用例要素循环在 0 要素时空转 | Nit | **采纳**：前置 `QVERIFY2(layer->featureCount() > 0, ...)` |
| 无条件追加 `-o`（调用方自带时重复） | Nit | **采纳**：两处 `main()` 先探测 `-o`/`-output` |
| U1 新增 Warning 无消费者（用户可见行为不变） | Nit | **不修**（不是缺陷）：U1 的目标就是 API 可区分；已在 ledger D1 显式记录 |
| U2 `continue` 对 `stitch` 的二阶影响（退化格不再以 pIn≡0 参与重叠混合） | Nit | **记录**：符合 #143 意图，已补进 PR 已知影响 |
| `unknown_schema` 非新增分支且为 Blocking | Nit | **记录**：任务书前提修正；分支顺序经 reviewer 复核**无洞** |
| D62 用例要素循环写法 | Nit | 同上（已加固） |

Reviewer 明确「Cannot verify」项：未在本 worktree 跑 ctest（静态推断）、`-o` 追加的 `QList<char*>` 生命周期未实测、
D62 图层要素数 ≥1 属静态推断。**本主 Agent 已用真实运行补齐前三项**（见 R5/R7 记录）。
