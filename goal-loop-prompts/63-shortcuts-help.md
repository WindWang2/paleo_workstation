# Goal-Loop 方向 63：快捷键体系与帮助面——总表 / 冲突检测 / F1 上下文帮助

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

- **快捷键散装无体系**：全 src/ui 命中 QShortcut/QKeySequence/
  setShortcut 共 **44 处**，散落各面板：主窗 Ctrl+1..N 页切换 +
  Ctrl+Tab（`paleomainwindow.cpp:966-985`）、保存 Ctrl+S
  （`:1119`）、locator Ctrl+K（`paleomainwindow_attach_shell.cpp:156`）、
  datalist 键盘操作（`datalist.cpp:912`）、图层树 Delete
  （`layertreepanel.cpp:186`）、布局 Undo/Redo
  （`layoutundostack.cpp:44-45`）等——**无总表、无冲突检测、
  无发现性**（用户不知道有 Ctrl+K）。
- **无帮助面**：全仓零 QWhatsThis/QHelpContent/F1 处理
 （rg 零命中）；新用户只能读 docs/（开发者文档非用户手册）。
- **冲突实例面**：Ctrl+Tab（主窗页切换）与 Qt 文档 Tab 导航
  语义、datalist 内部快捷键与表格编辑键的边界——无中央注册
  处无从审计。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\shortcuts-help -b goal/shortcuts-help-20261007 origin/master
cd .worktrees\shortcuts-help
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。UI 测试 offscreen（tst_panels 先例）。

## 目标形态（建议按序）

1. **快捷键中央注册**：`src/ui/shortcuts/`—— ShortcutRegistry
  （数据在 ui 层，纯 QObject 无 QtWidgets 依赖的部分可下沉
  services 供测试）：注册面（id/键序/上下文/描述/来源面板）；
   既有 44 处逐个收编（不改键序——行为保留，只集中登记）。
2. **冲突检测**：registry 构建后查重（同上下文同键序 = 冲突）；
   冲突入启动日志 + 测试断言当前零冲突（有冲突先解决再验收）。
3. **快捷键总表 UI**：F1 或「帮助→快捷键」打开的对话框
  （分组/可搜索/复制单条）；DESIGN.md token 遵守。
4. **QWhatsThis 上下文帮助**：核心面板（主窗六页/剖面 dock/
   数据页/综合图）关键控件加 setWhatsThis（一句话说明）；
   WhatsThis 模式入口（工具栏 ? 按钮或 Shift+F1）。
5. **帮助面骨架**：F1 默认打开快捷键总表（轻决策——完整用户
   手册属另一方向，本方向只立骨架 + 总表落地）；帮助菜单
  （关于/快捷键/WhatsThis 提示）。
6. **测试**：registry 单测（注册/查重/上下文优先级）；总表
   对话框 offscreen 测试（条目数 = 注册数）；快捷键行为回归
  （抽查主窗 Ctrl+1/Ctrl+K 触发不变）。

## 通用纪律（方向内全程有效）

- **分层**：registry 归 ui（或其无 QtWidgets 部分归 services）；
  对话框归 ui；`check_layering.py --strict` 绿。
- **行为红线**：既有 44 处键序/上下文/行为逐条不变——只集中
  不改键；「顺手改键」违规（改键想法记 TODOS）。
- **DESIGN.md**：总表/帮助对话框视觉先提案再实现。
- **i18n**：新文案 tr() 全覆盖（ whatsThis 描述也是用户可见
  文案）。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：分组 taxonomy 自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-shortcuts-help.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/行为保留/冲突检测真实/whatsThis 文案质量/i18n 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 收编完整：registry 条目数 ≥44（对照 R0 散装清单逐条核）；
   抽查 5 处快捷键行为回归不变（offscreen 触发测试）。
2. 冲突检测：当前注册零冲突（测试断言）；故意注册重复键
   能被打红（mutation 验证）。
3. 总表：F1 打开；条目可搜索/复制；条目数与 registry 一致
  （测试）。
4. WhatsThis：核心面板 ≥30 个控件有 whatsThis（清单入
   ledger）；WhatsThis 模式可进出（offscreen 测试）。
5. 帮助菜单：关于/快捷键/WhatsThis 三入口可达（测试）。
6. 全量 ctest 对照 R0 红集合 diff 为空；check_i18n 绿。
