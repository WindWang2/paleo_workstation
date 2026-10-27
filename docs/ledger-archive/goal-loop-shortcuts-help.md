# Goal Loop ledger — shortcuts-help（方向63：快捷键中央注册表 + 快捷键总表 + 帮助面）

- 分支：`goal/shortcuts-help-20261007`（自 origin/master `f130fb2` 起，worktree `/workspace/paleo_workstation-shortcuts-help`）
- 环境：Debian trixie 容器，**无 QGIS 运行库**；Qt 6.8.0（`/workspace/Qt/6.8.0/gcc_64`）可用。
  `sudo apt-get install debootstrap ubuntu-keyring`（为搭 Ubuntu 26.04 + QGIS 4.2.3 构建根）被自动安全审查拦截
  （系统软件改动需审批），未申请提权，改走「子集真编译 + 全量语法检查 + CI」路线（同 PR #247 先例）。
  见文末「验证证据与未验证项」。

## Oracle（验收，逐条需证据）

1. 收编完整：src/ 内（除 `src/ui/shortcuts/`）零 `new QShortcut` / `setShortcut(`；注册表条目数 ≥ R0 绑定数；
   ≥5 条快捷键 offscreen 行为回归（含 Ctrl+1、Ctrl+K）。
2. 冲突检测真实：启动日志输出冲突/遮蔽；测试断言零冲突；变异（故意重复键）能打红。
3. 总表：F1 与「帮助 › 快捷键总表…」打开；分组/搜索/复制单条；条目数 = 注册数（offscreen 测试）。
4. whatsThis：核心面板 ≥30 处；Shift+F1 / 菜单进入、Esc 退出。
5. 帮助菜单三入口（快捷键总表 / 这是什么？ / 关于）可达。
6. 门禁：check_layering --strict、check_i18n 绿；新字符串全 tr()/QT_TRANSLATE_NOOP。
7. 两轮以上 review，五维（分层 / 行为保持 / 冲突检测真实性 / whatsThis 文案 / i18n）零 High/Medium。

## 轮0 勘察结论（实测事实，基线 f130fb2）

- `rg -n "QShortcut|QKeySequence|setShortcut" src/ui` = **47 行**（7 行 `#include` + 40 行代码）；
  任务书的「44 处」与任何一种口径都对不上（token 数 62、代码行 40），下表按**实际绑定**逐条落账：
  src/ui 内 **26 个绑定** + src/app `crossplotcontroller.cpp:32` 1 个 = **27 个散装绑定**；
  `previewmappage.cpp:436` 的 `addTool(..., shortcut)` 形参全仓无调用方传值 → **0 个绑定**（死参数，改成按 id 形参，行为不变）。
- 事件序事实（决定冲突模型）：Qt 先发 ShortcutOverride，再查 QShortcutMap，最后才是 keyPressEvent；
  同一窗口两个同键 WindowShortcut 同时激活 → Qt 判歧义、**一个都不触发**（Ctrl+K 历史事故即此）；
  不可见控件上的快捷键不激活。
- 并行方向 PR：#246（l10n，`translations/*.ts`、`cmake/extra-ux.cmake`、`src/app/main.cpp`）、
  #247（issues sweep，`src/ui/paleomainwindow.cpp` buildShell 一处）、#248（任务书）、#249（审计尾巴，`CMakeLists.txt` 测试行）。

### R0 → 注册表映射（27 个绑定）

| # | f130fb2 位置 | 原键序 / 上下文 | 注册 id | 收编后 |
|---|---|---|---|---|
| 1–6 | paleomainwindow.cpp:966 循环 | Ctrl+1..6 / Window | `main.page.<页 id>` | `bindShortcut`，objectName `pageShortcut.*` 保留 |
| 7 | paleomainwindow.cpp:981 | Ctrl+Tab | `main.page.next` | 同上 |
| 8 | paleomainwindow.cpp:985 | Ctrl+Shift+Tab | `main.page.prev` | 同上 |
| 9 | attach_shell.cpp:168 + paleomainwindow.cpp:1119/1158 | QKeySequence::Save（地层对比页置空） | `main.project.save` | `setActionShortcutActive`（三处同一函数，页切换语义不变） |
| 10 | attach_shell.cpp:156 | Ctrl+K | `main.locator.focus` | `bindShortcut` |
| 11 | datapage.cpp:124 | Ctrl+Shift+P | `data.palette.open` | `bindShortcut` |
| 12–13 | dataopspalette.h:72/73 | Up / Down | `data.palette.up/down` | `bindShortcut` |
| 14–21 | datalist.cpp:918–939 | Ctrl+Z/Ctrl+Y/Ctrl+Shift+A/Ctrl+Shift+F/F2/?/Ctrl+F/Ctrl+Alt+V | `data.list.*` | `addShortcut(id, name, handler)`，`m_shortcuts` 文本取 `sc->key()`（与原 `key.toString()` 同值） |
| 22 | layertreepanel.cpp:186 | Delete | `layers.remove` | `bindAction` |
| 23–24 | layoutundostack.cpp:44/45 | StandardKey Undo/Redo | `layout.undo/redo` | `bindAction` |
| 25 | layoutdesignershell.cpp:273 | Delete | `layout.delete` | `bindAction` |
| 26 | wellcompositepanel.cpp:63 | Ctrl+G | `wellcomposite.gotoDepth` | `bindShortcut` |
| 27 | crossplotcontroller.cpp:32（src/app） | Ctrl+Alt+X | `main.crossplot.open` | `bindAction` |

全部原样 `Qt::WindowShortcut`（原代码都没改上下文），`tst_shortcuthelp::r0SitesPreserved` 27 行数据驱动逐条钉死。
新增 3 条：`main.help.shortcuts`（F1 = HelpContents）、`main.help.whatsThis`（Shift+F1 = WhatsThis）、`sheet.copy`（总表内 Copy）。
另登记 41 条 **KeyHandler**（`keyPressEvent` / 事件过滤器里的焦点内按键：数据树 Space/Delete/Vim、
派生图 Return/Space、柱状图画布、地图工具 Esc/Return/Delete、剖面画布、三维视口、停靠拖动 Esc），
只用于总表展示与冲突/遮蔽检测，**不改它们的代码**。合计 **71 条（30 bound + 41 key-handler）**。

## 决策账

| # | 决策 | 理由 / 代价 |
|---|------|-------------|
| D1 | 纯数据部分 `ShortcutRegistry` 放 `src/services/`（层：数据，只用 QtCore/QtGui 的 QKeySequence） | 方向说明允许；无 QtWidgets，check_layering --strict 绿；单测 `QTEST_GUILESS_MAIN` |
| D2 | 目录（唯一真源）与绑定入口放 `src/ui/shortcuts/shortcutcatalog.*` | 绑定 QShortcut/QAction 必然是视图层；键序只在这一个文件里出现，源码闸测试防回潮 |
| D3 | 作用域用 `/` 分段路径（`main`、`main/data`、`main/data/assets`、`layout-designer`、`dialog/…`），前缀覆盖 = 作用域包含 | 能同时表达「主窗级」「面板级」「焦点内」与「独立窗口」；按段比较，`main` 不覆盖 `mainx` |
| D4 | 冲突三类：Ambiguous（两条 Qt 快捷键作用域相同或嵌套，或一方 ApplicationShortcut）/ Duplicate（同作用域同键的其余组合）/ Shadowed（外层 Qt 快捷键盖住内层焦点内处理器，仅告警） | 对应 Qt 真实行为：歧义 = 一个都不触发（硬错）；遮蔽 = 内层处理器永远收不到（行为如此，但不是本方向要改的） |
| D5 | `resolve(key, ctx)`：覆盖该位置的 Qt 快捷键优先（多于 1 条 → 无解），否则最深的焦点内处理器（并列 → 无解） | 与事件序一致；单测覆盖 |
| D6 | 文案用 `QT_TRANSLATE_NOOP("PaleoShortcuts"/"PaleoWhatsThis")`，显示时 `QCoreApplication::translate` | 表驱动目录无法用 `tr()`；lupdate 可提取（已核） |
| D7 | 帮助面挂在 ribbon 右侧按钮组（`helpMenuButton`，InstantPopup 菜单），F1/Shift+F1 动作挂主窗 | 主窗是 SARibbon，没有传统菜单栏；「帮助」按钮紧挨「布局」按钮，入口可见 |
| D8 | 「这是什么？」只回填**空的** whatsThis，且控件必须在声明的面板类子树内 | 不覆盖已有说明；同名控件（各页都有 `runButton`）不串页；幂等可重复调用 |
| D9 | 不改任何键序；发现的可疑点进 TODOS（见「遮蔽告警」） | 红线：行为不变 |
| D10 | 数据页既有的「?」快捷键表（DataPage ShortcutsDialog）保留不动 | 行为保持；统一到新总表另列 TODOS |

### 分组 taxonomy（总表顺序）

全局导航 · 工程与文件 · 帮助 · 数据管理 · 综合柱状图 · 地图与图层 · 剖面与三维 · 图件设计器 · 停靠与对话框。
按「用户在哪个工作面」而非「源文件」分组；同组内按登记顺序（= 工作流顺序）。

### 总表视觉方案（实现前定稿）

- 非模态 680×540 对话框（`shortcutSheetDialog`），外边距 `spacingMd`(16)、控件间距 `spacingSm`(8)，DESIGN.md token，无裸色值。
- 顶部搜索框（按键 / 功能 / 面板 / 分组全文，大小写不敏感）；有「当前位置」时显示「只看当前位置可用的条目」勾选框。
- 三列树：按键（`PaleoTheme::monoFont()` JetBrains Mono 9pt）/ 功能 / 适用范围；组行加粗、首列跨列。
- 底栏：计数（`mutedCaptionStyleSheet`，「共 %n 条」/「显示 %1 / %2 条」/「没有匹配的条目」）+「复制所选」按钮；
  右键「复制此条」；Ctrl+C（经注册表 `sheet.copy`）。复制格式 `按键\t功能\t适用范围`。
- 底部提示行：Shift+F1 进入「这是什么？」。
- 按 F1 时取键盘焦点所在面板 → 选中覆盖该位置的最深条目（例：焦点在柱状图里 → Ctrl+G 高亮）。
- offscreen 截图核对：全表 71 条分 9 组；柱状图上下文过滤后 31/71 条、Ctrl+G 选中（证据截图留在构建机，未入仓）。

### 遮蔽告警（启动日志 info，测试钉死清单）

1. `layers.remove`（Delete，图层树面板的 QAction，WindowShortcut）↔ `data.assets.remove`（数据树焦点内 Delete）
2. `layers.remove` ↔ `map.vertex.delete`（节点编辑工具焦点内 Delete）

图层树面板在主窗内可见时，Delete 先被这条窗口级动作吃掉；f130fb2 即如此（未改）。是否改成
`WidgetWithChildrenShortcut` 属于行为改动，进 TODOS 待验证。

### whatsThis 清单（69 条，`src/ui/help/whatsthiscatalog.cpp`）

主窗外壳 16（页签、顶栏搜索、布局/处理/Web/帮助按钮、层位条、结束地图操作、状态栏四项、启动页四项）·
数据页 12 · 地层对比 2 · 预测编图 6 · 单因素图 5 · 智能编图 6 · 验证 4 · 地震剖面 dock 8 · 综合柱状图 10。
带快捷键的控件在说明末尾追加「（快捷键：…）」，键序取自注册表（改键自动跟随）。
回填时机：主窗构造末尾、`attachWorkflows` 末尾（页面板此时才在）、`WellCompositePanel` 构造末尾（它在数据预览里按需创建）。

## FILE CONFLICT MAP（对 open PR #246–#249）

| 本分支文件 | #246 l10n | #247 issues | #248 任务书 | #249 审计尾巴 | 结论 |
|---|---|---|---|---|---|
| `CMakeLists.txt`（`_paleo_extra_manifest` +1 行，~1137） | — | — | — | 改 ~1048 行测试登记 | 不同 hunk，可自动合并 |
| `src/ui/paleomainwindow.cpp`（include / buildRibbon / restoreCorrelationDocks / showPage / ctor 末尾） | — | buildShell ~628 一处 | — | — | 不同 hunk |
| `cmake/extra-shortcuts-help.cmake`（新） | — | — | — | — | 无 |
| 其余 src/ui、src/app、tests 新改文件 | — | — | — | — | 无重叠 |
| 不碰：`translations/*.ts`、`cmake/extra-ux.cmake`、`src/app/main.cpp`、`tools/check_l10n.py` | 改 | | | | 刻意避开；新文案的 .ts 刷新待 #246 合入后跑 lupdate（TODOS） |

## 轮次

### 轮1 — 注册表 + 收编 + 总表 + 帮助面 + 测试

产出见提交列表。轮1 review（五维）：

| 维度 | 发现 | 级别 | 处置 |
|---|---|---|---|
| 分层 | registry 进 services 仅依赖 QtGui；catalog/sheet/help 在 ui；check_layering --strict 绿 | — | — |
| 行为保持 | `helpsurface.h` 内联访问器解引用前置声明类型的 `QPointer` → moc 编译单元报错（子集构建发现） | Medium | 访问器移到 .cpp，已修 |
| 行为保持 | `datalist.cpp` 漏 include、另一处 include 误插在 `#if __has_include(moc_…)` 块内（语法检查发现） | Medium | 已修，语法检查 OK |
| 冲突检测 | 测试只断言「零冲突」不足以证明检测真实 | Medium | 加三类变异（Ctrl+K 历史事故 / 同作用域重复 Esc / 第二条 F1）+ 源码闸变异，均实测打红 |
| whatsThis 文案 | 「SHA 失配」是实现术语 | Low→已修 | 改为「文件校验值不一致」 |
| i18n | `translate(kTrContext, …)` 不被 lupdate 识别 | Medium | 改字面量上下文 `"PaleoWhatsThis"` |
| i18n | 总表勾选框提示「按按下 F1 时…」病句 | Low→已修 | |

### 轮2 — 复审（全 diff 重读）

| 维度 | 结论 |
|---|---|
| 分层 | 零 High/Medium。services 头无 QtWidgets；新目录头文件均有 `// 层：` 标记 |
| 行为保持 | 零 High/Medium。27 处键序/上下文逐条比对；objectName（`pageShortcut.*`、`sc*`）保留；`attachWorkflows` 二次调用早退语义未动（whatsThis 回填在置位前）；F1/Shift+F1 在 f130fb2 上无绑定 |
| 冲突检测 | 零 High/Medium。按 Qt 事件序建模，变异全部打红 |
| whatsThis 文案 | 零 High/Medium。69 条均为一句话用户语言，objectName 全部在源码中存在（脚本核对） |
| i18n | 零 High/Medium。check_i18n 绿；新增 `tr()` / `QT_TRANSLATE_NOOP` 全覆盖 |

Low（不阻塞，写进 PR 描述）：
- L1 保存按钮提示「保存工程（Ctrl+S）」与定位器占位符「…Ctrl+K」仍是硬编码键名（非本方向收编对象；改键需同步）。
- L2 遮蔽两条（见上），属既有行为。
- L3 总表与数据页「?」快捷键表并存。
- L4 关于框产品名未取 `applicationDisplayName`（main.cpp 未设置显示名，沿用窗口标题里的「Paleo Workbench」）。
- L5 新文案尚未进 `translations/paleo_zh_CN.ts`（源文即中文，显示无影响；#246 合入后 lupdate）。

## 验证证据与未验证项

见 PR 描述「验证」节（逐条命令与输出摘录）。未验证：真实全栈构建、`tst_shortcuts_shell`、全量串行 ctest 与 R0 红集对比——需 CI。
