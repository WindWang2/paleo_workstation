# 基线质量记录（full-audit-20261019）

- BASE_SHA f6963e0b 全量构建：**成功**（2310 目标，rc=0，~2.5h，-j4 flock）
- 基线全量 ctest（-j4）：**421/438 通过，17 失败（96%）**
- 日志：/tmp/paleo-audit-r3/baseline-{configure,build,tests}.log

## 基线红集分类

### 闸门红（10 项，全部为 2026-10-18 当天合入的 #546/#548/#552/#565/#566/#564 带入；本轮 G1 簇修复目标）

layering_transitive / layering_qgs_audit / qgis_spread(+strict) / ui_size(+strict) /
file_budget(+strict) / ui_tokens(+strict)

### 既有二进制失败（7 项，EXISTING_BASELINE_FAILURE——master 基线即红，非本轮引入）

| 测试 | 标签 | 备注 |
|---|---|---|
| tst_background_open | workflow | |
| tst_ui | core envdebt | envdebt_fixed 口径 |
| tst_assetpaging | ui | **SEGFAULT** |
| tst_ingestplan | core | 基线即红（本轮 A-03 改动须对照不致恶化） |
| tst_correlation_async | ui | |
| tst_derivationgraph | ui | |
| tst_petrophysbatch | services | |

另：known-reds 在册红（tst_cache_las / tst_singlefactor_perf 本机复测仍红，条目有效）。

## 结论

master 闸门回红（G1 簇）是本轮最高优先级修复对象；7 个二进制红为存量债
（多数挂 envdebt/known-reds），本轮如实分账不冒领也不放过新回归。
