# 基准运行手册（wave/io-perf-cache P4，D8）

## 1. selfcheck perf 组（D8.1）

```bash
ninja -C build paleo_selfcheck
QT_QPA_PLATFORM=offscreen ./build/paleo_selfcheck perf            # JSON → stdout
QT_QPA_PLATFORM=offscreen ./build/paleo_selfcheck perf --json out.json
```

- 26 项基准 / 6 组（las / segy / pyramid / catalog / io / memory），全部由
  `PerfFixtures` 现场生成确定性夹具（D8.5），**不依赖本地真实工程**。
- 带预算项超限 → `OVER BUDGET` 行 + 退出码 1（CI/脚本可直接判）。
- 原默认组（`paleo_selfcheck` / `paleo_selfcheck qgis`）行为不变。
- HTML 摘要（D8.6）：`BenchReport::toHtml/writeHtmlFile`（io/benchreport），
  单文件内联 CSS，分组表 + 预算条（超预算红条）。

## 2. 回归门（D8.2/D8.3）

- 机器无关基线：`docs/perf/baselines/ratios.json`（比率上限，非绝对毫秒）。
- 门 = 基线 × 1.2（允许 20% 劣化）；`ctest -R tst_perf_regress` 断言四比率：
  LAS 命中/冷、SEG-Y 命中/重建、SHA 命中/重算、catalog 5k/1k 查询伸缩。
  线性退化（索引失效、缓存失灵）会在伸缩门上爆红。
- 基线文件缺失/坏键 → `baselinesPresentAndWellFormed` 红（不静默跳过）。

## 3. 冷/热对比（D8.4）

`LasCache::lastTimings()` 暴露最近一次 coldParseNs / diskLoadNs /
memoryHitNs 三口径；`tst_perf_regress::coldVsHotBothReported` 断言三者可
观测。真实工程冷/热数字见 BASELINE.md §1/§4。

## 4. 真实工程测量（可复现）

`docs/perf/BASELINE.md` §1 的数字来自 1.4GB 真实 project_area（20 井曲线 +
1013MB SEG-Y + catalog），对照跑 master 同路径。环境：NVMe 本地盘、
RelWithDebInfo、j4。绝对毫秒随机器浮动——跨机器比较请看 §3 的比率门。

## 5. 夹具生成器（D8.5，io/perffixtures）

| 函数 | 产物 |
|---|---|
| `makeSyntheticLas(path, rows, curves, start, step)` | LAS 2.0，LCG 确定值，每 97 行一个 NULL |
| `makeSyntheticSegy(path, inl, xl, samples, ...)` | 标准 189/193 字节位 SEG-Y rev1 IEEE |
| `makeSyntheticCatalogDir(dir, n)` | n 资产工程目录（实体+资产+版本+链接+受管文件） |
| `makeSyntheticWellFiles(dir, n)` | 井口/分层/时深三件套 |
| `makeSyntheticGeoTiff(path, w, h, hole)` | Float32 GeoTIFF，可选中央 nodata 洞 |

确定性：LCG 种子固定（20260929 / 42），同参数跨机器字节一致，测试断言可
复用期望值（`PerfFixtures::lcgUnit`）。

### 相面覆盖绘制的交互延迟

用与应用相同的 Qt、QGIS 与 GEOS 前缀编译 `tst_faciespartition`。以下命令在 Linux 无显示器模式下测量。每轮重建带空间索引的内存图层，分别测第一次覆盖绘制，以及撤销回原图后的第二次绘制。冷态指新图层的首次编辑，操作系统与依赖库已经加载。

```bash
QT_QPA_PLATFORM=offscreen PALEO_FACIES_BENCH_ROUNDS=20 \
  PALEO_FACIES_BENCH_JSON="$PWD/facies-capture-samples.json" \
  ./build/tst_faciespartition capturePerformance
```

夹具按行生成 100 列、每格 10 × 10 米、四角闭合的矩形。总图斑数为 1,000、10,000 和 50,000，初始图元身份由行列位置确定。每次在同一格内落四个顶点、右键完成。视口、落点、可见图层和绘制线程数保持一致。每次撤销后检查覆盖几何与原始面数。

每组原始样本包含右键同步返回、第一帧绘制完成和下一个排队的画布移动事件处理完成的时间，单位均为毫秒。另记绘制开始到完成、撤销后一次全图校验、以及单个受影响图斑的相交与差集耗时。后两项独立计时。按 `faces`、`phase` 分组，以中位数和最近秩 P95 汇总；跨机器比较须同时记录编译配置、渲染平台、依赖版本及样本数。

## 6. 内存基线断言（D6.7）

- `tst_cache_core::budget*`：默认预算/超限逐出（最远未用先收）/压力广播/
  大对象审计。
- `tst_cache_las::memoryLruEviction`、`tst_cache_pyramid::pinPreventsEviction`：
  真实缓存的逐出与 pin 语义。
- selfcheck `budget_used_bytes` / `budget_registered_caches`：环境态输出。
