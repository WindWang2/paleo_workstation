# 200P 工区基线实测（BASELINE）

> wave/seismic-chain-deep · Phase 0 实测 · 基线 master@e1c132e（改造前）
> 测量夹具：`tests/tst_seismic_baseline.cpp`（`QT_QPA_PLATFORM=offscreen`），
> 数据源 = `tools/make_segy_fixture.py --mb 220` 生产形状体（与 966MB 真工区
> 同构：1024 样 × dt 2ms × 221 xline × 241 inline = 53,261 道，ordinal 道字）。
> 机器：Arch Linux · RTX 3080 Laptop GPU · NVMe。重复 3 轮取稳定值。

## 1. 索引耗时

| 指标 | 实测 | 说明 |
|---|---|---|
| `index_cold_full_scan` | **146–159 ms** | `SgyIndexCache::Remove` 后全卷道头扫描（53,261 道） |
| `index_warm_cache` | **67–68 ms** | `.sgyidx` 伴生/集中缓存加载（含反序列化+身份校验） |
| QuickOpen 首屏 | **19 ms** | 规则探针 + 128 列真振幅缩略（tst_seismic_perf 同夹具） |

966MB 真工区外推：道数 ≈ 220MB 的 4.4×，冷扫描按线性外推约 0.7s 量级
（顺序 4MiB 窗口读，HDD 上会显著放大——StorageProfile 自适应写批已就位）。

## 2. 切片时延

| 指标 | 实测 | 通道 |
|---|---|---|
| `open_direct` | 63–70 ms | `Dataset::Open(Auto)` 直读（热索引） |
| `slice_inline_cold_direct` | 22–24 ms | 首条 inline 剖面（221 列 × 1024 行） |
| `slice_inline_hot_direct` | 24 ms | 立即重读（引擎 SliceCache 64MB；该尺寸下未体现增益=测量噪声级） |
| `slice_time_cold_direct` | 31–33 ms | 首张时间片（221 × 241） |
| `slice_time_hot_direct` | 19 ms | 重读时间片 |
| `slice_inline_cold_workspace` | **347–368 ms** | .sf3c 首读（chunk 缓存冷，221 列跨多 chunk 全解压） |
| `slice_inline_hot_workspace` | **1–2 ms** | .sf3c 热读（ChunkCache 256MB 命中） |
| `open_workspace` / `open_paged` | <1 ms | 工作区/分页后端打开 |
| `voxel_window_64cubed_paged` | <1 ms | 64³ 体素窗口（页缓存热） |

**关键结论**：D6.1 预算（命中 <50ms、未命中 <500ms）在 220MB 档已满足；
.sf3c 冷首读 350ms 是最大未命中间隙（64³ chunk 粒度所致，paged 页粒度更细）。

## 3. 转码耗时与产物

| 指标 | 实测 |
|---|---|
| `transcode_sf3c_total` | **2.25–2.34 s**（53,261 道，输出 256 MiB raw） |
| `transcode_sf3p_pyramid` | **11.3–11.5 s**（L0 218.7 MiB + L1 14.1 MiB + L2 4.0 MiB） |

.sf3p 金字塔是 .sf3c 的 5 倍耗时（L0 转码 + 两层 LOD 各自全卷平均聚合）；
D1.1 的分阶段进度（扫描/写片/校验/金字塔各自百分比 + 总体 ETA）对 sf3p
尤其必要（11s 的黑盒进度条在 966MB 档会放大到分钟级）。

## 4. 三维帧率

| 指标 | 实测 |
|---|---|
| `gl_renderer` | NVIDIA GeForce RTX 3080 Laptop GPU |
| `fps_3d_offscreen_pipelined` | 80,000+ fps（3 纹理四边形 + 线框 @ 800×600） |
| `fps_3d_offscreen_synced`（逐帧 glFinish） | **18,461 fps（0.054 ms/帧）** |

**结论**：当前 3D 场景（三正交切片四边形）栅格化成本可忽略——交互期
帧率瓶颈是**切片数据提取 I/O**（22–350ms/张），不是 GL。D3.1 的 LOD 交互
降采样价值在降低提取量与上传量，而非栅格化。真机 vsync 下满帧可期。

## 5. 峰值内存

| 指标 | 实测 |
|---|---|
| `peak_rss`（整测试进程） | **686–701 MiB** |

含：220MB 夹具直读（SgyVolume 索引+切片）+ sf3c 转码（4×chunk 背压队列）
+ sf3p 金字塔（256MB LOD 构建页缓存）+ 切片/体素缓存全部叠加。
220MB 体全部装进内存无压力；D3.8/D6.8 的内存预算阈值以「体字节 > 可用
内存一半」为触发线（本机 32GB → 16GB 档，966MB 真工区远未触发；对超
大体必须有 paged 化路径——已就位）。

## 6. .sf3c / .sf3p 双通道现状与 meta 契约

- **并存语义**：`.sf3c` 走 Auto 自动升级（随机访问后端）；
  `.sf3p` 显式通道（LOD + 瓦片渐进）。两通道产物可同时存在于源旁。
- **.sf3c**：`<sgy>.sf3c.meta`（二进制小端：magic `SF3CHNK1`、
  formatVersion=3、几何/chunk 64³/codec/源身份/LOD 元数据/completion 位图/
  chunk 目录）+ `<sgy>.sf3.sNNN` 分片。**无文本 JSON；版本不符即拒读且
  无迁移**（D1.6）。
- **.sf3p**：单文件 v7 自描述头（4096B 对齐；源身份 size/mtime/fingerprint、
  真实 AxisDescriptor、LOD 元数据、coverage、buildGeneration）+ 页级 CRC +
  完成位图 + `.partial` 原子发布。兄弟层级 `<stem>.l1.sf3p`/`<stem>.l2.sf3p`，
  **层数固定 3 层不自适应**（D1.9）。
- **断点**：两通道完成位图均可续跑；但**无「继续转码」UI 入口**（D1.2）、
  取消后 .sf3c meta 在而分片不全（靠读失败回落兜底，D1.8 要堵假完成态）。

## 7. 复现

```bash
cmake -B build -G Ninja && ninja -C build -j4
QT_QPA_PLATFORM=offscreen ./build/tst_seismic_baseline   # 输出 BASELINE <metric> = <value> 行
```

夹具生成一次后缓存于 `build/seismic_perf/perf_big.sgy`（不入库）；
首轮含 numpy 生成约 5s（需 python3+numpy）。
