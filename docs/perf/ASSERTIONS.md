# 墙钟断言台账（goal/perf-systematize 簇3）

> 口径（TEST-02）：仓内 `ms <` 绝对墙钟断言要么**比率化**（在测参照 × 门，
> 机器无关、退化必红），要么**豁免入档**（实测锚 + 余量倍数 + 语义类别）。
> 禁止「放宽到必过」——每条豁免给出实测基线与余量，门仍保有判别力
>（灾难级回归必红）。盘点时间 2026-10-01，`grep -rnE "QVERIFY2?\([^)]*(Ms|ms)\s*[<>]" tests/`。

## 比例总览

| 处置 | 数量（断言处） | 占比 |
|---|---|---|
| 本波比率化（绝对 → 在测参照比率 + sanity） | 6 | 13% |
| 先例已是比率/自适应（未动） | 11 | 23% |
| 豁免入档（下方逐条） | 31 | 64% |

## 1. 本波比率化（6 处，均含 qInfo 实测锚 + 判别力论证）

| 文件:行（改前） | 旧断言 | 新门 | 实测（本机 2026-10-01） | 判别力 |
|---|---|---|---|---|
| tst_cache_las secondOpen | cold<50 / warm<5 / mem<5 | warm≤0.5×cold；mem≤0.5×cold；cold<2000 sanity | cold 13-17ms、warm≈0.05ms（比率≈0.003） | 缓存失效→warm≈cold→比率→1 红；sanity 拦挂死 |
| tst_perf_las parseDoc | ms<50 | ms<0.5×legacy（同文件旧 parse() 在测参照）+ <2000 sanity | parseDoc 19.5ms vs legacy 684.7ms（0.028） | 解析器退化回旧量级→比率→1 红 |
| tst_perf_segyindex cacheHit | ms<5 | ms<0.42×冷建（ratios.json segy_cached_vs_rebuild_max=0.35×1.2）+ <500 sanity | hit 3.2ms vs 冷建 17.8ms（0.181） | 索引缓存失效→比率→1 红 |
| tst_perf_catalog open10k | ms<500 | 10k/1k 打开比 ≤25 + 500 sanity 保留 | 386.9/44.0 = 8.8 | 打开退化超线性（逐实体重扫）→ 数百倍红 |

同波新增的比率门测试（非改写，全新）：tst_startup_trace（启动份额 ×4 道，
×2.5 容差 + 注劣化 1500ms 必红）；tst_ui_blocking（三修复构建 <0.5×解析 +
探活分片）；tst_mem_budget（RSS ≤0.5×体、循环增长 ≤max(32MiB,0.5%×体)）。

## 2. 先例已是比率/自适应（11 处，未动）

- tst_perf_regress：5 道门全部 vs `docs/perf/baselines/ratios.json` ×1.2
  （las/segy/sha 命中比 + catalog 查询/灌库伸缩比）。
- tst_seismic_sectionui `secondMs ≤ firstMs + 2`（缓存命中 vs 构建）；
  另一 `secondMs < 25` 见豁免表。
- tst_seismic_welltie `secondMs ≤ firstMs + 3`（同上模式）。
- tst_previewdoc `headerMs < fullMs || fullMs < 5`（头部解析 vs 整份，自带
  小文件逃逸）。
- tst_cache_async 高优完成预算：按平台放缩（Win 2 核 4000/其余 2000），
  语义=「高优先于低优完成」非绝对墙钟（文件内注记）。

## 3. 豁免清单（31 处，逐条理由）

### 3a. 宽余量预算（实测余量 ≥10×，灾难级回归仍必红）

| 断言 | 预算 | 实测锚 | 余量 | 出处 |
|---|---|---|---|---|
| tst_perfbudget projectOpen | 100ms | 1-2ms（200 版本 catalog+manifest） | ~50× | BASELINE.md §2 同锚 |
| tst_perfbudget folderEnumeration | 500ms（预热后） | 1ms（64 文件） | ~500× | 文件内注记 |
| tst_perfbudget catalogSave | 100ms | 1-2ms（~330KB JSON+.bak） | ~50× | 文件内注记 |
| tst_perf projectOpen | 10000ms | §41.6 工程打开 | sanity | — |
| tst_catalog open/save ×2 | 2000ms | 打开 136-145ms | ~14× | BASELINE.md B4 |
| tst_catalog 查询 ×5 | 100ms | 0-1ms | ~100× | BASELINE.md §1 |
| tst_perf_catalog open10k sanity | 500ms（保留） | 108-145ms（本波加比率门后保留为 sanity） | ~4× + 比率门 | 本波 §1 |
| tst_seismic_perf ×4（quickOpen/首切片/时间片/剖面） | 5000-8000ms | 271/64/30-59/97ms（966MB 实测） | 15-100× | docs/seismic/BASELINE.md |
| tst_seismic_baseline ×4 | 120s/5s/300s/600s | 冷索引 293ms/命中 21ms/转码/金字塔 | 400×+ | sanity（防挂死） |
| tst_seismic_budgets slice miss/hit | 500/50ms | 合成 220MB 冷 ~60ms | ~8× | docs/seismic/BASELINE.md |
| tst_previewmap_perf 首开/快照/采样 ×3 | 900/300/100ms | <50ms 常态（文件内注记「共享机 3 倍放宽」） | ~18×+ | 文件内注记 |
| tst_seismic_index 索引命中 | 100ms | 典型 <5ms | ~20× | 文件内注记 |
| tst_wellcomposite_perf 采样 | 500ms | PERF-BENCH 行打印 | bench 上限 | — |
| tst_correlation_full 井震对齐 | 300ms | 预热后对齐计算 | bench 上限 | — |
| tst_previewdoc headerMs | 50ms | 0.15-0.19ms | ~250×（主断言已是比率） | BASELINE.md §2 |

### 3b. 语义预算（物理/契约常量，非机器相关回归门）

| 断言 | 语义 |
|---|---|
| tst_seismic_sectionui perFrameMs<16 | 60fps 帧预算（交互渲染契约；LOD 抽稀路径） |
| tst_seismic_sectionui secondMs<25 | 剖面缓存命中显示延迟上限（主断言为比率，此为显示延迟契约） |
| tst_wellcomposite_perf bestMs<48 | 16ms/帧 ×3 共享机抖动（帧预算语义，文件内注记） |
| tst_correlation_async acceptMs<500 ×2 | 「受理不在 GUI 线程解析」语义门（实测 0ms；B1） |
| tst_seismic_index cancel<50ms | 取消响应延迟契约（协作取消及时性） |

### 3c. 非 SQL 计时的 grep 命中（值比较，不入墙钟账）

tst_mapping / tst_algorithm_harness / tst_factorworkflow 的命中为
`qAbs(x - 期望值) < ε` 数值断言（timeMs 字段值），非墙钟计时。

## 维护纪律

- 新增墙钟断言默认走比率门（在测参照），绝对值只允许 sanity 上限（≥50×
  实测余量）或语义常量（帧预算/延迟契约），且须在本台账登记。
- 台台账每年随 BASELINE.md 刷新复核余量；余量跌破 5× 的豁免条目转比率化。
