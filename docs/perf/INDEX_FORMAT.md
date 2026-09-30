# 磁盘缓存格式规范（wave/io-perf-cache P4）

所有落盘缓存共用 `io/cachecore` 底座；数值一律小端。

## 1. 公共文件头（36 字节定长，D2.3）

```
偏移  长度  字段
0     8    magic（各格式自定义 8 字节）
8     2    u16 formatVersion（格式版本——读侧按 [min,max] 门禁，过版即弃+自愈）
10    2    u16 flags（bit0 = payload zstd 压缩，D2.4）
12    4    u32 headerCrc（覆盖前 16 字节、该字段位置置零后计算——CRC-32/IEEE）
16    4    u32 payloadCrc（覆盖落盘形态的 payload，即压缩后字节）
20    8    u64 payloadSize（解压后字节数）
28    8    u64 storedSize（落盘 payload 字节数）
36    …    payload（可 zstd 压缩）
```

读侧五道闸（任一不过 → 调用方删文件自愈）：magic、版本区间、头 CRC、
payload 长度与文件实际尺寸一致、payload CRC。

发布（`writeCacheFileAtomic`，D2.1）：递归建父目录 → 写
`<path>.tmp<pid>_<rand>` → `paleoReplaceFile` 原子 rename（失败重试一次）
→ 仍失败如实 false（调用方无缓存继续）。**教训源头：vendor SgyIndexCache
曾因父目录未建在 rename 时报 "Publishing the cache file failed: No such
file or directory"。**

## 2. LAS 解析缓存 `.plc`（magic `PLASCHE`，version 1）

payload（cacheio 原语序）：

```
str  fingerprint           "canon|mtimeMs|size"（身份自证，防哈希碰撞错配）
f64  null 占位             （NULL 值已在解析期映射 NaN；占位保布局）
u32  curveCount
u32  rowCount
per curve: str name / str unit / str descr
per curve: rowCount × f64  行主序
```

文件名：`sha256(fingerprint)[0:24].plc`，位于
`<proj>/artifacts/index/las/`。

## 3. SEG-Y 道头索引 `.psx`（magic `PSGYIDX1`，version 1）

payload：

```
str  ident.canonicalPath     身份块（D2.2）
i64  ident.size
i64  ident.mtimeMs
u64  ident.inode            （POSIX；Windows 0）
str  prefixFingerprint      首 64KB SHA-256 hex（增量续扫判据，D2.5）
i32  samplesPerTrace        头块
i32  sampleIntervalUs
i32  formatCode
i32  binLineNo
i64  firstTraceOffset
f64  geometry.inlineMin/Max, xlineMin/Max     survey 几何（open 时冻结）
f64  geometry.cornerX[4] / cornerY[4]
f64  geometry.startTimeMs
u32  n                      道数
u32  bad                    坏道数（D2.7）
u8   'C' | 'P'              完整索引 | checkpoint（D2.8）
i64  scannedOffset          已扫到的文件偏移（完整时 = 文件尾）
i32[n] inlineNos            平行数组（顺序 = 文件道序；inlineNo 已按 ordinal
i32[n] xlineNos              方言解析为最终值——checkpoint 只在标准索引模式落）
i64[n] offsets
i64[bad] badTraceOffsets    损坏被跳过的道头偏移
```

文件名：`segyidx_<sha256(canonicalPath)[0:24]>.psx`，位于
`<proj>/artifacts/index/segy/`。zstd 压缩后合成测得 ratio 0.126（-87%）。

### checkpoint 完整性审计（D2.8）

读 checkpoint 时的附加闸：`firstTraceOffset ≤ scannedOffset ≤ size`、
`scannedOffset ≥ 末道覆盖偏移`、完整索引要求 `scannedOffset == size`——
任一不符删文件弃用。续扫判据（`loadForResume`）：inode 一致 + 前缀指纹一致
+ 增长量为整道（`(size - scannedOffset) % traceSize == 0`）。

### 变道长布局契约（B6，wave/deepen-perf 评审落档）

变道长文件（任一道头 `ns > 0` 且 ≠ 二进制头 `ns`）的现行契约与放宽：

| 能力 | 固定道长布局 | 变道长布局 |
|---|---|---|
| 顺序扫描（`open()`） | 支持；坏道（负 ns）跳过 + 记录（B6 起，原为整索引报错） | 支持（逐道按各自 ns 推进）；坏道整索引报错 |
| 并行扫描（`scanParallel`） | 支持（>1MB） | 不适用（步长假设） |
| checkpoint / 续扫 | 支持 | **不落 checkpoint**（B6 起；`SegyReader::variableTraceLayout()` 门） |
| 坏道跳过 | 三路径（顺序/并行/续扫）一致：`ns < 0` 或 `ns > binNs` → 跳过 + 记 `badTraceOffsets` | **不跳**——负 ns 后道边界不可恢复，宁可整索引报错 |

- 顺序路径坏道跳过（B6）：仅当扫描前缀尚未观察到变道长（所有道
  `ns == binNs` 或 `0`）时，负 ns 按固定步长 `240 + binNs*4` 跳过——与
  并行/resume 路径的 D2.7 语义对齐（此前小文件顺序路径坏道=整索引报错，
  大文件并行路径却跳过，口径不一致）。
- 变道长不落 checkpoint 的理由：`resumeScan` 按固定步长推进，变道长断点
  续扫会把错位的道头当好道收进索引（静默坏数据）——宁可重扫。落盘侧由
  `openCached` 的 `!variableTraceLayout()` 闸保证；旧版本已落的变道长
  checkpoint 由 loadForResume 的整道判据兜底（增长量恰为固定步长整数倍的
  碰撞窗口接受为残余边界，读侧审计闸仍在）。
- 测试：`tst_cache_segyindex::{sequentialBadTraceSkipMatchesParallelContract,
  variableLayoutFlagSurfaces, variableLayoutKeepsHardErrorOnCorruptNs,
  variableLayoutWritesNoCheckpoint}`。

## 4. 金字塔层级包 `z<N>.bin`（magic `PYRL`，追加式，不走 cachecore 头）

```
u32 magic 'PYRL'
u32 level z
u32 tilesX / u32 tilesY
u32 tileW / u32 tileH      （层内标准瓦片宽高；边缘瓦片按网格推导）
state[tilesX*tilesY]       每瓦片 1 字节：0=未建 1=已存 2=全nodata（D3.6）
offset[tilesX*tilesY]      每瓦片 u64 绝对偏移（0=无载荷）
payload 追加区             已建瓦片 w*h f32（行主序）
```

追加式更新：payload append → 回填 offset → 置 state。崩溃残片只会让该瓦片
保持未建（下次重建），不损整包。meta.json 记源身份（mtime/size/几何/层级
数），源变化 → 目录整体重建。

## 5. SHA 摘要表 `sha.json`（Compact JSON）

`{ "<canonicalPath>": "mtimeMs|size|sha256hex", … }`（上限 65k 条、按最近
命中收缩）。QSaveFile 原子写；坏 JSON 自愈空表。

## 6. cacheio 小端原语

putU16/U32/U64、I32/I64、F32/F64、putStr（u16 长度 + UTF-8）；对称读接口
带越界 `ok` 位（截断自愈路径的判定基础）。
