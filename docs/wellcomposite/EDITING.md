# 编辑套件指南（EDITING）

> wave/wellcomposite-deep D3.1–D3.15。核心纪律：**RAW 源数据永不被写**；
> 一切编辑进工作副本 + undo 栈；落盘形态 = 派生版本文档（catalog 版本链由壳接）
> + sidecar（指派/覆盖层/钉注等轻量状态）。

## 1. 组件分层

```
EditStack     undo/redo 栈（命令闭包，深 50，脏状态跟踪）   D3.8/D3.9
EditSession   工作文档 + 全部编辑操作 + 审计 + mtime 冲突    D3.1–D3.3/D3.10/D3.11
TopsEditor    CSV/剪贴板解析 + 预校验 + 冲突报告            D3.7
IntervalEditor 岩性/相区间编辑对话框 + 合并/拆分 + 三级校验  D3.4/D3.5/D3.13
StratAssign   地层单元显式指派（系/统/组/段）               D3.6
Panel         编辑模式开关/工具条/保存入口（视图编排）       D3.14/D3.9
io 层         writeComprehensiveWellXml（派生序列化+审计表） D3.3/D3.11
```

## 2. 操作清单（每个都是可撤销命令）

| 操作 | API（EditSession） | 审计码 |
|---|---|---|
| 移动标志层 | `moveMarker(name, depth)` | marker.move |
| 重命名 | `renameMarker(old, new)` | marker.rename |
| 插入 | `insertMarker(name, depth)` | marker.insert |
| 删除 | `removeMarker(name)` | marker.delete |
| 批量导入 | `applyBatchMarkers(rows)` | marker.batch |
| 岩性区间编辑/删/增 | `editLithoInterval/removeLithoInterval/appendLithoInterval` | litho.* |
| 合并相邻同岩性 | `mergeAdjacentLithoIntervals()` | litho.merge |
| 按界线拆分 | `splitLithoInterval(idx, depth)` | litho.split |
| 相区间编辑 | `editFaciesInterval(idx, fi)` | facies.edit |
| 地层指派应用 | `applyStratAssignments(assigns)` | strat.assign |

## 3. 批量 TOPs 导入（D3.7）

文本格式 `name,top[,base]`（逗号/Tab 混认；表头行自动跳过）：

```
T50,1900,1950
T60,2000
```

流程：`TopsEditor::parseImportText` → `validateAgainst(既有标志层)` →
报告（行级错误 + 同名深度冲突 + 导入内重复）→ 确认后
`applyBatchMarkers`（同名不同深=移动覆盖，新名=插入，非法行拒绝并计数）。

## 4. 三级联动校验（D3.5，微相⊂亚相⊂相）

`IntervalEditor::validateFaciesIntervals`：
1. 基本区间：底 > 顶。
2. 层级完备：有微相必有亚相与相；有亚相必有相。
3. 嵌套不越界：每个微相区间必须完整落在某段**同名**亚相区间内
   （同名亚相被切片为多段时任一段包住即合法）。

岩性侧 `validateLithoIntervals`：底>顶、非空名、排序后无重叠。

## 5. 地层指派（D3.6，程序不猜）

`StratAssign::unrecognizedLayerNames` 找出系/统为空的层名 →
`StratAssignmentDialog`（级联下拉：系→统查年代色标表，组/段可自由输入）→
存 sidecar `stratAssignments` → 显示时 `applyAssignments` 只填空不覆盖已识别。

## 6. 冲突检测与只读降级

- **D3.10**：`EditSession::setSourceMtime(源 mtime)`；编辑前
  `checkSourceConflict(当前 mtime)` → 变化即发 `sourceConflictDetected`
  （面板弹重载/分叉选择）。
- **D3.15**：`setReadOnly(true, 原因)` 后全部编辑 API 返回 false；
  面板「TOPs 编辑」按钮拒绝进入并显示原因（RAW 资产/未授权路径）。

## 7. 保存与派生版本（D3.3/D3.11/D3.9）

`WellCompositePanel::saveDerived()`：
1. `buildDerivedDocument()` 产出编辑后的完整文档；
2. 发 `derivedDocumentReady(doc, manifest 摘要)` 意图信号——**视图不写工程目录**，
   壳/测试负责落 catalog DERIVED 版本（测试链路：信号 →
   `io::writeComprehensiveWellXmlFile` → 重解析等价断言，见
   tst_wellcomposite_editing::testIoRoundTripDerivedChain）；
3. manifest 摘要 = `well= / source= / operations= / 逐条时间|操作|细节`；
   审计同时写入派生 XML「编辑审计」工作表与 sidecar auditLog；
4. `markSaved()` 清脏状态（未保存关闭 → 脏状态对话框由壳按 isDirty 弹）。
