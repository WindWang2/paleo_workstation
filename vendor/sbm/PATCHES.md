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

## 编译层适配（非源码补丁）

- `paleo_sbm` 目标加 `-fno-char8_t`（MSVC `/Zc:char8_t-`）：上游 30 处
  `.u8string()` 按 C++17 返回 `std::string` 的语义写；头文件零 char8_t
  使用，故 flag 不外泄到消费方 TU。
- `SEISMIC_HAVE_ZSTD`：系统 libzstd 存在时定义并链接。
