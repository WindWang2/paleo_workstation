## 摘要

方向 06（六页主界面、Dock/Ribbon、交互可用性、帮助体系）第 1 阶段。只改**已有**功能与自有 UI 基础设施，
不开新业务域、不动数据契约。本 PR 修复 1 个经红测证实的真实缺陷、对齐 4 条与现状不符的帮助条目，
并把其余观测与未做项如实记入 ledger。**它不是 A–G 全部完成的 PR**，未做项见文末。

### 1. 布局命令破坏页 dock 策略（缺陷，红→绿）
`QMainWindow::restoreState` 会经 `dock->setVisible` 触发 `PaleoDockWidget::setVisible`，后者把「页策略隐藏」记成用户意愿，结果：
- 在数据页退出 → 下次启动，图层树在编图页永不出现；
- 在数据页点「恢复默认布局」→ 图层树重现，破坏数据页三栏；
- 在数据页保存的布局，到编图页恢复 → 图层树被钉死。

修复：`PaleoDockManager` 增加可选意图钩子（capture / apply）。恢复后由宿主还原意愿并按当前页重落策略；
没装钩子的壳（设计器、数据预览页）行为不变。三个布局命令补状态栏反馈，`restoreState` 失败不再静默。
`showPage` 里的页策略抽成 `applyPageDockPolicy` / `hideAllDocksForCorrelation`，`showPage` 与恢复后重落策略共用，行为不变。

### 2. 帮助条目对齐现有行为
`statusCoords` / `statusCrs`（已配准工程显示经纬度与坐标系代号）、`publishButton`（还要求每口井有验证残差或原因）、
`correlationWebAddress`（作用域写错，`whatsThis` 从未回填）。新增 2 个用例：壳域目录条目必须落到同名控件；状态栏与发布钮帮助随双态。

## 证据（均为实际执行）
- base：`b0520429c21f858a1c053b876f511680088564a9`；代码提交 `fd3f7273`、`7ce9cf8c`。
- 红→绿：`tst_shell_chrome` 的 R-1 / R-2 / R-4 在 pristine master 两轮稳定为红（`layerTreeDock` 可见性违反页策略）；帮助 2 个新用例修复前为红。
- `cmake --build build --target paleo tst_shell_chrome tst_dockmanager tst_shortcuts_shell tst_ui_blocking tst_uxtheme tst_notifications tst_shortcutregistry tst_locator tst_shortcuthelp tst_ui -j4`：退出码 0。
- `ctest -R '^(tst_shell_chrome|tst_dockmanager|tst_shortcuts_shell|tst_ui_blocking|tst_uxtheme|tst_notifications|tst_shortcutregistry|tst_locator|tst_shortcuthelp)$' -j1` 连跑两遍：均 9/9 通过。
- `tst_ui` 去掉既有挂死用例后：36 通过 / 0 失败 / 8 跳过（可选截图开关）。
- `ctest -R 'layering|i18n|l10n|ui_tokens|ui_invariants|layoutdesigner'`：14/14 通过；`python3 tools/check_layering.py --strict` 与 `git diff --check`：退出码 0。
- 全程 `flock -x /tmp/paleo_workstation_global_build_j4.lock`，`-j4`，offscreen。未做性能优化，无性能数字。
- 基线与屏幕矩阵：`docs/progress/parallel-7/06-baseline.md`；逐轮记录与 review：`06-ui-shell-help.md`。

## 所有权与冲突排除
- 触及 11 个文件：`src/ui/paleodockmanager.{h,cpp}`、`src/ui/paleomainwindow{.cpp,_internal.h,_pages.cpp}`、`src/ui/help/whatsthiscatalog.cpp`、`translations/paleo_zh_CN.ts`、`cmake/extra-ux.cmake`（只加 1 行测试登记）、`tests/tst_shell_chrome.cpp`（新）、`tests/tst_dockmanager.cpp`、`tests/tst_shortcuts_shell.cpp`。
- **未改** `src/ui/paleomainwindow.h`、`paleomainwindow_attach_mapping.cpp`、`paleomainwindow_workbench.cpp`，也未改 `src/ui/pages/**`、`CMakeLists.txt`、`DESIGN.md`。
- 开放 PR 仅 #437（现 84 个文件）：与本分支 11 个文件交集为 0（`gh pr view 437 --json files` 实时比对）。
- 文件级重叠提示：`paleodockmanager.cpp` ↔ Issue #411。#411 的缺陷是 `addDock` 里 `DockScrollArea` 包装使 `m_seismic3dDock->widget()` 判空永假（`:79-83`），本 PR 未改那一段。
- 未触碰 Issue #358/#362/#364/#373/#374/#375/#401/#411–#413；也未重复 #426/#434 的交互修复。新出现的 #438–#444 都是 #437 的 review 衍生，与本方向无交集。
- 数据契约/Schema/图件版本/工具菜单保留；持久化只**新增**两个 QSettings 键（`windowDockIntent`、`<布局键>Intent`），旧设置缺键走「恢复前快照」兜底，旧版本读新设置会忽略未知键。

## Review
实现与评审分离：生产改动由另一执行者实现，lead 逐行审读全部生产 diff 与新增测试，无 blocker；审阅中发现并修复 1 处帮助措辞（`statusCrs` 在无 authid 时显示坐标系名称）。**限制**：本轮没有另起独立 reviewer 子代理；`tst_shell_chrome` 由 lead 自写，其有效性靠它在 pristine master 上的红→绿证明。详见 ledger「独立 review」。

## 已知风险与限制
- `tst_ui` 在 pristine master 即会挂死：`testSimultaneousWellsAndSurveyAreaVisibility` 读本机 fixture，gpkg 打开失败后经 `PaleoNotify::warning` 落到模态 `QDialog::exec()`，无头环境阻塞到看门狗终止。与本 PR 无关，未修，也没有跳过或掩盖；上面的数字是**去掉该用例**后跑的。
- 布局命令的反馈是状态栏 4 s 提示，不是通知卡；`m_defaultState` 为空时 `restoreDefaultLayout` 仍静默返回（只有设计器壳/预览页可能出现）。
- 最小窗口 1280×800（PLAN §42.11）与小逻辑高度屏冲突：1366×768 @1 窗口比屏幕高 36 px（实测）；1920×1080 @150% 的逻辑桌面 1280×720，窗口高 800（推算，超出约 80 px）。属设计决策，本 PR 不改下限。
- 屏幕矩阵来自 offscreen 一次性探针（未提交）。offscreen 插件的逻辑几何与 dpr 耦合，无法同时给出两者，HiDPI 行只是推算；不能当手工视觉验收。
- 记录但未处理（不属本方向）：约束页 `constraintDeleteButton` 灰着无原因提示（73 个禁用按钮中唯一一个）；`ToolAvailabilityService` 生产代码零调用；`DESIGN.md` 通知卡 340 vs 360 px、并发 4 vs 5 张自相矛盾（需用户批准）。

## 本 PR 没做的（续跑清单）
A 逐页视觉比较、C 通用 QAction 绑定件、E 术语一致性、F 键盘全路径/tab order/重复弹窗、G 截图与黄金比较、B 的真实 HiDPI/双屏/拖拽浮动，均未展开。计划与恢复方式见 `06-ui-shell-help.md` 末节。

## 下一阶段建议
1. 用真实显示服务器或能分别给出逻辑几何与 dpr 的方案复测 1366×768 / 150%，再请用户决定是否放宽最小窗口。
2. 由业务 owner 决定 `ToolAvailabilityService` 绑到哪些既有 QAction；本方向只提供通用绑定件。
3. 对约束页删除钮的禁用原因提示，转给 `src/ui/pages/**` 的 owner。

不合并。
