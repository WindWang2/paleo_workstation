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
  `tst_test_sandbox` 通过清除继承的沙箱环境后启动子进程，覆盖默认直跑入口；
  `tst_runtime` 按实际 `PALEO_TEST_REGISTRY_SANDBOX_ACTIVE` 状态判断配置路径约定。

向导行为回归覆盖 `taskGuideWalksPredictAndCompose`、
`taskGuideIgnoresWellPredictionDrafts` 和 `taskGuideTracksDisplayLocksAndRollback`，
使用实际开启编辑、绘制、锁定及撤销／回滚操作验证状态转换。

## 修后验证记录

待统一构建和测试后补齐。修复前两套件的通过结果不重复记为修后验收。
合入前需更新基线、重跑 Source gates，构建和测试并行度均不超过 8；
Windows 运行时行为须有该平台证据，Linux 编译或源码检查不能代替。

## 仍未覆盖

五个工作流工具页的真实桌面操作、高 DPI 和浅深主题人工验收仍待补齐。
上次窗口采集工具失败的限制仍保留，offscreen 测试不能代替有效的桌面操作
和截图证据。
