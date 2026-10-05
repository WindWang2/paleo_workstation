# Goal Loop Ledger — goal/attr-volume（地震属性体化：时间切片/整体扫描 + 属性图层入层树）

- Branch: `goal/attr-volume-20261004`
- Worktree: `.worktrees/attr-volume`
- BASE_SHA: b57e5f3（origin/master @ 2026-10-04）
- 迭代上限：5 轮

## Oracle（验收，全绿即收）

1. SATR round-trip：服务写端产物经 io/sattrio 读回器逐字段一致
   （参数/几何/值块，NaN 位一致）。
2. 时间切片：合成体扫描 → IL×XL 地理参考栅格（GeoTIFF + 全参
   geotransform + PALEO_* 元数据），数值与逐道重算**逐位相等**。
3. 体化：整体属性体落 `.sattr` SATV 容器（头/块表/EOF footer 统计），
   块表寻址 round-trip、任意 IL/XL/时间面抽取与直算一致；体扫描时间面
   == 时间切片扫描（双路径互证）；3D 体视渲染证据（无头 GL FBO 帧，
   `docs/evidence/attr-volume/3d-property-brick.png`，测试内含点亮像素
   数断言 >500）。
4. 入树：切片属性层树条目 type=raster、URI 指真实 GeoTIFF、DERIVED 版本
   parentVersionIds 锚源 RAW；剖面属性不出现在层树（无入树 API——剖面
   登记入口 registerAttributeSliceAsset 无 layerOut，由设计保证）。
5. 调度：扫描可取消（无半成品产物/无 DERIVED——.partial 删除）；同参数
   二次扫描命中缓存且 provenance（catalog extra `param_hash`/`cache_hit`）
   可见；相干加权/等权两档结果差异断言（各向异性道距合成体）。
6. 形状拒绝：剖面路径 Time 切片如实拒绝，文案指明「整道谱/垂向窗 ±N 样/
   三维窗」并引导扫描入口；采样号越界/测网过小如实拒绝。

## 轮0 勘察结论（事实底座）

- 属性核 `src/algorithms/seismicattr.{h,cpp}`：瞬时族需整道谱
  （complexTraceAnalysis）、时窗族需垂向窗、相干为三维窗核
  （`values[((il*nXl)+xl)*nS+s]` 布局）；相干无道距加权参数（本方向新增）。
- SATR 写端原在 `SeismicTaskService::registerAttributeSliceAsset`（魔数+
  版本+几何+JSON 头+小端 f32）；crossplot 有私有读端（本方向收敛共用）。
- `SeismicTaskService`：startBounded 闸（≤4 并发）、PaleoTask 协作取消、
  in-flight 顶替（propagation/tiled 先例）；剖面属性任务
  startAttributeSlice 拒绝 Time 切片。
- 3D 呈现面：`PropertyBrickAxes`（i→xline/j→inline/k→采样）+
  `UpdatePropertySlice`（三槽）+ `UpdatePropertyStackLayer`（16 层堆叠，
  此前无消费方——本方向接通）；喂入先例 finishPropertyModelRun
  （paleomainwindow_attach.cpp）。
- 地理栅格写口：horizonbinner 的 GeoTIFF 管线（局部测网 CRS + PALEO_*
  元数据）——时间切片栅格照此约定新建 `io/attrgridout`。
- paramHash 先例：propertymodelworkflow（SHA-256 域分隔）。
- i18n：仓库一门 zh_CN 骨架（源串中文 tr()），lupdate 更新入口
  tools/update_translations.sh。
- 夹具：tst_seismicattrsvc/tst_seismicattrui 的合成 SEG-Y writer
  （INLINE@188/XLINE@192、IEEE fmt=5）可仿写（本方向加道距各向异性坐标）。

## 技术选型（轮0 决议）

| 决策 | 选择 | 因由 |
|------|------|------|
| SATR 读回器位置 | `src/io/sattrio`（数据层） | 服务写端/crossplot 读端共用同一格式定义防漂移；测试直测 |
| 体容器格式 | `.sattr` 扩展名 + `SATV` 魔数区分 | 同族产物同扩展名；块 = blockIl 条 inline（≤32MiB/块），头 JSON（轴值表/参数/paramHash）+ 块表（偏移前置可算）+ payload + EOF footer(24B：值域/有效单元——流式写端统计值尾部才可知，头不预写假值) |
| 扫描调度 | SeismicTaskService 新增 start*（startBounded，禁新线程池） | 顶替/取消/任务面板语义复用；同体新扫描顶替在途（inFlightAttrScans_） |
| 时间切片相干 | 采样 slab（[s±tH] 全测网）→ 核 → 中心面 | slab 恰为该采样完整垂直窗（边缘缩窗与整体系数一致，测试双路径互证逐位相等） |
| 体相干 | 块+双侧邻线 mini 体（越界线 NaN 填充） | 块边界行 NaN 语义 == 整体核边界带；块与 SATV 写块同尺寸对齐 |
| 地理参考 | SgyCoordinateMapper 仿射 → 全参 geotransform（旋转项 GT2/GT4 如实保留） | 测网斜置不硬掰正北；拟合失败如实拒绝产栅格（诚实面） |
| 去重缓存 | 确定性文件名 `<attr>_<scope>_<hash12>.<ext>` + sidecar/容器头全量 hash 复验 | 扫描任务内先查缓存（命中不重算）；.partial + 原子改名（取消无残件）；登记按 param_hash 幂等复用版本 |
| 道距加权 | 内核 `semblanceCoherenceWeighted`（Equal/InverseDistance 双档） | Equal 档与原核同累加路径逐位一致（乘 1 不改浮点值）；InverseDistance w=1/(d+d₀)，d₀=min 两向道距；道距缺失全 NaN 不静默降级 |
| 3D 喂入 | 服务层 `loadAttributeVolumePreview`（读 SATV）+ 主窗口 updatePropertySlice/StackLayer | 视图层 io include 白名单仅 lasdoc.h——取数归服务，视口只收图像 |

## 迭代记录

### 轮1（实现批次 1–6 一次性落全，测试先行验证）

| 批次 | 改动 | 验证 |
|------|------|------|
| 1 SATR 读回器 | `src/io/sattrio.{h,cpp}`（SATR 读写收敛）；服务写端/crossplot 读端改共用 | tst_sattrio 7/7（round-trip 逐字段/逐位、损坏拒绝、极简头兼容）；tst_seismicattrsvc 9/9（服务产物读回一致）；tst_crossplot_samples 7/7 |
| 2 时间切片 | `startTimeSliceAttribute`（逐 inline 分块、逐道族列分片并行、相干 slab）+ `io/attrgridout`（GeoTIFF）+ `registerTimeSliceAttributeAsset`（DERIVED+LayerDeclaration）+ 剖面路径 Time 拒绝文案锐化 | tst_seismicattrscan：数值与逐道重算逐位相等、解析值（包络≈1/RMS≈1/√2/相干边界 NaN）、形状拒绝、取消无残件 |
| 3 体容器 | SATV 写/读端（块表寻址、footer 统计、abort()）+ `startAttributeVolume`（流式分块；相干滑窗）+ `registerAttributeVolumeAsset` | tst_sattrio 体 round-trip/抽取；tst_seismicattrscan：extractInline/Xline/Time 与直算一致、双路径互证（体时间面==切片扫描）、缓存命中 |
| 4 3D 消费 | `loadAttributeVolumePreview`（三中位面+16 堆叠层，extractTimePlanes 单遍块读）+ 主窗口 showAttributeVolumeIn3D | tst_seismic_3dui::attrVolumePropertyBrickHeadless（无头 GL 真渲染 + FBO 读回 + 像素断言 + PALEO_UI_CAPTURE 证据帧） |
| 5 层树入树 | dock `timeSliceAttrLayerReady`/`attrVolumeReady` 信号 + 主窗口 declare/喂 3D；面板「范围」组合（本剖面/时间切片/属性体）+ 采样位 + 加权档 | tst_seismicattrui 7/7（意图信号分派、dock 全链：层声明/tif 产物/DERIVED/缓存命中文案、体扫描信号） |
| 6 加权+缓存 | 内核 `semblanceCoherenceWeighted`、`SeismicAttrParams.coherenceWeighting`、attrScanParamHash、缓存文件/prov sidecar | tst_seismicattr 15/15（Equal 逐位一致/两档差异/同波体=1/缺道距 NaN）；tst_seismicattrscan 缓存命中 + provenance 字段断言 |

全量 ctest：见收尾记录（layering/layering_strict 全绿）。

### 过程发现（防回归）

- `std::vector<float> tr(n)` 在 QObject 成员函数 lambda 内会撞 `tr()`
  最令人头疼的解析——改 `traceBuf(static_cast<std::size_t>(n))`（换名+
  显式转型双保险）。
- Qt6 QVector 无 `toStdVector`——用 begin/end 构造。
- 花括号初始化 `std::vector<float> x{size_t}` 撞 initializer_list 语义
  （narrowing error）——计数构造一律圆括号+cast。
- QByteArray 未 open 的 `QFile(path).readAll()` 返回空——对空数组 `[0]`
  写入是共享 null 段 SEGV（测试踩坑）。
- `UpdatePropertySlice` 需预烘焙 rgba（生产路径 viewport::bakePropertyRgba
  负责）——无头直调渲染器须同语义烘焙。
- 时间片图像行序：SATV extractTime 是 [il0..ilN) 行序，Time 槽约定
  row0=最大 inline——整行交换翻序（平铺 std::reverse 会连列序一起翻）。

## 收尾记录

- 全量 ctest：**290 项，289 绿 + tst_startup_trace 满载 flake 1 例**（见
  下）；构建 `-j8`。
- tst_startup_trace 负载 flake 判定（非本方向回归）：`-j8` 全套并发时
  `service_assembly_share_max` 中位 0.173 > 门 0.1675（基线 0.067×2.5）；
  空载按官方复测程序 `tools/measure_startup.sh ./build/paleo 9` 实测中位
  **0.049（优于基线）**，单跑 ctest 亦绿。基线 _meta 自述「容差×2.5 覆盖
  负载分母波动」——极满载下仍偶越 3%。本方向不触启动路径（无静态初始化、
  服务构造未变）。
- i18n：paleo_zh_CN.ts 骨架经 tools/update_translations.sh 更新
  （59+4 新串）。
- 渲染证据：docs/evidence/attr-volume/3d-property-brick.png（480×360，
  三属性切片 + 16 层属性堆叠，无头 GL FBO 读回，测试内点亮像素断言）。

## Review 轮次

### 轮1（独立五维审查）

发现 2 High + 7 Medium + 10 Low；五维中分层/i18n 无发现，剖面属性确认
无层树路径。全部 High/Medium 修复 + Low 快修（L1/L2/L3/L4/L5/L6/L8/L9）：
- H1 体扫描空产物目录 → 服务前置如实拒绝 + dock 缺省 `<sgy>.attrs` 伴生目录
- H2 道距加权 + 测网无地理参考 → 前置如实拒绝（等权档不受限）
- M1 时间切片栅格三条失败路径清 .partial；M2 登记复用分支验产物在盘；
  M3 缓存命中经 readTimeSliceGeoTiffSummary 回填统计；M4 体登记失败消息
  回显面板；M5 扫描启动清旧剖面结果 + 登记按钮限剖面范围；M6 3D 预览取数
  移入静默任务（主线程只贴纹理）；M7 登记上下文按快照捕获
- 回归测试：tst_seismicattrscan::reviewRound1Regressions（H1/H2/M3）

### 轮2（修复后复审）

核验轮1全部修复闭环、未达零——余 2 Medium（均在 M6 新链路）+ 6 Low：
- M1-R2 预览任务无世代守卫（陈旧预览可乱序覆盖 3D）→ m_attrPreviewGen
  世代 + 旧预览 requestCancel + 迟到回调丢弃
- M2-R2 扫描回调裸 this（dock 析构窗口 UAF）→ QPointer guard（体传播同
  范式）+ 析构补 m_attrTask/m_attrPreviewTask 取消
- Low 快修：面板缺省采样 blockSignals（防误标用户触碰）；扫描结果
  registrable=false（登记按钮不再误亮）；SATV readBlock 补单精度设置；
  lupdate 补跑
- Low 记 PR body：参数交替重扫的版本历史累积（与既有登记路径同形态）；
  crossplot 取消粒度变粗（顺序读极快，实际无感）；逐 inline 线程
  spawn/join（FFT 主导下占比小）

### 轮3（终判）

轮2修复后全量 ctest 289 绿 + 启动门满载 flake 1 例（空载官方复测优于
基线，见收尾记录）；五维零 High/Medium——**达标收工**。
