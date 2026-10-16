# PR #457 剩余缺口补完 · 二轮（goal/singlefactor-pr457-closure）

## 与 #488 的关系（重要）

本轮与 #488 同源（同一 /goal 任务），并行执行：第一轮以 `dbb03269` 为基线
独立完成 Review→Fix→独立 Review→Re-fix（13 目标套件 + 全量 ctest 门禁）；
推送前发现 #488 已合入（其覆盖：toStoreWkt 严格等价、samePoint/orientSign
容差谓词、schema/enabled/blockMode 严格、新库建层带 SRS、legacy 迁移
inspectCrsStorage/migrateLegacyCrs、引号转义 helper、supercover 栅格化统一）。
本轮随即**语义 rebase 到最新 master（ddd1325e）**：#488 已覆盖的部分不再
重复提交，保留并叠加 #488 未覆盖的缺口（下表「二轮增量」）。两轮独立审查
互为印证（#488 的 review1 文档与本轮独立 review 结论高度重合）。

## 二轮增量（本 PR 实际提交内容）

1. **A1 计划/引擎域口径统一**（#488 的 segmentTouchesRect 仍是闭矩形）：
   半开谓词 x 左闭右开、y 上闭下开（与 cellOf 行号自上而下一致）；四边缘 ×
   三引擎一致性测试。
2. **旧 IDW 屏障逐线落格回执（R3）**：supercover 栅格化前置 + 逐部件 touched
   跟踪；未落格线回执 outside_compute_domain、不进凸包、PALEO_BREAK_LINES
   只计落格数；BFS 复用 mask。
3. **P0-2 可达性**：#488 的 migrateLegacyCrs 只有测试调用——补 workflow 门面
   + 约束页状态显示（⚠ assumed）与显式迁移按钮；层 CRS provenance
   （assumed_from_context / verified_from_file）。
4. **F4 fid 映射复核**：storeFidIndex（max fid + fid→身份表，含无 id 行）；
   undo 释放编号被外部占用后 redo 让位重放而非永久拒绝。
5. **R2/R8 生产域接线**：lastFactorComputeDomain（井点 bbox±10%）、
   buildBarrierMask 传精确网格域、inspectHorizonText 按层位分组
   （barrierHorizons）+ 参数表按本层位收敛；计划 id 归一；UI label 补全。
6. **F3/A3 层位名 '|' 拒绝**：#488 helper 只转义引号——'|' 破坏 OGR URI；
   helper 拒绝 + 六调用点接入 + 快照守卫 + 敌意层位测试（含注入语义）。
7. **结构面停线数值精确跳过（R4）**：GEOSDistance > softRadiusAuto 才跳过
   （软核紧支于半径 = 零数值作用），触域/半径内线不裁剪（数值逐位不变）。

## 第一轮过程记录（基线 dbb03269，含被 #488 覆盖的部分）

## Review 1 → Fix 清单

| 问题 | 修复 | 红测 → 绿测 | 证据 |
|------|------|------------|------|
| C1/C2 P0：任意米制工程 CRS 被视为与本地网格等价（identity 拷贝坐标）；来源 CRS 无效也 identity | `constraintcrs::toStoreWkt` 重写：identity 仅 `isStoreGridIdentity`（真实相等）；其余必须有注册坐标操作，否则拒绝；无效来源拒绝。`classifySourceCrs` 五态分类（Invalid/CanonicalGrid/UnspecifiedCartesian/ForeignEngineering/ForeignGeodetic） | `unrelatedEngineeringCrsIsNotStoreEquivalent`、`missingTransformRefusesWrite` | constraintcrs.cpp；tst_singlefactor_constraintcrs 16/16 |
| C3 P0：历史 CRS-less 层静默指派 CRS，无 provenance | `assignStoreCrsIfMissing` 返回 `StoreCrsAssignment{assigned,verified}` 并打 `paleoCrsProvenance=assumed_from_context`；`layerCrsProvenance()` 三态（none/assumed/verified） | `unknownSourceCrsRequiresVerifiedContext` | 同上 |
| C4 P0：新建 constraints 层无 SRS（外部 QGIS/GDAL 无法独立辨识） | Storage Contract：建层带规范本地网格 SRS（wkbUnknown=GEOMETRY 保留混合几何能力）；`layerCrsInfo()` 检测 legacy（srs_id≤0 / "undefined" / GDAL 99999 `LOCAL_CS["Undefined SRS"]` 占位）；`defineLayerCrs()` 显式迁移：整文件备份（.bak，拒绝覆盖）+ 单事务（SRS 行复用或注册 + 注册表更新 + 几何 blob 头 srs_id 重写（仅大端头 blob）+ crs_define 审计事件）+ 提交后核实（SRS 生效 + 逐要素不变 + OSRIsSame）失败从备份恢复；WAL/SHM 在场拒绝；幂等护栏 | `newStoreRegistersCanonicalSrs`、`legacyStoreExplicitMigrationRoundTrip`、`defineLayerCrsGuards` | constraintstore.cpp；测试断言外部 QGIS 直开 CRS=规范网格、迁移后 fid/身份/层位/几何不变、审计事件可查、再迁移被拒 |
| S1/S2/S3/S4 P1：schemaVersion 1.5 被 toInt 截断接受；enabled 非布尔静默 true；blockMode 非字符串静默 full_block；数值不查 finite | 严格校验：版本须整数值 double∈[0,2³¹)（列口径：浮点列须整数值、字符串严格转换）；enabled 必须布尔；blockMode 必须字符串；readNumber 拒非有限值；错误信息带约束 id | `fractionalSchemaVersionIsRejected`、`unsupportedSchemaVersionIsRejected`、`enabledMustBeBoolean`、`invalidConstraintParameterDoesNotReachEngine`（全方法矩阵不消费+blocking）、`legacyValidConstraintStillLoads`（兼容） | constraintsemantics.cpp；tst_singlefactor_constraintuse 29/29 |
| G1 P1：相交/回折判定用精确浮点符号（大坐标抖动） | `orient2d` filtered predicate：误差界 3.5ε(|t1|+|t2|) 自适应量级，不可靠符号按共线交包围盒判定；**顺带修出真缺陷：Qt 6.10 `QPointF::operator==` 是模糊比较（qFuzzyIsNull(±1e-12)）——去重/闭合判定改按位 `sameVertex`**，1e-12 量级坐标不再被吞成零长线 | `robustOrientationLargeCoordinates`（大坐标拐角/回折/tiny/抖动）、`robustPredicateMatchesExactOnRandomData`（4000 随机差分 vs 整数精确参照，含 ±1e6/−3e5/7e5 平移） | 同上 |
| F1/F2/F3 P1：层位名单引号未转义拼 subset（图层失效/注入跨层位泄漏）；`\|` 破坏 URI | `PaleoLayerVocabulary::escapeHorizonSql`（'' 翻倍）+ `constraintHorizonSubsetSuffix`（含 `\|` 返回空+原因）；6 处调用点全部接入：ensureConstraintLayer/addConstraint 拒绝并回滚刚写行，loadConstraints/syncConstraintGeometryViews 跳过声明并告警，mappingworkbench 快照拒绝（review A3） | `hostileHorizonNamesRoundTrip`（O'Connell-1/中文/空格/引号/反斜杠：图层有效、行数=store 直读、subset 字符串正确）、`hostileHorizonCannotChangeFilterSemantics`（注入名只见本层位 1 行）、`pipeHorizonIsRefusedNotTruncated`（拒绝且库/声明无半截） | layervocabulary.h、constraintworkflow.cpp、mappingworkbench_edit.cpp；tst_constraintediting 34/34 |
| F4 P1：会话新要素 fid 映射首见钉死，undo 释放编号被外部占用后 redo 永久拒绝 | `storeFidIndex()`（一次扫描：max fid + fid→身份表，含无 id 行——review A7）；snapshot 每次复核：占用=该 fid 存在且身份≠本要素 → 让位 `max(m_nextFid, live+1)`；自身行保持编号（幂等重放） | `redoSurvivesFidReuseByOtherWriter`（undo → 显式 FID 外部占号 → redo 同身份新编号重放，两层位数据完好） | qgisconstrainteditsession.cpp |
| R3 P1：旧 IDW 回执把未落格屏障记 consumed；`PALEO_BREAK_LINES` 计未落格数 | 网格计算前置；屏障先栅格化（逐部件 touched 跟踪）；未落格 → 回执 consumed=false/outside_compute_domain、不进凸包、元数据只计落格数；BFS 复用同一 mask；计划侧 legacy hard_barrier 同口径域判 | `griddingReceiptsMatchPlan`、`legacyReceiptsMatchPlan`（计划改传域后两侧一致）、`edgeAlignedBarrierIsOutsideForAllGridEngines`（x/y 边缘四例 × 三引擎） | paleoalgorithms.cpp |
| R1/R2 P1：计划闭矩形 vs 引擎半开像元两方向不一致；生产从不传 domain（域判定死代码） | `segmentTouchesRect` 半开化并经独立 review 修正 Y 轴方向（x 左闭右开；y 上闭下开——行号自上而下，与 cellOf 逐边一致：x==xMax/y==yMin 域外，x==xMin/y==yMax 域内）；生产接线：buildBarrierMask 传精确网格域、约束页传 `lastFactorComputeDomain`（已提取井点 bbox±10%）、inspectHorizonText 传散点域；R8：勘察按层位分组（`barrierHorizons`），参数表按本层位收敛屏障开关 | 同上 receipts 测试 + UI label 补全（not_consumed/other_horizon/unknown_type/ignored + invalid_params/unknown_type 前缀折叠） | constraintsemantics.cpp、surfacegridding.cpp、constraintworkflow.cpp、constraintpage_constraints.cpp、paleomainwindow_attach.cpp |
| R4 P2：结构面停线域外整段仍进软边界（回执虚报） | 引擎侧：`GEOSDistance(line, domain) > softRadiusAuto` 才跳过（数值精确判据——软核紧支于半径，距离超半径⇔零数值作用；触域/半径内可达线保留完整折线，不裁剪，数值逐位不变）；计划侧同口径触域判定 | `structuralReceiptsMatchPlan`（计划/引擎一致） | structural.cpp、constraintsemantics.cpp |
| R5/R6 P2：计划侧 id 不 trim 无 fid 兜底（回执对不上）；reason token 缺失/漂移 | 计划侧 id trim + fid 兜底（与三引擎读者同口径）；UI label 表补全 | 全套件回归 | constraintuseplan.cpp、constraintpage_constraints.cpp |
| C6（口径复核） | 地理 CRS 警告口径维持 PR #457 决定（`geographicCrsWarning` 已在引擎产出），未改 | — | — |

## 独立 Review（adversarial）发现 → 处置

| ID | 严重度 | 发现 | 处置 |
|----|--------|------|------|
| A1 | P1 | 计划侧 Y 轴半开方向与引擎行号相反（yMax 应闭、yMin 应开） | **已修**：openEdge={left闭,right开,top开,bottom闭}；单点判定 `y>top && y<=bottom`；R1 测试加 y 边缘两例（x/y 四边缘 × 三引擎一致） |
| A2 | P2 | defineLayerCrs/layerCrsInfo 无生产调用方（不可达 API） | **已修**：约束页显示 legacy 真实状态（⚠ 假定提示）+「为约束库登记本地网格坐标系…」显式迁移按钮（确认框含备份/核对/恢复说明，完成后刷新） |
| A3 | P2 | mappingworkbench 快照路径空 subset 退化为全表 | **已修**：空 suffix 如实失败返回 |
| A4 | P2 | 占位名子串匹配过宽/过窄 | **已收紧**：只认整名占位词（unknown/arbitrary/NonEarth (metres)）或 Unspecified*/Undefined* 起头 + 空名；其余工程 CRS 一律拒绝（宁拒不静默错位）。已知残留：非主流占位名（见剩余风险） |
| A5 | P2 | UI 计算域（未量化 bbox±10%）≠ 引擎 ceil 网格（≤1 cell 余量）；零宽轴 pad 口径差 | **记录为已知限制**：页面计划是咨询性显示（缺 cellSize 无法 ceil）；回执（CONSTRAINT_RECEIPTS）是事实契约，二者语义已在注释与本文说明 |
| A6 | P2 | 结构面停线「触域」计划 vs「距离≤半径」引擎的环形带差异 | **记录**：计划保守下界（触域即消费）；引擎精确；环形带内（域外但半径可达）引擎消费/计划标域外——方向为「计划少说、引擎如实」，不虚报 |
| A7 | P2 | fid 占用判定对无 id 占用者失明 | **已修**：owners 含无 id 行（空身份） |
| A8 | P2 | barrierHorizons 无消费方（R8 只做一半） | **已修**：attach 按资产层位收敛 hasConstraints |
| A9 | P3 | enabled 严格布尔对存量 `"enabled":1` 数据从静默启用变为 blocking | **有意行为**（S2 规格：非法输入不得宽松转换）；PR 描述明示数据兼容性变化 |
| A10 | P3 | blob 头重写假设大端 | **已修**：只重写 flags 字节偶数（大端头）blob，小端头 blob 原样保留 |
| A11 | P3 | 失败恢复在 Windows 上可能因文件锁失效 | 记录（本目标 Linux；CI Windows 另行验证） |
| A12 | P3 | barrierMask 无屏障也分配（1e8 格 ~100MB） | **已修**：仅 breakGeoms 非空时分配 |
| A13/A14 | P3 | pipe 拒绝后 m_inMemoryConstraints 残留至下次刷新（自愈）；untracked 文件 | 前者记录；后者随本次提交入库（run-target-tests.sh 为本地门禁工具） |

复核「Verified sound」：defineLayerCrs 事务/备份/核实机制、toStoreWkt 双向
管线、旧 IDW 重组无 use-before-def、结构面跳过判据数值精确、鲁棒谓词、
转义覆盖 grep-clean、schema 严格性、i18n 参数、#458 智能预测文件集不相交。

## 测试矩阵（本地门禁，2026-10-16）

| 套件 | 状态 | 说明 |
|------|------|------|
| tst_singlefactor_constraintcrs | PASS 16/16 | 含 6 项 P0-1 必测 + 新库 SRS/迁移/守卫 |
| tst_singlefactor_constraintuse | PASS 29/29 | 含 schema/enabled/鲁棒谓词/随机差分 |
| tst_constraintediting | PASS 34/34 | 含敌意层位矩阵 + F4 |
| tst_singlefactor_constraintreceipts | PASS | 含四边缘 × 三引擎 + legacy/structural 域一致 |
| tst_constraint_draw / constraintstore / constraintevidence | PASS | 回归 |
| tst_factorworkflow / mappingworkbench / mappingpages | PASS | 含 A2/A8 接线回归 |
| tst_geostat_workflow / geostat_perf / singlefactor_contours | PASS | 回归（#458/PR#457 成果无回归） |
| 全量 ctest（408 项） | 401/408 | 首轮 403/408（5 项 perf 类）；复核轮因 A2 初版引入 ui→io include 导致 layering/layering_strict 2 项门禁红 + perf 类负载抖动（含 tst_shell_projectlifecycle）；已修（改经 ConstraintWorkflow 门面）并复验 layering 绿、shell/其余 perf 隔离复跑 PASS。tst_singlefactor_perf L=21.1s 超预算为**既有基线失败**（同机 master 侧旧构建 L=20.4s 同超，diff 未触及该引擎路径） |
| check_layering / git diff --check | PASS | 无层违规 |

## 剩余风险（未解决项，全部明示）

1. **A5/A6 环形带差异**：约束页「在计算域外」为保守判定（未量化域 + 触域
   判据）；引擎回执为准。结构性改进需把 cellSize/半径传进页面计划，未做。
2. **A4 占位名识别**为启发式白名单；非主流本地 CRS 名（如 MapInfo 变体）会
   被拒绝并要求用户重新导出——安全方向（宁拒不静默错位），但用户体验代价。
3. **tst_singlefactor_perf L 预算**在本机基线即超（20.4s/15s）；与本分支无关，
   建议单列 issue 调整预算或标注机器档位。
4. **Windows**（A11 + 迁移文件锁）未验证：本机 Linux，CI Windows 任务历史
   上常被并发组取消。
5. **A9 存量数据**：外部手工写的 `"enabled":1`/`schemaVersion:"1"` 行从
   静默宽容变为 invalid_params（对 local_direction_* 方法 blocking）——按规
   格有意收紧，见 PR 描述「数据兼容性」。
6. **导入门对无 .prj 的 shapefile**：QGIS 读为「未指定 Cartesian」→
   UnspecifiedCartesian 接受（按工程上下文假定）——与 PR #457 行为一致；
   识别假定为假定的能力见 provenance，不冒充核实。
