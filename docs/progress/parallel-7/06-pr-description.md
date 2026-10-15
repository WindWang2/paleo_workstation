## 摘要

方向 06（六页主界面、Dock/Ribbon、交互可用性、帮助体系）第 3 阶段增量。前置交付（dock 布局意图、
帮助条目修正、ribbon 键盘可达、镜像帮助、定位器键盘出口）已经由已合并的 #445 进入 master
（squash `a93f8b6d`）；本 PR 只含 3 个新提交，完成上一轮留在 ledger 里的两个决策项：

1. **`ToolAvailabilityService` 接线**（`39502a87`）：该服务此前生产代码零调用。逐条读 52 个
   ribbon 命令的 handler 后只绑五个数据导入口——无工程时导入必然失败（`DataImportService`
   「project dir is not set」），界面却先弹文件对话框、选完才报错。工程开合经
   `projectOpened`/`projectClosed` 驱动 `setGlobalGate`，ribbon 命令（新增
   `PaleoRibbon::applyGate`：`paleoGateReason` 动态属性 + 镜像 resync，workbench 重建镜像
   不失效）与数据页按钮一起置灰，tooltip 写明「先新建或打开工程」。**不动**预测/编图/验证的
   门控（页 owner 的理由更具体）与编辑动作（走 `QgisEditingService` 自己的闸）；逐层
   `layerBusy` 是否显示仍归业务 owner。
2. **ribbon 帮助全覆盖**（`b0f24912`）：98 个可见 ribbon 命令钮原先只有 14 个带 Shift+F1
   帮助。帮助文本全部复用作者已写的用途串——动作 tip、编辑条 `hint` 属性、页按钮
   `accessibleDescription`/目录条目——只有方向线/间隔线两条是新写的，语义取自
   `paleoalgorithms.h:32-37`（break_line 硬屏障绕行、direction_line 各向异性轴）。新增
   `everyVisibleRibbonCommandHasHelp` 回归用例钉住 100% 覆盖。

### 行为变化（有意）

无工程时五个导入入口整体置灰并写明原因——此前是点了、选完文件、才在 `DataImportService` 失败。

## 证据（均为实际执行）

- base：`origin/master` = `447a9d04d59d75183b1b85f8ff254d30c2df2f93`（#445 已并入后 rebase，
  `git rebase --onto origin/master 0208f48e` 无冲突）。
- 红→绿：`importCommandsAreGatedUntilProjectOpens` 修复前在「无工程」与「工程关闭后」两点为红
  （verbatim：`no project: ribbonImportFolder must be disabled`）；`everyVisibleRibbonCommandHasHelp`
  修复前报 17 个钮（11 个唯一命令）无帮助，消息里逐条列出。修复后全绿。
- `cmake --build build --target paleo` + 相关测试目标 `-j4`：退出码 0（rebase 后整树重编 273 步）。
- `ctest -R '^(tst_shell_chrome|tst_dockmanager|tst_shortcuts_shell|tst_ui_blocking|tst_uxtheme|tst_notifications|tst_shortcutregistry|tst_locator|tst_shortcuthelp|tst_canvas_tools)$' -j1`：10/10 通过；`tst_shell_chrome` 18 个用例全过。
- `ctest -R 'layering|i18n|l10n|ui_tokens|ui_invariants|layoutdesigner'`：14/14 通过；
  `python3 tools/check_layering.py --strict` 与 `git diff --check`：退出码 0。
- ribbon 帮助覆盖探针（未提交）：`withHelp` 14→81→98（修复三步实测），`uniqueWithoutHelp` 42→0。
- 全程 `flock -x /tmp/paleo_workstation_global_build_j4.lock`，`-j4`，offscreen。未做性能优化，无性能数字。
- 基线与逐轮记录：`docs/progress/parallel-7/06-baseline.md`、`06-ui-shell-help.md`。

## 所有权与冲突排除

- 本 PR 触及： `src/ui/help/whatsthiscatalog.cpp`、`src/ui/paleomainwindow{.cpp,_attach.cpp,_attach_shell.cpp,_sections.cpp}`、
  `src/ui/paleoribbon.{h,cpp}`、`src/ui/ribbonpanels.cpp`、`tests/tst_shell_chrome.cpp`、
  `translations/paleo_zh_CN.ts`（仅 ±3 条 message，不重排 `<location>`）、`docs/progress/parallel-7/`。
- **未改** `src/ui/paleomainwindow.h`、`paleomainwindow_attach_mapping.cpp`、`paleomainwindow_workbench.cpp`，
  也未改 `src/ui/pages/**`、`src/services/**`、`vendor/**`、`CMakeLists.txt`、`DESIGN.md`。
- 开放 PR 仅 #437（84 个文件，与本 PR 交集 0，`gh pr view 437 --json files` 实时比对）。方向
  01–05、07 的 PR（#446–#452）与 #447 主干修复均已并入 master，本分支已 rebase 到其上。
- 数据契约/Schema/图件版本/工具菜单保留；无新增持久化键。

## Review 记录与限制

- 实现与评审分离：生产改动由另一执行者实现，lead 逐行读完全部生产 diff。审阅发现并修复：
  页内导入按钮不随 ribbon 置灰（同一入口两份，已补置灰并加断言）；`.ts` 195 条 `<location>`
  机械重排（回退为仅语义改动）；拆分时误删一个 `<message>` 开标签（lrelease 报错后修复，
  `xml.dom.minidom` 验证）。
- 没有另起独立 reviewer 子代理；新增测试由 lead 编写，有效性靠修复前红→绿证明。
- 门控覆盖面有意收窄：只接全局工程门；逐层 `layerBusy` 不叠到 ribbon（页级理由更具体），
  `applyGate` 钩子可直接复用。
- `tst_ui` 的既有挂死用例 `testSimultaneousWellsAndSurveyAreaVisibility` 在 pristine master 即
  挂死（本机 fixture 弹模态框），与本 PR 无关，未修未跳过。
- 最小窗口 1280×800 与小逻辑屏冲突的三处越界（1366×768@1 高 36px、1920×1080@150% 高 82px、
  @200% 放不下）是设计决策，本 PR 不改下限。

## 本 PR 没做的

- F：dock 内 `dockScrollHost` / `scrollableToolRow` 是无名称的 Tab 停靠点；dialog 重复弹窗、
  动态主题切换下焦点环、tab order 整体顺序未审计。
- G：没有手工视觉验收；真实双屏、dock 拖拽未测。
- 约束页 `constraintDeleteButton` 灰着无原因提示 → 归 `src/ui/pages/**` owner。

不合并。

Generated with [Devin](https://devin.ai)
