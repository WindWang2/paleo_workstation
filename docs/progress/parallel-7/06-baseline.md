# 方向 06 · R0 基线：六页主界面 / Dock·Ribbon / 交互可用性 / 帮助体系

分支 `goal7/ui-shell-help-20261014`，基线 `origin/master` = `b0520429c21f858a1c053b876f511680088564a9`
（2026-10-14 取，运行期间复查未变）。独立 worktree `../paleo-wt-goal7-ui-shell`，
Debug / Ninja / vendored QGIS prefix（symlink 自主 checkout，只读使用）。

本文是 R0 记录：读了什么、测了什么、哪些被排除、哪些进入开发清单。逐轮验证 ledger
（`06-ui-shell-help.md`）尚未建立，待修复经验证后补写，不预先声称。

## 1. 围栏（写前先查，写后复查）

| 围栏 | 内容 | 检查方式 |
|---|---|---|
| 开放 PR | 仅 #437（`codex/evidence-mapping-integration`，80 个文件，PR 明写「不合并」）。其 CMakeLists.txt、`paleomainwindow.h`、`paleomainwindow_attach_mapping.cpp`、`paleomainwindow_workbench.cpp`、`pages/composepage|datagraphpage|mappingworkbenchpage.*` 等一律不写 | `gh pr view 437 --json files` 实时取；`/tmp/goal7/fence_check.py` 逐文件比对 `git diff --name-only origin/master...HEAD` |
| 开放 Issue | 执行中 70 → 77 条（新增 #438–#444 全是 #437 的 review 项，与本方向无交集）。不认领、不修、不改任何 Issue | 抽取 Issue 正文里出现的文件路径作文件级指纹，与 diff 比对 |
| Issue 文件级重叠（须绕开具体缺陷行） | `paleodockmanager.cpp`（#411 第 2 项：`DockScrollArea` 包装使 `m_seismic3dDock->widget()` 回退取面板永假——缺陷在 `attach_models.cpp`，本方向不动 `addDock` 的包装逻辑）；`decorations/paleodecorations.cpp`（#411 第 4 项）；`paleomainwindow_attach_models.cpp` / `attach_validate.cpp`（#411）；`taskpanel.cpp`（#374 引用）；`tests/tst_notifications.cpp`（#408 的 `\|\| true`） | 这些文件不写；`paleodockmanager.cpp` 只改布局命令路径，不碰 `addDock` |
| 不可新增 `cmake/extra-*.cmake` | 新文件须在 `CMakeLists.txt` 的 `_paleo_extra_manifest` 登记，而 `CMakeLists.txt` 被 #437 占用。新测试只能挂进已登记的 `cmake/extra-ux.cmake` / `extra-shortcuts-help.cmake` | — |
| `DESIGN.md` | 契约文件，改动需用户批准。本轮不改；内部矛盾记入 §5 | — |

主写范围（任务书白名单）实际触达：`src/ui/paleodockmanager.*`、`src/ui/paleomainwindow.cpp` /
`paleomainwindow_pages.cpp` / `paleomainwindow_internal.h`（主窗薄壳，非 #437 保护文件）、
`src/ui/help/`、`translations/paleo_zh_CN.ts`（帮助/布局文案）、对应测试。

## 2. 真实读到的东西（调用图 / 状态机）

- **窗口壳**：`PaleoMainWindow::buildShell` → `buildCenterArea` / `buildLayerTreeDock` /
  `buildPageDocks` / `buildRibbon` / `buildStatusBar`；`setMinimumSize(1280, 800)`（PLAN §42.11
  用户选择 A，`tst_ui::windowMeetsMinSize` 钉住 ≥1280×800）。
- **Dock 管理**：`PaleoDockManager`（`QMainWindow` 原生停靠 + 自绘拖拽目标提示 + 「布局与面板」
  菜单：自适应 / 保存 / 恢复保存 / 恢复默认 / 显隐 / 移动）；设计器壳、数据预览页各一个实例。
  `PaleoDockWidget`（定义在受保护的 `paleomainwindow.h`）用 `m_userWantsVisible` +
  `setProgrammaticVisible` 区分「用户意愿」和「页策略显隐」。
- **页 dock 策略**（`paleomainwindow_pages.cpp::showPage`）：数据页藏图层树 / 底栏 / 连井剖面、显数据列表；
  其余页按 `userWantsVisible` 恢复；地层对比页藏全部 dock。
- **Ribbon**：SARibbon 六个 category = `kPageIds`；页 ribbon 按钮经 `PaleoRibbon::mirror`
  （`ButtonMirror`）镜像页内按钮的 enabled / tooltip / checked；右侧按钮组 = 定位器 / 处理算法 /
  布局 / 帮助 / Web 服务。
- **帮助**：`help/whatsthiscatalog.cpp` 共 69 条 `(scopeClass, objectName, text[, shortcutId])`，
  `applyWhatsThis` 按 objectName + 作用域类回填；`shortcuts/shortcutcatalog.cpp` 为键位唯一真源
  （源码闸 `noScatteredBindingsInSource`）。
- **通知**：`PaleoNotify` → `ErrorHub` → `NotificationCenter`（≤4 张、卡宽 360、同键 60 s 去重、
  severe 模态）；确认类走 `PaleoNotify::ask*`。
- **门禁**（均为 ctest 项，已在基线实跑）：`layering(_strict/_selftest)`、`ui_invariants(…)`、
  `ui_tokens(…)`（我方范围例外仅 4 条）、`i18n(_selftest)`、`l10n(_selftest)`。

## 3. 基线实测（pristine master + 我方 2 个新测试文件，本机）

构建：configure 6 s（等锁 1916 s，兄弟 worktree 全量构建占锁），`paleo` 852 s，11 个测试目标 64 s（均 `-j4`、全程 flock）。

| 测试 | 结果 |
|---|---|
| tst_uxtheme / tst_notifications / tst_shortcutregistry / tst_locator / tst_shortcuthelp / tst_dockmanager / tst_shortcuts_shell / tst_ui_blocking | 两轮全绿 |
| `tst_ui` | **基线红（pristine master，两轮复现）**：`testSimultaneousWellsAndSurveyAreaVisibility`（读本机 `/home/kevin/projects/paleo_data` fixture）在 gpkg 打开失败后经 `PaleoNotify::warning` 落到模态 `QDialog::exec()`，无头永久阻塞，QtTest 看门狗 300 s 后 SIGABRT。非本方向缺陷、不修；回归时跳过该 slot |
| 门禁 14 项（layering×3、i18n×2、l10n×2、ui_tokens×3、ui_invariants×3、tst_layoutdesigner_full） | 全绿；`check_layering.py --strict`、`git diff --check` 干净 |
| `tst_shell_chrome`（本方向新增） | 5 个 slot：基线 + R-3 绿，**R-1 / R-2 / R-4 红且两轮稳定复现** → 见 §4 F1 |

## 4. 发现分组

### 4.1 经核实、未被占用 → 进入开发清单

| # | 方向 | 发现 | 证据 |
|---|---|---|---|
| F1 | B 布局 | **布局命令破坏页 dock 策略**：`QMainWindow::restoreState` / `restoreDefaultLayout` 经虚函数 `setVisible` 把「页策略隐藏」误当作用户意愿。①在数据页退出 → 下次启动图层树在编图页永不出现；②数据页点「恢复默认布局」→ 图层树重现、破坏三栏；③数据页保存布局、编图页恢复 → 图层树被钉死 | `tst_shell_chrome` R-1/R-2/R-4 红（`'!dock(win,"layerTreeDock")->isHidden()' returned FALSE`）；根因 `PaleoDockWidget::setVisible`（`paleomainwindow.h:71-76`，非程序化 `setVisible` 即改写 `m_userWantsVisible`）+ origin/master 的 `paleodockmanager.cpp:117`（恢复默认）、`:149`（恢复保存）直接 `restoreState` |
| F2 | B/D 反馈 | 「保存当前布局 / 恢复保存的布局 / 恢复默认布局」三个命令无任何成功/失败反馈；`restoreState` 返回 false（保存的状态与当前面板不兼容）被静默吞掉 | origin/master 的 `paleodockmanager.cpp:143-150`，`restoreState` 返回值未读，三个命令均无状态栏/通知反馈 |
| F3 | D 帮助 | 帮助目录 4 条与现状不符（静态审计 69 条全量，逐条追到创建点与 handler）：`statusCoords`/`statusCrs` 只讲「工程坐标·米」，实际已配准工程显示经纬度与坐标系代号（`paleomainwindow_status.cpp:92-121`，DESIGN 2026-10-07 决策）；`publishButton` 只讲「需先导出 PDF」，实际还要求每口井有验证残差/原因（`composepage.cpp:465-482`）；`correlationWebAddress` 作用域写成 `StratigraphicWebPage`，控件实际在主窗 ribbon，`whatsThis` 从未回填（orphan） | `/tmp/goal7/help_audit.md` 全表；快捷键目录 50 余条 0 orphan、0 偏差 |
| F4 | C 可用性 | `ToolAvailabilityService`（DESIGN「禁用工具的 reason 提示」契约，PLAN §35/§42.13）在生产代码里零调用：`PaleoMainWindow::m_tools` 只存不用，`setGlobalGate` / `noteTaskOnLayer` / `check` 在 `src/` 除自身外无调用点 | `grep` 全仓；仅 `tst_canvas_tools` 覆盖服务本体。是否接线属业务 owner 决策，见 §5 |

### 4.2 已有 Issue / PR 占用（排除，不处理）

| 项 | 归属 |
|---|---|
| `tst_notifications.cpp` fixture 构造里 `\|\| true` | Issue #408 |
| `DockScrollArea` 包装使 `m_seismic3dDock->widget()` 判空永假 | Issue #411 第 2 项 |
| `PaleoDecorationManager` setter 无 null 守卫 | Issue #411 第 4 项 |
| `taskpanel`/`batchjobpanel` 行回收、`sequenceframework` 双发 | Issue #373/#374/#375 |
| #437 的主窗证据联动、domain 页面 | PR #437（只读） |
| #438–#444（证据/版本/图谱） | PR #437 review 衍生，非本方向 |

### 4.3 低价值 / 不做

- 通知卡「340 vs 360 px」「并发 4 vs 5 张」：`DESIGN.md` 内部三处口径不一（见 §5），代码取 360 / 4 且与「错误呈现」节一致，无需改码。
- 视觉 token：我方范围 `ui-token-exceptions.json` 仅 4 条（均在 `paleomainwindow*.cpp` 的数据符号例外），门禁已绿，无可收缩项。
- `tst_ui` 基线悬挂：非本方向，仅在 PR 风险说明里记录。

## 5. 留给 Issue Agent / 用户裁决的记录（本分支不动）

1. **`DESIGN.md` 自相矛盾**：「错误呈现 (Error Presentation)」表写卡宽 340 px，同文件「错误呈现」节与代码均为 360 px；Decisions Log 2026-10-06 写并发上限 5 张，节文与代码为 4 张。改 DESIGN 需用户批准，本轮仅记录。
2. **最小窗口 1280×800（PLAN §42.11，用户选择 A）与 1366×768 / 150% 缩放笔记本的冲突**：证据见 §6，是否放宽是设计决策，本轮不改下限。
3. **`ToolAvailabilityService` 未接线**：把全局闸「未开工程」绑到哪些既有 QAction 是业务 owner 的决定；本分支只可提供通用绑定件，不替各页决定「何时不可用」。
4. **约束页「删除选中约束」(`constraintDeleteButton`) 灰着却无禁用原因**：探针枚举 73 个禁用按钮，72 个带原因提示，仅此 1 个没有（`src/ui/pages/constraintpage_constraints.cpp:249` 创建、`:65` 控启停）。`src/ui/pages/**` 归其他方向且无 Issue 认领，本分支只记录不改。

## 6. 屏幕 / DPI 矩阵证据

探针是一个只观测、不断言的一次性 Qt 测试（源码未随 PR 提交，不进 ctest；原因：无断言的“测试”会掩盖失败，
证据以下表保存）。它在真实六页壳（含 attachWorkflows）上读窗口/dock/ribbon/状态栏尺寸，TSV 在
`/tmp/goal7/probe2/`，8 次运行退出码均为 0。屏幕配置走 offscreen 插件的 `configfile`。

**测量局限（必读）**：offscreen 插件满足 `geom = 尺寸·logicalBase/logical`、`dpr_eff = dpr·logical/logicalBase`，
两者耦合，无法同时得到“逻辑几何 = 物理÷缩放”与“dpr = 目标缩放”。本表采用 `dpr=目标、logicalDpi=96`，
因此 TSV 里 `geom`/`fitsAvail`/`overflow` 都是**物理像素**口径；真实逻辑桌面 = 物理 ÷ 缩放，下表单独推算。

| 规格（物理@缩放） | 报告 dpr | 窗口 | 物理口径 fitsAvail | ribbon 高 | 状态栏高 | 中央区（数据 / 编图页） |
|---|---|---|---|---|---|---|
| 1024×768 @1 | 1 | 1280×800 | 否，超出 260×36 | 126 | 30 | 624×644 / 624×644 |
| 1366×768 @1 | 1 | 1280×800 | 否，超出 0×36 | 126 | 30 | 同上 |
| 1920×1080 @1 | 1 | 1280×800 | 是 | 126 | 30 | 同上 |
| 1920×1080 @1.25 | 1.25 | 1280×800 | 是 | 126 | 30 | 同上 |
| 1920×1080 @1.5 | 1.5 | 1280×800 | 是（物理口径） | 126 | 30 | 同上 |
| 3840×2160 @1 / @1.5 / @2 | 1 / 1.5 / 2 | 1280×800 | 是 | 126 | 30 | 同上 |

实测事实：
1. 窗口在全部 8 个场景里都停在 1280×800 下限，既不随屏幕变大也不被屏幕夹紧。
2. dock 布局在所有场景一致：数据页 = 数据列表 + 页面板各 320×644，编图页 = 图层树 + 页面板各 320×644，
   对比页无 dock（中央区 1276×644）；624+320+320 = 1264 < 1280。
3. 字体恒为 `Noto Sans SC 9pt`；ribbon 126、状态栏 30 不随缩放变化（逻辑 DPI 恒 96）。

**由实测推算（非测量）**：1366×768 @1 的窗口比屏幕高 36 px（实测）；1920×1080 @1.5 的逻辑桌面是 1280×720，
窗口高 800 > 720，超出约 80 px（推算）；1920×1080 @1.25 的逻辑桌面 1536×864，放得下；
3840×2160 @1.5 / @2 的逻辑桌面 2560×1440 / 1920×1080，放得下。这是 §42.11 下限与小逻辑高度屏的真实冲突，
放宽与否属设计决策，本分支不改下限。

未测量：ribbon 宽度、dock 与中央区的逐像素重叠（TSV 只有各 dock 的 `名称:可见:浮动:宽x高`）、像素级裁切、
双屏、dock 拖拽/浮动。offscreen 结果不能当作手工视觉验收。

## 7. 复现

```bash
git fetch origin --prune && git worktree add -b goal7/ui-shell-help-20261014 ../paleo-wt-goal7-ui-shell origin/master
cd ../paleo-wt-goal7-ui-shell
ln -s <主仓>/vendor/superbuild/prefix vendor/superbuild/prefix && ln -s <主仓>/vendor/onnxruntime vendor/onnxruntime
export CMAKE_BUILD_PARALLEL_LEVEL=4 CTEST_PARALLEL_LEVEL=4 MAKEFLAGS=-j4
L=/tmp/paleo_workstation_global_build_j4.lock
flock -x $L cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DQGIS_PREFIX=<主仓>/vendor/superbuild/prefix
flock -x $L cmake --build build --target paleo tst_shell_chrome -j4
flock -x $L ctest --test-dir build -R '^tst_shell_chrome$' --output-on-failure
```
