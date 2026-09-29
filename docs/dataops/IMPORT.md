# 数据管理页导入流增强（P3 交付文档）

日期：2026-09-29。分支：`wave/data-page-operations`。
实现：`dataops/dataopsimportlogic.h`（纯逻辑）+ `dataopsimportui.h`
（队列面板/报告/预设/查重/预估对话框）+ datalist 接线。

## 1. 架构位

导入的**重活**（扫描/解析/入库）在功能层（`workflow/folderimport` +
`dialogs/folderconfirm`，W2 既有）——本波在**视图层**补齐队列/预设/
查重/预估/报告五个 UI 面；队列执行器是**注入钩子**（`ImportQueuePanel::
setRunner`），生产接线点见 GAPS.md。

## 2. 逐文件进度 + 可取消单项（D8.1）

`ImportQueuePanel`（`importQueuePanel`）：

- 表四列：文件/状态/进度（QProgressBar cellWidget）/操作。
- 状态机七态：排队/导入中/完成/失败/跳过/已取消/等待重试
  （`ImportQueueItem::stateText`）。
- 行级操作：进行中→「取消」；失败/已取消→「重试」+「跳过」。
- 队列头部：计数（N 项 · 待处理 M）/「摘要」/「清已完成」/收起。
- 入口：外部文件拖入（D3.2）或 `enqueuePaths(paths, typeByExt)`。

## 3. 失败重试队列（D8.2）

`ImportRetryQueue` 状态机：

- `markFailed`：自动重试额度（默认 2）未尽 → **RetryWait**（1.5s 定时器
  `promoteRetryWaiters` 驱动回 Queued）；额度尽 → Failed。
- 手动：`retryItem`（Failed/Canceled/Skipped → Queued）、`cancelItem`。
- Done/Canceled 不可再取消；`clearFinished` 清已终结行。

## 4. 导入预设（D8.3）

QSettings `dataops/importPresets`：名称 → `{dirs: [...], typeMap:
{扩展名: 类型}}`。`ImportPresetDialog`（`importPresetDialog`）列表展示
（目录数/类型映射），保存=当前设置入预设，删除=选中预设删除。
拖入文件时按预设 typeMap 预填队列条目类型。

## 5. 重复检测（D8.4）

`detectDuplicates(cat, paths)`：双口径——

- **同名**：displayName（文件 basename）与库内资产撞名（一次建索引）。
- **SHA-256**：流式哈希后 `versionBySha256` 反查（读不动的文件只走同名
  口径）。

`DuplicateDialog`（`duplicateDialog`）：命中表（新文件/命中类型/处置
下拉）+ 三个一键「全部跳过/全部重命名/全部作新版本」；处置三值
`Skip`/`Rename`/`Derive`（覆盖→派生新版本）。

## 6. 导入摘要报告（D8.5）

`ImportRetryQueue::summaryText`：「共 N 项 — 成功 X、失败 Y、跳过 Z、
取消 W」+ 逐条失败明细（路径 + 原因）。`ImportReportDialog`
（`importReportDialog`）只读展示 + 「复制」进剪贴板。

## 7. 大目录预估与分批确认（D8.6）

`estimateDirectory(dir)`：递归计数/总字节/扩展名分布（"las:5, sgy:1"）/
>100MB 大文件数；5 万文件扫描上限（防百万文件目录，`canceled` 标记）。
`ImportEstimateDialog`（`importEstimateDialog`）：预估文案 +
「一次全部导入」/「分批导入（每批 200）」选择——确认后才发
`externalImportRequested(paths)` 给壳的 FolderImport 流。

## 8. 触发路径

```
外部拖入（D3.2）→ DataNavTree::externalFilesDropped
  → DataListPanel::handleExternalFiles
      ├─ 单文件/少量 → 直接 externalImportRequested(paths)
      └─ 目录或 >20 文件 → ImportEstimateDialog → 确认 → 同上
  → ImportQueuePanel::enqueuePaths（登记 + 预设类型预填）
  → 壳接 externalImportRequested → FolderImport 确认表（T22，复用）
```

导入按钮（`importWells` 等）路径不变——壳的既有编排。
