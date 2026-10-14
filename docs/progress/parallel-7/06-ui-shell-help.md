# 方向 06 · 分阶段 ledger：六页主界面 / Dock·Ribbon / 交互可用性 / 帮助体系

- 分支：`goal7/ui-shell-help-20261014`（worktree `../paleo-wt-goal7-ui-shell`）
- base：`origin/master` = `b0520429c21f858a1c053b876f511680088564a9`（开工与 push 前两次核对，未移动）
- 代码提交：`fd3f7273`（dock 策略 + 布局反馈）、`7ce9cf8c`（帮助条目）
- 基线与证据细节：`06-baseline.md`；PR 正文：`06-pr-description.md`
- 构建：Debug / Ninja，全程 `flock -x /tmp/paleo_workstation_global_build_j4.lock`，`-j4`

## 阶段 1 微循环（共 5 轮，均有验证记录）

| 轮 | 焦点 | 文件 / 函数 | Oracle | 结果 |
|---|---|---|---|---|
| L0 | R0 围栏与基线 | 全仓只读；`gh pr/issue`；`fence_check.py` | 开放 PR #437 文件集、Issue 文件级指纹、基线 build+test | 基线 `paleo` 构建 0；`tst_ui` 在 pristine master 即挂死（见 baseline §3），记为既有红，不修 |
| L1 | B 布局：写红测 | `tests/tst_shell_chrome.cpp`（新） | 在 pristine master 上复现 | 当时的 5 个用例中 R-1 / R-2 / R-4 **两轮稳定为红**；R-3 与页策略基本用例为绿（非回归场景）。随后补了 2 个持久化用例（跨会话保持用户隐藏、旧设置缺键），这两个只在修复后树上跑过，没有在 pristine master 上测过红 |
| L2 | B 布局：修复 | `PaleoDockManager::restoreLayout` / `setIntentHooks` / `captureIntent`；`PaleoMainWindow::buildShell`（钩子）、`saveWindowState`、`restoreWindowState`；`applyPageDockPolicy`、`hideAllDocksForCorrelation`（自 `showPage` 抽出，`showPage` 与恢复后重落策略共用） | `tst_shell_chrome` 全绿；`tst_dockmanager` 新增 4 例；不改 `paleomainwindow.h` | 绿，两遍一致 |
| L3 | D 帮助 | `src/ui/help/whatsthiscatalog.cpp`（4 条）、`translations/paleo_zh_CN.ts` | `tst_shortcuts_shell` 新增 2 例，修复前红（`correlationWebAddress` 落空、`statusCrs` 帮助不含「工程坐标 · 米 · 未投影」）→ 修复后绿 | 绿；`statusCrs` 措辞经复核补「没有代号时显示坐标系名称」（对照 `paleomainwindow_status.cpp:119-120`）后复测通过 |
| L4 | B 屏幕矩阵 | 临时探针（未提交） | 8 个屏幕规格各跑一次，退出码 0 | 窗口恒为 1280×800；1366×768 @1 实测超屏 36 px；HiDPI 受 offscreen 耦合限制，只能推算（baseline §6） |

## 最终验证（均在最终提交树上实跑，探针已移除）

| 项 | 命令 | 结果 |
|---|---|---|
| 构建 | `cmake --build build --target paleo tst_shell_chrome tst_dockmanager tst_shortcuts_shell tst_ui_blocking tst_uxtheme tst_notifications tst_shortcutregistry tst_locator tst_shortcuthelp tst_ui -j4` | 退出码 0 |
| 相关测试×2 | `ctest -R '^(tst_shell_chrome\|tst_dockmanager\|tst_shortcuts_shell\|tst_ui_blocking\|tst_uxtheme\|tst_notifications\|tst_shortcutregistry\|tst_locator\|tst_shortcuthelp)$' -j1` | 两轮均 9/9 通过，退出码 0 |
| tst_ui | 41 个用例去掉既有挂死用例 `testSimultaneousWellsAndSurveyAreaVisibility` | 36 通过 / 0 失败 / 8 跳过（可选截图开关），退出码 0 |
| 门禁 | `ctest -R 'layering\|i18n\|l10n\|ui_tokens\|ui_invariants\|layoutdesigner'` | 14/14 通过 |
| | `python3 tools/check_layering.py --strict` | 退出码 0 |
| | `git diff --check` | 退出码 0 |
| 围栏 | `fence_check.py` + 与 `gh pr view 437 --json files` 求交 | #437 现 84 个文件，与本分支 11 个文件交集 0；唯一提示 `paleodockmanager.cpp` ↔ Issue #411 为文件级重叠，#411 的缺陷在 `addDock` 的 `DockScrollArea` 包装（`:79-83`），本分支未改 |

未做性能优化，故无性能 before/after；不声称加速。

## 独立 review（结果）

实现与评审分离：生产改动由另一执行者实现，我（lead）逐行读完全部生产 diff 与新增测试；`tst_shell_chrome`
由我本人编写，所以该文件未由第三方审阅，已由它在 pristine master 上的红→绿证明有效性。本轮没有另起独立
reviewer 子代理，PR 正文如实写明。

审阅结论（无 blocker）：
1. 启动期：`restoreWindowState`（`paleomainwindow.cpp:184`）在 `buildShell`（`:126`）和 `showStartup()` 之后调用，此时 `m_currentPage` 已是 `startup`（`:410`），不在 `kPageIds`，钩子只还原意愿、不重落页策略，也不会触碰未建好的 dock；页策略仍由随后的 `showPage` 落下。
2. 生命周期：钩子 lambda 捕获主窗 `this`，`PaleoDockManager` 以主窗为 parent，随主窗销毁，无悬挂。
3. 持久化：只新增 `windowDockIntent` 与 `<布局键>Intent` 两个 QSettings 键；旧设置缺键走「恢复前快照」兜底（`legacySettingsWithoutIntentStillRestore`、`intentHooksLegacyStateOnly…` 覆盖）；旧版本读新设置时忽略未知键，窗口几何/`lastPage`/`windowState` 口径不变。
4. `restoreDefaultLayout` 先 `hide()` 全部已登记 dock（沿袭原行为），其后以 `captureDefaultLayout` 时的意愿快照还原；晚建面板不在快照内，保持隐藏即默认，行为与修复前一致。
5. 失败路径：`restoreState` 返回 false 时不调用 apply、不改意愿，状态栏提示；有测试（`intentHooksRestoreFailureReportsAndSkipsApply`）。
6. 层契约：未新增 include 越界，`check_layering.py --strict` 通过；未改受保护的 `paleomainwindow.h`。
7. 轻微（不阻断）：布局命令的反馈用状态栏 4 s 提示，没有走通知卡；`m_defaultState` 为空时 `restoreDefaultLayout` 仍静默返回（仅设计器壳/预览页可能出现）。已列入风险说明。
8. 复核中发现并修复 1 处措辞：`statusCrs` 帮助只写「地图坐标系代号」，代码在无 authid 时显示坐标系名称，已补充并复测。

## 排除与留给其他 owner

| 项 | 归属 | 本分支动作 |
|---|---|---|
| `tst_ui` `testSimultaneousWellsAndSurveyAreaVisibility` 在无头环境因模态 `QDialog::exec()` 永久阻塞 | 非本方向；pristine master 即复现 | 不修，仅在风险说明里记录 |
| 约束页 `constraintDeleteButton` 禁用却无原因提示（73 个禁用按钮中唯一一个） | `src/ui/pages/**`，其他方向，无 Issue 认领 | 只记录 |
| `ToolAvailabilityService` 生产代码零调用（`m_tools` 只存不用） | 业务 owner 决定绑到哪些 QAction | 只记录 |
| `DESIGN.md` 通知卡 340 vs 360 px、并发 4 vs 5 张自相矛盾 | 需用户批准 | 只记录 |
| #408 `tst_notifications` 的 `\|\| true`、#411 各项、#373/#374/#375、#438–#444（#437 review 衍生） | 已有 Issue | 未触碰 |

## 阶段 2 续跑清单（本轮未做，不声称已完成）

- A：除既有 token/不变量门禁外，尚无「六页 ribbon/dock/chips/dialog 视觉比较」的逐页证据。
- C：只有观测（禁用原因覆盖 72/73、ToolAvailability 未接线）；通用 QAction 绑定件未实现。
- E：中英文术语一致性未审计。
- F：键盘全路径、tab order、重复弹窗未审计（焦点链只在探针里计数，没有断言）。
- G：未采集截图/黄金比较。
- B 的 HiDPI：需用真实显示服务器或能同时给出逻辑几何与 dpr 的方案复测；双屏、拖拽/浮动未测。
- 恢复方式：读本 ledger 与 `06-baseline.md`，从 `origin/master` 重新取围栏后，按上表未做项各开一轮。
