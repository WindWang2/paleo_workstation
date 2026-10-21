# Full Audit 2026-10-22 — 07 Fix 2（R2 复审意见处理结果）

R2 报告：06-independent-review-r2.md（MUST_FIX 1 / SHOULD_FIX 4 / DOCUMENT_ONLY 10 /
FALSE_POSITIVE 0）。逐项处置：

| R2 项 | 原始问题 | 二次修改 | 测试证据 | 最终状态 |
|-------|---------|---------|---------|---------|
| R2-M1 | C-8 重构丢 `!path.isEmpty()` 守卫：无测井井 explicitAxis 误翻转，整批预测报 MD 错、破坏 depth_mock 回退 | mappingworkbench_predict.cpp 恢复守卫 + 注释 | tst_mappingworkbench 新增 predictionWellWithoutLogFallsBackToMockDepth（无测井井 predict 零 errorOccurred；修前该用例红——errors=1 含 MD） | FIXED(75f3a54e) |
| R2-S1 | C-9 五条拒收路径仅覆盖部分 | 二选一取「改判结转」：wellsection/mappingworkbench 基准面已有既有覆盖（unifiedCurveBodiesRequireMdAndUnits / predictionPreparationRejectsNonMdText），本轮新测 predictionLog/mdAxis/petrophys 三面；propertymodelworkflow/sectionworkbench 记独立测试债（登记册已注明） | — | 结转（登记册注明） |
| R2-S2 | requestProfile 空回调产生空「剖面 N」图例项；Histogram/Profile 失败无测试 | tabhorizon/tabmapproduct 空样本早退；rasterRequestFailureStillCallsReady 扩四路断言（Summary/Extrema/Histogram/Profile 恰一次 + 空载荷） | tst_previewmap_assets 通过（97s） | FIXED(75f3a54e) |
| R2-S3 | readCurveTvd legacy 只认 FT/F/M/空，与 mdAxisInMetres 词表分裂（METERS/FOOT/FEET 一侧可用一侧拒） | legacy 链改 depthScaleToMetres（空=米惯例、别名可折算、未知非空拒收） | tst_welllogset_tvd 全绿（既有 FT/M 用例回归）+ 本轮 B-02 用例 | FIXED(75f3a54e) |
| R2-S4 | qgis-spread-baseline 头注「条目数：149」漂移 | 改「初冻 149；E-2 扩面后现值 172」 | check_qgis_spread 复跑（Phase 7） | FIXED(75f3a54e) |
| D1 | tst_batchjobqueue 优先级注释过时（3/4 参已默认 Normal） | 注释同步 | 编译通过 | FIXED(75f3a54e) |
| D2-D10 | 文档级备忘（空单位双口径、混合分隔符、Low 无老化、理论 flake 窗等） | 不改代码；07/登记册留档，收敛批次引用 | — | DOCUMENT_ONLY |

## Fix 2 验证

- 构建：EXIT=0（flock -j4）。
- 定向 7 套件：tst_mappingworkbench / tst_previewmap_assets（含新四路失败断言）/
  tst_welllogset(_tvd)（词表对齐回归）/ tst_batchjobqueue / tst_wellsection
  全绿；tst_datapreview 1 红与纯净基线同签名（wellLogPreviewShowsCuttings
  LithologyTrack——纯净 b301f2e8 复现在案（见 06-baseline-quality），属基线
  既有红而非本轮引入；known-reds.json 未登记该条，随基线红清账批次处理）。
- 全量回归与闸门复跑见 08-final-independent-review.md 前的 Phase 7 记录。
