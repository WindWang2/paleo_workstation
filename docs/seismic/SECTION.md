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
ctest --test-dir build -R tst_seismic_sectionui --output-on-failure  # 16 用例
```

## 6. 三后端渐进策略复核（wave/deepen-perf A3）

剖面/时间片取数在 sf3c / sf3p / 直读三后端上的空态、取消、回落语义对照
（改了什么 / 为何不用改，逐项可追溯）：

| 链路 | 后端 | 空态 | 取消/顶替 | 回落 | 结论 |
|---|---|---|---|---|---|
| 数据页时间切片（瓦片通道） | sf3p | NaN 灰底图（未到瓦片不冒充零振幅）；**失败 → clearData + 原因态（本次新增，改前留整幅灰无解释）** | **同路径新请求启动即取消旧在途（服务级 `inFlightTiledTasks_`，本次新增）** + 采样号世代过滤（原有） | 显式 paged 失败如实上报不回落（契约） | 已改（datapreviewtabs + seismictaskservice） |
| 数据页时间切片（整图通道） | 直读 / sf3c | **失败 → clearData + 原因态（本次新增，改前静默留旧图冒充新采样）** | 120ms 防抖 + 世代过滤（原有）；服务级 startSliceExtraction 不做跨消费方取代——3D 面板与数据页各自持有在途语义，服务层再加一层会双杀（记档不改） | Auto 通道引擎失败 → volume 直读保底（服务层既有） | 已改（空态）；取消维持消费方治理 |
| 3D 三槽切片 | 三后端（pagedPath_ 透传） | 失败 QgsMessageLog 告警（视口保留旧帧/包围盒；GL 失败另有 2D 回退件）——3D 无文字原因态面，视觉语义由回退件承担（记档不改） | **槽位在途 → 新值 pending + requestCancel 协作中止（本次新增）；本端主动取消免告警（本次新增）** | 同上 Auto 保底 | 已改（A2） |
| 剖面 dock 任意线 | 三后端 | D2.14 原因态（失败/空线两分支，既有） | m_extraction->requestCancel() + generation 守卫（既有） | ReadSection useReadPlan → 插值模式回落 legacy BuildLineSection（既有） | 无需改 |
| 剖面 dock IL/XL/Time 切换 | 三后端（经 SeismicTaskService） | D2.14 原因态（既有，迁移后语义不变） | **同型同号在途去抖 + 新请求 requestCancel 顶替（worker 逐线检查点退出，不再占闸）；回调按「世代号+请求号」守卫——被顶替/取消的旧结果静默丢弃（cancelled ≠ failed，不弹误导错误）** | Auto 通道引擎失败 → volume 直读保底（服务层既有）；切片进共享 LRU（换线回头即命中） | **已迁（goal/seismic-runtime-closure）**：TODOS P2 裸 QThreadPool 双轨遗留收口；切体/任意线双向取消对端在途任务（setVolume/extractSectionFromVolumeAsync/extractSliceAsync 互顶替），进度条改任务字节进度驱动 |
| 剖面 dock 卷帘 B 图（D2.10） | 三后端（同上通道） | 失败 → setCompareData 空 + 原因文案（迁移新增；原裸路径静默吞 err） | 新相邻线请求顶替旧在途（requestCancel + 请求号守卫） | 同上；与主切片共用 LRU | **已迁（goal/seismic-runtime-closure）** |
| 3D 堆叠层（D3.1） | sf3p（体窗合并）/ 其余逐层 | 失败回落逐层切片 + 告警日志（本次新增路径）；取消态不回落 | 单请求单段（体窗 ReadBox 无瓦片回调，粒度=整窗，记档） | paged 预算超限/引擎失败 → 逐层切片（如实、留痕） | 已改（A1） |
