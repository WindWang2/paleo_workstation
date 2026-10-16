# Full Audit 20261017 — 07 Fix 2（Review 2 结果处置）

Review 2 报告：`findings/review-r2.md`（快照；原始 /tmp/audit-findings-20261017/review-r2.md）。
结论总览：**MUST_FIX 0 / SHOULD_FIX 4 / DOCUMENT_ONLY 7 / VERIFIED-OK 5 / EXTERNAL_OWNED 1**。
复审者独立推导证实了 F2 雅可比公式、F8 严格全序、F5 supercover 参数映射、F17 语义恒等、
F7 守卫覆盖，并实测 check_layering --strict 通过。

## 逐项处置

| R2 ID | 级别 | 处置 | commit |
|---|---|---|---|
| R2-01 compareVersions 缺 pin | SHOULD_FIX | ✅ 已修（补 AttributionPin + 更正反查 pin 注释） | 1f1b1d15 |
| R2-02 F9 缺承诺测试 | SHOULD_FIX | ✅ 补 rejectsOversizedInputBeforeReading | 1f1b1d15 |
| R2-03 还原删除次序 | SHOULD_FIX | ✅ 次序倒置（先伴生件后主文件） | 1f1b1d15 |
| R2-04 与主 checkout 未提交簇文件重叠 | SHOULD_FIX(流程) | ✅ 本文档记录 + PR 描述声明：实测 hunk 不重叠（主 checkout :307 一行/文件尾 vs 本分支 :784-880/测试中部），自动合并可行；如持有会话继续写入同区须人工协调 | — |
| R2-05① NaN 进中文串 | DOCUMENT_ONLY | ✅ 顺手修（「无成功道」） | 1f1b1d15 |
| R2-06 pin 后缓存不回落 | DOCUMENT_ONLY | ✅ 顺手修（pin 归零裁剪回 8 + 测试断言释放后 ≤8） | 1f1b1d15 |
| R2-07② 光标非 RAII | DOCUMENT_ONLY | ✅ 顺手修（WaitCursorGuard）；③ D-02 异步任务化正式记入 09-ledger Remaining | 1f1b1d15 |
| R2-08 F13/F14 零测试 | DOCUMENT_ONLY | ✅ 补两测（F12 反演统计测试仍缺——需体级夹具，记 Remaining） | 1f1b1d15 |
| R2-09 半开注释残留 | DOCUMENT_ONLY | ✅ 顺手修 | 1f1b1d15 |
| R2-10 neighborhood 注释不精确 | DOCUMENT_ONLY | 记录不修（行为正确） | — |
| R2-05② F12 无测试 | DOCUMENT_ONLY | 记录（Remaining） | — |
| R2-13 尾注 assetops 断言未加 | 记录 | addVersion 级测试已覆盖根因 | — |
| R2-17 inversionworkflow:594 join | EXTERNAL_OWNED | 归 codex/audit-remediation | — |

## Fix 2 验证

- 构建：绿（-parallel 4；一次 collect2 段错误系本机资源压力，-j2 重跑通过）
- 定向测试：tst_composeattribution（含 outer pin/裁剪断言）、tst_batchjobpanel（F14）、
  tst_seismichorizoncluster（F13，TimeRaster 路径首覆盖）、tst_import_geoaffine（F9）、
  tst_singlefactor_store、tst_mappingworkbench、tst_impactrecompute——**全部通过**
