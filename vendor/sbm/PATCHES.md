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

## 编译层适配（非源码补丁）

- `paleo_sbm` 目标加 `-fno-char8_t`（MSVC `/Zc:char8_t-`）：上游 30 处
  `.u8string()` 按 C++17 返回 `std::string` 的语义写；头文件零 char8_t
  使用，故 flag 不外泄到消费方 TU。
- `SEISMIC_HAVE_ZSTD`：系统 libzstd 存在时定义并链接。
