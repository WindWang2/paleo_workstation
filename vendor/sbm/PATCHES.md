# vendor/sbm 本地补丁清单

相对于上游 pinned commit `aae56c77` 的全部有意分叉。同步上游时需逐一重放。

## P1 · `src/Data/Sgy/SgyIndexCache.{h,cpp}` — 伴生 `.sgyidx` + 项目化缓存目录

来自 paleo_workstation 移植期的本地增强，上游无此能力：

- `CompanionPathFor()`：`Load()` 优先读源文件旁的 `<name>.sgyidx` 伴生文件，
  再回落集中式缓存目录——工区搬机器/共享盘场景直接带走索引。
- `LoadFromPath()` 独立接口、`Save(..., targetCachePath)` 可选落盘目标。
- 缓存目录 `SeismicF3Viewer` → `paleo_workstation`（`%LOCALAPPDATA%`/平台等价物下）。
- POSIX：`<unistd.h>`/`getpid()` 守护；路径转 UTF-8 走 `sgyio::ToUtf8Path`。

## P2 · `src/Data/Sgy/SgyVolume.{h,cpp}` — `TimeGridCache` + mmap 并行 `ReadSlice`

时间片读取的项目实现（原 085e88e WIP），替代上游整行顺序读：

- `TimeGridCache`/`EnsureTimeSliceGrid()`：inline×xline→trace 序号网格只算一次。
- `ReadSlice` 快路径：`QFile::map` 映射整卷 + `std::thread`（≤16 线程）按格式码
  直接解码样本（IEEE/IBM float、int16/int32/int8）；失败回落上游逐道顺序读。
- 依赖 `Qt6::Core`（QFile/QString）——vendor 库因此私有链接 Qt。

## P3 · `src/Data/Sgy/SgySequentialScan.cpp` — POSIX 同步读 buffer 分配

上游 `singleBuffer` 仅在 `queueDepth == 1` 时分配（Windows 异步路径用
`reader.Buffers()` 每槽独立 buffer）。POSIX 分支恒走 `singleBuffer` 同步读，
`ClassifyPath` 返回 Unknown 时 `readQueueDepth=2` → 空 vector `.data()` 为
nullptr → `istream::read(nullptr, bytes)` 段错误。补丁：POSIX 下无条件分配。

## P4 · `src/Engine/StorageProfile.cpp` — POSIX 回退

上游仅 Windows 实现（IOCTL_STORAGE_QUERY_PROPERTY）。POSIX 分支读
`/sys/block/*/queue/rotational` 判定介质类型；探测不到返回 Unknown，
不影响功能，只影响预读/页大小这类启发式默认值。

## P5 · `src/Data/Sgy/SgyIo.{h,cpp}` + 6 个调用点 — paleo 生产道字约定回退

`paleo_workstation` 生产工区（含 966MB 真实体与 `testdata/segy/synthetic_4x5.sgy`）的
INLINE@189/CROSSLINE@193 两字恒 0，实际编码为 field record@9（inline）+ CDP ensemble@21
（crossline）——`src/io/segyreader.cpp` 冻结的约定（见 tools/make_segy_fixture.py 头注）。
上游只读标准字，这些文件在 probe/index/read 链上全部退化为文本头回退或失败。

- `sgyio::ReadTraceKeyWords(header)`（SgyIo）：先读标准 INLINE/CROSSLINE；仅当两字
  同时为 0 才回退 @9/@21。上游规范文件行为逐位不变。
- 调用点统一换用该 helper（与 probe 同一取字口径，读回校验永不自相矛盾）：
  `SgyRuleLayout.cpp` ReadTraceKey（probe）、`SgyIndexBuilder.cpp` 全卷扫描、
  `SgySequentialScan.cpp` 记录循环（.pair 落盘同步换字）、`SgyVolume.cpp` /
  `SgyReadSession.cpp` ValidateRuleTrace、`SgyFileReader.cpp` 摘要 declared ranges。
- 未动 `Engine/RoiWorkspaceBuilder.cpp`（上游实验路径，SDK 链路不经过）。

## P9 · `src/Engine/PagedPipeline.{h,cpp}` — L0 质量账目（paleo P5 Phase 1）

- `PagedBuildResult` 新增 `missingTraceCount`/`damagedTraceCount`/
  `damagedTraceSample`（前 32 个）/`valueMin`/`valueMax`。
- L0 道循环：缺席（FindTraceIndex<0）与损坏（ReadTrace 失败）计数 + NaN
  填充 + 记样，损坏道不再让整个金字塔 IoError 失败；值域 NaN 感知统计。
  LOD 层沿 L0 指针聚合（LOD 构建本身无源道概念）。

## P6 · `src/Engine/TranscodeJob.{h,cpp}` — 并行分片编码 + 质量账目 + scanning 阶段（paleo P5 Phase 1）

- `TranscodeOptions.writerThreads`（1..4，默认 1=上游行为）：codec=zstd 时
  产者 → N 编码线程（BoundedQueue 容量 8）→ 单写线程（容量 4）。写序与
  可续跑布局不变；raw codec 自动退单线程（无编码可并行）。**关队语义**：
  产者只关 encodeQueue；最后一个编码器负责关 writeQueue（否则在途编码块
  会被已关闭的写队列丢弃——静默丢块 bug，测试 tst_seismic_transcode 锁定）。
- `TranscodeResult` 新增缺失/损坏道计数、损坏道样、值域（与 P9 同形）；
  坏道 NaN 填充不炸整体（上游 ReadTrace 失败原本就是静默 continue，此处
  只是把账记全）。
- `TranscodeProgress.phase="scanning"` 头文档承诺但从未发射——现在在
  SgyVolumeSource::Open（索引/续跑探测）前真实发射。

## P7 · `src/Engine/WorkspaceFormat.{h,cpp}` — WriteChunkPrepared + meta 头探测（paleo P5 Phase 1）

- `WorkspaceWriter::WriteChunkPrepared(cs,ci,cx,payload,bytes,err)`：
  写已编码载荷；`WriteChunk` = 编码 + 该函数（P6 编码池消费）。
- `WorkspaceMetaSummary` + `ProbeWorkspaceMeta(base,...)`：只读头与完成
  位图（不碰 chunk 表/shard），供续跑 UX 与 Auto 就绪判定廉价调用。
- `WorkspaceReader::IsComplete()`：完成位图 popcount ≥ chunkCount。

## P8 · `src/Engine/Sdk.cpp` — Auto 只认完整工作区（paleo P5 Phase 1）

`Dataset::Open(Auto)` 的伴生 `.sf3c.meta` 发现改为：`ProbeWorkspaceMeta`
判定 complete 才升级工作区后端；半成品（取消留下的可续跑 meta）保持直读
后端并置 `FellBackToDirect()`（消费侧如实看到「伴生存在但未就绪」）。
杜绝「半个有效 meta 的假完成态」被 Auto 当成品读出错。

## P10 · `src/Data/Sgy/SgyVolume.{h,cpp}` — 切片/时间网格尺寸 64 位运算与上限

inline/xline 计数由文件内容决定，`inlCount * xlCount`（int）可回绕：对角测网
65536 道时乘积为 2^32 → 0，`assign(0)` 后按真实行列写越界（ASan SEGV）。

- `GridCellCount(a, b)`：64 位求积，超过 2^28 格视为异常几何返回 0。
- `EnsureTimeSliceGrid()` 改返回 `bool`；`ExtractSlice` 的 Time/Inline/Xline 分支在
  分配前拒绝超限尺寸并给出错误，所有行列下标改为 `size_t` 运算。
- 回归：`tests/tst_seismic_engine.cpp::timeSliceGridOverflowRejected`。

## 编译层适配（非源码补丁）

- `paleo_sbm` 目标加 `-fno-char8_t`（MSVC `/Zc:char8_t-`）：上游 30 处
  `.u8string()` 按 C++17 返回 `std::string` 的语义写；头文件零 char8_t
  使用，故 flag 不外泄到消费方 TU。
- `SEISMIC_HAVE_ZSTD`：系统 libzstd 存在时定义并链接。
