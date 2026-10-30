# 证据面板：宽度存储按工程作用域 + 画布按需创建（#745、#746）

日期：2026-10-31
分支：`fix/evidence-panel-store`（堆叠在 `fix/gate-and-stratigraphy-honesty` / PR #831 之上）
范围：`src/ui/pages/mappingreferencepanel.*`、`tests/tst_mappingworkbench.cpp`

## 改动

| Issue | 改动 | 结果 |
|---|---|---|
| #745 | 宽度键由全局 `mappingEvidence/viewWidths` 改为 `mappingEvidence/viewWidths/<工程文件 SHA1>`（无工程时 `unscoped`）；`removeView` 删除对应序号并立即 `saveWidths()` 落盘 | 关闭：删中间视图后宽度不再左移套用；A 工程布局不再影响 B 工程 |
| #746 | 证据画布延迟到首次 `setReference()` 才创建（`ensureView()`）；创建前到达的联动/激活状态补应用；新增 `viewCreated(QWidget*)` 信号让宿主播装滚轮过滤器 | 部分：默认 4 个空视图不再常驻 4 套 `QgsMapCanvas` + 装饰器；「默认视图数按视口宽度」仍待产品裁决（见下） |

## 证据

- `tst_mappingworkbench evidenceViewWidthsFollowProjectAndDeletion`（新增）：
  同工程两个面板沿用 333 宽度；删除该视图后新建面板不再出现 333；换成另一工程的面板也不出现。
- `tst_mappingworkbench evidenceViewsCreateCanvasLazily`（新增）：4 个空视图
  `canvas()==nullptr` 且 `referenceCanvas` 数量为 0；首个视图选源后只有它创建画布，
  其余仍为 0。
- 既有用例：`evidenceViewWidthsAreAdjustableAndDriveVisibleCount`（宽度键已同步新格式）、
  `evidencePanelAddsViewsAndPreservesOtherSelections`、
  `evidenceViewsStayInOneRowWhenResizedAndScrollWithWheel`（滚轮横向滚动依赖新装的过滤器）、
  `referenceViewSelectsSourcesAndKeepsPinnedVersion`、
  `referenceLegendFollowsPinnedLevelAndClearsOnSourceChange`、`referenceCanvasHonoursClassPins`
  全部通过（7 passed）。
- 源码闸门：`layering --strict/--transitive`、`ui_invariants --strict`、`ui_size --strict`、
  `file_budget --strict`、`docs_drift` 均退出码 0。

## 未做（#746 剩余部分）

「默认视图数按视口宽度取 `max(1, viewport/kMinViewWidth)`」与现有测试口径冲突：
`tests/tst_ui.cpp:2121`、`tests/tst_mappingworkbench.cpp:3012` 都断言默认 4 视图，
且默认 4 视图是 #716 的产品决定。是否改成随停靠栏宽度自适应（含窄栏首屏不横滚）
需要产品确认；本轮只解决「空视图常驻画布」的资源问题，不动默认数量与最小宽度。

## 备注

- 宽度键复用同一套 SHA1 派生逻辑在测试里重算一次（`TestMappingWorkbench::evidenceWidthsKey`），
  测试用独立临时工程路径隔离，不会跨用例或跨机器污染。
- `tst_ui::mappingWorkbenchCanvasRibbonAndReferences` 在本机仍在更早的
  `canvas()->layers().contains(...)` 断言处失败（既有环境问题），其后的参考证据断言
  无法在本机验证；参考面板行为由上述 `tst_mappingworkbench` 用例覆盖。
