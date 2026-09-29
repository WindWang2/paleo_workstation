# 剖面 2D（SECTION）

> wave/seismic-chain-deep · Phase 2 交付（D2.1–D2.14）
> 组件：`src/ui/seismicsection/seismicsectioncanvas.{h,cpp}`（画布）、
> `seismicsectiondockwidget.{h,cpp}`（dock：工具栏+显示控制行+状态栏）、
> `seismicpickpanel.{h,cpp}`（解释面板，见 INTERPRETATION.md）

## 1. 渲染管线

```
SgySliceImage(values float, NaN=缺失)
  └─ rebuildDisplayValues()      极性/增益曲线/AGC（D2.3/D2.4；空=直通）
  └─ rebuildImage() → paintValueRegion()
       LUT[256]（colormap×反转, D2.8） + 阈值压零（D2.3）
       → QImage(采样分辨率) → 纹理缓存 LRU≤4（D2.1）
  └─ paintEvent
       zoomY<1 → lodImage() 步长抽稀带内 max|v|（D2.6，保幅保事件）
       密度/波形/混合三模（D2.2）；wiggle=正相位涂黑+中线（paintWiggleOverlay）
       卷帘 A/B 分区（D2.10）→ 井层/解释叠加 → 刻度/色标
```

NaN 恒 `#303131` 深灰（不冒充零振幅）——贯穿 2D/3D/回退件。

## 2. 各交付项落点

| 项 | API / 行为 |
|---|---|
| D2.1 纹理缓存 | `(内容指纹^增益^色标^阈值^极性^AGC)` 键控 LRU≤4；`setSectionData` 命中即出（<25ms 断言） |
| D2.2 三模 | `SectionDisplayMode::Density/WiggleVA/Mixed`（混合=密度 0.42 淡显+全强波形） |
| D2.3 阈值/极性 | `setAmplitudeThreshold(0..0.95)` 归一化阈值压噪底；`setPolarityInverted` 显示值取反 |
| D2.4 AGC | `setAgcEnabled(on, windowMs)`：道内滑动窗 RMS 归一（前缀和 O(n)），截幅 ±5×；增益曲线 `setGainCurve`（TWT→倍数分段线性，文本编辑器）；`displayValueAt` 公共查询 |
| D2.5 双刻度 | `setDualScaleEnabled`：左 TWT + 右缘深度轴（`TimeDepthModel` 反算贴准；无效模型自动隐藏；右缘加宽 32px） |
| D2.6 帧预算 | LOD 抽稀路径平移 2.2ms/帧实测（预算 16ms） |
| D2.7 纵向拉伸 | `setVerticalExaggeration(0.1..20x)`（视口中心锚定缩放） |
| D2.8 色标 | 8 预设 + `setColorMapInverted`；色标条渐变直采 LUT（全同步） |
| D2.9 导出 | `exportPng(path, scale=2)`（含坐标轴/色标/井层） |
| D2.10 卷帘 | `setCompareData/setCurtainPos`；B=相邻线异步提取；画布内拖分割线（±7px 命中，SplitHCursor）；A/B 标签 |
| D2.11 道头卡 | `SeismicTaskService::readTraceHeader(sgy, traceIndex)` 静态解码 240B（INLINE/CROSSLINE/field record/CDP ensemble/CDP XY 含比例因子/ns/dt/文件偏移）；点击道弹非模态卡 |
| D2.12 书签 | `SectionBookmark`（线号+视口）QSettings 按体身份持久化；addBookmark/applyBookmark/removeBookmark |
| D2.13 打印/复制 | grabCanvasImage(2x) → 剪贴板 / QPrintDialog 等比居中 |
| D2.14 原因态 | `setNoDataReason`：提取失败/空切片显示可读原因（如「该线无有效地震道」）而非空白 |

## 3. 交互

- 左键拖：平移（帘线附近=拖帘；解释模式=拾取/断层，见 INTERPRETATION.md）
- 滚轮：双轴同因子缩放（锚点=光标）；`1:1`=fitToWindow×vExag
- 双击：适应窗口；悬停：十字丝+状态栏读数（道/TWT/深度/振幅/XY）
- Ctrl+点击拖（3D 视口联动）：见 3D.md D3.2

## 4. 性能预算（tst_seismic_sectionui / budgets）

- 纹理缓存命中 <25ms；LOD 平移 2.2ms/帧（<16ms 预算）
- 220MB 体直读切片 27ms 冷 / 26ms 热（<500/50ms，D6.1）

## 5. 复现

```bash
QT_QPA_PLATFORM=offscreen ./build/tst_seismic_sectionui   # 16 用例
```
