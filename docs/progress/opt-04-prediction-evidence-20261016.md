# opt-04 智能预测、成果管理、人工修订与预测证据链深度完善

任务专属账本。分支 `goal/paleo-opt-04-prediction-evidence-20261016`，
worktree `../paleo_workstation-opt-04-prediction-evidence`。

## R0 基线冻结

| 项 | 值 |
|---|---|
| BASE_SHA | `dbb03269ca1809145e1ee39b699c6fd08916a317`（origin/master，2026-10-16 快照一致） |
| 主 checkout | `C:/Users/wangj.KEVIN/projects/paleo_workstation` @ `198401ab`（**全程零修改**） |
| 分支 | `goal/paleo-opt-04-prediction-evidence-20261016` |
| 编译 | `-j4`，跨 worktree 锁 `.git/paleo-shared-build-j4.lockdir` |
| ORT | 本机 vendor 缺失（Landmine 10），已装 `vendor/onnxruntime` 1.19.2（gitignored），8 个 ORT 测试可见 |

### 排除矩阵（EXCLUDED）

**Open PR（文件级禁写）**

| PR | 文件 | 处置 |
|---|---|---|
| #486 | `src/domain/welllogfacies.cpp`、`src/ui/seismic3d/seismic3dviewpanel.{h,cpp}`、`tests/tst_wellfacies.cpp` | 不碰、不重复实现 |
| #487 | `src/qgis/qgisstyleservice.cpp`、`tests/tst_services2.cpp` | 不碰 |

**Open Issues（指纹级禁写）** — 26 个（#460–#485）。与本域直接相关的指纹：

| Issue | 指纹 | 本任务处置 |
|---|---|---|
| #460 | tileinference 去垫片在形状校验前索引输出张量 | 不调整 de-pad/形状校验**顺序** |
| #469 | 远端预测响应体无大小上限 | 不加 size cap |
| #479 P2 | `remotepredictrouter.cpp:340` degradeOrDie + `onnxlocalpredictor.cpp:37-53` 主线程同步 ORT 降级 | 不改降级时机/不迁 worker |
| #479 P2 | `onnxpredictionservice.cpp:687-713,406-593` 单互斥锁粒度 | 不改锁粒度/临界区 |
| #479 P3 | `horizonsuggest.cpp:93-135` O(n²) 去重 | 不提 |
| #479 P3 | `wellfaciesservice.cpp:323` `m_accepted` 无界 | 不提 |
| #479 P3 | `tst_tileinference` 缺「形状不符」用例 | 不补该用例（本任务补的是语义/取值域用例） |
| #476 P3 | `welllogfacies.cpp:180-191,220,227-230` 采样步长判据 | 不提（且属 PR #486 文件） |
| #480 P3 | `mappingworkbench_predict.cpp:813-824` 登记成功+井表合并失败并存 | 不提 |
| #481 P2/P3 | `paleomainwindow_attach_predict.cpp:109-111` 无守卫 removeRecursively；`:100-137` contour handler 重复 | 不提 |
| #464/#465/#472/#473/#475 | busy-token / saveEditingVersion / Qgs\* 越层 / attachWorkbench / CI 红项 | 不提 |

### 环境阻塞（已解决，非本任务缺陷）

`vendor/qgis_attribute_table` 在 git 中**只有 .cpp/.h，没有 .ui**，
`ui_qgsattributetabledialog.h` 等三个由 uic 生成的头因此无法生成 →
`paleo_qgis_attribute_table`（被 `paleo_ui`→`paleo_core` 依赖）在本机不可编译，
`tst_mappingworkbench`/`tst_aiwiring` 等伞式测试全部无法构建。

处置（**全部 host-local，不落进 PR**）：
1. 从 QGIS `final-4_2_2`（与该 vendor README 声明的同源版本）取 `src/ui/*.ui` 三个文件；
2. `uic` 生成到 `build/gen-ui/`；
3. `CMAKE_CXX_FLAGS += /I<...>/build/gen-ui`（configure 时传入，注意 Git Bash 路径转换须 `MSYS_NO_PATHCONV=1`）；
4. Qt 6.11 把 `QAction` 从 QtWidgets 迁到 QtGui，但 uic 仍吐 `<QtWidgets/QAction>` → 加一行 shim `build/gen-ui/QtWidgets/QAction` 转发 `<QtGui/QAction>`。

### 只读基线（已核实「已实现」，不作为本任务交付）

- 轮次号/世代号四层守卫（`remotepredictrouter.cpp:256/262/277/286/298/309/328` + 传输层 `:47/:93/:99`）——有 3 个测试守护。
- 层位顶底精确裁剪（`welllogfacies.cpp:382-454`）+ 修订必须落 `draft.`（`wellpredictionreview.cpp:78/131`）。
- `evidence::build` / `parameters` 簿记字段剔除 / `interpretationevidence::revise` 乐观并发。
- 密钥处理（keychain + 0600 回落 + 错误串脱敏）。
- 已合并：#437（证据/图斑快照）、#458（预测闭环）、#459。

## R1 运行与测试基线

定向 14 项 ctest（与 `.github/workflows/ci.yml` 同款静态门禁 + 本域预测/证据链/ONNX/路由/adapter 测试）：

| 测试 | 基线 | 说明 |
|---|---|---|
| `tst_mappingworkbench` | 45 passed / 1 failed | 唯一失败 `removeDraftDeletesWorkingCopy`：临时目录内含**只读文件**导致 Windows 无法删除，宿主环境项、非分支引入 |
| `tst_onnxworkflow` | 14 passed / 0 failed（退出码 `0xc0000409`） | 用例全过，崩在 `QgisRuntime::shutdown()` 之后的 teardown；**基线既存**，与用例无关 |
| `tst_predictionevidence` | 4 passed | — |
| `tst_tileinference` | 19 passed | — |
| `tst_remotepredictrouter` / `tst_evidencegraph` / `tst_evidencesourcevalidation` / `tst_faciescatalog` / `tst_aiwiring` / `tst_modelregistry` / `tst_onnxsessions` / `tst_onnx` / `tst_predict_challenger_r2` | 17/10/15/6/9/19/24/9/18 passed | 全部 0 failed |
| `tst_interpretationevidence` | 无 exe | 宿主未构建该目标 → ctest 记 `Not Run`/跳过，非用例失败 |

## R2 可执行改善 backlog

分类：**(a)** 已实现且有测试 / **(b)** 已实现但需加强 / **(c)** 新发现未立案缺陷。
经 4 路只读探查 + 主 Agent 复核，收敛为 5 个可交付改善单元（U1/U2/U4/U5/U6），
U3、U7+ 因落在排除矩阵或属产品决策而未立（见 ledger「决策账」D5/D6）。

### 改善单元（本 PR 交付）

| # | 单元 | 文件 | Oracle | 红→绿 | commit |
|---|---|---|---|---|---|
| U1 | 证据链「缺记录 / 未知 schema」显式 `Warning`，不再与「证据健康」同形 | `src/domain/predictionevidence.cpp` | O1 | ✅ | `baac8371` |
| U2 | `probabilities` 语义置信度修正：退化向量 → NoData(255)/conf 0；`classes==1` 改用 `\|p−0.5\|×2` | `src/ai/tileinference.cpp` | O2 | ✅ | `baac8371` |
| U4 | 主动取消不再误报「编图操作未完成：已取消」 | `src/workflow/mappingworkbench_predict.cpp` | O3 | ✅ | `f69fb2c5` |
| U5 | 全 NoData 结果不发布空图；空网格 / 格数不符原因不套用「全部为无数据」 | `src/workflow/mappingworkbench_predict.cpp` | O4 / O5 | ✅ | `f69fb2c5` |
| U6 | 真实 API 2.0 协议 E2E：空输出 / 空模型列表 / 非默认层位证据链落盘 | `tests/tst_mappingworkbench.cpp` | O6 / O7 | ✅ | 本 PR |

### 新增 / 加固用例（12 个，全部先验证「改前为红」）

| 文件 | 用例 | 守护单元 |
|---|---|---|
| `tst_predictionevidence` | `validateWarnsOnMissingEvidence` / `validateDistinguishesUnknownSchemaFromAbsent` / `validateMissingSourceMessageMatchesBehavior` | U1 |
| `tst_tileinference` | `probabilitiesSingleChannelConfidenceIsNotConstant` / `probabilitiesDegenerateVectorIsNodata` / `probabilitiesMultiChannelConfidenceUnchanged` | U2 |
| `tst_mappingworkbench` | `userCancellationIsNotReportedAsFailure` / `allNodataSeismicResultIsNotPublished` | U4 / U5 |
| `tst_mappingworkbench` | `emptyGridSeismicResultReportsShapeMismatch` | U5（原因不串味，R6 新增） |
| `tst_mappingworkbench` | `wellsRemoteApiEmptyPredictionsPublishNothing` / `wellsRemoteApiEmptyModelListPublishNothing` / `wellsRemoteApiNonDefaultHorizonEvidenceIsRecorded` | U6 |

## R7 收尾验证与交付

| 门禁 | 命令 | 结果 |
|---|---|---|
| 空白/冲突标记 | `git diff --check` | ✅ 干净 |
| 分层 | `python tools/check_layering.py --strict` | ✅ 0 违规 |
| UI 不变量 | `python tools/check_ui_invariants.py --strict` | ✅ `violations=0, baseline=0` |
| i18n | `python tools/check_i18n.py` | ✅ 无裸字面量 |

定向 ctest 复跑（二进制含 R6 加固）：`tst_mappingworkbench` **51 passed / 1 failed**（唯一失败仍为上述基线环境项）、
`tst_predictionevidence` 7 passed、`tst_tileinference` 22 passed、`tst_onnxworkflow` 14 passed，
其余 9 项零回归，`tst_interpretationevidence` 仍为宿主未构建项。

**合计**：+12 用例（45→51、4→7、19→22），零既有用例回归。
