# wave/deepen-perf — 域深化 + 性能完善（四轨并行 + lead 集成）

> 分支 `wave/deepen-perf`（base master `a8c009c`，worktree ../pw-deepen）。
> charter：`docs/agent-prompts/x1-deepen-perf.md`。四 worker 并行
>（A 地震链路 / B IO·缓存·目录 / C 编图域 / D 井综合·连井剖面），文件域
> 互斥（cmake/extra-deepen-{a,b,c,d}.cmake 各自挂载零冲突），lead 负责接线、
> 裁决与验收。本文是该 wave 的进度底账；TODOS.md 对账见同批提交。

## 交付一览

| 轨 | 交付 | 新测试 |
|---|---|---|
| A1 | ReadVoxelWindow 消费接线：3D 16 层堆叠取数 16 请求→**1** 体窗任务（paged 预算内单次读窗切层 + 逐层回落含告警留痕）；SBM 四入口消费核账表（ARCHITECTURE §9b）——TODOS P2「SBM Engine 剩余入口」关闭 | engine×2 |
| A2 | 三维拖动链路取数合并：拖动在途请求 supersede 取消、松手精化只补脏槽位、progressive 粗开 350ms 静止自动升 L0；手势切片请求数 5→**3**（交错好时 2） | 3dui×3 + budgets 扩展 |
| A3 | 时间切片页两通道失败原因态 + `startTimeSliceTiled` 同路径新请求即取消旧在途（瓦片粒度协作中止）；sf3c/sf3p/直读三后端复核对照表（SECTION §6） | engine×1 |
| A4 | 966MiB 真工区复测（env 门控 tst_seismic_realarea，源目录逐字节不变断言）：冷开 172–204ms / 暖开 32–43ms / IL 冷 15–16ms / 时间片冷 21–22ms / 64³ 体窗 17–18ms / 任意线 23–28ms / 3D fps 17,143（BASELINE §8） | realarea×2 |
| B1 | 大 LAS `lasAt` UI 线程解阻：correlation 链路 quiet 异步 + per-well 世代号 + 同井协作取消；59MB 调用点阻塞 **442ms→0ms**（解析 369–399ms 池内） | correlation_async 8+1 |
| B2 | 导入队列真进度条：整体进度/ETA/全部取消 + `cancelHook`（取消≠失败不重试）+ `FolderImportQueueAdapter` 生产 runner（GAPS G-2.3 收口，串行驱动 + loud 任务池） | importqueue_progress 7 |
| B3 | 金字塔消费侧：导入 Lazy `ensureRasterPyramids` + GDAL 外部 .ovr（受管 RAW 字节不动）+ 预览大图预热；4096² f32 视口读块 **14ms→4ms**（概览一次性 86–112ms） | pyramid_consume 4 |
| B4 | catalog 100k 夹具评估：打开 136ms(10k)→1408ms(100k，线性)，查询面 entityById/links/列表/计数全 O(1) **零劣化**——ADR 0056 触发条件未达，记档；新发现 mutator 写路径 10k 6.2s→100k 949.7s（疑二次，>50k profiling 递延） | catalog_scale 1（env 门控） |
| B5 | ctest -j2 QSettings 竞态：四轮全量 -j4（127 项）历史竞态点全绿——XDG/HOME 沙箱（add_paleo_test）已根治，TODOS P4 条目关闭 | —（证据在 BASELINE §6） |
| B6 | SEG-Y 坏道跳过放宽：顺序固定步长布局负 ns 坏道跳过（对齐并行/resume D2.7 语义，原整索引报错）；变道长布局保持整索引报错 + 取消不落 checkpoint（resume 契约门）；变道长契约表 INDEX_FORMAT §3 | cache_segyindex +4 |
| C1 | ConstraintIDW typed 语义差分：break_line 贯通屏障（ROI 凸包贡献 + 半格超采样栅格化 + 4-连通 BFS 阻断）、direction_line 各向异性方向场（长度加权倍角合成 θ，d²=u²/r²+v²·r²，ANISO_RATIO=2.0）；无 type/旧词**逐位保持旧行为**；ALGORITHM_AUDIT §3a | algorithm_harness +3 |
| C2 | 相界地质语义单类型跑通（断层切割 fault_cut）：boundary_kind 属性 schema → composepage「相界类型」下拉 → `applyFaciesBoundaryStyle` 断层红粗边；4 类词面冻结（workflow/boundarysemantics.h） | composeworkflow +1 / mappingpages 扩 1 / services2 +2 |
| C3 | 井类别符号全集（Q/HS 1011—2016 表 K.1 十二类）：数据字段驱动分类渲染 + `normalizeWellCategory` 归一；无类别字段回落通用「探井」（外细环+实心盘 0.73 外径比）；壳接线=appcontext 类别字段探测（lead） | services2 +2 |
| C4 | layerId↔assetId 激活：七个产物出口 commit 后盖 `paleoAssetId` + catalog extra `manifest_layer_id` 权威反链 + LayerPropertiesDialog 业务页开闸；`paleoCreatedAt` 基线已存在（核实） | layerproperties +1 / factorworkflow 扩 1 |
| C5 | `paleo:paleo_distance_transform` 绕障距离引擎（welldist 实装）：无屏障=精确欧氏（与 paleo_welldist 输出零容差对拍）、break_line 屏障=8 邻接 Dijkstra；confidence 维持冻结拒绝（ONNX 单输出张量，记档）；strathick 核实主线 6 已接 | algorithm_harness +1 / factorworkflow 翻转 |
| D1 | wellcomposite 壳侧接线：`WellCompositeDerivedSink`（derivedDocumentReady → DerivedAssetRegistrar 受管 DERIVED 登记，sha256/只读/find-or-create；井斜/时深自动喂 DepthTransform）+ attach 分段 + 组装根 io 注入（lead 落 main.cpp） | wellcomposite_shell 6 |
| D2 | 连井剖面生命周期边界穷举（真实壳 AppContext+PaleoMainWindow）：切体/换层位/关工程/重开对话框期间事件；**发现并修 SeismicSectionTool 析构悬空**（橡皮带属 canvas scene，决策①同款——lead 落修复） | sectionlifecycle 9 |
| D3 | 打印原生接线：`exportToPagedDevice(QPagedPaintDevice&)` 共用管线（QPdfWriter/QPrinter 同源，设备 dpi 换算比例尺）+ `nativePrintAvailable()` 探测 + 无打印环境降级 PDF（如实告知） | shell 内 1 用例 |
| D4 | 简化 composer 评估**记档不建**：标准导出流两步全自动（buildHorizonMapLayout A4 横版含标题/图例/比例尺/指北针/CRS 页脚）+ 完整设计器已按 ET9 裁剪；唯一缺口=图签块（~30 行 workflow 层，递延 TODOS） | — |
| D5.4 | datum 校平像素级同高修复：旧实现各井用自身 span，双栏体高差即错位 40% 基准——改共同深度偏移（multiwell 基线红转绿） | 既有用例转绿 |

lead 集成接线（7 处 + 2 项测试卫生）：`corrPanel->setTaskService`（B1）、
`FolderImportQueueAdapter` 壳挂接 + `DataListPanel::importQueuePanel()`/
`DataPage::listPanel()` 访问器（B2）、main.cpp 组装根 io 注入（D1）、
`SeismicSectionTool` 析构修复（D2 发现，A 域）、singlefactordef welldist 标签
翻转「绕障距离变换」（C5）、appcontext 井类别字段探测接线（C3）、
tst_wellcomposite_visual 补 `pinRenderEnvironment()` + golden 基线钉死环境重生成
（三连绿）、tst_seismic_3dui qWaitFor nodiscard 修零新警告。

**两个 master 既有基线红一并根治**（父 worktree HEAD 干净树复现实锤，非本
wave 引入）：① tst_wellcomposite_visual 黄金图抽样 12/48 超容差——渲染环境
未钉死（跨机字体/palette 漂移 10-66/255），补 `pinRenderEnvironment()`（fe7f226
同款机制）+ 钉死环境重生成 golden；② tst_panels::dataops_d6_tabFocusTraversal
——披露式 UX 改版（默认收起 dataListAdvancedOptions）后测试过期：默认面
Tab 环到不了过滤条，测试改为先点开「选项」披露再审计（设计本意即如此）。

## 性能数字汇总（口径见 docs/seismic/BASELINE.md §8 / docs/perf/BASELINE.md §6）

| 项 | 改前 | 改后 |
|---|---|---|
| 3D 体渲染堆叠开（16 层）取数请求数 | 16 | **1**（体窗任务） |
| 拖放手势切片请求数（press→8 连发→release→350ms 精化） | 5 | **3**（交错好时 2） |
| progressive 静止自动精化 | 不发生（恒最粗层） | 350ms 升 L0 |
| fps 基线（合成 220MB 逐帧 glFinish） | 三切片 18,461（0.054ms/帧） | +stack16 满配 **13,333**（0.075ms/帧） |
| 大 LAS 调用点阻塞（59MB A13.Las，真工区） | 442 ms | **0 ms**（解析池内 369–399ms） |
| 大图视口读块（4096² f32 64MB 合成） | 14 ms | **4 ms**（概览一次性 86–112ms） |
| catalog 查询面（10k→100k） | 0 ms | 0 ms（O(1) 保持，未达 sqlite 触发条件） |

## 语义决策（评审重点）

1. **直读后端体渲染不合并**（3D.md §3）：引擎直读体窗是逐道顺序整读，
   合并反而劣化——按后端分策略，仅 paged/sf3c 走体窗合并。
2. **瓦片取代取消语义**：同路径新时间片请求即取消旧在途任务（终态出表、
   瓦片粒度协作中止）——被顶替的整图不再排队占并发闸跑完。
3. **B3 选 GDAL .ovr 而非自绘瓦片上屏**：QGIS provider 原生走概览零渲染
   管线风险；自绘瓦片需复刻伪彩 renderer 色映射 + 换层语义，近似色低清
   overlay 反而劣化 D2.11 调过的低清先行 UX——记档（BASELINE §6）。
4. **C1 各向异性为全局方向场**（全部 direction_line 长度加权单轴 θ）；
   凹形屏障同连通域内直线欧氏（低估路径长度，只影响权重不影响阻断正确性）
   ——近似声明在 ALGORITHM_AUDIT §3a。
5. **C2 相界 kind 落要素级**（fault_cut 多边形整圈红边）：逐弧段 kind 需
   boundary-graph 线层，递延。
6. **B6 变道长布局坏道保持整索引报错**（对齐不可恢复，如实）；变道长
   取消不落 checkpoint（resume 固定步长契约门）。
7. **D1 序列化/解析经组装根注入**：视图层 include io/* 被白名单挡死是
   既定分层纪律——io 函数装配期注入 sink，未注入走诚实失败路径。

## 已知边界 / 递延（TODOS.md 同批登记）

- 剖面 dock IL/XL/Time 切换仍裸 QThreadPool 直调（无并发闸/取消/LRU）
  ——src/ui/seismicsection 属 D lane 本轮未迁移，SECTION §6 记档，迁
  SeismicTaskService 待裁决。
- TimePlaneCache（timeCachePath 精确时间缓存）不启用：会在源文件旁落
  ≤256MB 缓存，违反真工区只读约束。
- 真工区 paged/sf3c 通道数字未测（转码需在工程目录旁写 GB 级产物），
  沿用合成夹具口径。
- catalog mutator 写路径超线性（10k 6.2s→100k 949.7s 疑二次）——>50k
  目录 profiling。
- buildHorizonMapLayout 图签块（编制/审核/日期，~30 行 workflow 层）。
- 表 K.1 十二类符号图式 fidelity 建议地质评审（C 按 prompt 指名 6 类 +
  resources/geology catalog 语义色补全 6 类）。
- C4 首跑盖章缺口：层生成时未实例化则 paleoAssetId 盖不上，catalog extra
  manifest_layer_id 兜底 + 重跑幂等补章；catalog 显式关联表仍递延。
- `SeismicSectionDockWidget::setInterpretationCatalog` 仍未接线（P5 递延，
  落点 paleomainwindow_sections.cpp）。
- wellcompositestore 迁 services 门面（ui 白名单届时只需放行门面头）。

## 验证口径

- 全量 ninja 零新增警告（自有文件仅 4 条既有 qWaitFor/qFile nodiscard，
  本 wave 2 处新引入已修）；ctest 串行全绿（golden 基线钉死环境重生成后
  tst_wellcomposite_visual 三连绿）；`check_layering.py --strict` 绿；
  selfcheck perf 26 项无新增超预算（空闲机复测）；tst_perf_regress 四比率
  门绿。同机 HEAD 基线（父 worktree selfcheck）留档 /tmp/baseline-head.json
  口径写入 BASELINE 文档。
