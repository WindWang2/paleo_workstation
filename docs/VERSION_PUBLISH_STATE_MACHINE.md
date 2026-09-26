# 保存/发布状态机规格（VERSION_PUBLISH_STATE_MACHINE）

> 集中化 `PALEO_QGIS_PLAN.md` §41.7（保存/发布语义）、§34（统一 Undo / 编辑会话
> 模型，含 L1223 版本提交边界）与 `PROJECT_AREA_PLAN.md` 阶段E（L177 发布门、
> L260 PDF 产物继承、pass-2 D10 数值残差下限、D9 硬门覆盖 warn-not-disable）。
> 每条规格标注「现状」＝当前实现（文件/测试可查证）；不一致处进 §10 缺口清单。

## 1. 目的与范围

层位（horizon）地图从编辑到成品的生命周期由三个动作驱动：**保存版本**、
**导出 PDF**、**发布**。本文定义三者的状态机、门条件、快照语义与 undo 边界，
使「这份已发布的图到底从哪版数据来」始终可答。

作用域：每层位一条版本链（`map_versions` 按 horizon 分链，版本号层位内递增）。
栅格/矢量图层声明（`LayerManifest`）是层位工作副本的一部分；catalog 资产
版本（RAW/DERIVED）是数据侧的血缘，不在本状态机内但被 provenance 引用。

## 2. 状态与生命周期

```
            保存版本（commit + 版本号递增）
  (无版本) ──────────────────────────────►  Editing vN
                                                │  ▲
                              继续编辑 + 保存版本 │  │（vN+1，新行）
                                                ▼  │
                                           Published vN
                                                │
             发布（G1–G8 全过 → result/<h>/vN/ 快照，只读）
```

- 每条版本行只有两个状态：`Editing` / `Published`。
- **发布后可以再编辑**：继续工作并「保存版本」产生 v(N+1)（state=Editing），
  已发布快照 **永不回写**（字节级只读，见 §6）。
- 同一版本行 **不能重复发布**（门 G3）：再次发布必须先保存新版本——这保证
  「Published 行 = 一个确定的内容快照」。
- 旧 schema 行（无 `pdf_asset_id` 列值的 `Published`）**读侧归一为 Editing**，
  永不回写（§9）。

## 3. 保存版本（saveVersion）

**规格**
1. 「保存版本」= 编辑会话 commit → 版本号递增 + provenance 记录（§41.7）。
2. commit 边界（§1223）：该层位全部矢量图层逐一 `commitChanges()`；commit 后
   undo 栈清空，**undo 不可跨越版本边界**；栅格图层无编辑缓冲，跳过。
3. 版本行继承「最近一次登记的 PDF 产物引用」（asset id + sha256，L260）——
   导出动作本身不改已存在的版本行；下一次保存才把产物引用抄进新行。
   因此合法次序是 **导出 → 保存 → 发布**。
4. 图层清单读失败时不得出版本（未提交编辑悬空时禁止递增版本号）。

**现状**：`MapVersionController::saveVersion`（`src/workflow/mapversioncontroller.cpp`）
逐层 commit + `undoStack()->clear()` 兜底；`MapVersionStore::saveVersion`
（`src/metadata/mapversionstore.cpp`）插行 `state='Editing'`、从 `map_products`
最近一条 `kind='pdf'` 行抄产物引用。清单读失败 → 空版本 + 错误文案。
测试：`tst_versions.cpp::saveVersionCommitsAndClearsUndo`、
`manifestReadFailureFailsVersionOps`。

## 4. 导出 PDF（export）

**规格**
1. 层位图导出为 PDF 后登记 catalog **OUTPUT 资产**（受管路径 + SHA-256）；
   登记失败的导出不算完成——发布门要求的是「已登记的 PDF」，不是「写出过文件」。
2. 产物再记入版本存储的 `map_products` 行（horizon、path、asset_id、sha256）。
3. 导出不产生版本、不改版本状态。

**现状**：`registerMapPdfAsset`（`src/workflow/mapexport.cpp`）+
`MapVersionStore::recordLayoutProduct`；UI 侧 `paleomainwindow.cpp`
exportPdfRequested（失败重试对话，§215）。测试：`tst_mapping.cpp::exportPdfWritesD61Map`。

## 5. 发布门（publish gate）

按 `MapVersionStore::publish` 的判定顺序（先拒先报）：

| # | 门 | 拒绝文案要点 | 现状 |
|---|---|---|---|
| G1 | meta 库可开 | sqlite 错误 | `ensureOpen` |
| G2 | 有版本（version>0） | 「还没有保存的版本」 | ✓ |
| G3 | 最新版本非 Published | 「已发布 — 保存新版本后再发布」 | ✓ |
| G4 | 版本行有 pdf_asset_id + pdf_sha256 | 「没有 PDF 资产记录 — 导出 PDF 后再保存」 | ✓（§3.3 继承机制） |
| G5 | 残差摘要完备：wells_total>0 且 covered==total 且 missing 为空 | 「还有 N/M 口井没有残差或原因 — 先在验证页运行验证」 | `residualSummaryComplete` |
| G6 | **数值残差下限（D10）**：rows[] 中 kind=="residual" 的行数 ≥ requiredNumericResiduals(wells_total) | 「数值残差只有 X/T 口（发布需 ≥R）— 补齐井分层/时深/测网覆盖」 | ✓（本包新增） |
| G7 | 图层清单可读（controller 层） | 清单读失败原文 | `tryDeclared` |
| G8 | 版本行绑定的 PDF 文件在盘且能进快照 | 「发布的 PDF 文件已丢失」 | ✓ |

**D10 参数**：批准记录（PROJECT_AREA_PLAN L1077）「≥15/20 numeric」——真工区
20 口井对应绝对 15；小工区按同一比例 3/4 向下取整、至少 1 口
（`requiredNumericResiduals`）。**这是对 PALEO_QGIS_PLAN「warn-not-disable」
指导的刻意硬门覆盖**（pass-2 D9 决议，成图未达井控密度不许出厂）。

**现状**：G1–G8 全部实现于 `MapVersionStore::publish` +
`MapVersionController::publish`。测试：`tst_versions.cpp::publishGateSnapshotAndImmutability`
（G2/G4/G5）、`publishGateNumericResidualFloor`（G6：14/20 拒、15/20 过）。

## 6. 发布动作与快照

**规格**
1. 快照目录 `<工程目录>/result/<horizon>/v<N>/`：该层位全部声明的文件图层拷贝
   + 绑定的 PDF（+无资产归属的旧产物行，兼容）。
2. 快照内文件一律去写位（444 只读）——Published 是字节级不可变。
3. 版本行落 `state='Published'` + `published_path` + `residual_summary`
   （发布时冻结的逐井「残差或原因」JSON——发布后井数据再变不影响本次发布的
   证据快照）。
4. 快照失败（目录建不出、文件拷不动、绑定 PDF 丢失）→ 发布整体失败，版本行
   不动（不留半发布态）。

**现状**：`MapVersionStore::publish` 末段（`copyReadOnly`）；测试同 §5，另断言
v2 快照在发布 v3 后字节不变且仍只读。

## 7. 发布后再编辑 / 工作副本发散规则

**规格**
- 已发布快照是历史事实；工作副本（图层声明 + 编辑缓冲）继续演化。
- 发散的显式标记 = **新版本行**：保存 v(N+1) 后 `isPublished()` 回到 false，
  UI 版本态回到「v(N+1) 未发布」。
- 已发布版本的再生成只能通过「新版本 + 再发布」产生新快照 v(N+1)/，不覆盖。

**现状**：如上实现；UI `setVersionState(version, published)` 反映。缺口见 G-04
（无层粒度 dirty 标记，版本号是唯一发散信号）。

## 8. Undo 边界（§34 桥接）

**规格**
- 会话级撤销：编辑会话内顶点/要素修改进 `QgsVectorLayer` undo 栈，Ctrl+Z 会话内有效。
- 版本提交边界：「保存版本」= commit → 版本递增；commit 清 undo 栈，undo 不跨版本边界。
- 放弃会话：roll back buffer，不产生新版本。
- 版本回退：从历史版本重新检出为新的工作副本（衍生新版本），不是 undo 回放。
- UI 语义：工具栏撤销只作用于未提交编辑；版本历史面板只作用于已提交版本。

**现状**：会话撤销 + 提交边界由 `saveVersion` 落实（commit 原生清栈 + `clear()`
兜底，测试断言 undoStack()->count()==0）。放弃会话依赖 QGIS 编辑工具原生
rollback，版本体系无钩子（缺口 G-02）；版本回退未实现（缺口 G-01）。

## 9. 持久化与 schema 迁移

- 表建在与 `LayerManifest` 同一工程 meta sqlite（`<qgz>.project.sqlite`）：
  `map_versions(id, horizon, version, provenance, state, published_path,
  created_utc, pdf_asset_id, pdf_sha256, residual_summary)`、
  `map_products(id, horizon, kind, path, created_utc, asset_id, sha256)`。
- 迁移：`CREATE TABLE IF NOT EXISTS` + 逐列 `PRAGMA table_info` 补列（可空）；
  旧行读成未发布且**永不回写**（读侧归一）。测试：
  `tst_versions.cpp::oldSchemaRowsLoadUnpublished`。
- 版本行不存内容快照本体——本体在 `result/` 与图层声明源文件；行只存引用与
  冻结证据（PDF 资产、残差摘要）。

## 10. 现状 vs 规格缺口清单

| # | 规格 | 现状 | 差距与建议 |
|---|---|---|---|
| G-01 | 版本回退 = 检出历史版本为新工作副本（§34） | 未实现（无 checkout API/UI） | 快照本体已在 `result/<h>/vN/`，可从快照重建声明集；建议作为独立任务（涉及图层声明重写与编辑冲突语义） |
| G-02 | 「放弃会话」roll back（§34） | QGIS 编辑工具原生支持；版本体系无显式入口/钩子 | 低风险补 UI 入口即可，不动状态机 |
| G-03 | 发布门可解释性（§35）：门条件在按钮态可见 | `refreshPublishGate` 只显示 PDF 资产有无 + 覆盖 N/M；**D10 数值下限不在按钮态/tootip 中**，仅发布被拒时经错误文案可见 | `src/ui` 属 C 包接缝；建议 C 包把 `numericResidualCount/requiredNumericResiduals` 纳入 `setPublishState` 与确认对话（数据口径已就绪） |
| G-04 | 工作副本发散规则 | 版本号递增是唯一发散信号；层粒度「有未提交编辑」不入版本历史 | 可选增强：saveVersion 前检查 `isEditable()`/dirty 层并在 provenance 记录 |
| G-05 | §41.2 写次序（gpkg commit → .qgz 备份 → .qgz 原子写） | 属 `PaleoProjectStore`/工程保存链路，不在 saveVersion 路径上（版本行在 meta sqlite，不触 gpkg/.qgz） | 边界写明：本状态机的「保存版本」不等于「保存工程文件」；两者独立触发 |
| G-06 | 残差摘要冻结 | 发布时冻结 ✓；保存版本不冻结（spec 亦如此） | 无差距，备忘：摘要生成在 `MapVersionController::residualSummaryJson`（发布时实时计算再冻结） |

## 11. 实现索引

| 关注点 | 文件 | 测试 |
|---|---|---|
| 版本行/产物表 | `src/metadata/mapversionstore.{h,cpp}` | `tests/tst_versions.cpp` |
| 保存/发布编排、残差摘要 | `src/workflow/mapversioncontroller.{h,cpp}` | `tests/tst_versions.cpp`、`tst_mapping.cpp` |
| PDF 导出与 OUTPUT 资产登记 | `src/workflow/mapexport.{h,cpp}` | `tests/tst_mapping.cpp::exportPdfWritesD61Map` |
| 发布门 UI 状态 | `src/ui/paleomainwindow.cpp` attachMapping | `tst_panels`/手动 |
| D10 门参数 | `MapVersionStore::requiredNumericResiduals` | `tst_versions.cpp::numericResidualFloorArithmetic` |
