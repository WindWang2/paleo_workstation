## 摘要

方向 06（六页主界面、Dock/Ribbon、交互可用性、帮助体系）。只改**已有**功能与自有 UI 基础设施，不开新业务域、不动数据契约。
本 PR 修复 5 类经红测证实的缺陷，其余观测与未做项如实记入 ledger。**A–G 并非都有生产改动**：A、E 审计后没有发现本方向拥有的缺陷，
C、G 以记录和证据为主，仍有未做项（见文末），不是「全部完成」。

### 修了什么

1. **布局命令破坏页 dock 策略**（`fd3f7273`）。`QMainWindow::restoreState` 经 `setVisible` 把「页策略隐藏」记成用户意愿：数据页退出后图层树在编图页永不出现；数据页点「恢复默认布局」图层树重现、破坏三栏；数据页保存的布局到编图页恢复会把图层树钉死。`PaleoDockManager` 增加可选意图钩子，恢复后还原意愿并按当前页重落策略；没装钩子的壳行为不变。三个布局命令补状态栏反馈，`restoreState` 失败不再静默。前后对比图在 `docs/progress/parallel-7/assets/dock-*.png`。
2. **帮助条目与现状不符**（`7ce9cf8c`）：`statusCoords` / `statusCrs`（已配准工程显示经纬度与坐标系代号）、`publishButton`（还要求每口井有验证残差或原因）、`correlationWebAddress`（作用域写错，帮助从未回填）。
3. **ribbon 命令钮键盘不可达**（`dd36bac9`）：SARibbon 把全部命令钮设成 `NoFocus`，54 个启用的命令钮 Tab 到不了。补 `TabFocus`，聚焦被裁掉的钮时把类别直接滚进可视区（无动画）。
4. **ribbon 帮助缺失**（`dd36bac9`）：ribbon 按钮是页内按钮的镜像，`whatsThis` 不镜像，101 个命令钮 Shift+F1 什么也不显示。镜像现在跟随页内按钮的帮助（13 个，含撤销/重做，后者直挂 QAction，另写图层编辑语义的帮助）。溢出出现的 12 个左右滚动钮补无障碍名称与提示。
5. **定位器吞 Tab，键盘出不来**（`dd36bac9`）：QGIS 定位器用 Tab 做补全，Esc 只收列表。空闲（无文本、无真实结果行）时 Tab/Shift+Tab 放行，Esc 把焦点还给进入前的控件；有文本或真实结果行时仍归 QGIS。搜索框补无障碍名。

## 证据（均为实际执行）
- base：`b0520429c21f858a1c053b876f511680088564a9`；代码提交 `fd3f7273`、`7ce9cf8c`、`dd36bac9`。
- 红→绿（每个缺陷用例都记录了修复前的失败信息，见 ledger）：`tst_shell_chrome` 的 R-1 / R-2 / R-4 在 pristine master 两轮稳定为红；帮助 2 个用例、滚动钮、镜像帮助、命令钮 Tab 可达、聚焦滚入、定位器各一个用例，修复前为红。
- `cmake --build build --target paleo tst_shell_chrome tst_dockmanager tst_shortcuts_shell tst_ui_blocking tst_uxtheme tst_notifications tst_shortcutregistry tst_locator tst_shortcuthelp tst_ui -j4`：退出码 0。
- `ctest -R '^(tst_shell_chrome|tst_dockmanager|tst_shortcuts_shell|tst_ui_blocking|tst_uxtheme|tst_notifications|tst_shortcutregistry|tst_locator|tst_shortcuthelp)$' -j1` 连跑两遍：均 9/9 通过；`tst_shell_chrome` 14 个用例、`tst_dockmanager` 15 项、`tst_shortcuts_shell` 15 项全过。定位器用例单槽连跑 5 次 5/5，不是时序偶然。
- `tst_ui` 去掉既有挂死用例后：36 通过 / 0 失败 / 8 跳过（可选截图开关）。
- `ctest -R 'layering|i18n|l10n|ui_tokens|ui_invariants|layoutdesigner'`：14/14 通过；`python3 tools/check_layering.py --strict` 与 `git diff --check`：退出码 0。
- 全程 `flock -x /tmp/paleo_workstation_global_build_j4.lock`，`-j4`，offscreen。未做性能优化，无性能数字。
- 基线、屏幕/缩放矩阵与第 2 阶段审计：`docs/progress/parallel-7/06-baseline.md`；逐轮记录与 review：`06-ui-shell-help.md`。

## 所有权与冲突排除
- 触及的生产/测试文件：`src/ui/paleodockmanager.{h,cpp}`、`src/ui/paleoribbon.{h,cpp}`、`src/ui/paleomainwindow{.cpp,_internal.h,_pages.cpp,_ribbon.cpp,_attach.cpp,_attach_shell.cpp}`、`src/ui/locator/paleolocatorfilters.{h,cpp}`、`src/ui/help/whatsthiscatalog.cpp`、`translations/paleo_zh_CN.ts`、`cmake/extra-ux.cmake`（只加 1 行测试登记）、`tests/tst_shell_chrome.cpp`（新）、`tests/tst_dockmanager.cpp`、`tests/tst_shortcuts_shell.cpp`；另有 `docs/progress/parallel-7/`。
- **未改** `src/ui/paleomainwindow.h`、`paleomainwindow_attach_mapping.cpp`、`paleomainwindow_workbench.cpp`，也未改 `src/ui/pages/**`、`vendor/**`、`CMakeLists.txt`、`DESIGN.md`。
- 开放 PR 仅 #437：与本分支改动文件交集为 0（`gh pr view 437 --json files` 实时比对，push 前再查一次）。
- 文件级重叠提示：`paleodockmanager.cpp` ↔ Issue #411。#411 的缺陷是 `addDock` 里 `DockScrollArea` 包装使 `m_seismic3dDock->widget()` 判空永假（`:79-83`），本 PR 未改那一段。
- 未触碰 Issue #358/#362/#364/#373/#374/#375/#401/#411–#413；也未重复 #426/#434 的交互修复。新出现的 #438–#444 都是 #437 的 review 衍生，与本方向无交集。
- 数据契约/Schema/图件版本/工具菜单保留；持久化只**新增**两个 QSettings 键（`windowDockIntent`、`<布局键>Intent`），旧设置缺键走「恢复前快照」兜底，旧版本读新设置会忽略未知键。

## Review
实现与评审分离：生产改动由另一执行者实现，lead 逐行审读全部生产 diff，无 blocker；审阅中发现并修复 2 处（`statusCrs` 帮助措辞；`lupdate` 读不了三目里的字面量）。测试迭代中暴露并修了 2 处设计问题：镜像帮助测试按文案配对会把「撤销」配到数据列表撤销上；定位器首版用「浮窗隐藏」判空闲，QGIS 聚焦后弹出空浮窗，测试因只等 30 ms 偶然通过，改成等 300 ms 复现后换成「真实结果行数」判据。
**限制**：没有另起独立 reviewer 子代理；全部新增测试由 lead 编写，有效性靠修复前的红→绿证明。`LocatorKeyboardExit` 依赖 QGIS 内部细节（结果视图类名、伪结果过滤器名 `filters`、`ResultData` 角色），降级方向是安全的，QGIS 升级后需复查。详见 ledger「独立 review」。

## 已知风险与限制
- `tst_ui` 在 pristine master 即会挂死：`testSimultaneousWellsAndSurveyAreaVisibility` 读本机 fixture，gpkg 打开失败后经 `PaleoNotify::warning` 落到模态 `QDialog::exec()`，无头环境阻塞到看门狗终止。与本 PR 无关，未修，也没有跳过或掩盖；上面的数字是**去掉该用例**后跑的。
- 最小窗口 1280×800（PLAN §42.11）与小逻辑屏冲突，**测得**三处越界：1366×768 @100% 窗口比屏幕高 36 px；1920×1080 @150%（逻辑 1280×720）高 82 px，状态栏在屏幕外；@200% 宽高都放不下（`QT_SCALE_FACTOR` 矩阵，见 baseline §6）。属设计决策，本 PR 不改下限。
- 布局命令的反馈是状态栏 4 s 提示，不是通知卡；`m_defaultState` 为空时 `restoreDefaultLayout` 仍静默返回（只有设计器壳/预览页可能出现）。
- 撤销/重做的帮助文案在 `attach_shell.cpp`，不在帮助目录（这两个动作不是带 `objectName` 的控件，目录回填不到）。
- 屏幕矩阵和截图来自 offscreen 一次性探针（未提交），不能当手工视觉验收；真实双屏、dock 拖拽未测。
- 记录但未处理（不属本方向）：约束页 `constraintDeleteButton` 灰着无原因提示；页面控件高度 28/30 px 的 2 px 差异；`ToolAvailabilityService` 生产代码零调用；`DESIGN.md` 通知卡 340 vs 360 px、并发 4 vs 5 张自相矛盾（需用户批准）；`layervocabulary.h` 里「编图页」同词异指；映射工作流里两处「项目」应为「工程」。

## 本 PR 没做的（续跑清单）
- D：约 88 个 ribbon 命令钮仍没有帮助条目，需逐个对照 handler 写出真实用途，不为凑数硬写。
- C：`ToolAvailabilityService` 绑定到既有 QAction（业务 owner 决策）。
- F：dock 内 `dockScrollHost` / `scrollableToolRow` 是无名称的 Tab 停靠点；dialog 重复弹窗、动态主题切换下焦点环、tab order 整体顺序未审计。
- G：没有手工视觉验收。

## 下一阶段建议
1. 请用户决定是否放宽最小窗口（或在小逻辑屏上收紧下限）；用真实显示器复核 150% 缩放的 1080p 笔记本。
2. 由业务 owner 决定 `ToolAvailabilityService` 绑到哪些既有 QAction；本方向只提供通用绑定件。
3. 逐个补 ribbon 命令钮的帮助条目（先做高频命令），每条对照 handler。
4. 对约束页删除钮的禁用原因提示，转给 `src/ui/pages/**` 的 owner。

不合并。
