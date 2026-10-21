## 问题

**视图层规模失衡**：`src/ui` 113,101 行占全仓 38.6%，43 个 >1000 行文件、TOP5 巨型文件 4 个在 ui/，且无任何防增长闸。

## 证据（master @ ddd1325e）

模块行数：

| 模块 | 行数 | 占比 |
|---|---|---|
| src/ui | 113,101 | 38.6% |
| src/workflow | 45,645 | 15.6% |
| src/algorithms | 36,704 | 12.5% |
| src/io | 24,830 | 8.5% |
| src/services | 20,701 | 7.1% |
| src/qgis | 18,328 | 6.3% |
| 其余（domain/catalog/ai/metadata/app/linkage/selfcheck） | 34,035 | 11.6% |

巨型文件：

- >1000 行：43 个；>1500 行：5 个 —— `seismic3dviewpanel.cpp` 1811 · `entitypanel.cpp` 1620 · `wellsectionpanel.cpp` 1541 · `mappingworkbenchpage.cpp` 1517 · `segyreader.cpp` 1515（4/5 在 ui/）。
- >800 行：70 个。

## 影响

- AGENTS.md / docs/UI_LAYER_PLAN.md 契约要求「视图只发信号不干活」，体量分布与该契约存在张力；#473 已点名 `attachWorkbench` 1060 行巨型视图函数，本条是其**面上版本**。
- 巨型视图文件是合并冲突震中（TODOS.md「同域高频补丁大文件拆分候选清单」同族结论），也是分层退化的入口。
- 方向 96 / 97 已成功拆掉 constraintpage、datapreviewtabs，但新巨型文件仍在冒出 —— 说明缺的是闸而不是一次性拆分。

## 建议

- 加「UI 单文件 ≤1000 行 / 单函数 ≤200 行」防增长闸，与 #474（check_layering 防增长闸）同族，可并入同一工具与 baseline 机制，口径**只缩不涨**。
- 沿用已验证模式（ctor 编排主文件 + 域 TU + `*_internal.h`，逐字节保留行为）继续拆 TOP5：seismic3dviewpanel、entitypanel、wellsectionpanel、mappingworkbenchpage。
- 闸接线时给出当前 43 个文件的 baseline，避免一次性全红。

## 验证

闸接线且 TOP5 中至少 2 个降到 1000 行以下。

## 关联

同族：#473（attachWorkbench）、#474（防增长闸）；
