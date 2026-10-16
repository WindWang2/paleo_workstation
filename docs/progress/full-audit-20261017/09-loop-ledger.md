# Full Audit 20261017 — Goal-Loop 台账

格式：每轮 Inspect → Hypothesis → Change → Verify → Independent Challenge → Record → Next。

## Round 0 — 启动（2026-10-17）

- Finding IDs: —
- Changed Files: docs/progress/full-audit-20261017/{00-baseline,01-conflict-matrix,09-loop-ledger}.md
- Verification: BASE_SHA=c720aef5 锁定；33 issues + PR#506 + 4 活跃分支排除矩阵建立；基线构建进行中
- Review Result: —
- Remaining Work: 5 路审计 subagents 运行中；基线构建/测试
- Next Action: 聚合 findings → 04-finding-register → 05-priority-and-fix-plan → Fix 1

## Round 1 — Deep Review + Fix 1（2026-10-17）

- Finding IDs: F1–F17（见 04-finding-register.md；收敛对 C-01≡D-03、B-01≡C-02、D-05≡E-03）
- Changed Files:
  - F1: src/workflow/mappingworkbench.{h}、mappingevidencereview.cpp、mappingworkbench_impact.cpp（AttributionPin 会话期禁驱逐 + cache 测试钩子）
  - F2: src/algorithms/velocitymodel.cpp（∂T/∂k 补 1/k）
  - F3: src/catalog/datacatalog.{h}、datacatalog_derivation.cpp、datacatalog_versions.cpp（级联 excludeId）
  - F4: src/io/constraintstore.{h,cpp}（事务前独占 checkpoint + restoreFileFromBackup 三件还原 + 连接先关）
  - F5: src/workflow/surfacegridding.cpp（supercover 统一）
  - F6: src/ui/pages/mappingworkbenchpage.{h,cpp}、src/ui/paleomainwindow_workbench.cpp（三按钮门控 + 影响就绪回喂 + 等待光标 + 逐条失败上屏 + 报告缺失文案）
  - F7: src/services/seismictaskservice_slice.cpp（JoinWorkers RAII）
  - F8: src/algorithms/geostat/{sgs.cpp,sgs3.cpp,neighborhood.h}（确定性次键）
  - F9: src/io/geojsonaffine.cpp（500MB 闸 ×2 入口）
  - F10: CMakeLists.txt（tst_wellsiting_kernel 挂 perf/RUN_SERIAL）
  - F11–F17: constraintwellfactors.cpp（死代码）、inversionworkflow.cpp（均值分母）、seismichorizoncluster.cpp（nodata float 口径）、paleotaskservice.cpp（percent/ETA 钳制）、qgisconstrainteditsession.cpp（快照单次扫描）、run-target-tests.sh（mktemp）、tst_singlefactor_constraintreceipts.cpp（测试名/域注释/补左边缘）
  - 新测试：tst_velocitymodel（最小二乘 oracle）、tst_catalog（出生不 stale）、tst_composeattribution（10 输入 pin）、tst_singlefactor_store（BUSY 拒绝 + WAL 还原）、tst_surfacegridding_thread（贴边屏障）、tst_mappingworkbench（三按钮门控）
- Verification: 基线构建绿 + 基线 ctest 412/415（3 既有红：startup_trace/evidencee2e/shell_projectlifecycle，见 06-baseline-quality.md）；Fix 1 增量重建 + 定向测试进行中
- Review Result: 待 Independent Review 2（未参与修复的 subagent）
- Remaining Work: F18–F21 视容量；Review 2 → Fix 2 → 终审 → PR
- Next Action: 重建通过后跑定向测试 → 分簇 commit → 启动 Review 2
