# 性能基线（wave/io-perf-cache P4）

> 机器：Arch Linux x86_64 · GCC 16.2.1 · Qt 6.11.2 · QGIS 4.2.2 ·
> RelWithDebInfo (-O2) · NVMe 本地盘 · j4。
> 「改前」= master e1c132e 同代码路径；「改后」= wave/io-perf-cache。
> 可复现：`./build/paleo_selfcheck perf [--json out.json]`（合成夹具）；
> 真实工程口径见 §1（/home/kevin/projects/paleo_project/data/project_area）。

## 1. 真实工程 project_area（1.4GB，20 井曲线 + 1013MB SEG-Y + catalog）

| 指标 | 改前（master 路径） | 改后 | 倍率 |
|---|---|---|---|
| LAS 全量解析 20 个文件（332,739 数据行） | `LasParser::parse` 5894 ms | `parseDoc` 705 ms | **8.4×** |
| 最大单井 A13.Las（59MB）冷解析 | ~430 ms（含在上述口径） | 435 ms 冷 / **44 ms** 磁盘命中 / **0.077 ms** 内存命中 | 命中 10×/5600× |
| SEG-Y 1013MB 道头重扫（263,451 道） | `open()` 顺序 449 ms | 并行冷建 293 ms；**会话重开命中 20.7 ms** | 冷 1.5×；重开 **21.7×** |
| SEG-Y 索引统计（空洞报告） | 无此能力 | inline[1315..1725] ×line[4165..4805]，密度 100.00%，坏道 0 | 新增 |
| catalog 打开（38 实体/98 链接）+ 全实体链接查询 | 打开 ~9 ms；查询线性 | 打开 8.7 ms；查询 **0.04 ms**（邻接索引） | 查询 ~O(1) |
| 类型计数 | 每次全量扫 | `entityCountsByType()` 即时（索引维护） | 新增 |

> SEG-Y 命中 20.7ms 的构成：读 ~2.4MB zstd 索引 + 解压 + 重建行/道哈希。
> 改前每个会话对每个打开的剖面资产都要重付 449ms 重扫（previewdoc 仅会话
> 内缓存，跨会话重付）。

## 2. 合成夹具基准（selfcheck perf 组，确定性、无本地数据依赖）

| 基准 | 值 | 预算 | 结论 |
|---|---|---|---|
| las_legacy_parse_ms（15581 点，旧路径对照） | 162–204 | — | 旧路径基线 |
| las_cold_parse_ms（D1.5） | 13–17 | <50 | ✅（真实工区口径 ~0.9ms/千行） |
| las_cached_ms（D1.1 二次打开） | 0.05 | <5 | ✅ |
| las_memory_hit_ms | 0.03 | <1 | ✅ |
| las_header_ms（header-only） | 0.15–0.19 | <5 | ✅ |
| las_range_2k_ms（区间流式） | 0.4–0.6 | <25 | ✅ |
| segy_rescan_ms（10k 道顺序，对照） | 5–6 | — | |
| segy_index_build_ms（并行 ≤4 + zstd 发布） | 6–9 | — | 小文件并行开销≈收益；大文件见 §1 |
| segy_index_cached_ms | 0.7–0.9 | <5 | ✅ |
| segy_index_compress_ratio（D2.4） | **0.126** | <0.5 | ✅ 体积 -87% |
| segy_index_hit_equiv（命中==冷扫） | 1.0 | =1 | ✅ |
| pyramid_lazy_ensure_ms / first_tile / hit | 0.01 / 35 / 0.05–0.3 | — | ✅ |
| catalog_build_1k/10k_ms（夹具灌库） | 240 / 7.2s | — | 灌库一次性 |
| **catalog_open_10k_ms（D5.7）** | **108–145** | **<500** | ✅ |
| catalog_query_1k_ids_ms（1000 次 entityById） | 0.5–0.9 | <100 | ✅ O(1) |
| sha_hash_64mb_ms / sha_cached_ms | 37–45 / 0.04–0.06 | — | 指纹命中免重算 |

## 3. 机器无关回归门（docs/perf/baselines/ratios.json，D8.2/D8.3）

| 比率 | 基线上限 | 门（基线×1.2） | 断言处 |
|---|---|---|---|
| las 磁盘命中 / 冷解析 | 0.30 | 0.36 | tst_perf_regress |
| segy 索引命中 / 冷重建 | 0.35 | 0.42 | tst_perf_regress |
| sha 命中 / 重算 | 0.02 | 0.024 | tst_perf_regress |
| catalog 5k 表查询 / 1k 表查询 | 4.0 | 4.8 | tst_perf_regress（线性退化即红） |

## 4. 冷/热启动对比（D8.4）

- **冷**（无任何缓存，首次打开）：真实工程 LAS 705ms 全量 / A13 单井 435ms；
  SEG-Y 并行冷建 293ms。
- **热**（磁盘缓存就位，新进程）：LAS 44ms（59MB 单井）/ 内存 0.077ms；
  SEG-Y 21ms。
- 观测面：`LasCache::lastTimings()`（coldParseNs/diskLoadNs/memoryHitNs），
  tst_perf_regress::coldVsHotBothReported 断言三口径均可观测。

## 5. 峰值内存（D6）

- 缓存预算默认 512MB（`CacheBudgetManager`，QSettings `cache/budgetMiB` 可
  覆盖）；LAS 内存层默认 64MB、金字塔瓦片 128MB，均注册受治。
- 超限逐出：最远未用缓存先收缩，pin 条目不动；>75%/>90% 两档压力广播。
- 大对象审计（D6.6）：`parseDoc` 对 >500MB 拒绝整读（D1.8）；>10MB 单次
  读取登记 `noteLargeAllocation`（selfcheck `budget_used_bytes` 可查）。
- 本机实测：全部基准跑完 `budget_used_bytes` ≈ 0.6MB（合成夹具小；真实
  59MB LAS 单井 doc ≈ 27MB 值内存，在 64MB 层内）。
