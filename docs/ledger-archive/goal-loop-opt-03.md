# goal-loop 账本 · opt-03 单因素取数/约束线绘编/插值与成果质控（2026-10-16）

## R0 基线冻结

- 基线 `BASE_SHA = dbb03269ca1809145e1ee39b699c6fd08916a317`（= fetch 后 origin/master，与任务快照一致）
- worktree：`../paleo_workstation-opt-03-singlefactor-constraint`，分支
  `goal/paleo-opt-03-singlefactor-constraint-20261016`（唯一推送分支）
- 主 checkout 零改动承诺：所有写入仅在 worktree。
- vendor 接线：`vendor/superbuild/prefix`、`vendor/onnxruntime` symlink 指主仓（只读共享，
  不清理不重建）；configure 命令：
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix`
  （QScintilla 头缺失 warning 为 BUILDING.md 已知 Linux 允许降级）
- 编译纪律：全部 `-j4` 且持跨 worktree 锁
  `/home/kevin/projects/paleo_workstation/.git/paleo-shared-build-j4.lock`。
- skills 检查：goal-loop / matt / gstack / planning-with-files 均未安装（~/.zcode、项目 .zcode
  无命中），按任务书「不要因为不存在而停止主任务」直接执行任务文本协议，客观 Oracle +
  逐轮证据账本（本文件）。
- 首次全量构建：vendor/sbm Sdk.cpp 汇编器内部段错误（gas 崩溃，瞬时故障）→ 重试构建进行中。

## 排除矩阵（EXCLUDED_ISSUES / PR 指纹）

- Open Issues #460–#485（26 个，gh 实测清单与任务快照一致）全部排除；清单类 Issue
  #476/#478/#480/#482/#483 已读全文，逐条指纹进入候选自查。
- Open PR #486（codex/predict-fallback-gl-watchdog：welllogfacies.cpp、seismic3dviewpanel、
  tst_wellfacies）与 #487（codex/facies-legend-groups：**qgisstyleservice.cpp**、tst_services2）
  文件级排除，不碰不改不 cherry-pick。
- 已合并交付视为现状（不许重新包装）：#451 D1–D12、#457 A1–A15/B1–B10/C1–C8/D1–D4
  （docs/progress/singlefactor-deep.md 全表 + parallel-7/04-pr-description.md）。
- 写域自查：src/qgis/ 内只允许约束专属文件；factorcontour.cpp 属因素等值线服务——
  本任务**不改**该文件（FIXED_LEVELS 上限在 UI/工作流两层落地），记入取舍。

## R2 改善 backlog（四路只读探查 → 主 Agent 抽查证据核实）

四路探查（S1 取数/S2 绘编/S3 插值/S4 成果等值线）共产出 24 个候选；主 Agent 对
load-bearing 证据逐一 sed 复核（快照守卫、structural 吞错、clamp、刷新时机、FIXED_LEVELS
capInterval=0 均属实）。入选 10 个单元（按文件族互斥分组），未入选候选与理由见文末。

### 入选单元

| # | 单元 | 域 | 文件族 | 证据锚点 |
|---|------|----|--------|----------|
| U1 | 采样快照守卫覆盖测井矢量属性表（well-log-attributes.gpkg 活读 → 快照可污染） | S1 | workflow/constraintwellfactors.cpp | :528-531 frozen.paths 只含因子表；worker 经 wellsectionworkflow.cpp:562-563 活读 intervals 表 |
| U2 | 井点图层同井重复要素/规范化井名碰撞（双计或静默吞井） | S1 | 同上 | :156-186 first-wins 索引无检测；属性表侧 :187-190 已是硬失败口径 |
| U3 | structural_idw POWER≤0 静默全 NaN / extend 模式误导错误（吞 evaluateAt InvalidInput） | S3 | algorithms/singlefactor/structural.cpp + structuralalgorithm.cpp | structural.cpp:442-455 无 else；structuralalgorithm.cpp:451 无校验；姊妹引擎 :537/:911 均拒绝 |
| U4 | 参数钳制回执诚实化：local_kriging K 钳到 [1,64] 无回执；地统显式 nugget<0/ratio<1 静默钳制无 provenance | S3 | localidw.cpp + localdirectionalgorithm.cpp + constraintfactorjobs_geostat.cpp | clampKrigingParams :28-41；QC 记请求值 :124；geostat :490-499 max(0)/max(1) |
| U5 | structural QC 完整性：参数指纹漏 SEARCH_RADIUS 等数值生效参数；counts 缺 finite/nodata → 血缘恒 null | S3 | structuralalgorithm.cpp + constraintfactorjobs_factor.cpp(只读血缘段) | :683-693 指纹键集；:667-670 counts；:932-935 血缘读键 |
| U6 | 约束 reason 词表与主链显示：unknown_type:line 裸 token、other_horizon/ignored 等无中文标签 | S2 | ui/pages/constraintpage_constraints.cpp | labels :414-435；域层 token constraintsemantics.cpp:156/671/695/585/577 |
| U7 | 解释性等值线/制图工作场发布后成果表不刷新 + savedFactorResults 漏 cartographic_work 行 | S4 | ui/pages/constraintpage_sections.cpp + workflow/constraintwellfactors.cpp | sections :149-161 只 contoursGenerated 刷新；wellfactors :894-904 kind 白名单漏 |
| U8 | 等值线类成果 lineage 第 4 列内容（等值距/级别/SHA/条数）+ 解释性等值线版本缺 analysis_sha256 | S4 | constraintwellfactors.cpp(lineage) + constraintfactorjobs_contours/interpretive.cpp | lineage :817-879 只读 raster 键；interpretive :629-638 无 SHA；0 条也发布无计数 :277-291 |
| U9 | FIXED_LEVELS 级别数无上限（#149 自动/间距路径的对偶缺口） | S4 | ui/constraintpage_internal.h + workflow/constraintfactorjobs_contours/interpretive.cpp | factorcontour.cpp:186-207 capInterval=0（服务层不改，双层闸） |
| U10 | 因子表每勾选一次全量重取井点字段（GUI 线程全图层扫描，无缓存） | S1 | ui/pages/constraintpage_wellfactors.cpp | :103-109 itemChanged→refreshWellFactorFields；:258-284 全量取数 |

### 文件族所有权（并发互斥）

- S1-impl：workflow/constraintwellfactors.cpp、ui/pages/constraintpage_wellfactors.cpp、
  tests/tst_factorworkflow.cpp（U1/U2/U7-wf 部分/U8-lineage 部分/U10）
- S2-impl：ui/pages/constraintpage_constraints.cpp、tests/tst_constraint_draw.cpp（U6）
- S3-impl：algorithms/singlefactor/{structural,structuralalgorithm,localidw,localdirectionalgorithm}.cpp、
  workflow/constraintfactorjobs_geostat.cpp、tests/tst_singlefactor_{structural,kriging}*.cpp、
  tst_geostat_workflow.cpp（U3/U4/U5）
- S4-impl：ui/pages/constraintpage_sections.cpp、ui/constraintpage_internal.h、
  workflow/constraintfactorjobs_{contours,interpretive}.cpp、tests/tst_singlefactor_{contours,asynccontour}.cpp（U7-UI/U9/U8-jobs）
- 主 Agent：协调、复审、账本/进度文档、PR；不与 subagent 同时编辑同一文件。

### 未入选候选（记录理由）

- S1-4 百分号单位污染：涉及「% 剥离是否换算」语义决策，风险高（会改变既有单格式列输出
  量级），留待用户决策后另做；不在本 PR 偷改口径。
- S1-5 字段语义标注（坐标/代码列当指标可选）：产品口径未定（剔除 vs 标注），最小改动
  只加标签价值有限，递延。
- S1-6 layer_id 双插死代码：一行修复但血缘键语义变化（extra["layer_id"] 从 product.<vid>
  变 wellfactor.*），可能有隐性消费方，需单独排查消费面，递延。
- S2-2 undo/redo 伴随视图重同步：真实缺口，但 undo/redo 高频触发 + 全量 store reload 的
  性能面需设计（仅类型集变化才同步），本轮不做（避免大改会话层）；记录为后续候选。
- S2-3 双控制器僵尸 active/teardown 误杀：真实，但修法涉及 canvas mapToolChanged 重入
  设计，风险中高，递延。
- S2-4 约束层 parent-recursive reveal：产品决策（用户显式隐藏组是否强揭），递延。
- S2-5 captureFailed 页面反馈：低价值纯 UI，本轮不占单元。
- S2-6 回显测试加固：有价值但属测试加固，若 U6 顺手附带，不单列。
- S3-4 三引擎 LOO 交叉验证：最有价值的后续增强，但每折重建引擎（方向核/软边界/屏障栅格）
  的成本/阈值设计需专门轮次，本轮不做半成品，列为下一轮首选。
- S4-5 等值线「基于旧版等值面」陈旧标记：判据（最新 factor 版本号）需对齐 manifest 代次
  语义，单独轮次做。

## R1 运行与测试基线

- 全量构建：首次在 vendor/sbm Sdk.cpp 遭汇编器内部段错误（gas 瞬时故障）；重试后因
  同机 opt-02 任务持全局构建锁而排队（跨 worktree 锁按设计串行化，非本任务问题）。
- 干净基线定向测试（主 checkout build @ dbb03269，Debug，ctest -j1，域内 15 测试）：
  14 通过 / 1 失败——`tst_singlefactor_kriging::gridScaleRatioGate` 300s 看门狗超时。
  定性：环境性基线红——该用例跑 128²+256² 网格 kriging 并断言耗时扩展比 ≤8，Debug
  无优化 + 同机 opt-02 正持锁 -j4 构建占满 CPU；功能性用例（exactAtSamples/变差拟合/
  屏障分厢/回退诚实）全部通过。待本 worktree RelWithDebInfo 构建完成后复核；若仍红
  再按 #475 口径分诊。不因基线红放宽任何断言。
- 其余 14 个域内测试全绿（tst_factorworkflow、constraintreceipts/constraintuse/
  constraintcrs 类、contours/asynccontour、structural/structural_unit、geostat_workflow、
  constraintediting、constraint_draw、mappingpages、constraintevidence、acquisition、samples）。

## 轮次记录

### R3 实现（四实现 agent + 主 Agent 补遗）

- S1-impl（5 单元）：U1 快照守卫 + maintain 同修；U2 重复井硬失败/名碰撞拆行；
  U7 sections 刷新 + cartographic_work 行；U8 lineage 读侧；U10 字段缓存。
  tst_factorworkflow 38/38（1 skip 为既有环境门）、tst_mappingpages 全绿、
  回归 6 目标全绿（含 tst_wellattributes/tst_panels/tst_ui）。
- S2-impl（U6）：词表 6 token + 前缀处理 + 未类型化计数 + structural 开关属性注入
  （默认 true 零行为变化，喂唯一解释器 planConstraintUse）。tst_constraint_draw
  9/9；其曾做基线对照实验（临时覆写后恢复），主 Agent 复核 diff 完整无残留。
- S3-impl（U3/U4/U5）：POWER 入口 throw + else 传播；K 钳制 issue 回执 +
  kriging_max_points_effective（仅钳制时）；geostat nugget/aniso clamped provenance；
  structural QC parameters +9 键（resolve 后生效值）+ counts finite/nodata。
  4 目标全绿；golden 无需改期望；power=2 数值回归逐位不变；K=64 vs K=100 逐位一致。
- S4-impl（U8 写侧/U9）：contourFeatureCountOf（OGR 只读计数，失败 -1 哨兵不改
  发布语义）；两类等值线版本 extra 补 contour_feature_count，解释性补
  analysis_sha256；parseLevels 上限截断+dropped；两个 prepare 闸（>10000 拒绝）。
  tst_singlefactor_asynccontour 14/14；factorworkflow/faults 回归绿。
- 主 Agent 补遗：U9 UI 层测试 parseLevelsAboveCapCountsDropped（S4 遗留——
  tst_mappingpages.cpp 当时为 S1 所有权）。过程中发现并记录：QLineEdit 默认
  maxLength=32767 会先截断超长粘贴（紧凑 token 才能真正越过 parseLevels 上限）。
  tst_mappingpages 47/47 全绿。
- 主 Agent 复审：全部源码 diff 逐 hunk 过（U2 状态机推演、U1 路径与
  wellattributestore.cpp:35-37 对照、姊妹引擎 counts 口径对照、GDAL 句柄配对、
  键契约两侧一致性、S2 文件完整性）——未发现阻断问题。

### R1 补记

- RelWithDebInfo 构建下 tst_singlefactor_kriging 19/19（22s）——`gridScaleRatioGate`
  基线红确证为 Debug+CPU 争用环境因素，非功能缺陷。

### R4 收官 Oracle（第一遍）

- 域内定向 21 测试：21/21 全绿（ctest -j2 持锁）。
- 静态检查：`git diff --check` 干净；`check_layering.py --strict` 无违规；
  `check_ui_invariants.py --strict` clean；`check_i18n.py` 通过。
- 全量 ctest（build 全目标 RelWithDebInfo）：**408/408 全绿**（含全部域外目标），
  总耗时 1086s——较 #457 时代记录的「399/407、8 个基线红」更干净（本机本构建
  无已知红项；#475 所列红项在本环境未复现）。
- 提交：5 个主题 commit（361b5a78 / 393b5c53 / 91567c96 / 1a2ddde9 / d05fbd2d），
  docs 待 S5 审查结论后随终稿一并提交。
- 主 Agent 第二遍自查（S5 前置作业）：claimedBy/byId/byName 状态机三井组合手工
  推演通过；parseLevels 与 prepare 闸在恰好 10000 条自洽；-1 哨兵读侧无误报路径。

### 待办

- S5 独立非作者审查：首次派出因账号 5 小时用量上限失败（04:08 触顶，
  06:30:26 重置）——07:20 重派成功，审查对象为 5 个 commit 的累计 diff。

### R5 S5 独立审查 + 作者修正 + 二次复审（Review→Fix→Review 闭环）

- S5 首轮结论：**需修后可合**——1 Med（U10 缓存键不覆盖既有 wells 图层就地
  编辑的失效路径：不换声明、不发 wellAttributesChanged、showEvent 命中缓存
  不能自愈）+ 3 Low（invalid_params 前缀 detail 用 section(1) 会被未来含冒号的
  detail 截断；裸 invalid_params 回退回显英文 token；contourFeatureCountOf 两
  文件逐字复制）+ 若干 Info（路径常量双写、wf 身份不在键内等）。其余全部
  核过：状态机三井推演、10000 边界自洽、-1 哨兵读侧、maintain 删除文件口径、
  线程亲和、历史兼容、伪断言抽查 4 例改前红成立、夹具真实性（asynccontour
  新例走真实 prepare/compute/publish + 独立 GDAL 对照）。
- 作者修正（commit 8dc14c65）：缓存键每个 wells 声明条目追加 source 文件
  size+mtime（与 U1 快照守卫同配方）；两处 section(1,-1)；裸回退改词表；
  helper 收编 constraintfactorjobs_internal.h（inline 防跨 TU 重复符号）；
  冗余 include 清理。新测试 wellFactorCacheInvalidatesOnLayerFileEdit
  （size 变化保证对 mtime 毫秒碰撞免疫，改前必红）。
- S5 二次复审结论：**可合**——四处修正全部闭合、无新引入问题；残余仅
  「程序化同毫秒等长改写」理论窗口（无生产路径，与全项目守卫配方同界）。
- 修正后验证：tst_mappingpages 48/48、tst_constraint_draw、
  tst_singlefactor_asynccontour 全绿；静态四件套通过；15 项域内关键测试
  修正后复跑 100%（15/15）。

### R6 终验

- S5 修正后全量 ctest：408/408 全绿（与修正前同绿）。
- 提交序列：361b5a78 → 393b5c53 → 91567c96 → 1a2ddde9 → d05fbd2d → 8dc14c65
  （6 个 commit：5 实现 + 1 审查修正）。

（逐轮追加）

## 交付

- PR：https://github.com/WindWang2/paleo_workstation/pull/492（OPEN，goal/paleo-opt-03-singlefactor-constraint-20261016 -> master，标题以 [opt-03] 开头）。只提交 PR，未等待线上 CI，未合并。
- 主 checkout 零改动核验：HEAD=dbb03269，无 tracked 文件改动。
- 未触碰任何 Open Issue（#460–#485 全程零评论/零关闭/零标签）、Open PR #486/#487 文件、分支保护与 CI 配置。
