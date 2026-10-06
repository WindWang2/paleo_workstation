# 数据管理页撤销/重做（P3 交付文档）

日期：2026-09-29。分支：`wave/data-page-operations`。
实现：`src/ui/pages/dataopsundo.h`（栈）+ `dataops/dataopscommands.h`
（命令对象）+ datalist.cpp 接线。

## 1. 统一接口（D5.7）

```cpp
class DataOpCommand {
  virtual void redo() = 0;             // 应用到 catalog/sidecar（纯操作）
  virtual void undo() = 0;
  virtual QString text() const = 0;    // 「挂接 A1.Las→well-A1」主谓宾
  virtual QByteArray id() const;       // 合并键（空 = 不合并）
  virtual bool mergeWith(const DataOpCommand *other);  // 相邻同类吞并
};
```

命令对象**只做数据操作**，不触发 UI——刷新由 `pushCommand` 统一
（一次 undo 不连发 N 次全表重建）。

## 2. 命令清单（全部可撤销）

| 命令 | redo | undo |
|---|---|---|
| `AttachLinkCmd` | attachLink（未决→已决） | setLinkUnresolved |
| `DetachLinkCmd` | setLinkUnresolved | attachLink 回原实体 |
| `TransferLinkCmd` | 同一链接行 解挂→挂目标 | 反向 |
| `SetPrimaryCmd` | setLinkPrimary | 恢复前主关联 |
| `TagCmd` | 标签 add/remove（sidecar） | 反向（可合并） |
| `TypeOverrideCmd` | 类型改写（sidecar） | 回前值/清除 |
| `SoftDeleteCmd` | 进/出可回收清单（sidecar） | 反向 |
| `EntityEditCmd` | 实体名/坐标/备注 override | 回前值 |
| `EntityCreateCmd` | addEntity + 恢复可见 | **软删**（catalog 无 removeEntity） |

身份寻址沿 T28：动作时刻重扫 `links()`（`indexOfLink(assetId, role,
entityId)`），不持行下标。

## 3. 栈语义

- **深度 50**（`kMaxDepth`，D5.3）：超限弹底（最老命令成既成事实）。
- **相邻合并**（D5.3）：栈顶与入栈命令 `id()` 相同且 `mergeWith` 接受
  → 合并为一条（TagCmd 同资产同标签同向演示；跨对象操作不合并）。
- **redo 支路**：新 push 清空 redo 栈（标准分支语义）。

## 4. 入口与反馈

- Ctrl+Z / Ctrl+Y（列表侧 QShortcut）+ `dataUndoButton`/`dataRedoButton`
  （工具行）。
- **按钮/菜单文案带操作名**（D5.2）：「撤销 打标签 ast-free「核心」」
  （150px 截断，tooltip 全文）。
- **状态栏确认**（D5.4）：`statusMessage("已撤销：…")`（DataPage 转发，
  壳接状态栏；测试 spy 断言）。
- 一次 undo/redo 后 `refreshAssetTable` + `entityRefreshRequested`。

## 5. 不可撤销操作（D5.5）

- **角色变更**（addLink 新角色，无删除对偶）：确认对话框明示
  「此操作不可撤销」。
- **清空可回收清单**：二次确认明示「记录将不可恢复」。
- 均走模态显式确认（方向64 起经 `PaleoNotify::ask` 收口，底层仍为 QMessageBox，按钮/默认键不变），文案说明原因。

## 6. 会话清空（D5.6）

catalog 路径变化（开新工程/重开）→ `DataOpsUndoStack::clear()` +
`OperationsHistory::clear()`（旧工程的命令对新 catalog 无意义）。
`dataops_d5_stackClearsOnCatalogSessionChange` 断言。

## 7. 与 T28 undo vault 的关系

既有 per-asset 挂接撤销（`.paleo/undo_stack.json`，跨 open() 持久 +
note 记忆 + 降级主关联恢复）**原样保留**——行内「撤销」按钮与既有测试
不动。P3 命令栈是**会话级**全局栈（Ctrl+Z 驱动），两者对同一
attachLink/setLinkUnresolved 底座操作，不冲突（vault 记录在 attach 时
落档，命令栈撤销走的也是 setLinkUnresolved，vault 副作用清洗逻辑
`loadUndoVault` 已按链接在场性过滤）。

## 8. 操作历史（D4.10）

`OperationsHistory`（会话内环形缓冲 100 条，最新在前）：批量操作、
CRUD、角色变更经 `m_history->push(文案)`；属性面板「历史」按钮开
`OperationsHistoryDialog`（审计面——不回放）。
