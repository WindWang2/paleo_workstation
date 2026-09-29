# 多井对比指南（CORRELATION）

> wave/wellcomposite-deep D5.1–D5.7 + D2.9 视口同步锁 + D1.12 Y 缩放联动。

## 1. 布局

`MultiWellView` 容器三种模式：`Single`（1 槽）/ `Dual`（1×2 并排，D5.1）/
`Quad`（2×2，D5.2）。槽位经 `setPanel(slot, panel)` 装配
`WellCompositePanel`；空槽显示占位提示。

## 2. 锁步滚动（D5.1 = D2.9 视口同步锁 = D1.12 多道 Y 联动的多井语义）

`setLinkScroll(true)`（缺省开）：任一井深度滚动广播视口顶深到其它井
（防重入守卫）。关闭后各井独立。单画布内的多道始终共享统一深度轴（标尺道
即坐标源），联动开关作用于多画布层。

## 3. 同名标志层连线（D5.3）

`setShowCorrelationLines(true)`（缺省开）：透明 overlay 在相邻井画布之间
绘制同名标志层直线（琥珀点划线）。数据面 `correlationPairs()` 返回
相邻井同名标志层对（测试/工具可查）。仅计算当前布局容量内的槽位。

## 4. 基准面校平（D5.4）

`alignToDatum(markerName)`：各井滚动使其该标志层对齐到视口 40% 高度
（深度不同的同名标志层屏幕上等高 → correlation 线拉平）。校平期间临时
断开锁步广播（否则后设的井会把先设的拉回去）。`clearDatum()` 退出。
`datumMarker()` 查询当前基准面。

## 5. 井选择器（D5.5）

`WellSelectionDialog`：复选列表，测区井（蓝 `#1B73D0`）与参考井
（琥珀 `#92400E`）分区着色（D7.10 语义与单井面板徽章一致）。
`MultiWellView::setAvailableWells/setSelectedWells` 承接结果。

## 6. 对比模板（D5.6）

井集 + 道集（TrackSpec 列表）+ 锁步开关存取 sidecar
`comparisonTemplates`（`WellCompositeStore`）。模板恢复 = 按 wells 建
面板 + 按模板 specs 装配道。

## 7. 厚度差表（D5.7）

`deltaTableText()`：行 = 相邻标志层段（T30->T35…按井内标志层序列），
列 = 各井段厚（m，同名标志层深度差）；缺失段「—」。TSV 格式可直接贴表。
少于两口井时为空。

## 8. 测试锚点

`tst_wellcomposite_multiwell`：布局/槽位/锁步开合/correlation 对/
datum 等高断言/选择器勾选语义/差表数值/模板 sidecar 往返。
