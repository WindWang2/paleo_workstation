# goal-loop ledger — opt-05 QGIS 图层/编图/证据/版式/验证发布深化

- 分支 `goal/paleo-opt-05-qgis-compose-publish-20261016`，BASE `dbb03269`（origin/master @ 2026-10-16）
- worktree `/home/kevin/projects/paleo_workstation-opt-05-qgis-compose-publish`，独立 `build/`，vendored prefix 只读符号链接
- 资源纪律：编译一律 `flock $GIT_COMMON_DIR/paleo-shared-build-j4.lock` + `-j4`；subagents ≤5（S1–S4 探查 + S5 独立 review）
- 排除：Open Issues #460–#485 全部；Open PR #486（welllogfacies/seismic3dviewpanel/tst_wellfacies）、#487（qgisstyleservice.cpp/tst_services2.cpp）；指纹明细见 `docs/progress/opt-05-qgis-compose-publish-20261016.md` §R0
- skill 核查：`goal-loop`/`goal` skill 未安装于本机（`~/.claude/skills` 无 goal 目录），按任务书「不因缺失停主任务」执行同等纪律：客观 Oracle、逐轮账本、关键断言复验

## R0 证据账本（可观察现行行为，file:line）

| # | 行为 | 证据 |
|---|---|---|
| B1 | 地图册索引页/manifest 写失败只记 `failures[]`、不计 `result.failed`，`Result::summary()`「失败 N」与明细不一致 | `src/workflow/mapbookqueue.cpp:517-531`（failures.append 无 `++result.failed`）、`:155-159`（summary 用 failed 字段） |
| B2 | `validationworkflow.cpp` 顶部 include 块逐字重复（qgislayerservice/paleoprojectstore/projectdata/QVector ×2） | `src/workflow/validationworkflow.cpp:5-20` |
| B3 | 验证工作流同步路径 `startValidation` 无任务服务时直连 `validate()` + `validationFinished(true)`；异步路径冻结声明快照+mutationSeq 代次守卫，过期结果 `discardPublication` | `src/workflow/validationworkflow.cpp:252-330` |

（S1–S4 审计结果并入后继续追加）

## 落地单元（R2–R3，均带 Oracle；明细见 progress doc §R2）

U1 发布清单 title 往返 / U3 corrupt-manifest diff 门禁（releasestore.cpp）；
U2 地图册装配失败计入 complete()/summary()（mapbookqueue）；
U6 clearResults 清孤儿 cellWidget（validatepage）；
U4 fuseFactors 空层位/跨层位输入拒绝（compositionworkflow）；
U8 综合页目标/清单按 activeHorizon 收窄 + 壳侧跨层位完成不接管
（composepage + attach_compose；SelectionContext::activeHorizonChanged 喂页）；
U5 ensureSharedDataVisible 写「被应用的主题」而非 m_currentPage（qgislayerprofile）；
U7 resolveLayer 原生 QgsMapLayer::id() 兜底（layerpropertiesdialog）；
U9a organizer 批量路径共享 declared() 快照（qgislayerorganizer）；
U10 moveCurrentNode 同步 customLayerOrder 不打平置顶 pin（layertreepanel）；
U9b buildHorizonMapLayout 活图层样式快照挂布局还原（layoutexport）。

## 验证记录

- 定向测试（flock + ctest -j2）：11/11 通过——tst_release, tst_mapbook, tst_panels,
  tst_workflows, tst_mappingpages, tst_composeworkflow, tst_layerplatform,
  tst_layerproperties, tst_layertreepanel, tst_mapping, tst_layerorganizer。
- 门禁：diff --check 干净；layering/strict/selftest 过；ui_invariants/strict 0 违规；
  i18n/selftest 过；ui_tokens violations=0。
- 验证期修复 2 处自引入回归：attach_compose 空 activeHorizon 误拦（悬垂指针
  SIGSEGV 于既有用例）；新用例未清单例工程污染。
- S5 独立 review 完成：1 MAJOR（U10 拖拽 InternalMove/组移动不同步——已补
  rowsInserted 收集 + 排队回填 + 组子树逐层回填，新增 dragMoveSyncs… oracle）
  + 4 MINOR 修复（标注快照泄漏、因子清单口径、首建主题愈合、manifestCorrupt
  标记）+ 2 MINOR 登记为已知限制 + NIT 文档对齐。
- 终验：定向 11 测试双跑 11/11 ×2 全绿；diff --check / layering(+strict) /
  ui_invariants / i18n 门禁全过。

## 反向验证（每轮提交前执行）

- `git diff origin/master...HEAD --name-only` ∩ PR#486/#487 files = ∅
- 改动 hunk ∩ Issue 指纹表（§R0 of progress doc）= ∅
- 主 checkout `git status` 干净（除既有 untracked `SWARM_REVIEW_2026-10-15.md`）

## 轮次

- R0–R4 完成（见 progress doc 轮次日志）；R5：S5 review → 终验 → push + PR。
