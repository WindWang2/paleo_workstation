# 地震链路架构（ARCHITECTURE）

> ⚠️ **冻结基线档案**（2026-10-02 标注）：本文是 wave/seismic-chain-deep
> Phase 0 侦察产物，基线 master@e1c132e——描述的是**改造前**状态快照，
> 不再声称是现行权威账本（「改动请同步更新」声明作废）。D1–D6 后的现行
> 架构看 `docs/progress/seismic-runtime-closure.md`、`seismic-attributes.md`、
> `horizon-autotrack.md` 与 TRANSCODE/SECTION/INTERPRETATION/3D 交付文档；
> 差异举例：剖面 dock 已改走 SeismicTaskService（非裸 QThreadPool）、
> 服务通道已超 12 个、质量报告/任务互斥/属性叠加均已落地。

## 1. 总览

地震子系统四个层次（自下而上）：

```
vendor/sbm（pinned aae56c77 + 5 补丁，见 PATCHES.md）
  Data/Sgy:  SgyIndex / SgyIndexCache(.sgyidx) / SgyVolume / SgyDataCache /
             SgySectionBuilder / SgyRuleLayout / SgyIo(P5 道字)
  Engine:    Sdk::Dataset(Auto/Direct/Workspace/Paged) / TranscodeJob(.sf3c) /
             PagedPipeline(.sf3p+L1/L2) / QuickOpen / ReadPlan / LodBuilder /
             TimePlaneCache / StorageProfile / CancelToken
        ↑ include 根 vendor/sbm/src（namespace seismic::{engine,sdk}）
src/services/seismictaskservice  —— 唯一服务级消费点：12 个 start* 异步通道 +
  数据集条目注册表（≤8 条 LRU + 每条目互斥）+ 内存切片 LRU（SgyDataCache 256MB）
src/ui/seismicsection|seismic3d  —— 视图层：2D 剖面画布(QPainter) / 3D 视口(GL 3.3)
src/ui/datapreview（seismic 分支）—— 数据页地震资产：秒开/转码/瓦片时间片/3D
```

主窗口剖面 dock 与数据页预览是**双轨并存**：dock 走裸 QThreadPool 直调
`SgyVolume::ExtractSlice`（无缓存/无取消），预览页全走 `SeismicTaskService`。

## 2. 转码管线

### 2.1 .sf3c 通道（startWorkspaceTranscode → TranscodeSegyToWorkspace）

- 入口 `SeismicTaskService::startWorkspaceTranscode`（seismictaskservice.cpp:483）；
  workspaceBase 空 → base=sgyPath，产物 `<sgy>.sf3c.meta` + `<sgy>.sf3.sNNN` 分片。
- 引擎内部：SgyVolumeSource::Open（要求索引 complete，未建先全扫/命中 .sgyidx）
  → ROI 校验 → WorkspaceInfo（sourceIdentityHash=fileSize^(mtimeTicks<<1)）
  → 续跑探测（meta 在则 probe，打不开删 meta+前 1024 shard 重建）
  → StorageProfile 自适应写批（HDD 大批/NVMe 小批）
  → 双线程有界流水线（主线程产 chunk + writer std::thread，BoundedQueue 容量 4，
    内存上限 4×chunkBytes；≤64MB 走 trace-major 快路径）
  → Finalize 原子发布 meta+完成位图。**取消也 Finalize（保续跑）**。
- meta 二进制小端：magic `SF3CHNK1` + formatVersion=3 + 几何/chunk 几何/codec +
  源身份 + LOD 元数据 + completion 位图 + 每 chunk 目录（shard/offset/bytes/checksum）。
- 进度：单一 `reportBytes(chunksDone+Skipped, chunksTotal)`，detail 标阶段
  （"transcoding"/"finalizing"；TranscodeProgress.phase 文档里的 "scanning" 从未发射）。
- **TranscodeResult 的质量字段（tracesRead/bytesWritten/maxQueueDepth/writeCalls/
  elapsed）服务层全部丢弃**——无日志、无报告、无 UI 汇总（D1.4/D1.10 缺口）。
- 并发：无任务间互斥（同输出文件两个转码任务并发 = 未定义）（D1.7 缺口）。

### 2.2 .sf3p 通道（startPagedTranscode → BuildPagedPyramid）

- 三阶段：L0 转码（l0-transcode）→ L1 金字塔（l1-build, 4×4×1 Average）→
  L2（l2-build, 8×8×1 Average）→ finalizing；L1/L2 始终从 L0 直建（不级联）。
- 产物：`<sgy>.sf3p`（L0 本体）+ `<stem>.l1.sf3p`/`<stem>.l2.sf3p` 兄弟文件；
  写 `.partial` 原子 rename；完成位图持久化可续跑。
- **层数固定 3 层**（buildLod1/buildLod2 布尔同置），不自适应数据量（D1.9 缺口）。
- 进度：各阶段独立 0–100（阶段切换归零），无全局加权（D1.1 缺口）。
- 转码 LOD 构建期另有 ChunkCache(256MB) 页缓存 + 每层 1 个 writer 线程。

### 2.3 后端选择语义

- `Dataset::Open(Auto)`：仅当 `<path>.sf3c.meta` 伴生才升级 Workspace，
  **永不自动升级 .sf3p**（引擎语义，paleo 应用层显式选择：预览页检测
  `<sgy>.sf3p` 存在即显式走 paged 通道）。
- 双通道可并存：.sf3c 走 Auto（随机访问），.sf3p 走显式（LOD+瓦片）。
- 消费侧安全网：引擎读失败 → volume 直读回落（仅 Auto 通道；显式 paged
  失败如实上报不回落）。

## 3. 切片/剖面读取管线

### 3.1 读通道

| 通道 | 路径 | 缓存 |
|---|---|---|
| 直读 | SgyVolume::ExtractSlice（P2 mmap 并行解码） | 应用级 SgyDataCache LRU 256MB（key=kind+fileSize+sliceIndex） |
| .sf3c | sdk::Dataset::ReadInline/ReadCrossline/ReadTimeSlice | 引擎 ChunkCache 256MB + SliceCache 64MB（每 Dataset） |
| .sf3p | 同上 + ReadTimeSliceTiled（瓦片流）+ ReadVoxelWindow（3D 堆叠层合并取数） | 同上 + LOD 激活切换时重置 |
| 任意线 | sdk::Dataset::ReadSection（useReadPlan：去重+升序+范围合并）；失败回落 BuildLineSection | 无切片缓存（D5.2 缺口：每线全扫） |

**ReadVoxelWindow 消费（wave/deepen-perf A1）**：3D 体渲染堆叠层在 paged
通道预算内（估算 ≤256MB，按激活 LOD 面积因子 16^level 缩减）走单次全窗
体素读取，`SeismicTaskService::slicePlaneFromWindow` 切 16 层平面（与
ReadTimeSlice 逐位同构，`voxelWindowPlanesMatchTimeSlice` 锁定）；直读/
工作区后端保持逐层切片（引擎体窗在直读源是逐道顺序整读，无窄读优势）。

### 3.2 数据形状

Slice2D：width×height float values（NaN=缺失）+ rgba 预烘焙 + section 附加列
（distances/inlineNos/xlineNos/traceIndices/xy/validMask）+ plan 指标。
ColorizeValues 在 vendor 侧把 values→rgba（contrast 1.45/gamma 0.82 固定，
仅 3D 通道用 rgba；2D canvas 用 values 自行上色）。

## 4. 渲染路径

### 4.1 2D 剖面（SeismicSectionCanvas，QPainter）

- 数据→像素：`setSectionData` 存 values → `rebuildImage` 造 QImage(traces×samples)
  → `paintValueRegion` 逐像素（absMax 归一 × gain × contrast → clamp → 内联
  colormap 3 选 1：RedWhiteBlue/Grayscale/Rainbow；NaN→#303131）。
- paintEvent：QPainter SmoothPixmapTransform 把采样分辨率 QImage 拉伸到屏幕
  （单级、无 LOD 抽稀——D2.6 缺口）+ 矢量井层/刻度/色标。
- 交互：平移=pan 偏移+整窗重绘（无脏区）；滚轮双轴同因子（保持夸张比）；
  fitToWindow 独立算 zoomX/zoomY（夸张比=窗口形状决定，无显式系数——D2.7 缺口）。
- 模式：仅变面积密度。**无 wiggle**（D2.2 缺口）、无 AGC（D2.4）、无双刻度
  （TWT/深度二选一，D2.5）、colormap 仅 3 档无反转（D2.8）、无书签（D2.12）、
  导出=QWidget::render 含工具栏白底（D2.9/D2.13 待改）。
- 死信号：`traceClicked` 声明未 emit（D2.11/D4.1 入口）。

### 4.2 3D 视口（Seismic3DViewportWidget，GL 3.3 Core）

- SeismicSliceRenderer：4 槽位（IL/XL/Time/Line）纹理四边形，GL_LINEAR，
  纹理内容=vendor 预烘焙 rgba（**3D 无 colormap/增益可调——D3.5 缺口**）。
- 几何：IL/XL/采样归一化到固定盒 6.0×4.4（**非真实纵横比**）。
- LOD：仅 paged 通道；拖动切最粗层（L2），静止 350ms 升 L0 重取三槽
  （体渲染 LOD 交互降采样 D3.1 部分达成；无静止精渲差分）。
- 相机：轨道 yaw/pitch/distance/target；预设等轴测/俯视/正视/侧视（D3.6
  部分达成，无书签）；无惯性（D3.11）。
- **无 GL 回退**：初始化失败仅 qWarning + Render no-op（D3.9 缺口）。
- **无体渲染**：startVoxelWindow 服务入口空置无 UI 调用方（D3.1 部分缺口）。
- **3D 无井**（D3.4 缺口）；无截图导出（D3.7）；无帧率读数（D3.10）。

## 5. 缓存现状（层次）

1. 应用级 SgyDataCache（直读通道切片 LRU 256MB，in-use 不逐出；仅 Auto 通道查存）
2. 数据集条目注册表（path|backend → Dataset，≤8 条 LRU，shared_ptr 防析构竞态）
3. 引擎内 ChunkCache 256MB + SliceCache 64MB（每 Dataset；SetActiveLod 重置）
4. 磁盘 .sgyidx（伴生优先；身份=路径+size+mtime+首尾 32KB FNV；原子发布）
5. 磁盘 .sf3c/.sf3p（转码产物+完成位图）
6. TimePlaneCache（timeCachePath 精确时间片缓存）——**服务层未启用**
7. 2D 纹理缓存：**无**（(line,增益,colormap,范围) 键控 LRU 是 D2.1 缺口）

## 6. CancelToken 桥接现状

统一模式：worker 内 `engine::CancelToken cancel; cancel.SetPredicate([task]{
return task->cancelRequested(); })`，引擎轮询谓词。桥接点：切片/剖面/
sf3c 转码/QuickOpen/sf3p 转码/瓦片/体素（7 处）。

- **能取消**：索引、两类转码（保续跑）、QuickOpen、切片/剖面/瓦片/体素引擎读。
- **不能取消**：startPagedOpen/startLodSwitch/startBackendProbe（短操作无谓词）、
  startVolumeLoad（仅靠 progress 回调取消）、LRU 命中路径（即回即达）、
  Dataset::Open 与条目互斥等待。
- 纪律：取消后不发布不完整结果；StatusCode::Cancelled → 任务终态 Cancelled；
  服务析构对在途任务 requestCancel+detach。

**wave/deepen-perf 追加（A2/A3 取代语义）**：

- 3D 面板：槽位在提取中新值到来 → 在途任务 `requestCancel()`（被顶替读
  协作中止，不跑完全程）；本端主动取消不刷告警日志（`slotSuperseded_`）。
- `startTimeSliceTiled`：同 `.sf3p` 新请求启动即取消旧在途任务
  （`inFlightTiledTasks_`，瓦片粒度 unwind——被顶替的整图不再排队占闸；
  消费侧采样号世代过滤双保险）。`serviceTiledSupersedeCancelsStale` 锁定。
- paged 体窗的取消粒度 = 整个 ReadBox（引擎单段调用，无瓦片回调）——
  3D 堆叠层单请求即单段，可接受；直读源体窗按道粒度轮询谓词。

## 7. 线程并发纪律

- 所有任务跑 `QThreadPool::globalInstance()`，**无专用上限**（与 LAS/层位等
  共享；D6.4 缺口：地震任务间无 ≤4 纪律闸）。
- sf3c 转码峰值 = 池线程 1 + 私有 writer 线程 1；LOD 构建同构。
- P2 补丁 mmap 并行解码最多拉起 16 std::thread（不受池管）。
- 跨线程发布：瓦片 QMetaObject::invokeMethod(QueuedConnection)+QPointer；
  进度 ≤20Hz 节流。

## 8. meta 契约（版本化现状）

- .sf3c：formatVersion=3（读取端版本不符即拒）；algorithmVersion 独立字段。
  **无迁移机制**（旧版本文件直接不可读——D1.6 缺口）。
- .sf3p：version=7（legacy 6 仍可读）；algorithmVersion、buildGeneration、
  源身份（path 之外 size/mtime/fingerprint）。
- .sgyidx：cacheFormatVersion=3、algorithmVersion=2。
- 断点续跑契约：完成位图（两通道都有）；半成品 .sf3p 只有 .partial 不发布；
  .sf3c 取消后 meta 在但分片可能不全（消费侧靠读失败回落兜底，**无「继续
  转码」UI 入口**——D1.2 缺口）。

## 9. 井震联动现状

- 井投射到剖面：SectionWellProjector 已建成（垂直井筒+分层+测井曲线+顶尺针），
  **但主窗口链路未传 candidateWells（不投井）**；时间切片向井位标记把 map
  坐标当测线号用（坐标语义混用）。
- 时深转换：TimeDepthModel/TD 表插值（seismicmapping 契约：不外推、双向）。
- 3D 完全无井。
- 地图↔剖面联动：SeismicMapLink 画线拉剖面 + 悬停返投地图十字。

## 9b. 消费侧接线对照（wave/deepen-perf A1 核账）

| 引擎入口 | 服务通道 | 消费视图 |
|---|---|---|
| QuickOpenSegyPreview | startQuickOpen | 数据页地震资产秒开行 + 中央测线缩略进 2D 剖面 |
| ReadTimeSliceTiled | startTimeSliceTiled | 数据页时间切片页（瓦片渐进 + A3 取代取消/空态） |
| progressiveLod / SetActiveLod | startPagedOpen / startLodSwitch | 3D 面板（粗开→拖动粗层→静止精化；A2 自动精化） |
| ReadVoxelWindow | startVoxelWindow + slicePlaneFromWindow | 3D 体渲染堆叠层（paged 预算内单请求切 16 层） |

## 10. 缺口总账（D 系列对照）

| 领域 | 缺口 | 交付项 |
|---|---|---|
| 转码 | 分阶段加权进度+ETA | D1.1 |
| 转码 | 续跑 UI 入口+断点校验 | D1.2 |
| 转码 | 并发分片写入线程池 | D1.3 |
| 转码 | 质量报告落任务结果 | D1.4 |
| 转码 | 损坏源道跳过+记录 | D1.5 |
| 转码 | meta 版本迁移 | D1.6 |
| 转码 | 同输出互斥 | D1.7 |
| 转码 | 取消不留假完成态 | D1.8 |
| 转码 | 金字塔层数自适应 | D1.9 |
| 转码 | 结构化日志 | D1.10 |
| 剖面 | 纹理 LRU/三模/阈值极性/AGC/双刻度/LOD/拉伸/colormap/导出/卷帘/道头卡/书签/打印/原因态 | D2.1–D2.14 |
| 三维 | 体渲染 LOD/可拖切片/裁剪盒/井位/colormap 编辑/相机书签/截图/内存预算/GL 回退/帧率/惯性/多体 | D3.1–D3.12 |
| 解释 | 种子点/互相关追踪/层位资产/断层/列表面板/undo/网格化/会话/多解释者/置信度 | D4.1–D4.10 |
| 井震 | 任意线编辑器/窗口缓存复用/井轨迹投影/合成记录/分层标注/井旁道/多井开关 | D5.1–D5.7 |
| 性能 | 时延/帧率预算入 selfcheck/内存治理/并发审计/取消全链/错误分类/自动保存点/流式化 | D6.1–D6.8 |

## 11. 已固化的引擎语义（改造不得违反）

1. Dataset 单线程独占（条目互斥锁串行化）。
2. Auto 永不升级 .sf3p；瓦片时间片要求 paged L0 激活层。
3. 取消后不发布不完整结果；.sf3c 取消留续跑位图。
4. NaN=缺失不冒充零振幅（#303131）。
5. worker 持 shared_ptr registry 引用防析构竞态（0b557ee 模式，新代码照抄）。
6. 上游规范文件逐位不变（P5 道字回退只在两字恒 0 时触发）。
