# P3 单因素与约束插值的单元身份和证据发布（2026-11-19）

任务分支 `codex/paleo-p3-factor-publication-20261119`，基线 `origin/master =
08ca3e52b829f928d38e34d3f7fa3e58376f6076`（2026-11-19 快照）。
方向 B｜地质分析身份与成果发布；覆盖 #972 的单因素/插值/constraint.idw 子集
及 #1004/#1005 的当前剩余缺口。不宣称修完 #972 的预测/编图/地图版本链。

## 一、开工核对（scope ledger）

| 项 | 结论 |
|---|---|
| 开放 PR | 0（2026-11-20 复核）；#913/#912/#914/#915/#932/#933/#1038/#1042–#1048 已在基线内，只复用不重做 |
| 并行任务 | P2（seismic cache，src/io/*）、P6（mapversion）worktree 已建，文件集与本任务无交集；协调目录 `paleo-parallel-20261119/` |
| 依赖前缀 | 主机无任何已准备受控前缀戳（.paleo-deb-closure/.paleo-superbuild-base 均无）。旧 superbuild prefix 无 V4 收据。按 deb 闭包路线（加速档）以 08ca3e52 的 fetch-deps.sh 准备共享前缀（glibc 2.44 ≥ 2.43 满足运行时）；dpkg-deb 缺失 → ar+tar shim（~/paleo-shims）。前缀落中立共享路径，登记协调目录 |
| 受控依赖纪律 | 不升级子模块（Data_service @ 905b5c1 gitlink）；vendor/cache 共享 |

## 二、现状核查结论（依据 08ca3e52 实读，三大链路全量走读）

### W1 引擎入口单元身份
- `freezeAnalysisUnit`（services/analysisunitcontext.cpp:21-41）已做 DS 实体核验
  （scheme/revision/interval/sub 归属），但单因素链里只有 `extractWellFactorsImpl`
  （constraintwellfactors.cpp:843-847）在 params 带 unitSelection 时调用；
  `generateFactor`/`prepareGeostatJob`/`runConstraintIDW` 全不校验。
- `generateFactor` 对缺 analysisLayerKey 仍回退 horizon（constraintfactorjobs_factor.cpp:87-88）。
- 生产 UI 两个入口（paleomainwindow_attach_constraint.cpp:84-90 makeAnalysisRun /
  runConstraintPageIdw；mappingworkbench ready()）已要求单元，但引擎层不设防，
  批任务/Host/测试可绕过。⇒ 本次补引擎级强制。

### W2 同一身份贯穿
- UI 把 `bind.analysisKey` 当 horizon 传入后，geostat/localdirection 的约束存取
  （store->load(horizon)、`constraints.%1`）用原始 horizon 参数＝analysisKey，
  与约束实际归属键（父单元上界面）错位；factor.cpp IDW 路径已用
  factorConstraintHorizonKey，两族不一致。
- 井点发布 id `wellfactor.<horizon>.<factorId>`（constraintwellfactors.cpp:929）
  用原始 horizon 实参：UI 流传 T70（constraintHorizon 语义）、验收流传 key——
  身份双语义依赖调用方自觉。

### W3 父版本冻结
- `prepareFactorInputs`（constraintwellfactors.cpp:1177-1223）：
  `factorInputPrepared=true` 直接短路（1179），调用方可伪造；调用方传入的
  `parentVersionIds` 原样并入发布父集合（factor.cpp:317-318、geostat.cpp:1673-1674）。
  mappingworkbench 传的是 ensureInputVersion 真版本——需要的是核验而非禁止。

### W4 constraint.idw Host 链
- executeConstraint（capabilitygateway.cpp:823-929）仅查 `subinterval_id` 非空字符串
  （830-831）；`points_ref`/`constraints_ref` 从不解析，无父版本；
  freezeNativeOperation 只给 seismicTrace 填父版本（capabilitynativeoperation.cpp:56-60）；
  worker 产出 artifact extra 为空 → 发布成员无单元身份。
- legacy publishFiles 分支生产不可达（nativeWorker 强制 compute-only；
  capabilitytaskrunner 拒 publish；chat 走 hosted 路径），保留同一验证路径。

### W5 证据成员覆盖
- syncAnalysisResults resultKind（analysisresultsync.cpp:96-103）：
  `constraint_idw_raster` 不含 "factor" → 跳过；`agent_result` → 跳过；
  缺 unitSelection 的产物被 identity() 过滤（212）。砂地比井点绑定已有
  （autoBindSandRatioExtraction），插值主栅格/QC 无绑定路径。

### W6/W7 完成状态与恢复
- 提取后 DS 绑定失败只在成功消息后拼 warning（constraintwellfactors.cpp:976-982）；
  无持久未完成记录；后台 ProjectLayerRefresh 会再投影但完成判定从不查
  目标绑定是否存在。

## 三、实现设计（概要）

1. 新任务私有组件 `src/workflow/constraintanalysisidentity.{h,cpp}`（功能层）：
   `resolveAnalysisIdentity(projectDir, horizon, params, allowAggregate, out, error)`
   - params 带 unitSelection → freezeAnalysisUnit 实体核验（aggregate 按入口裁决）；
   - 无 unitSelection → 工程有已提交地层方案（listProjectSchemes 非空）即拒绝；
     无方案工程（无可选单元）维持 legacy（key=horizon），与 syncAnalysisResults
     的 legacy 工区先例同口径；
   - 输出冻结 unit / analysisLayerKey / constraintHorizon，统一写入 params。
2. `generateFactor`/`extractWellFactorsImpl`/`prepareGeostatJob`/`runConstraintIDW`
   入口统一走 resolveAnalysisIdentity；factor/IDW/geostat 成图拒绝 aggregate；
   analysisLayerKey 不再回退 horizon（legacy 工程除外）。
3. `prepareFactorInputs`：`factorInputPrepared` 短路改为状态核验
   （preparedWellFactorVersion 匹配才快路径，否则去标志走恢复/提取）；
   调用方 parentVersionIds 逐个 catalog 存在性核验，伪造即拒绝。
4. geostat/localdirection 约束存取改用 factorConstraintHorizonKey；
   井点/成果键统一由冻结身份导出。
5. Host 链（freezeInput constraint.idw 分支 + executeConstraint）：
   受信解析当前 selection 单元（freezeAnalysisUnit）；`points_ref`/`constraints_ref`
   按 catalog 版本 id / extra layer 键解析真实父版本；无效 subinterval 在数值前拒绝；
   worker artifact extra 携带宿主冻结的单元身份与算法契约（成员 extra 来源＝冻结
   请求，非用户可控）。
6. syncAnalysisResults 最小分支：`constraint_idw_raster` 与「带可信单元 +
   paleo:paleo_constraint_idw 契约」的 `agent_result` → single_factor。
7. 证据确认协调（任务私有）：发布尾部即时投影 + 目标绑定存在性核验；
   DS 失败/缺绑定 → 版本 extra `evidence_pending`（updateVersionExtra 既有接缝，
   与 stale 投影同机制）记录精确版本与原因；`retry` 入口按 binding id 幂等补确认。
8. 测试：新增 tst_p3_factor_identity（W1–W3 矩阵）＋扩展
   tst_analysisresultsync / tst_capability_recovery_native / tst_factorworkflow。

## 三点五、实现落点（与设计的差异说明）

1. **「无单元即拒绝」的适用面**：工程存在至少一个已提交地层方案（存在可选
   单元）时，正式单因素/插值/地统入口缺 UnitSelection 一律拒绝；无任何方案
   的工程（无可选单元）维持 legacy 键（horizon 直为成果键），与
   syncAnalysisResults「Legacy projects remain readable」先例同口径。验收
   矩阵的拒绝行均以「有方案工程」为测试夹具。
2. **Host 单元权威**：constraint.idw 的单元来自宿主捕获的当前选择
   （freezeInput 钩子，capabilityhostwiring）；请求自带 subinterval_id 与
   之不符 → subinterval_mismatch。受信冻结结果（unit_selection/scheme 字段/
   parent_version_ids）覆盖调用方自带值后进入意图摘要与冻结计划；恢复与
   source_operation_id 复用沿 stagePlan.parents/成员清单继承原上下文。
3. **遗留 publishFiles 分支**（生产不可达，仅进程内直调）加同一验证：
   无宿主核验标记（subinterval_not_verified）拒绝正式发布。
4. **证据确认协调**（factorevidenceconfirmation）：确认=DS 中存在
   id="catalog.<versionId>" 的绑定（不把同步函数返回值当确认）；失败沿
   版本 extra.evidence_pending（updateVersionExtra，与 stale 投影同一接缝）
   持久化 {step,error,bindingId,unitKey}；retryPendingFactorEvidence 按
   binding id 幂等补确认。不声称跨存储 ACID。

## 四、工作包状态

- [x] W1 引擎入口强制解析稳定分析单元（实现完成，验证见下）
- [x] W2 样点/主结果/旁路证据同一身份
- [x] W3 冻结实际消费的输入引用与父版本
- [x] W4 真实 constraint.idw Host 路径接完整
- [x] W5 既有模型内覆盖单因素证据成员
- [x] W6 区分 Catalog 提交与证据确认完成状态
- [x] W7 恢复、重试与后台重复投影幂等
- [x] W8 跨入口端到端测试与失败注入（新增 tst_p3_factor_identity /
  tst_p3_factor_evidence；扩展 tst_capability_recovery_native）
- [ ] 本地验证 + Source gates（见「验证记录」）
- [ ] 独立复审（见「独立复审」）

## 五、验证记录

环境：CachyOS（glibc 2.44 / GCC 16.2.1 / clang 23.1.5）；deb 闭包受控前缀
`/home/kevin/projects/paleo-deb-prefix-20261119/usr`（915 包，SHA256 锁校验，
`.paleo-deb-closure.sha256` 匹配 `vendor/deb-closure.lock`，shim：dpkg-deb
ar+tar、dpkg-architecture）；worktree `../paleo-p3-factor-publication-20261119`，
分支 `codex/paleo-p3-factor-publication-20261119`（base `origin/master`
08ca3e52）。

### 构建

- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu -DCMAKE_C_FLAGS=-g1
  -DCMAKE_CXX_FLAGS=-g1 -DQGIS_PREFIX=<prefix>/usr` + `cmake --build build -j4`：
  **EXIT 0**（GCC16 全量 RelWithDebInfo 触发 ld「relocation truncated /
  bad reloc」（大库 debug_info 超限），`-g1` 行号表后稳定；tst_dockmanager
  串行重链一次通过——均为工具链环境因素，非本 PR 代码）。

### 测试（系统 ctest 4.5 与主机 libxml2 ABI 不匹配——缺
XML_GetCurrentColumnNumber64，与仓库代码无关；用 tools/run_tests_without_ctest.py
按 CTestTestfile 语义驱动，-j4）

- 新增：`tst_p3_factor_identity` 9/9、`tst_p3_factor_evidence` 6/6 全绿。
- 扩展：`tst_capability_recovery_native` 10/10（含新 Host 链 E2E：
  compute-only→source_operation_id 正式发布、成员 extra 单元身份/父版本、
  投影绑定指向真实单元）。
- 全量非 core 套件（复审修复后终跑）：**596/601 通过**。5 个失败均为环境/
  计时/并发类，与本 PR 代码路径无关（已逐一定位）：
  1. `first_configure`（稳定复现）：其子 cmake 首配探测未传
     CMAKE_LIBRARY_ARCHITECTURE，deb 路线 multiarch 推断为空（`lib//`）；
     本 PR 主构建显式传该变量后 configure 正常——属该测试自身的环境探测
     接线差异。
  2. `tst_startup_trace`（稳定复现）：QGIS provider 加载计时份额断言
     （劣化 0.486 < 3×健康 0.313），单核固定重跑仍不过——机器/前缀加载
     特征；该测试零处触及本 PR 改动的 constraint/factor/analysis 代码。
  3. `tst_constraint_draw` / `tst_catalog` / `tst_perfbudget`（并行调度下偶现）：
     单独串行重跑均通过——计时/并发噪声（catalog 为 sqlite 只读 42ms vs
     可写 40ms 的 ms 级抖动）。
  4. `l10n`：本 PR 新 tr() 串未填译文——已修复（补 zh/en 译文后转绿）。

### Source gates（本地）

check_layering --strict / check_file_budget --strict / check_qgis_spread /
check_layering --qgs-audit --strict / check_i18n / check_ui_tokens /
check_docs_drift / check_ts_stale / l10n：**全绿**（P3 修正轮后复跑）。

## 六、独立复审

（复审后填写）
