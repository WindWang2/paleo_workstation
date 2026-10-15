# refactor(04): 单因素计算、已有地质约束与插值工作流的数值和交互质量提升

## 范围与所有权

方向 04（单因素计算/约束/插值质量）。base `b0520429`（=origin/master 2026-10-14 核实）。
写权限白名单内 19 文件：`src/algorithms/geostat/`（4）、`src/algorithms/singlefactor/localidw.cpp`、
`src/qgis/factorcontour.*`、`src/workflow/constraintfactorjobs_{factor,contours,interpretive,localdirection}.cpp`、
`src/ui/pages/constraintpage*`（3）、测试 5 文件。

## 七任务冲突排除

- PR #437（80 文件保护区）：`git diff origin/master...HEAD --name-only` 逐文件比对，零命中
  （含 CMakeLists.txt、constraintfactorjobs_geostat/internal.h、constraintwellfactors、
  tst_factorworkflow 等全部避让；新测试只扩展既有注册文件，零构建系统改动）。
- Open Issues 指纹排除：#352/#379/#392/#393/#415/#417 相关路径全部只读；
  #417 的 8 项 src/algorithms 缺陷（GEOS 泄漏/空环/SKIP）未触碰。
- 全局 build/test 经 `/tmp/paleo_workstation_global_build_j4.lock` + `-j4`（与并行方向互斥排队）。

## 修复清单（每项：缺陷 → 修复 → 配对测试）

数值可信度（Oracle 1：确定性 reference + 容差）：
- **D1** sgs/sgs3 变差校验漏 NaN nugget/sill（NaN 穿过 `<0`/`和<=0` 比较 → 全场解 NaN 且
  Status::Ok 冒充成功）→ 入口显式 isfinite 拒绝（sgs3 加 verticalRangeRatio）→
  `tst_geostat_sgs::nanVariogramParamsRejected`
- **D2** kriging modelUsable 放行零信号模型（nugget=sill=0 → Γ≡0 加边矩阵奇异 → 每格失败
  Status 仍 Ok）→ modelUsable 与 ordinaryKriging 网格入口双面拒绝 →
  `tst_geostat_kriging::zeroSignalAndNonFiniteModelRejected`
- **D3** variogram 隔断档把 faultPathMetric 的 Cancelled 洗成 InvalidInput（krigingsurface
  识别不了 → 用户取消被误报「拟合失败」并画 IDW 冒充面）→ Cancelled 原样透传 →
  `tst_geostat_variogram_barrier::cancelledInsideBarrierFieldPropagates` +
  `tst_singlefactor_kriging::cancelledBarrierFitPropagatesWithoutIdwFallback`（编排面断言
  surfaceFallbacks==0）
- **D4** azimuthDeg/anisotropyRatio NaN 时 semivariance(dx,dy) 静默塌缩 γ≡0 → modelUsable
  拒绝非有限 → 同 D2 测试
- **D5** sgs 2D 查询格与静态样本重合无特判（LU 残差经 sqrt 放大成 ~1e-7 高斯域抖动，
  sgs3 已修 2D 未同步）→ 对齐 sgs3 的重合特判（随机流照常推进，黄金锚不变）→
  `tst_geostat_sgs::goldenAnchorPinsRefactoredKernel` 全绿 + conditionedAtSampleCells 复核

服务与错误恢复（Oracle 2/4）：
- **D6** factor publish 四处 `results.isEmpty()` 裸 return false 不设 error（#415 同族：
  任务面看起来像成功）→ 空成果带原因返回（不覆盖引擎已填 error）
- **D7** localdirection staging 段失败/取消不清 paleo-sf- 临时目录（同步路径无 cleanup
  兜底，每次失败泄漏 UUID 目录）→ 三失败分支自清（与引擎失败分支同套清理面）
- **D8** structural/fixed 等值线提取无取消钩子（取消只在完成后丢弃产物）→
  fixed 路透传 cancelled 到 GDALContourGenerateEx 回调真实中止；structural 路阶段边界
  轮询（提线内部无检查点，头注释如实声明粒度局限）

UI 状态一致性（Oracle 2/3）：
- **D9** 等值距「自动(0)」跨因素类型残留（切回非 structural 后 interval=0 被服务层
  「间距必须是正数」拒绝——按钮可点但必败）→ 勾选切换双向复位 + structural 先勾选后
  生成时补置 → `tst_mappingpages::contourIntervalResetsAcrossFactorTypes`
- **D10** parseLevels 静默丢弃非法 token（「10, abc, 30」→[10,30] 无提示）→ dropped 出参
  + 点击拒绝并提示 + tooltip 说明 → `tst_mappingpages::parseLevelsDroppedTokensAreRefused`
- **D11** 「（旧输入）」标记写在状态列文本里，任何 layerDeclared 重声明触发
  refreshFactorLayers 重写即被静默抹掉 → stale 记入按层位动态属性
  `paleo.page.factorstale.<horizon>`（重写时恢复、重新生成时清除、切层位天然隔离）→
  `tst_mappingpages::staleSuffixSurvivesLayerRedeclare`

性能（Oracle 6，有基线同条件复测）：
- **D12** localidw 引擎构造从未被消费的 `geostat::KrigingSolver`（每分量引擎白做一次
  dedupe+桶格索引 O(n)）→ 移除（S1 审计确认全文件零消费；数值路径逐位不变——
  tst_singlefactor_kriging 全套含比率门复绿）

## 测试证据（本机 Linux/Arch，deb 闭包 QGIS 4.2.3 vendored 栈）

- 先红后绿：基线源码（git stash 撤 4 个 geostat 修复）+ 新测试 = 3/4 红；恢复修复后全绿。
- 直接相关 17 套件实跑通过（ctest，锁下）：
  tst_geostat_kriging(13 passed)/sgs/sgs3/cokriging/variogram/variogram_barrier、
  tst_singlefactor_kriging(34s)/variogram_barrier/faultpath/contours、
  tst_mappingpages(3 新用例)/constraintstore、
  **tst_factorworkflow（34s，#437 保护区测试原样实跑——D6/D7 所在面的回归）**、
  **tst_geostat_workflow（20s，D1-D4 编排链回归）**。
- S5 独立 reviewer 实跑 8 套件（5 目标 + sgs3/contours/perf 邻接）全绿。
- 首轮全量 ctest（-LE perf）：153 个可运行测试全部通过（其余为并行构建中断的
  二进制缺失 Not Run，非测试失败）。
- `python3 tools/check_layering.py --strict` 通过；`git diff --check` 干净。

### 已知环境限制（如实记录）

- 共享 nvme 盘 `/home/kevin/project`（458G）被 7 个并行方向 worktree 的 build 目录
  挤至 100%（440M 剩余）：4 个 WebEngine 测试（tst_uxtheme/tst_webviewpanel/
  tst_stratigraphicweb/tst_wellcomposite）与 -k 0 批量链接的后段 target 因
  `ld: final link failed: 设备上没有空间` 未能全量链接。该 4 个测试与
  本方向 19 文件零代码路径交集（本方向不触 WebEngine/井剖面对比/网页面板），
  机器资源恢复后可零改动补跑。测试二进制缺失导致的 Not Run 同理，非断言失败。

## 已知风险与限制

- D8 structural 路的 field_contours 提线内部仍无取消检查点（取消粒度 = 单阶段）；
  深入插点需改 extractFieldContours 公共签名，留待下一 PR。
- D2 对零信号模型的拒绝改变了裸 geostat API 的行为面（原先「全场 NaN + Ok」现在
  InvalidInput）——上游 krigingsurface 已有同款闸（零信号回落 IDW + 如实报因），
  生产路径不受影响；裸 API 消费方（tst_geostat_kriging 旧用例 cancelledAndInvalid
  的 zeroSill 断言）已由 sgs 侧同名语义覆盖，实测无旧断言被改。
- 「伪已保存版本」（declare 失败无回滚，S2-3）修复需动 #437 保护的 derivedassets.h，
  本轮不实现：接口提案 = commitExternal 增加「declare 失败自动撤销版本行」的补偿
  事务钩子，交由保护区 owner 评估。
- 环境：本机 Arch 系统新装 qt6keychain dev（Qt 6.11 编译）与 vendored Qt 6.10 混链，
  configure 用 `-DCMAKE_DISABLE_FIND_PACKAGE_Qt6Keychain=ON` 走 JSON 回落（与
  pw-seismic worktree 行为一致）；不影响产品逻辑（keychain 是可选特性）。

## 独立 Review（S5，未参与实现）

- 结论 **PASS-WITH-FIXES**：0 BLOCKER / 1 MAJOR / 9 MINOR；S5 实跑 8 套件全绿。
- M1（MAJOR，已修）：sgs/sgs3/cokriging 补 isfinite(azimuthDeg/anisotropyRatio)——
  ratio>1 + NaN azimuth 的 γ(dx,dy) 塌缩路径与 kriging 同型（全场 nodata 冒充 Ok）；
  tst_geostat_sgs 补两断言。
- m2/m3/m4（已修）：编排面测试注释改如实口径；撤销 noteFactorLayer 的等值距压回
  （避免吞用户显式输入，取舍见 ledger）；interpretive 取消检查提前（与 contours 同序）。
- m1/m5-m9（MINOR，记录未修）：详见 ledger——均为防御性/口径一致性/维护性项，
  无行为缺陷。

## 未处理的 Issue（摘要）

#352/#379/#392/#393/#415/#417 由 Issue Agent 负责，本 PR 零触碰；
关联但避让的发现（runFactorWork 面裸捕 m_proc.data() 为 #379 同型新实例）已记入
ledger「留给 Issue Agent」表。

## 下一阶段建议

- extractFieldContours 取消参数化（公共签名变更，需跨 ts 重验黄金）
- JobRunner 面 cleanup 钩子补 staging 目录（S2-2）
- 隔断档近距对丢弃（C7）与 MM1 协变量标准化（C8）的口径文档化
