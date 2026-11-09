# UI 命令注册表（UI 重设计 U0 + U1）

状态：U0 + U1 已落地（分支 `goal/ui-cmdregistry-20261109`，基于 master 5ccdb322）。
完整重设计方案（U0–U6）见 UI 重设计设计稿 §9 契约草图 / §10 分期；本文只记已实现部分。

## 目标

同一个动作只有一个身份（命令 id）和一个执行入口。Ribbon、文件菜单、快速访问栏、
Ctrl+K、快捷键、面板右键菜单，以及以后的 AI / 批处理，都经 `CommandDispatcher` 触发；
计算类命令（Compute）经 `CapabilityRegistry` / `capability::execute` 网关执行。
界面外观与行为保持不变。

## 分层

| 层 | 文件 | 作用 |
|---|---|---|
| 数据层 | `src/services/commandspec.{h,cpp}` | `CommandSpec` 登记表（94 条）+ `CommandRegistry::validate()` 自检 |
| 功能层 | `src/workflow/commanddispatcher.{h,cpp}` | 绑定处理器 / 状态（禁用必带原因）/ 触发；Compute → `PreparedPlan` → 网关 |
| QGIS 封装 | `src/qgis/commandlocatorfilter.{h,cpp}`、`commandlocatorentry.h` | Ctrl+K 过滤器 `paleo_commands`（前缀 `c`） |
| 视图层 | `src/ui/commandactionbinder.{h,cpp}` | QAction ↔ 命令：`route` / `bindMirror` / `bindNative` |
| 视图层 | `src/ui/paleomainwindow_commands.cpp` | 主窗命令处理器（project.* / view.* 等）+ Ctrl+K 数据面 |

## 登记表（CommandSpec）

字段：`commandId`、`contractVersion`、`kind`（Query / Command / Compute / Workflow / View）、
`capabilityId`、`title`/`tooltip`（翻译上下文 `PaleoCommands`）、`stageId`、`ribbonGroup`、
`requiredContext`、`docKinds`、`effect`、`surfaces`（Ribbon / Locator / ContextMenu /
Shortcut / Agent / Menu 位）、`shortcutId`。

`validate()` 不变式：id 唯一且格式合法；阶段/效果在词表内；至少一个入口；shortcutId
唯一且只配 Shortcut 入口；View 命令不带能力；Compute 命令的能力可解析且 id = 能力 id；
Agent 入口只给 `Callable` 能力（与 `CapabilityRegistry::callableIds()` 精确相等）。

## 三种绑定

* **处理器（handler）**：实现在分派器里（`PaleoMainWindow::bindCommand`），QAction 只 `route`。
* **镜像（mirror）**：页面按钮仍是状态源（enabled + 原因 tooltip，`PaleoRibbon::mirror`）；
  命令处理器经 `PaleoRibbon::activateMirror` 激活源按钮。U3 页面重构时退役。
* **原生（native）**：QAction 自身即实现（QGIS 地图工具、停靠开关、编辑工具条），
  状态取自 action，可勾选动作按 `checked` 参数幂等。

## 禁用原因

`CommandDispatcher::state()` 对每种禁用情形都给原因：未登记、未接线、入口不允许、
能力缺失、状态提供者给出（缺省「当前不可用」）。Ctrl+K 中禁用命令照样列出并写
「不可用：原因」；触发被拒时状态栏显示原因。

## Compute 桥

`prepare(id, ctx, params, inputs)` → `PreparedPlan`（含 `capability::ExecuteRequest`）：
校验能力可调用、入口许可、参数（`effectiveScalarParams`），缺 `clientRequestId` 时生成 UUID，
最后调宿主绑定器补 `operationDir` / `documentId`。`execute(plan)` 调执行器（缺省
`capability::execute`）并发 `operationChanged(operationId, commandId)`。
`tst_commanddispatcher::bridgeMatchesDirectGateway` 证明经分派器与直接调网关结果一致。

## 门禁

* `tst_commandregistry`：登记表自检、Agent 面 = Callable 能力集。
* `tst_commanddispatcher`：绑定/状态/触发/拒绝原因、Compute 桥与直接网关一致。
* `tst_commandshell`（真实主窗）：Ribbon 面板 / 快速访问栏 / 文件菜单每个 QAction 都挂
  已登记且已接线的命令 id；Ctrl+K 有 `paleo_commands` 过滤器；源码闸：
  `paleomainwindow_ribbon.cpp` / `ribbonpanels.cpp` 不再有 `findChild<QPushButton>()->click()`。
