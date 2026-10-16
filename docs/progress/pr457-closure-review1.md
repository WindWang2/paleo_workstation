# PR #457 剩余缺口补完 — Review–Fix–Review 全程记录

分支 `goal/singlefactor-pr457-closure`（基线 origin/master dbb03269，含 #457/#458/#459）。
本文件是 Review 1 报告 + Re-fix 轮记录；PR 正文另附完整证据。

## 一、Review 1（问题清单）

| ID | 优先级 | 文件/函数 | 具体问题 | 状态 |
|---|---|---|---|---|
| R1 | P0 | qgis/constraintcrs.cpp toStoreWkt | 任意米制 Engineering CRS 视为本地网格 identity 直通；无效来源直通 | CONFIRMED |
| R2 | P0 | io/constraintstore.cpp | 建库 wkbUnknown 无 SRS；legacy 无检测/无显式迁移 | CONFIRMED |
| R3 | P0 | domain/constraintsemantics.cpp | schemaVersion 非整数被 toInt 截断接受；<1 通过 | CONFIRMED |
| R4 | P0 | 同上 | enabled 非布尔经 toBool(true) 默认启用 | CONFIRMED |
| R5 | P1 | 同上 | 自交判定精确浮点相等（cross==0），大坐标/平移不稳 | CONFIRMED（确定性 fixture：同一几何平移后分类翻转） |
| R6 | P1 | workflow/constraintworkflow.cpp ×5 | subset=horizon='%1' 未转义 | CONFIRMED |
| R7 | P1 | algorithms/constraintbarriers.cpp | 半像元采样漏格（对角链 4-不连通） | CONFIRMED |
| R8 | P1 | 同上 | 回执不区分 rasterized/actually-consumed；计划侧与引擎侧域判定边界分叉 | CONFIRMED |
| R9 | P1 | workflow/constraintfactorjobs_geostat.cpp | 经纬度井位角度当米静默计算 | CONFIRMED |
| R10 | P2 | UI | constraintsready 门方法无关；运行按钮不含 blocking | CONFIRMED（doc A12 已知，本轮不改——见剩余风险） |
| R11 | P1 | constraintsemantics.cpp | blockMode 非字符串静默按 full_block | CONFIRMED |
| R12 | P1 | qgisconstrainteditsession.cpp | FID 预分配无冲突回归测试锁定 | LIKELY-OK（已补特征测试证实安全） |
| R13 | P2 | readNumber | 数值字段非有限性未查 | CONFIRMED（JSON 解析层已挡 + 本轮补 isfinite） |

ALREADY FIXED / 设计如此：B7 会话语义、structural_idw 解释边界、地统不读约束 + QC 标注、
ρ=0 退化、零值/缺失区分、store 读路径 quoteSql。

## 二、Fix 清单（8 个 commit + Re-fix 1 个 commit）

| 问题 | 修复文件 | 解决方式 | 红测 | 绿测 | Commit |
|---|---|---|---|---|---|
| R1 | qgis/constraintcrs.{h,cpp} | 等价只认 QGIS 相等或 WKT2 全等；无效来源拒绝；其余需有效坐标操作 | unrelatedEngineeringCrsIsNotStoreEquivalent + unknownSourceCrsRequiresVerifiedContext | tst_singlefactor_constraintcrs 11/11 | de8fc8b0 |
| R2 | io/constraintstore.{h,cpp} | 建库带 SRS（wkbUnknown 不变）；inspectCrsStorage 检测；migrateLegacyCrs 显式迁移（备份+事务+审计+校验+还原） | newStoreCarriesCrsMetadata / legacyStoreDetectionAndExplicitMigration / migrationRefusesForeignSrs | tst_singlefactor_store 8→9/9；相邻 6 套全绿 | f1c0ce48（+Re-fix 25d43b62*） |
| R3/R4/R11/R13 | domain/constraintsemantics.cpp | 版本严格整数+范围；enabled 严格布尔；blockMode 严格字符串；数值有限性 | fractionalSchemaVersionIsRejected / enabledMustBeBoolean / invalidConstraintParameterDoesNotReachEngine / unsupportedSchemaVersionIsRejected / legacyValidConstraintStillLoads | tst_singlefactor_constraintuse 26→30/30 | e4da7b26 |
| R5 | domain/constraintsemantics.cpp | orientSign/onSegment/foldsBack/samePoint 全部 K·(M·L+L²) 相对阈（64u，误差分析入注）；扫描剪枝不变 | selfIntersectionIsTranslationInvariant（确定性 fixture + 3000 例电池）/ nearCollinearLargeCoordinatesAreStable（4000 例）/ tinySegmentsAndNearDuplicateVertices / closedRingsAndRealIntersectionsStaySeparable | constraintuse 30/30；perf 162s 通过（无 O(n²) 回退） | 83159985 |
| R6 | workflow/workflows_internal.h + constraintworkflow.cpp ×5 + mappingworkbench_edit.cpp | constraintHorizonSubsetSuffix() 单点转义（单引号翻倍） | specialCharacterHorizonsStayIsolated（stash 复证红：O'Connell-1 featureCount=-1） | tst_constraintstore 8/8；mappingworkbench 46→47/47 | a8608181 |
| R7/R8 | algorithms/constraintbarriers.{h,cpp} | supercover 格网遍历（闭域裁剪+clamp+4-连通链）；回执 rasterized/rasterized_cells/new_cells/fully_shadowed | rasterizationIsFourConnectedAlongDiagonals / BoundaryAndCornerTouchStayInDomain / ReceiptsCarryCellCounts / CancelStops | constraintreceipts 15→19/19；algorithm_harness 26→27/27 | a45341c1（+Re-fix 扩面） |
| R9 | workflow/constraintfactorjobs_geostat.cpp + 3 个测试 fixture | compute 入口 geographic 拒绝；fixture 改 EPSG:3857 | geographicWellsAreRejected（+V7 无 CRS 分支） | geostat_workflow 13/13；factorworkflow 33/33；workflows 21/21 | 44613f02（+Re-fix） |
| R12 | tests/tst_constraintediting.cpp | 特征锁定：三种撞号整批拒绝无半提交 | —（行为已安全，锁定用） | constraintediting 30→31/31 | 29395458 |

## 三、独立 Review 2（Explore 子代理，只读找反例）与 Re-fix

发现 15 项（V1-V15）；3 Major 全部 Re-fix：

| ID | 严重度 | 问题 | Re-fix |
|---|---|---|---|
| V1 | Major | 导入门（acceptsLocalGrid）仍是「任意米制 ENGCRS≈本地网格」，与 C1 画布门分叉且无留痕 | 保留 #457 既有导入口语义（测试固化的合法工作流，任务 req 8），但语义文档化 + provenance 留痕：外来 CRS 导入记 crs_verified=false + crs_source + crs_note，同一网格 CRS 记 crs_verified=true（任务 req 7「核实 vs 推测」）。红测 constraintImportRecordsCrsProvenance |
| V2 | Major | supercover 只改了 2/4 栅格化点：paleo_constraint_idw 与 local_direction labelHardBarriers 仍半格采样（对角漏格），注释不实 | 抽公共 supercoverPolylineCells（支持非方格 pixelWidth/Height），四个栅格化点统一；不实注释改写 |
| V3 | Major | migrateLegacyCrs 备份 WAL 盲区、并发还原可覆盖合法提交、还原失败谎报、绕过单写者队列 | 重写为 migrateLegacyCrsLocked：经 m_enqueue 单写者队列 → wal_checkpoint(TRUNCATE) → 事务持锁窗内预扫描+备份+迁移+提交前校验 → 提交后独立重开校验，还原失败如实上报（含备份位置）；审计事件写失败整单回滚 |
| V5 | Minor | newStoreSrs 失败静默建无 SRS 层 | createConstraintsLayer 显式失败带原因 |
| V6 | Minor | 迁移审计 OGR_L_CreateFeature 未检查 | 已检查并入失败路径 |
| V7 | Minor | 地统计守卫不拦无效 CRS | 加 !crs().isValid() 拒绝（提示声明 CRS）+ 测试 |
| V8 | Minor | 层位名含 '\|' 破坏 datasource URI（subset 之外第二路径） | appendInternal 入口拒绝 + horizonWithPipeIsRejected |
| V9 | Info | 扫描剪枝「判定结果不变」注释在 ε 模型下不准确 | 注释改写 |
| V4 | Minor | inspect/migrate 无生产调用方（契约 API 暂未接线 UI） | 明示为显式迁移 API；接线属后续（见剩余风险） |
| V10-V15 | Info | 反例核查不成立/说明项（微段容差语义、cols=0 防御已含、血缘解析 additive、对角屏障格网结果有意变化等） | 记录在案 |

## 四、测试矩阵（关键套件）

| 测试 | 状态 | 结果 | 说明 |
|---|---|---|---|
| tst_singlefactor_constraintcrs | PASS | 11/11 | CRS 契约（含新 6 项断言） |
| tst_singlefactor_store | PASS | 9/9 | SRS 建库/迁移/V8 |
| tst_singlefactor_constraintuse | PASS | 30/30 | schema 严格化 + 几何鲁棒 |
| tst_singlefactor_constraintreceipts | PASS | 19/19 | 栅格化 + 回执 |
| tst_constraintediting | PASS | 31/31 | 会话/FID 契约 |
| tst_constraintstore | PASS | 8/8 | 转义 + 端到端 |
| tst_constraint_draw / tst_constraintevidence | PASS | 7/7, 7/7 | 绘制/证据 |
| tst_mappingworkbench | PASS | 47/47 | 导入 provenance + 既有 |
| tst_factorworkflow | PASS | 33/33+1skip | 因子全链（fixture 3857） |
| tst_geostat_workflow | PASS | 13/13 | geographic/无 CRS 拒绝 |
| tst_algorithm_harness | PASS | 27/27 | 对角密封 + 探针语义 |
| tst_singlefactor_perf | PASS | 3/3（162s） | 扫描剪枝无回退 |
| tst_workflows / tst_algorithms / tst_mappingpages | PASS | 21/21, 20/20, 13/13 | 共享 fixture 波及面 |
| ctest -R 'singlefactor\|constraint\|geostat\|factorworkflow\|mappingworkbench' | PASS | 47/47（Re-fix 前轮） | Re-fix 后全量重跑见 PR |

## 五、剩余风险（未解决项，全部明示）

1. R10/N3/N4（UI）：constraintsready 门方法无关（地统方法仍要求备齐间隔线+方向线）、
   运行按钮不含 plan.blocking、A10 UI 计数无专门断言——UX 缺口已知（doc A12），本轮
   按任务边界未动 UI 行为，留待专门 UI 轮。
2. V4：inspectCrsStorage/migrateLegacyCrs 为显式契约 API，尚无 UI/CLI 入口接线
   （legacy 库检测提示 + 一键迁移按钮属 UI 工作）。
3. R12 残余：进程内无现实 FID 竞争窗口（主线程 + enqueueWrite 串行 + 首快照即持久化），
   外部进程直写 GPKG 与编辑会话并存的场景靠安全失败兜底（已锁定），不做分布式锁。
4. M1-M4（#457 遗留）：CAD dock/Backspace 无头不可驱动、真实工区 SKIP、Windows 未跑。
5. 对角走向/贴格边屏障的格网结果与 #457 基线有合法差异（漏格修复所致），无 golden 值
   钉住旧漏格结果；血缘新字段（rasterized_cells 等）为 additive。
