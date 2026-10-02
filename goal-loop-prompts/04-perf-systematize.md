# Goal Loop — goal/perf-systematize：性能工程体系化（优化方向）

你接手一个自治迭代循环。方向：**把性能从「单点优化」升级为「可持续工程体系」**——
启动延迟、UI 线程阻塞清零、内存预算、构建期优化实验、门框架构升级。

## 背景事实

- 现有性能基建：`tst_perf_*`/`tst_cache_*`/`tst_seismic_perf` 等测试族、
  `docs/perf/BASELINE.md`+`docs/seismic/BASELINE.md` 基线档、`tst_perf_regress`
  比率门机制（hit/cold ≤ gate 判定劣化）、`PALEO_REAL_PROJECT_AREA` env 门控实测、
  `paleo_selfcheck perf [--json]` 数据层基准组。
- 实测锚点：966MB SEG-Y open ~271ms/IL 64ms/TS 30-59ms/voxel64 46ms/diag 97ms；
  LAS 大文件曾 442ms 阻塞→0ms（已修先例）；4096² 视口 14ms→4ms 先例。
- 已知痛点（勘察起点）：
  - 启动路径无分段耗时测量（进程→QgsApplication→主窗口首帧无仪表）；
  - UI 线程同步 IO 残留：`datapreviewtabs` 曾有 loadVolumeIfNeeded 同步加载先例
    （已两段式修复）——同类模式需全仓清扫；
  - 绝对墙钟断言还在零星分布（TEST-02 口径：应全改比率门/基线对比）；
  - 构建期优化未实验：RelWithDebInfo -O2 现状；LTO/PGO/-O3 对热路径的收益未测；
  - 内存 footprint 无预算：966MB 体打开后 RSS、缓存容量策略无门禁。
- 本机工具链提示：gcc 16.2.1 偶发 ICE/ld 崩（重试续传）；LTO 实验若爆雷如实记录
  为「实验结论」即可，不强行落地。

## 范围（主线簇，按序或并行推进）

1. **启动仪表**：分段计时桩（main 起→QgsApplication init→服务装配→首帧 paint）
   输出 JSON；预算门（相对基线比率）；`paleo_selfcheck` 新增 startup 组或独立
   `tools/measure_startup.*`。
2. **UI 线程阻塞清扫**：静态扫描（grep `\.open\(|readAll|waitForFinished|exec\(\)`
   在 src/ui 下）+ 动态验证（offscreen 起 app，事件循环探活打点）；
   发现的同步 IO/重计算逐个迁服务层或异步化，每个修复配测试。
3. **断言现代化**：全仓墙钟断言盘点（`grep "ms <" tests/`），改比率门/环境自适应
   （baseline 文件读基线 × 容忍系数），修后禁止退化为「放宽到必过」——要有
   真实判别力（篡改阈值在 ledger 逐条说明理由）。
4. **构建优化实验**：同一 scratch build 测 `-O3`/`LTO`/`PGO`（perf 组基准前后对比），
   有收益且稳定则给出开关化接入（CMake option + BUILDING.md 记档）；
   不稳定/无收益则文档记「已实验否决」。
5. **内存预算**：966MB 体打开+3 切片后 RSS 上限门（读 /proc/self 或 QProcessInfo），
   泄漏嗅探（重复 open/close N 轮 RSS 增长率门）。
6. **基线档案刷新**：docs/perf/BASELINE.md 重测重记 + `docs/progress/perf-system.md`
   本波账本（每簇改动与数字）。

## Oracle

1. 启动分段耗时工具可跑，基线入档，比率门测试注册绿。
2. UI 阻塞清扫：发现清单全部处置（修复或带理由豁免）；至少 3 处真实修复+测试。
3. 墙钟断言清零目标：仓内 `ms <` 绝对断言要么比率化要么豁免注明；比例入档。
4. LTO/PGO 实验报告入 docs（结论含「采用/否决+证据」）。
5. 966MB 实测内存预算门一个（RSS 上限 + 无泄漏增长率），全绿。
6. ctest 全绿、`check_layering --strict` 绿、ledger、push + `gh pr create`。

## 勘察指引

- `src/selfcheck/perfgroup.cpp`：perf 组调度现状（BenchResult/预算语义）。
- `tests/tst_perf_regress.cpp`：比率门实现先例（hit/cold ≤ gate）照搬。
- `paleo-dev` 脚本挂点、tests 沙箱 ENVIRONMENT（XDG/TMPDIR 已隔离先例）。
- 审计 UI 阻塞：从 `datapreviewtabs.cpp`（已修）向同模式蔓延搜；
  `src/services/paleotaskservice.*` 是异步任务统一出口。
- Qt6 里 `QThread::isCurrentThread`/`Q_ASSERT` 打点可用于阻塞断言测试。

## 禁区

- 不改算法正确性——性能方向只做通路/调度/测量层。
- 不删测试不降覆盖；改断言要有判别力证据（故意注入劣化必须触发红）。
- 不引入 heaptrack/valgrind 等外部依赖进 CI 路径（工具可用但不进默认门禁）。
- 并行闪红（tst_panels 模态拍序类）非本轮范围——发现只记录移交。

## 迭代协议

- 轮0-1：清扫扫描 + 启动仪表骨架（先可测再优化）。
- 中段：每簇一轮推进；构建实验单独一轮（可并行后台跑）。
- 完成定义 = Oracle 6 条全绿。数字必须实测可复现，禁拍脑袋预算值。
