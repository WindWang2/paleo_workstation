# 方向 06 · 分阶段 ledger：六页主界面 / Dock·Ribbon / 交互可用性 / 帮助体系

- 分支：`goal7/ui-shell-help-20261014`（worktree `../paleo-wt-goal7-ui-shell`）
- base：`origin/master` = `447a9d04d59d75183b1b85f8ff254d30c2df2f93`（#445 已于 2026-10-15 合并为 `a93f8b6d`，阶段 3 提交 rebase 到其上，`--onto` 无冲突）
- 代码提交：`fd3f7273`（dock 策略 + 布局反馈）、`7ce9cf8c`（帮助条目）、`dd36bac9`（ribbon 键盘可达 / 镜像帮助 / 定位器键盘出口）、阶段 3（rebase 后）：`39502a87`（导入命令接 `ToolAvailabilityService` 全局门）、`b0f24912`（ribbon 帮助全覆盖）
- 基线与运行期审计：`06-baseline.md`；PR 正文：`06-pr-description.md`；截图：`assets/`
- 构建：Debug / Ninja，全程 `flock -x /tmp/paleo_workstation_global_build_j4.lock`，`-j4`

## 阶段 1 微循环

| 轮 | 焦点 | 文件 / 函数 | Oracle | 结果 |
|---|---|---|---|---|
| L0 | R0 围栏与基线 | 全仓只读；`gh pr/issue`；`fence_check.py` | 开放 PR #437 文件集、Issue 文件级指纹、基线 build+test | 基线 `paleo` 构建 0；`tst_ui` 在 pristine master 即挂死（见 baseline §3），记为既有红，不修 |
| L1 | B 布局：写红测 | `tests/tst_shell_chrome.cpp`（新） | 在 pristine master 上复现 | 当时的 5 个用例中 R-1 / R-2 / R-4 **两轮稳定为红**；R-3 与页策略基本用例为绿（非回归场景）。随后补了 2 个持久化用例（跨会话保持用户隐藏、旧设置缺键），这两个只在修复后树上跑过，没有在 pristine master 上测过红 |
| L2 | B 布局：修复 | `PaleoDockManager::restoreLayout` / `setIntentHooks` / `captureIntent`；`PaleoMainWindow::buildShell`（钩子）、`saveWindowState`、`restoreWindowState`；`applyPageDockPolicy`、`hideAllDocksForCorrelation`（自 `showPage` 抽出） | `tst_shell_chrome` 全绿；`tst_dockmanager` 新增 4 例；不改 `paleomainwindow.h` | 绿，两遍一致 |
| L3 | D 帮助 | `src/ui/help/whatsthiscatalog.cpp`（4 条）、`translations/paleo_zh_CN.ts` | `tst_shortcuts_shell` 新增 2 例，修复前红 → 修复后绿 | 绿；`statusCrs` 措辞经复核补「没有代号时显示坐标系名称」后复测通过 |
| L4 | B 屏幕矩阵 | 一次性探针（未提交） | 8 个屏幕规格各跑一次，退出码 0 | 窗口恒为 1280×800；因 offscreen 的 dpr 与逻辑 DPI 耦合，HiDPI 行当时只能推算，已在阶段 2 用 `QT_SCALE_FACTOR` 重测 |

## 阶段 2 微循环

| 轮 | 焦点 | 文件 / 函数 | Oracle | 结果 |
|---|---|---|---|---|
| L5 | B HiDPI 重测 | 探针改用 `QT_SCALE_FACTOR`（逻辑几何与 dpr 同时正确） | 10 次运行退出码 0，`#screen` 逻辑几何 = 物理 ÷ 缩放 | 测得 3 处越界：1366×768@1 超 36 px，1920×1080@150% 超 82 px，@200% 放不下（baseline §6） |
| L6 | F 无名滚动钮 | `PaleoRibbon::nameScrollButtons`；`buildRibbon` / `attachWorkflows` 调用 | `ribbonScrollButtonsHaveNamesAndTips`：修复前 12 个全空 → 绿 | 绿；`lupdate` 只认字面量，故两个字面量各自进 `translate()` |
| L7 | D ribbon 帮助镜像 | `ButtonMirror::sync` 同步 `whatsThis`；`PaleoRibbon::syncMirrors`（`applyWhatsThis` 后补同步）；撤销/重做动作另写编辑语义帮助（`attach_shell`） | `ribbonButtonsAdoptHelpOfTheirSourceButtons`（修复前 13/13 缺帮助）、`ribbonUndoRedoCarryEditingHelp` | 绿。首版测试按文案配对会把「撤销」配到数据列表撤销上（两件事），改为只配对帮助唯一的键，撤销/重做单测 |
| L8 | F ribbon 键盘可达 | `PaleoRibbon::makeKeyboardReachable`（补 `TabFocus` + 聚焦滚入）；`FocusScrollFilter`；`buildRibbon` / `attachWorkflows` / `showPage` 调用 | `ribbonCommandButtonsAreKeyboardReachable`（修复前 54/54 不可达）、`ribbonFocusScrollsClippedButtonIntoView`（修复前 3 页的「连井 / 时深对齐」按钮 x=1376..1728 超出 1276） | 绿。vendor 文档说正 px 右滚，实测与源码都是负 px 才露出右侧；聚焦瞬间的几何是旧值，改为推到下一轮事件循环再量，仍是一次直滚、无动画 |
| L9 | F 定位器键盘出口 | `installLocatorKeyboardExit` / `LocatorKeyboardExit`（`paleolocatorfilters.cpp`）；`attach_shell` 安装 + 无障碍名 | `locatorDoesNotTrapKeyboardFocus`：修复前空闲 Tab 不离开；修复后文本非空时 Tab 仍留在框内、空闲 Tab/Shift+Tab 放行、Esc 还焦点；单槽连跑 5 次 5/5 | 绿。两次修正：①首版以「浮窗隐藏」判空闲，QGIS 聚焦 ~110 ms 后会弹出空浮窗，测试等待 30 ms 才偶然过，改成等 300 ms 后红，再改判据；②改用「结果行数」时发现空查询的列表里是 QGIS「可用过滤器」伪结果行（过滤器名 `filters`），只数真实命中行 |
| L10 | A / C / E / G 审计 | 探针 + 静态扫描；截图入 `assets/` | 见 baseline §8 | A、E 没有生产改动；C 记录反例与未接线项；G 留前后对比图 |

## 阶段 3 微循环（`ToolAvailabilityService` 接线 + ribbon 帮助收口）

| 轮 | 焦点 | 文件 / 函数 | Oracle | 结果 |
|---|---|---|---|---|
| L11 | 决策：门绑到哪些命令 | 逐条读 `ribbonpanels.cpp` 52 个命令的 handler + `toolavailability.cpp` + `dataimport_document.cpp:424` | 门控必须有真实行为依据，不为一刀切置灰 | 结论：**只绑五个数据导入命令**——无工程时 `DataImportService` 必然报「project dir is not set」，界面上唯一成立的通用理由就是「先新建或打开工程」；预测/编图/验证命令已有页 owner 写的更具体的门控，覆盖它们只会替换成更差的理由；编辑动作走 `QgisEditingService` 自己的闸，不动 |
| L12 | 门控实现 | `PaleoRibbon::applyGate`（`paleoGateReason` 动态属性 + 镜像 resync，属性跟 action 走，workbench 重建镜像不失效）；`paleomainwindow.cpp` 工程开合驱动 `setGlobalGate`（不走 `showStartup`，它也是回首页导航）；`attachWorkflows` 把 `check()` 结果刷到五个 ribbon action + 五个页内按钮（页按钮构造后无人碰 enabled，置灰安全） | `importCommandsAreGatedUntilProjectOpens`：红（`no project: ribbonImportFolder must be disabled` / `project closed` 两处）→ 绿；ribbon 钮与页按钮同置灰、原因进 tooltip、开关工程往返、无关命令不受影响 | 绿 |
| L13 | ribbon 帮助收口 | 帮助全部取自作者已写的用途串：`newAction` 的 tip 同时写 `whatsThis`；镜像退到源按钮 `whatsThis`→`accessibleDescription`；编辑条动作的 `hint` 属性统一回填；对比服务五个命令（页自建、只读文件）在 attach 处把动作 tip 固化；方向线/间隔线加目录条目（语义取自 `paleoalgorithms.h:32-37` 注释：break_line=硬屏障绕行、direction_line=各向异性轴） | `everyVisibleRibbonCommandHasHelp`：红（98 个可见钮中 17 个无帮助，11 个唯一命令）→ 绿；探针 uniqueWithoutHelp 42→0 | 绿；`everyShellScopedCatalogEntryIsApplied` 仍绿（新目录条目落到真实控件） |

### 阶段 3 复核发现

- `.ts` 处理教训：执行者一度把 6 个文件的 195 条 `<location>` 行全部重编号（+398/−207 行噪音，扩大与其他分支的冲突面）——已回退为只加 3 条语义改动；我在拆分时又误删了一条 `<message>` 开标签（lrelease 报错才发现），已修复并用 `xml.dom.minidom` 验证。教训：`.ts` 只动手工改的位置，不做机械重排。
- 门的覆盖面：global gate 只表达「工程未开」。逐层 `layerBusy`/任务忙的细粒度理由仍留在各页现有门控里（它们理由更具体），本适配层不接管；将来 owner 若想显示「图层忙」，同一 `applyGate` 钩子可复用。
- 行为变化（有意）：无工程时导入入口整体置灰——此前是点了、选完文件、才在 `DataImportService` 里失败。

## 最终验证（在 rebase 后 `b0f24912` 的代码上实跑，探针已移除）

| 项 | 命令 | 结果 |
|---|---|---|
| 构建 | `cmake --build build --target paleo tst_shell_chrome tst_dockmanager tst_shortcuts_shell tst_ui_blocking tst_uxtheme tst_notifications tst_shortcutregistry tst_locator tst_shortcuthelp tst_ui -j4` | 退出码 0 |
| 相关测试 | `ctest -R '^(tst_shell_chrome\|tst_dockmanager\|tst_shortcuts_shell\|tst_ui_blocking\|tst_uxtheme\|tst_notifications\|tst_shortcutregistry\|tst_locator\|tst_shortcuthelp\|tst_canvas_tools)$' -j1` | 10/10 通过；`tst_shell_chrome` 18 个用例、`tst_dockmanager` 15 项、`tst_shortcuts_shell` 15 项全过（阶段 2 树上曾连跑两轮 9/9） |
| 定位器用例复跑 | `./tst_shell_chrome locatorDoesNotTrapKeyboardFocus` ×5 | 5/5 通过（不是时序偶然） |
| tst_ui | 41 个用例去掉既有挂死用例 `testSimultaneousWellsAndSurveyAreaVisibility` | 36 通过 / 0 失败 / 8 跳过（可选截图开关） |
| 门禁 | `ctest -R 'layering\|i18n\|l10n\|ui_tokens\|ui_invariants\|layoutdesigner'` | 14/14 通过 |
| | `python3 tools/check_layering.py --strict`、`git diff --check` | 退出码 0 |

未做性能优化，故无性能 before/after；不声称加速。`showPage` 每次切页会遍历一次 ribbon 命令钮（幂等），耗时没有测量。

## 独立 review（结果）

实现与评审分离：生产改动由另一执行者实现，我（lead）逐行读完全部生产 diff；全部新增测试由我编写，所以它们没有第三方审阅，
有效性靠修复前的红→绿证明（每个缺陷用例都记录了修复前的失败信息）。本轮没有另起独立 reviewer 子代理。

审阅结论（无 blocker）：
1. 启动期：`restoreWindowState`（`paleomainwindow.cpp:184`）在 `buildShell`（`:126`）和 `showStartup()` 之后调用，此时 `m_currentPage` 已是 `startup`（`:410`），不在 `kPageIds`，钩子只还原意愿、不重落页策略。
2. 生命周期：钩子 lambda 捕获主窗 `this`，`PaleoDockManager` 以主窗为 parent；`FocusScrollFilter`、`LocatorKeyboardExit` 都以所属窗口/定位器为 parent，延迟回调用 `QPointer` 守卫，`focusChanged` 连接带 context 对象。
3. 持久化：只新增 `windowDockIntent` 与 `<布局键>Intent` 两个 QSettings 键；旧设置缺键走「恢复前快照」兜底；旧版本读新设置忽略未知键。
4. 层契约：未新增越界 include，`check_layering.py --strict` 通过；未改受保护的 `paleomainwindow.h`。
5. `LocatorKeyboardExit` 依赖 QGIS 内部细节：结果视图类名 `QgsLocatorResultsView`、伪结果过滤器名 `filters`、`QgsLocatorModel::CustomRole::ResultData`。降级方向是安全的：找不到视图时从不接管；伪结果名变了则 `rows>0`，Tab 回到 QGIS 原行为。QGIS 升级后需复查。
6. 撤销/重做的帮助文案写在 `attach_shell.cpp`（`tr`），没有进 `whatsthiscatalog`：这两个动作不是带 `objectName` 的控件，目录按 `objectName` 回填不到；属于既有约定的例外，已在此记录。
7. 轻微（不阻断）：布局命令的反馈是状态栏 4 s 提示而非通知卡；`m_defaultState` 为空时 `restoreDefaultLayout` 仍静默返回（只有设计器壳/预览页可能出现）。
8. 复核中修了 2 处：`statusCrs` 帮助措辞；`nameScrollButtons` 里三目表达式不被 `lupdate` 识别，拆成两个字面量。
9. 环境异常：执行者报告 `paleoribbon.cpp` 在其工作期间被反复覆盖（内容在两个版本间来回，疑似 IDE 缓冲区），最终以磁盘内容为准重做了 FIX-4 的 hunk；我逐行读了磁盘上的最终 diff，并确认之前的 `translate()` 改动保留。

## 排除与留给其他 owner

| 项 | 归属 | 本分支动作 |
|---|---|---|
| `tst_ui` `testSimultaneousWellsAndSurveyAreaVisibility` 在无头环境因模态 `QDialog::exec()` 永久阻塞 | 非本方向；pristine master 即复现 | 不修，仅在风险说明里记录 |
| 约束页 `constraintDeleteButton` 禁用却无原因提示（73 个禁用按钮中唯一一个） | `src/ui/pages/**`，其他方向，无 Issue 认领 | 只记录 |
| 页面控件高度 28 / 30 px 的 2 px 差异（采样值） | `src/ui/pages/**` | 只记录 |
| ~~`ToolAvailabilityService` 生产代码零调用~~ → 已接入：全局工程门 → 五个导入入口（ribbon + 页内按钮），`9e30e17a` | 逐层 busy 态绑定仍归各页 owner（理由更具体） | 全局门已实现 |
| `DESIGN.md` 通知卡 340 vs 360 px、并发 4 vs 5 张自相矛盾 | 需用户批准 | 只记录 |
| `layervocabulary.h:125-129` 把四张地图页统称「编图页」，与页签「智能编图」同词异指 | `src/qgis`，不在本方向 | 只记录 |
| 「本项目纹理库」「未绑定项目数据」（应为工程） | `src/workflow/mappingworkbench*` / `mappingworkflow*`，#437 / 其他方向 | 只记录 |
| #408 `tst_notifications` 的 `\|\| true`、#411 各项、#373/#374/#375、#438–#444（#437 review 衍生） | 已有 Issue | 未触碰 |

## 仍未完成（不声称已完成）

- B：最小窗口 1280×800 与小逻辑屏的冲突（三处测得越界）等用户决定是否放宽；真实双屏、dock 拖拽/浮动未测；offscreen 不能代替手工视觉验收。
- C：`ToolAvailabilityService` 的逐层 `layerBusy`/任务忙理由仍不显示在 ribbon 上——现有页级门控的理由更具体，是否还要把 busy 态叠上去归业务 owner；`applyGate` 钩子留好了。
- F：dock 内 `dockScrollHost` / `scrollableToolRow` 的无名 Tab 停靠点；dialog 重复弹窗；动态主题切换下焦点环；tab order 整体顺序。
- G：没有手工视觉验收；24 张整窗截图只在 `/tmp/goal7/probe3/shots/`，未入库。
- 恢复方式：读本 ledger 与 `06-baseline.md`，从 `origin/master` 重新取围栏后，按上表未做项各开一轮。
