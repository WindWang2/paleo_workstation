# 数据管理页键盘与命令面板（P3 交付文档）

日期：2026-09-29。分支：`wave/data-page-operations`。
实现：`dataopspalette.h`（面板/快捷键表）+ `dataops/dataopsfuzzy.h`
（模糊匹配 + 命令注册表）+ datalist/datapage 接线。

## 1. 快捷键表（数据页内）

| 键 | 动作 | objectName（QShortcut） |
|---|---|---|
| Ctrl+K | 命令面板（D6.1） | `scCommandPalette`（DataPage 层） |
| Ctrl+Z / Ctrl+Y | 撤销 / 重做（D5.2） | `scUndo` / `scRedo` |
| Ctrl+A | 全选可见项（表/树内建） | — |
| Ctrl+Shift+A | 反选 | `scInvert` |
| Ctrl+Shift+F | 按过滤器选中 | `scSelectFiltered` |
| Ctrl+F | 聚焦搜索框 | `scFocusSearch` |
| F2 | 重命名当前实体（树内，D4.1） | `scRename` |
| ? | 快捷键表对话框（D6.3） | `scShortcuts` |
| Ctrl+Alt+V | Vim 风导航开关（D6.5，默认关） | `scVimToggle` |
| ↑↓ / Enter / Esc | 视图内建导航/激活/取消 | — |

全键盘审计结论（D6.4）：Tab 环路 = Qt 焦点链（新部件全部
`StrongFocus`）；Enter 激活 = 树/表/图标视图 itemActivated；Esc =
对话框 reject / 拖放取消（Qt 内建）；Space 在 ExtendedSelection 下为
Qt 内建无勾选语义（列表无勾选列，如实）。

## 2. 命令面板（D6.1/D6.2）

`Ctrl+K` → `DataCommandPalette`（模态工具窗）：

- **三类源**：命令注册表（动作/过滤器预设）、资产、实体——统一模糊
  打分排序（title 主、subtitle 半权）。
- **交互**：输入即筛（↑↓ 选、回车执行、Esc 关）；空查询全命中；
  零命中给「无匹配 — 换个关键词试试」。
- **执行**：命令条目跑 `trigger` 回调 + `commandChosen`；资产/实体条目
  发 `assetChosen`/`entityChosen`（DataPage 接到 selectAsset/
  selectAssetsForEntities 定位）。

`CommandRegistry`（D6.2）是唯一登记处：id/标题/分类/关键词/快捷键串/
使能谓词/触发器——命令面板、快捷键表对话框、（未来）菜单共用。
`registerCommands()` 装填 20+ 动作（选择/批量/过滤/视图/刷新）。

## 3. 模糊匹配器

subsequence 打分：连续命中 +8/步、词首命中 +6（`_ - .` 与空格后）、
基础 +2；大小写不敏感；候选短于查询 = 0。无第三方依赖，
10k 条目毫秒级。

## 4. Vim 风可选导航（D6.5，默认关）

`DataNavTree::setVimMode`（QSettings `dataops/vimMode`）：

- `j`/`k`：展平序上/下一项（QTreeWidgetItemIterator 序 = 视觉序）
- `g` 跳顶 / `G` 跳底（Shift+G）
- `/` 发 `searchFocusRequested` → 聚焦 assetSearchEdit

关闭时全部键回落 Qt 内建行为（`dataops_d6_vimTreeNavigation`
双态断言）。

## 5. 冲突检测与提示（D6.6）

`CommandRegistry::shortcutConflicts()`：同快捷键串多命令 → 冲突对。
`ShortcutsDialog` 加载注册表时检测，冲突在表下方红字列出
（`shortcutConflictsLabel`，无冲突隐藏）。测试注入重复 Ctrl+Z 断言。

## 6. 焦点指示（D6.7）

- 全局 2px focus-ring（主窗 QSS，tst_ui 既有断言）覆盖新部件的
  QLineEdit/QTableView/QComboBox。
- 新交互部件（CRUD 条按钮等）显式 `StrongFocus`。
- 命令面板输入框/结果表同吃全局焦点环。

## 7. 测试驱动注意

合成 QDropEvent/DragMove 会被 QApplication 吞（无 QDrag 会话）——
拖放测试走 `DataNavTree::handleDrop` 等直调口；键盘测试用
`QTest::keyClick`（真实键事件，不受影响）。
