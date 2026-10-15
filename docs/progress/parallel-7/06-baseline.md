# 方向 06 · R0 基线：六页主界面 / Dock·Ribbon / 交互可用性 / 帮助体系

分支 `goal7/ui-shell-help-20261014`，基线 `origin/master` = `b0520429c21f858a1c053b876f511680088564a9`
（2026-10-14 取，运行期间复查未变）。独立 worktree `../paleo-wt-goal7-ui-shell`，
Debug / Ninja / vendored QGIS prefix（symlink 自主 checkout，只读使用）。

本文是 R0 记录：读了什么、测了什么、哪些被排除、哪些进入开发清单。逐轮验证 ledger
（`06-ui-shell-help.md`）尚未建立，待修复经验证后补写，不预先声称。

## 1. 围栏（写前先查，写后复查）

| 围栏 | 内容 | 检查方式 |
|---|---|---|
| 开放 PR | 开工时仅 #437（`codex/evidence-mapping-integration`，PR 明写「不合并」；任务书记 80 个文件，终查 84 个）。收尾时新出现 #446（02 方向，井 / 交会分析，16 个文件，与本分支交集 0）。其 CMakeLists.txt、`paleomainwindow.h`、`paleomainwindow_attach_mapping.cpp`、`paleomainwindow_workbench.cpp`、`pages/composepage|datagraphpage|mappingworkbenchpage.*` 等一律不写 | `gh pr view 437 --json files` 实时取；`/tmp/goal7/fence_check.py` 逐文件比对 `git diff --name-only origin/master...HEAD` |
| 开放 Issue | 执行中 70 → 77 条（新增 #438–#444 全是 #437 的 review 项，与本方向无交集）。不认领、不修、不改任何 Issue | 抽取 Issue 正文里出现的文件路径作文件级指纹，与 diff 比对 |
| Issue 文件级重叠（须绕开具体缺陷行） | `paleodockmanager.cpp`（#411 第 2 项：`DockScrollArea` 包装使 `m_seismic3dDock->widget()` 回退取面板永假——缺陷在 `attach_models.cpp`，本方向不动 `addDock` 的包装逻辑）；`decorations/paleodecorations.cpp`（#411 第 4 项）；`paleomainwindow_attach_models.cpp` / `attach_validate.cpp`（#411）；`taskpanel.cpp`（#374 引用）；`tests/tst_notifications.cpp`（#408 的 `\|\| true`） | 这些文件不写；`paleodockmanager.cpp` 只改布局命令路径，不碰 `addDock` |
| 不可新增 `cmake/extra-*.cmake` | 新文件须在 `CMakeLists.txt` 的 `_paleo_extra_manifest` 登记，而 `CMakeLists.txt` 被 #437 占用。新测试只能挂进已登记的 `cmake/extra-ux.cmake` / `extra-shortcuts-help.cmake` | — |
| `DESIGN.md` | 契约文件，改动需用户批准。本轮不改；内部矛盾记入 §5 | — |

主写范围（任务书白名单）实际触达：`src/ui/paleodockmanager.*`、`src/ui/paleoribbon.*`、`src/ui/locator/paleolocatorfilters.*`、
`src/ui/help/`、主窗薄壳 `paleomainwindow.cpp` / `_internal.h` / `_pages.cpp` / `_ribbon.cpp` / `_attach.cpp` / `_attach_shell.cpp`
（均非 #437 保护文件）、`translations/paleo_zh_CN.ts`（帮助/布局文案，#437 不改）、对应测试。

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

一次性探针（不提交、不进 ctest：无断言的“测试”会掩盖失败）在真实六页壳上读窗口/dock/ribbon/状态栏尺寸。
屏幕用 offscreen `configfile` 给物理分辨率（dpr=1），缩放用 `QT_SCALE_FACTOR` 实现——这样
`#screen` 行同时给出**逻辑几何与 dpr**（第一次用 offscreen 自己的 dpr/logicalDpi 配置，两者耦合、dpr 被平方，
已弃用）。TSV 在 `/tmp/goal7/probe3/`，10 次运行退出码均为 0；本表在修复前的构建上测得（修复不改布局）。

| 物理 @ 缩放 | 报告 dpr | 逻辑屏幕 | 窗口 | 放得进屏幕 | 超出（宽×高） | ribbon 高 | 状态栏高 |
|---|---|---|---|---|---|---|---|
| 1366×768 @1 | 1 | 1366×768 | 1280×800 | 否 | 0×36 | 126 | 30 |
| 1920×1080 @1 | 1 | 1920×1080 | 1280×800 | 是 | 0×0 | 126 | 30 |
| 1920×1080 @1.25 | 1.25 | 1536×864 | 1280×800 | 是 | 0×0 | 126 | 30 |
| 1920×1080 @1.5 | 1.5 | 1280×720 | 1280×800 | **否** | **2×82** | 126 | 30 |
| 1920×1080 @2 | 2 | 960×540 | 1280×800 | **否** | **322×262** | 126 | 30 |
| 2560×1440 @1.25 / @1.5 | 1.25 / 1.5 | 2048×1152 / 1707×960 | 1280×800 | 是 | 0×0 | 126 | 30 |
| 3840×2160 @1 / @1.5 / @2 | 1 / 1.5 / 2 | 3840×2160 / 2560×1440 / 1920×1080 | 1280×800 | 是 | 0×0 | 126 | 30 |

实测事实：
1. 窗口在全部场景都停在 1280×800 下限，不随屏幕变大也不被屏幕夹紧；ribbon 高 126、状态栏高 30、字体 `Noto Sans SC 9pt` 不随缩放变化。
2. 真实冲突有三处，都是**测得**的：1366×768 @100% 窗口比屏幕高 36 px；1920×1080 @150%（逻辑 1280×720）高 82 px，状态栏在屏幕外；1920×1080 @200%（逻辑 960×540）宽高都放不下。1920×1080 @125% 及所有 2K/4K 场景放得下。
3. dock 布局在所有场景一致：数据页 = 数据列表 + 页面板各 320×644，编图页 = 图层树 + 页面板各 320×644，对比页无 dock（中央区 1276×644）；624+320+320 = 1264 < 1280。
4. 窗口停在最小宽度 1280 时，predict / constraint / compose 三页 ribbon 内容放不下，右侧出现 12×96 的 SARibbon 滚动钮（见 §8 F）。

这是 §42.11 下限（用户选择 A）与小逻辑屏的真实冲突；放宽与否属设计决策，本分支不改下限。
未测量：ribbon 宽度、dock 与中央区的逐像素重叠（TSV 只有各 dock 的 `名称:可见:浮动:宽x高`）、真实双屏、
dock 拖拽。offscreen 结果不能当作手工视觉验收。

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

## 8. 第 2 阶段运行期审计（A / B / C / D / E / F / G）

方法：同一个一次性探针在真实六页壳上枚举 ribbon / dock / 状态栏 / 焦点链 / 帮助覆盖（TSV 在 `/tmp/goal7/probe3/`、
`probe4/`、`probe5/`、`probe6/`）；确认的缺陷用 `tst_shell_chrome` / `tst_dockmanager` 里的红测钉住，修复后绿。

### A 视觉契约（对照 DESIGN.md 字阶）
- 现有行为 1：ribbon 栏、全部 dock 标题、页面控件正文都是 `Noto Sans SC 9pt`；状态栏坐标/比例尺是 `JetBrains Mono 9pt`；
  CRS 与底图来源标注是 8pt。与 DESIGN「正文 9pt、次级标签 8pt、坐标数值 mono 9pt」逐项一致。
- 现有行为 2：约束页数值输入框 8 个是 `JetBrains Mono 9pt`，其余输入框是正文字体；符合「数值读数用 mono」。
- 现有行为 3：ribbon 命令钮只有两档高度，大按钮 73 px、小按钮 23 px，各页一致（共 101 个命令钮）。
- 观察（页面归属，只记录）：采样的页面控件里 `QPushButton` 高 28 px（29 个）、30 px（4 个），`QComboBox` 全部 30 px。2 px 差异来自页面自己的控件，`src/ui/pages/**` 归其他方向，本分支不改。
- 结论：本方向拥有的 chrome（ribbon、dock、状态栏）没有发现与 DESIGN 的视觉分裂，`ui_tokens` / `ui_invariants` 门禁保持绿，**A 没有生产改动**。

### B 窗口布局
- 屏幕/缩放矩阵见 §6（含三处测得的越界）。
- 非回归（已入测试）：浮动 dock 在屏幕外保存、恢复后抓手区仍落在可用屏幕内；屏内位置恢复后不被挪动（`tst_dockmanager`）。红测没有复现缺陷——当前 Qt 已保证这一点，用例带「dock 确实先被放到屏外」的前置断言，不会空洞通过。
- 真实缺陷（已修）：布局命令破坏页 dock 策略，见 PR 正文与 `06-ui-shell-help.md`。

### C 交互可用性
- 现有行为 1：整窗枚举到 73 个禁用按钮（含页内与 ribbon），72 个带「为什么灰」提示。
- 现有行为 2：六页来回切三轮，dock 策略每轮一致（`repeatedPageCyclingKeepsDockPolicy`，非回归）。
- 现有行为 3：撤销/重做、保存/放弃编辑的禁用提示都是具体原因（「没有可撤销的编辑」「当前没有进行中的编辑会话」）。
- 反例：约束页 `constraintDeleteButton` 灰着无原因（页面归属，只记录）。
- `ToolAvailabilityService` 生产代码原零调用。第 3 阶段已接入（`9e30e17a`）：全局工程门 → 五个导入入口（ribbon 命令 + 数据页按钮），无工程置灰并在 tooltip 写明「先新建或打开工程」——依据 `dataimport_document.cpp:424`：无工程时导入必失败，此前要先弹文件对话框、选完才报错。逐层 `layerBusy` 绑定仍归各页 owner（预测/编图/验证已有更具体的门控，编辑动作走 `QgisEditingService`）。

### D 帮助
- 现有行为 1：帮助目录 69 条，除 4 条不实外都落到真实控件。
- 现有行为 2：页内按钮有帮助时，点的却是 ribbon 上的镜像按钮——101 个 ribbon 命令钮的 `whatsThis` 全是空（Shift+F1 点 ribbon 什么也不显示）。
- 现有行为 3：撤销/重做是图层编辑的撤销，直挂 QAction，不经镜像；与数据列表的撤销（目录里「撤销上一步数据列表操作」）是两件事，按文案配对会配错。
- 已修：镜像按钮跟随页内按钮的 `whatsThis`/`accessibleDescription`（13 个，含 6 个撤销/重做另写编辑语义帮助）。
- 第 3 阶段已补齐（`6d0f82db`）：98 个可见 ribbon 命令钮 100% 带 `whatsThis`（探针口径 14→81→98，唯一命令缺口 42→0）。文本全部复用作者已写的用途串——动作 tip、编辑条 `hint` 属性、页按钮目录条目——只有方向线/间隔线两条是新写的，语义取自 `paleoalgorithms.h:32-37`（break_line 硬屏障绕行、direction_line 各向异性轴）。回归钉：`everyVisibleRibbonCommandHasHelp`。

### E 术语
- 6264 条界面字符串（`.ts` 源串）逐条对照 `tools/check_l10n.py` 的 `BANNED_TERMS` 全表（37 个禁用词）：0 命中；`单因素` 70 条、`单因子` 0 条；`check_l10n.py` 保持绿。
- 含「项目」的 12 条：10 条是独立 Web 工作台的「独立项目」（刻意与「工程」区分，`stratigraphicweb*`），2 条是映射工作流里的「本项目纹理库」「未绑定项目数据」；后者在 `src/workflow/mappingworkbench*` / `mappingworkflow*`，属 #437 / 其他方向，只记录。
- 观察：`src/qgis/layervocabulary.h:125-129` 的提示把四张地图页统称「编图页」，与页签名「智能编图」同词异指（该文件不在本方向）。
- **E 没有生产改动。**

### F 可访问性与键盘
- 现有行为 1（反例，已修）：SARibbon 把所有命令钮设成 `Qt::NoFocus`，Tab 到不了 ribbon——54 个启用的命令钮全不在 Tab 链上。
- 现有行为 2（反例，已修）：窗口停在最小宽度时三页 ribbon 溢出，出现的 12 个左右滚动钮是匿名纯图标件，无名称无提示。
- 现有行为 3（反例，已修）：定位器吞 Tab——探针连按 160 次 Tab，从第 23 次起焦点一直停在定位器输入框；独立的 `QgsLocatorWidget`（周围放两个普通输入框）复现同样结果，是 QGIS 原生设计（上游 `qgslocatorwidget.cpp` 的 `Key_Tab` → 补全 / 下一条结果，`Key_Escape` 只收结果列表）。
- 修复后实测：Tab 链含 ribbon 命令钮，焦点环可见（截图 `assets/ribbon-button-*.png`），Space 进入按下态；聚焦被裁掉的钮会把类别滚进可视区（直接 `scroll()`，无动画）；定位器空闲时 Tab / Shift+Tab 放行，有文本或真实结果行时仍归 QGIS，Esc 把焦点还给进入前的控件。空查询时 QGIS 在列表里摆的是「可用过滤器」伪结果行，判据因此改为「真实结果行数」而不是浮窗是否可见。
- **未处理**：dock 内的 `dockScrollHost` / `scrollableToolRow`（`QScrollArea`）是无名称的 Tab 停靠点；dialog 重复弹窗、动态主题切换下的焦点环、tab order 的整体顺序未审计。

### G 截图与对比
- `assets/dock-r2-data-*.png`、`assets/dock-r4-predict-*.png`：同一窗口（1440×900）修复前（关闭意图钩子，等价 origin/master 的布局命令路径）与修复后的并排。R-2：修复前数据页的图层树在恢复默认布局后重新冒出，压在数据列表下方；修复后保持三栏。R-4：修复前预测页恢复保存的布局后图层树整个消失；修复后在左侧。逐像素差异：R-2 18247 / 1296000，R-4 139006 / 1296000。
- 六页 × 浅/深色共 24 张整窗截图（offscreen，1440×900）保存在 `/tmp/goal7/probe3/shots/`，未入库；offscreen 截图**不能**当作手工视觉验收。
