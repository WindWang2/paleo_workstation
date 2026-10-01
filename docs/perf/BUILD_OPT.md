# 构建期优化实验报告（goal/perf-systematize 簇4）

> 日期 2026-10-01 · Arch x86_64 · GCC 16.2.1 · Qt 6.11.2 · QGIS 4.2.2
> vendored · 16 核 · 本机有并行会话负载。基准面 = `paleo_selfcheck perf`
> （18 项数据层基准）+ `tools/measure_startup.sh`（启动分段）。
>
> **方法纪律**：同机三构建（`build/`=O2 现状、`build-x-o3`、`build-x-lto`）
> **交错三轮**（O2→O3→LTO 交替，取中位数）——首轮顺序测量被否决：O2 基线
> 恰逢并行编译，main_to_first_frame 虚高 3.2×（3716ms vs 常态 ~1100ms），
> 得出「O3 启动 -69%」的错误结论；交错复测后启动份额三构建完全一致
>（qgis 0.10/0.10/0.10，loader 0.563/0.568/0.568）。**单次顺序对比不算数。**

## 结论一览

| 实验 | 判定 | 依据 |
|---|---|---|
| `-O3` | **否决（不设默认/不接开关）** | 收益面大但存在一致回归项：`catalog_open_10k` +35%（两轮 +37%/+37% 复现），`sha_hash_64mb` +12% |
| `-flto=8`（LTO） | **采纳为实验档**：`PALEO_ENABLE_LTO=ON`（默认 OFF） | 11/18 项 -5% 以上，热路径收益稳定且无一致回归 |
| PGO（`-fprofile-generate/use`） | 见 §4 | —— |
| 启动段 | 三者份额一致 | codegen 不改变 .so 装载/重定位构成；启动优化战场在链接布局/预加载，不在 -O 级别 |

## 1. -O3（否决）——交错三轮中位数

收益项（-5% 以下）：las_cold_parse **-31.5%**、catalog_build_10k **-24.7%**、
segy_index_cached **-53.3%**、las_range_2k -20.5%、catalog_query_1k -12.9%。

回归项（判否决的关键）：

| 基准 | O2 轮值 | O3 轮值 | 判定 |
|---|---|---|---|
| catalog_open_10k_ms | 274 / 272 / 395 | **377 / 370** / 296 | 前两轮一致 +37%——真回归（非噪声） |
| sha_hash_64mb_ms | 104 / 56 / 66 | 74 / 77 / 59 | 中位数 +12%（方差大，边缘） |

## 2. LTO（采纳为默认 OFF 的实验档）——交错三轮中位数

| 基准 | O2 | LTO | Δ |
|---|---|---|---|
| las_cold_parse_ms | 45.5 | **27.0** | **-40.7%**（轮值 27/27/24 极稳） |
| segy_index_cached_ms | 3.11 | 1.70 | -45.2% |
| catalog_build_1k_ms | 495.7 | 372.4 | -24.9% |
| catalog_query_1k_ids_ms | 1.44 | 1.17 | -18.2% |
| las_range_2k_ms | 2.45 | 2.15 | -12.5% |
| catalog_build_10k_ms | 4277 | 3475 | -18.8% |
| segy_index_build_ms | 16.5 | 15.2 | -7.7% |

回归检查：catalog_open_10k +6.9%（轮值 293/277/305 vs O2 274/272/395——
O2 自身方差内）；pyramid_lazy_ensure 高方差（36-129ms 全构建皆然）不判。

代价：全量构建 12.6min vs 5.6min（~2.3×，本机 gcc 16.2.1 偶发 ld 崩需
重试一次）；`-ffat-lto-objects` 保持普通 ar 兼容。

## 3. 启动分段（份额比率，交错 ×2 轮 ×3 次/轮）

| 份额 | O2 | O3 | LTO |
|---|---|---|---|
| qgis_init | 0.102 | 0.103 | 0.100 |
| service_assembly | 0.079 | 0.066 | 0.062 |
| show_to_paint | 0.385 | 0.370 | 0.370 |
| loader（process→main） | 0.563 | 0.568 | 0.568 |

codegen 级别不动启动构成；loader 份额 ~0.56（exec→首屏最大单段）的
下手面是动态链接布局（预加载/prelink/库裁剪），已登记 BASELINE.md §7。

## 4. PGO（两阶段）

（实验跑批中——`tools/pgo_experiment.sh`，结果回填。）

## 复现

```bash
tools/buildopt_experiment.sh          # O3/LTO scratch 构建 + 首测（顺序法，仅冒烟）
# 公平对比（交错三轮）：
for r in 1 2 3; do
  for d in build build-x-o3 build-x-lto; do
    QGIS_PREFIX_PATH=<prefix> ./$d/paleo_selfcheck perf --json /tmp/${d}-r${r}.json
  done
done
```
