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

## Round 2 — Review 2 + Fix 2（2026-10-17）

- Finding IDs: R2-01…R2-17（review-r2.md）
- Changed Files: 见 07-fix-r2-results.md（commit 1f1b1d15）
- Verification: SHOULD_FIX 4 全处置（3 修复 + 1 流程记录）；DOCUMENT_ONLY 中 5 项廉价收口；定向测试全绿
- Review Result: 独立复审证实 Fix 1 核心修复（F1/F2/F5/F8/F17 数学与语义独立验证）
- Remaining Work: D-02 影响分析异步任务化（#494 同步重 IO——本轮只做了门控+等待光标+RAII，
  完整 PaleoTask 化递延）；F12 反演统计回归测试（需体级夹具）；F18-F21 视容量递延
- Next Action: 独立终审 → 全量回归 + 门禁 → PR

## Round 3 — 独立终审 + 收口（2026-10-17）

- Finding IDs: FR-01…FR-08（08-final-independent-review.md）
- Changed Files: constraintstore.cpp / mappingevidencereview.cpp / 两个测试文件（commit 见 git log）
- Verification: 全量 ctest 414/415（唯一红 = tst_evidencee2e 基线已知 flaky，基线轮同红）；
  四项质量门禁绿（layering --strict / ui_invariants --strict / i18n / ui_tokens --strict）；
  终审收口重建 + 定向复跑绿
- Review Result: PASS_WITH_NOTES——前两轮结论未被推翻；FR-01/02/05 已收口；
  FR-03/04/06/08 记录不修；FR-07（PR#507 重叠）入 PR 描述
- Remaining Work: D-02 异步化、F12 体级测试、F18-F21、issue 族整改（外部持有）
- Next Action: 重新对账 master/PR → push → PR
