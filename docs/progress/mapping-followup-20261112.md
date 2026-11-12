# 编图任务向导与测试环境后续修复

日期：2026-11-12。起点：主线 `d8aa8aed`。实现分支：`codex/mapping-followup-fixes`。

## 修复前基线

核对 `TODOS.md` 的历史失败记录时，取得以下完整套件结果。编图结果来自
PR #932 合并前的工作分支 `e4dc51b8`，数据预览结果来自合并后主线 `d8aa8aed`。
这些是本轮修复前的依据，不能作为后续修改的回归结果。

| 套件 | 完整结果 | 耗时与证据 |
|---|---|---|
| `tst_mappingworkbench`（`e4dc51b8`） | 135 通过、0 失败、1 个既有可选跳过 | QtTest 179.061 秒；`build/Testing/qtest-first-run/tst_mappingworkbench.txt` |
| `tst_datapreview`（`d8aa8aed`） | 43 通过、0 失败、1 个既有可选跳过 | CTest 59.62 秒，QtTest 58.753 秒；`/tmp/paleo-followup-datapreview-test.log`、`build/Testing/qtest-first-run/tst_datapreview.txt` |

数据预览跳过项是 `captureSynchronousLoadingState` 的可选人工截图；
`reselectFocusesExistingTab`、其 `makeStack` 初始化，以及其后的测井、地震、
Office、XML 和工区导航用例均完成。测试维持原 300 秒预算，没有删除失败用例
或增加超时阈值。日志属于本机运行证据，后续复跑可能覆盖 `build/Testing` 文件。

2026-10-24 的四项编图界面断言失败见
[测井相支持记录](well-facies-data-support-20261024.md)；2026-11-05 的两套件
300 秒未完成记录见[阶段审查记录](staged-review-20261105.md)。本轮完整运行
没有复现旧超时，未取得该历史停顿的调用栈，不将其归因为 QSettings、QGIS 或
某次后续修复。

## 本轮修复范围

- **编图工作副本识别。** 任务向导排除测井预测的点草稿，通过现有
  `isWellPrediction` 判定复用 `result_kind` 元数据，不把尚未实例化的预测点草稿
  当作可进行相面编辑的副本。
- **真实修改与编辑会话分开。** 新增只读 `hasUnpublishedChanges()`，向导依据
  QGIS 编辑缓冲的实际修改或已有要素身份观察器记录的未发布空间修改进入
  “检查保存”；仅开启空编辑会话仍处于相面编辑步骤。
  导出继续使用 `hasUnpublishedEdits()`，未结束的编辑会话仍阻止导出。
- **步骤跟随编辑实时更新。** 绘制、显示层级锁定、撤销、重做和回滚后，通过
  `displayChanged` / `faciesEdited` 更新向导，不依赖调用者手动刷新界面。
- **Windows 测试直跑隔离。** `paleo_test_registry_sandbox.cpp` 在沙箱成功激活后，
  为未指定配置目录的直跑补上各测试独立的 `QGIS_CUSTOM_CONFIG_PATH`，保留显式路径。
  配置目录和沙箱生效标记同时同步 CRT 与 Win32 环境块，分别覆盖 QGIS 的 `getenv`、
  Qt 的宽字符读取和子进程继承；不再仅依赖 `SetEnvironmentVariableW`。
  `tst_test_sandbox` 通过清除继承的沙箱环境后启动子进程，覆盖默认直跑入口；
  `tst_runtime` 按实际 `PALEO_TEST_REGISTRY_SANDBOX_ACTIVE` 状态判断配置路径约定。

向导行为回归覆盖 `taskGuideWalksPredictAndCompose`、
`taskGuideIgnoresWellPredictionDrafts` 和 `taskGuideTracksDisplayLocksAndRollback`，
使用实际开启编辑、绘制、锁定及撤销／回滚操作验证状态转换。

## 第一轮修后验证记录

应用 `paleo` 和下列测试目标通过 `cmake --build build -j8 --target ...` 构建，
Office 编辑组件链接保持可用。使用 `./paleo-dev test -j4 -R ...` 运行 12 项 CTest，
全部成功退出，总耗时 213.89 秒；没有提高原有测试超时。QtTest 细分如下：

| 套件 | 通过 / 失败 / 跳过 |
|---|---|
| `tst_mappingworkbench` | 137 / 0 / 1 |
| `tst_datapreview` | 43 / 0 / 1 |
| `tst_projectsetup` | 46 / 0 / 7 |
| `tst_shell_projectlifecycle` | 19 / 0 / 1 |
| `tst_document_sources` | 23 / 0 / 0 |
| `tst_document_relations` | 18 / 0 / 0 |
| `tst_document_relation_session` | 10 / 0 / 0 |
| `tst_document_relation_reader` | 6 / 0 / 0 |
| `tst_document_sources_hy` | 0 / 0 / 1 |
| `tst_runtime` | 13 / 0 / 0 |
| `tst_startupdatabases` | 8 / 0 / 0 |
| `tst_test_sandbox` | 3 / 0 / 4 |

新增编图用例及原有任务向导用例全部通过。`tst_document_sources_hy` 因未设置外部
HY 数据路径整套跳过；工程设置等已有 opt-in 实数据／截图用例继续跳过。
本机为 Linux，沙箱套件的 4 个 Windows 场景未执行，不能把它们计为 Windows
修复实证。最后一轮 CRT 同步变更后另行重建、复跑 `tst_test_sandbox`。

构建与运行日志：`/tmp/paleo-followup-build.log`、
`/tmp/paleo-followup-related-build.log`、`/tmp/paleo-followup-tests.log`；
QtTest 明细在 `build/Testing/qtest-first-run/`。最后一次 `git fetch origin`、
`git rebase origin/master` 后重跑 Source gates，33/33 通过，未增加任何基线；
完整记录在 `/tmp/paleo-followup-source-gates.log`。CRT 同步修订后的沙箱重跑也通过，
日志为 `/tmp/paleo-followup-sandbox-test.log`（Linux 3 通过、4 个 Windows 场景跳过）。

## 追加问题修复

继续排查时，GitHub 没有开放 issue；本轮依据实现和回归用例确认并修复以下遗漏：

- **已提交工作文件的未发布状态。** 相面锁定等纯属性修改不产生几何审计事件。
  原生提交结束后，旧查询只检查编辑缓冲与几何审计，误把修改视为已发布。
  Observer 现在单独记录成功提交后的未登记状态，只在新版本登记成功后确认；
  后续空会话、切换编辑副本和回滚不会丢失先前已提交的修改。登记失败后向导仍提示
  检查保存，地图和矢量导出仍禁用，排除故障后可重试保存。状态沿用现有 Observer
  的会话生命周期，本轮不新增工程重开后的持久差异追踪格式。
- **来源别名随内容换版失效。** 旧逻辑按绑定 ID 查询当前来源，遗漏用户创建的
  文件别名。投影现在用资产与用途槽建立来源索引，内容变化时保留旧 SourceRef、
  取消主选并降为 Unresolved。同内容重定位继续有效；同井同用途的其他资产不受影响。
  该判定仅作只读投影，不改目录修订，也不按名称或路径重绑。
- **PDF 摘要大小写。** 发布门、快照复制和目标复核三处统一使用现有
  `shaEquals()`，使原生与 prepared 保存／发布对大小写等价的 SHA 采用相同口径。
  真正不同的摘要仍拒绝，准备失败清理暂存目录，不推进版本或发布状态。

新增回归分别为 `committedDisplayLocksStayPendingUntilVersionRegistration`、
`fileAliasSelectionTracksSourceRevision`（两数据行）、
`frozenPdfBindingUsesCaseInsensitiveSha` 和 `preparedSnapshotUsesCaseInsensitiveSha`
（合计八数据行）。它们检查实际提交、失败、重试、重开和输出文件内容。

应用及相关测试目标以 `-j8` 重建成功。追加修复后的六个功能套件逐项通过：

| 套件 | 通过 / 失败 / 跳过 |
|---|---|
| `tst_mappingworkbench` | 138 / 0 / 1 |
| `tst_featureidentity` | 34 / 0 / 0 |
| `tst_document_sources` | 25 / 0 / 0 |
| `tst_document_relations` | 18 / 0 / 0 |
| `tst_document_relation_session` | 10 / 0 / 0 |
| `tst_versions` | 46 / 0 / 0 |

编图 CTest 耗时 213 秒；相关测试使用 `-j2`、编图使用 `-j1`，未增加超时。
最后一次 rebase 后 Source gates 33/33 通过，基线未增长。日志分别为
`/tmp/paleo-issues-build.log`、`/tmp/paleo-issues-mapping-test.log`、
`/tmp/paleo-issues-related-tests.log`、`/tmp/paleo-issues-source-gates.log`。

额外的 `tst_mapsnapshot_scale` 不计入上述通过结果：坏解释拒绝用例和
`10k-short` 完成，后者两次保存的 worker 耗时分别为 38,910 / 127,691 ms。
运行到 `10k-long` 时主动终止整个额外规模测试，因此相关 CTest 命令以非零退出，
该项记录为 `Subprocess terminated`；长历史和两个 100k 行均未完成。
GDB 附加被系统 ptrace 策略拒绝，不能仅凭高 CPU 推断热点或死锁。
本轮不宣称规模性能通过或提速；待进一步阶段计时与调用栈定位。

## 仍未覆盖

五个工作流工具页的真实桌面操作、高 DPI 和浅深主题人工验收仍待补齐。
上次窗口采集工具失败的限制仍保留，offscreen 测试不能代替有效的桌面操作
和截图证据。
