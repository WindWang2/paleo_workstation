# 优先级与修复计划（full-audit-20261018）

## 基线构建/测试（回填）

- configure：RelWithDebInfo + superbuild prefix，首轮漏 vendor include 后二次 configure 恢复
  （BUILDING.md 已知接线问题），BUILD_EXIT=0（/tmp/paleo-audit/baseline-build.log）。
- 基线 ctest：见下「基线测试结果」。

## 修复优先序（Phase 3 执行）

| # | Finding | 级别 | 修复簇/commit | 回归测试 |
|---|---|---|---|---|
| 1 | FC-001 | P1 | fix(workflow): 预测 INPUT 冻结 URI 快照 + worker 自建层（照 runFactorAlgorithm 范式） | tst_workflows 新增「prepare 后 params 无活层 + 全链绿」 |
| 2 | FB-001 | P2 | fix(algorithms): smoothMajority 并列取小码（对齐 GeologicalSmoothing 确定性口径） | tst_polygonize 双 seed 逐位一致 + 全链 sha |
| 3 | FA-001+FA-002 | P2+P3 | fix(io): 工作簿累计格数预算（4M 格）+ .xml 大小闸（512MB，zip 层同口径） | tst_io_outsource + tst_wellcomposite 构造放大夹具 |
| 4 | FB-002+FB-003 | P2+P3 | fix(algorithms): SGS/sgs3 并行 worker 工作集计入峰值预算（收缩 workers，资源面不动语义）+ sgsfill 聚合输出闸 | 预算函数单测：{R=4,cells=1e8} 收缩、{R=64,cells=8.4M} 放行、fillSgs 超预算拒收 |
| 5 | FD-001 | P2 | fix(ui): EvolutionPlayerPanel::reloadFrames() + onProjectOpened 同点位调用 | tst_panels/tst_ui：自定义词表 reload 后 frameOrder 跟随 |
| 6 | FA-004 | P3 | fix(catalog): loadTables ord 偏离已读行数 +4096 判损坏行跳过 | tst_catalogstore ord=2e9 行被跳过 |
| 7 | FA-003 | P3 | fix(io): scanParallel 区分取消/读错误——读错误不 checkpoint、回落顺序扫描 | tst_segy truncate-during-scan 注入 |
| 8 | FC-002 | P3 | fix(workflow): 同步 saveVersion retain 失败改 errorOccurred/qWarning 披露，不再写 *error | tst_versions/tst_mapsnapshot |
| 9 | FC-003 | P3 | fix(workflow): publish legacy 来源 commit 前 size+mtime 指纹复验 | tst_mapsnapshot legacy 变更拒发 |
| 10 | FD-002 | P3 | fix(qgis): PreviewMeasureTool::deactivate 隐藏 band | tst_datapreview_map 切工具 band 不可见 |
| 11 | FD-003 | P3 | fix(qgis): setLayers 置换摘除旧层三层记账 | tst_datapreview_map 记账不残留 |
| 12 | FD-004 | P3 | fix(qgis,ui): 属性表宿主注入 ask seam（缺省回落裸框），壳侧接 PaleoNotify::askSaveDiscard | seam 无头用例三分支 |
| 13 | FE-001 | P3 | fix(tests): 永真断言改全部枚举往返+期望字面量 | 变异验证（临时改实现应红） |
| 14 | FE-002 | P3 | fix(tests): tst_perf 两槽位绑生产真值（workerPool()->maxThreadCount()==4；gridDimsForExtent 真策略） | 变异验证 |
| 15 | FE-005 | P3 | fix(build): 删死 translations.qrc + extra-ux.cmake 注明唯一嵌入点 | configure 无回归 |
| 16 | FE-004 | P3 | fix(io): LibreOffice profile 改 QTemporaryDir（服务存续期保活、自动清理） | 转换后 /tmp 无 paleo-lo-profile-* 残留 |
| 17 | FE-003 | P3 | fix(tools): AppImage runtime 钉 20251108 + sha256 校验（2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d） | 脚本静态自检（URL 非 continuous + 校验行在场） |

记录不改：FC-004（落点在主仓未提交导出簇 attach_compose/composepage）、FD-005（#481+#518 领地）、
S-1/2/3（SUSPECTED）。

## 基线测试结果（2026-10-18 实测）

- ctest 全量 421 项：**420 通过 / 1 失败**（`tst_catalog`）——失败项隔离复跑
  **通过**（`ctest -R '^tst_catalog$'` 100%），判定为 -j4 并发下的既有 flaky
  （EXISTING_BASELINE_FAILURE·flaky-under-parallel，与 #501 绿灯通胀主题相邻，
  不归本轮修复）。总时长 1168s（-j4，本机）。
- 结论：基线绿（无确定性红项；历史文档化红 golden/perf 项在本轮全量中均通过）。
