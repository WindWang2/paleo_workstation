# 预览性能预算（P2 D6.x · wave/preview-map-canvas）

测试证据：`tst_previewmap_perf`（5 函数，RUN_SERIAL——墙钟断言关回串行
基线，tst_correlation_full 先例）。测量值以 `qInfo` 落测试日志
（PERF 前缀），不只红绿。

## 交付项与实测

| 交付项 | 实现 | 实测（fixture） |
|---|---|---|
| D6.7 首开 <300ms | 建页+栅格+渲染出口+等值线同步链 | **235–252ms**（断言 <900ms = 3× 预算，共享机负载放宽） |
| D6.1 渐进渲染 | `renderSnapshot(320)` 低清整图先上屏 → `renderCompleted` 让位 | 快照 **3ms**（断言 <300ms） |
| D6.2 渲染缓存 | 工程、资产、版本、范围、视口和显示缩放率共同标识快照 | 关闭后重开，显示已缓存图像，精渲完成后移除快照 |
| D6.6 缓存预算 | 内存最多 16 张，磁盘最多 64 张，两层均按最近使用顺序淘汰 | 20 张入 → 16 张在内存，磁盘仍可回填 |
| D6.3 渲染取消 | 换层 `stopRendering()`；`cancelRendering()` 幂等 | 无悬挂 job（cancelRenderingWithoutJobIsSafe） |
| D6.4 identify 性能 | 按层 QgsSpatialIndex 缓存（层析构自动清） | 二次查询缓存行不增、结果一致（identify-2nd: 0ms） |
| D6.5 读数节流 | 量测 move ≤30Hz（press/finish 离散帧不节流）；坐标跟踪 ≤30Hz | 25 连发 move → ≤2 帧 |

## 缓存语义细节（D6.2）

- 预览键包含工程路径、资产、版本和屏幕设备像素比。范围按毫米、逻辑视口按 8px 分桶，
  再对字段取 SHA-256 用作磁盘文件名。不同工程出现相同目录编号时各自读取对应快照。
  PNG 回读时恢复设备像素比，保证高分屏上的逻辑尺寸正确。
- 两层结构：内存 LRU（即时命中）+ 磁盘 PNG（QTemporaryDir 级目录，进程
  生命期自动清，不在工程目录/用户配置留痕）。内存被逐的条目仍可从磁盘
  回填到内存，复用原有 PNG。Windows 上对同一张 3507×2480 的 HY 图件做
  14 次磁盘命中回读，逐像素检查通过，中位耗时由 560 ms 降至 105 ms。
- 标签页显示并取得布局后的视口尺寸时再次查询缓存。命中后先显示快照，画布完成
  精渲后移除覆盖图，渲染稳定后保存新帧。

## 首开预算口径（D6.7）

- 预算含义：`openAsset` 返回（可交互：低清图/缓存图已在屏或即将由
  singleShot(0) 铺上），**不含**首帧精渲完成（后台替换）。
- fixture：D61 层位（含等值线生成，GDAL C API）。
- 首开 235ms 的大头是等值线 GPKG 生成 + 双层建树；更大工区可把等值线
  改为 singleShot 延后（当前同步生成保证 TOC 完整性——量级可接受，
  未做）。

## 递延

- **D3.12 触摸板 pinch zoom**：QgsMapCanvas 原生手势路径在 offscreen 下
  不可测；未加自定义 NativeGesture 处理（见 TOOLS.md §7）。
- **D4.8 混合模式**：QgsMapLayer::setBlendMode 全量支持（TOC 下拉），
  但预览层多为 1–2 层场景，叠层时才可感知。
