# Full Audit 20261017 — 基线质量记录

## 构建

- 命令：`cmake --build build --parallel 4`（flock 全局锁）
- 结果：**成功**（exit 0，2231/2231 targets；Ninja/RelWithDebInfo/vendored QGIS prefix）
- 日志：`/tmp/baseline-build.log`（0 FAILED、0 error:）

## 全量测试（BASE_SHA c720aef5 原始代码）

- 命令：`ctest --test-dir build -j4 --output-on-failure`
- 结果：**412/415 通过，3 失败**（exit 8）
- 日志：`/tmp/baseline-tests.log`

### 失败分类（EXISTING_BASELINE_FAILURE，非本轮回归）

| 测试 | 标签 | 定性 |
|---|---|---|
| tst_startup_trace | envdebt/envdebt_exempt/perf-host | 既有环境债豁免项（#501 族） |
| tst_evidencee2e (Subprocess aborted) | workflow | master 已知并行红/串行绿 flaky（E 组审计记档，根因未定位；#501 放行机制族） |
| tst_shell_projectlifecycle (214.5s) | core | 既有红（超时型；不在本轮修复池，无 issue 持有时留观） |

## 代表性性能参照

本机基线数据点（供 Fix 2/终审对比）：全量构建 ~65 分钟（-parallel 4）；
全量 ctest ~31 分钟（-j4）。本轮修复均为局部正确性/资源守卫修复，
不涉及热路径算法复杂度变化（F17 是降低每命令 GDAL 全表扫描次数）。
