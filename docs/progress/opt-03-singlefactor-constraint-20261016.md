# opt-03：单因素取数、约束线 QGIS 绘编、插值方法与成果质控进一步提升

任务分支 `goal/paleo-opt-03-singlefactor-constraint-20261016`，worktree
`../paleo_workstation-opt-03-singlefactor-constraint`，基线 `BASE_SHA = dbb03269`
（2026-10-16 fetch 后 origin/master）。目标：在不开展新方向、完全避开 26 个 Open
Issues（#460–#485）与 Open PR #486/#487 的前提下，对本域现有功能落地真实改进、
定向测试与独立 review，提交单一 PR（不等待 CI、不合并）。

执行账本（逐轮证据）：`docs/ledger-archive/goal-loop-opt-03.md`（仓库根）。

## 现状基线（R0/R1）

- 排除矩阵与基线冻结见账本 R0 节。26 个 Open Issue 中清单类（#476/#478/#480/#482/#483）
  已逐条读全文并进入候选自查；#487 的 `src/qgis/qgisstyleservice.cpp`、#486 的四个文件
  全部只读。
- 已合并交付视为现状：#451（D1–D12 数值/交互修复）、#457（约束语义统一、引擎回执、
  CRS、混合几何、逐版本色带、成果血缘列等 A1–A15/B1–B10/C1–C8/D1–D4）。
- R1 干净基线测试（主 checkout build @ dbb03269，Debug，ctest -j1）：结果见账本 R1 节。

## 改进单元（R2 backlog → R3 实现）

十个单元、四个互斥文件族（S1 取数/页面、S2 约束词表、S3 插值数值、S4 等值线写侧），
每单元执行「红测试 → 最小修复 → 绿 → 记录」；逐单元验收证据如下。

| # | 单元 | 一句话价值 | 状态 |
|---|------|-----------|------|
| U1 | 采样快照守卫覆盖测井矢量属性表 | 异步提取期间编辑岩性井段不再能污染"不可变"快照 | ✅ |
| U2 | 井点图层重复井/规范化井名碰撞检测 | 同井双计（权重×2）与静默吞井不再发生 | ✅ |
| U3 | structural_idw POWER 入口校验+错误传播 | POWER≤0 不再静默全 NaN 或误报"缺井控" | ✅ |
| U4 | 参数钳制回执诚实化（K 钳制 + 地统显式变差钳制） | QC/血缘如实记录"请求值→生效值" | ✅ |
| U5 | structural QC 完整性（指纹参数+finite/nodata 计数） | 不同参数不再撞同一 parameter_hash；血缘计数不再恒空 | ✅ |
| U6 | 约束 reason 词表与未类型化计数 | 主链画线不再显示裸 token；计数不漏行 | ✅ |
| U7 | 解释性等值线/制图工作场进成果表（刷新+行） | P4 血缘定位链在解释性成果上贯通 | ✅ |
| U8 | 等值线版本 analysis_sha256/contour_feature_count 写侧 + lineage 读侧 | 等值线成果第 4 列有内容；0 条如实可见 | ✅ |
| U9 | FIXED_LEVELS 级别数上限（UI+工作流双层闸） | 固定级别路径不再可被数万级冻结（#149 对偶） | ✅ |
| U10 | 因子表字段缓存（勾选不重取） | 高频勾选不再每击全图层扫描（GUI 线程） | ✅ |

### U1 采样快照守卫（constraintwellfactors.cpp）

- 改前：`extractWellFactorsImpl` 冻结快照只含井点图层 + 因子属性表；但 worker 内砂厚经
  `WellSectionWorkflow::lithologyFor` → `WellAttributeStore::rows(dir,nullptr,false,…)` 活读
  `artifacts/layers/well-log-attributes.gpkg`（路径与 `wellattributestore.cpp:35-37` 一致），
  提取期间编辑该表不触发 publish 的「井点来源已变更」——混合时点数据被发布为版本。
- 改后：文件存在即加入 `frozen.paths`（→ sourceFileStates → publish 校验 +
  `preparedWellFactorVersion` 复验）；`maintainWellFactors` 同洞同修（维护期间来源被编辑
  则拒绝落种）。新工程无该文件则跳过。
- 测试：`extractionSnapshotGuardsWellLogAttributeTable`（砂厚来源优先级固化、
  source_file_states 覆盖、append 后版本不复用）+
  `maintainWellFactorsSeedsFromGuardedSources`（维护路径回归护栏）。
- 限制：等长同 mtime 重写不可检（与既有守卫同界）；维护路径的「中途编辑拒绝」
  为同步路径无法插桩，未做交错测试。

### U2 重复井与井名碰撞（constraintwellfactors.cpp sourceFor）

- 改前：井点图层两要素同 `well_id` → 两行都进提取（同井双样本、双倍权重、LOO 虚高）；
  两口目录井规范化名相同（"A-1"/"A_1"）→ 后者改写前者身份，一口井样本静默消失。
- 改后：同 id 重复要素硬失败（文案对齐因子属性表「同井/层位重复」口径，点名 id）；
  名碰撞时后来的井不合并已认领行，追加独立行并注 `identity_note`（不丢行不改名）。
- 测试：`duplicateWellIdsInPointsLayerRefuseExtraction`（改前提取成功=双样本，改后拒绝）、
  `normalizedWellNameCollisionKeepsBothWells`（2 行、双井 participating、
  identity_note 在场）。既有单命中/无目录用例保持绿。

### U3 structural POWER（structuralalgorithm.cpp / structural.cpp）

- 改前：POWER=0/-1/NaN 在姊妹引擎被入口拒绝，structural 引擎静默产出全 NaN 面；
  extend 模式（默认）统一误报「缺少可用井控」。
- 改后：入口 throw「POWER 必须为有限正数」（对齐 localdirectionalgorithm 文案）；
  `buildStructuralSurface` 对 evaluateAt 的非 Ok 非 Cancelled 状态如实传播 message。
- 测试：`buildSurfaceRejectsNonPositivePowerHonestly`（0/-1/NaN + power=2 回归）、
  `powerZeroRejectedAtEntry`（Processing 级）。`syntheticGolden` 逐位不变。

### U4 参数钳制回执（localidw.cpp / localdirectionalgorithm.cpp / constraintfactorjobs_geostat.cpp）

- local_kriging K：`clampKrigingParams` 钳 K 到 [1,64] 但 QC 记请求值 → 现在发生钳制时
  issues 增 `kriging_neighbourhood_clamped 请求->生效`，QC 另记
  `kriging_max_points_effective`（仅钳制时插；K≤64 指纹不变）。
- 地统显式变差：负块金→0、ratio<1→1 的静默钳制 → `model_provenance` 增
  `nugget_clamped/nugget_effective`、`anisotropy_clamped/anisotropy_effective`
  （仅发生钳制时，对齐 rho_clamped 先例）。
- 测试：`krigingNeighbourhoodClampReceipt`（K=64 与 K=100 数值逐位一致）、
  `localKrigingClampReceiptInQc`、`explicitVariogramClampProvenance`（QC+血缘双面）。

### U5 structural QC 完整性（structuralalgorithm.cpp）

- 指纹：QC parameters 补 9 个 resolve 后生效参数（search_radius[extend→null]、
  min/max_points、interpretive_strength、barrier_buffer、barrier_shape_radius/strength、
  enable_barriers/enable_directions）——不同 SEARCH_RADIUS 等不再撞同一 parameter_hash。
  决策：CONTOUR_STOP_BUFFER 等仅影响制图输出的参数不进指纹（代码注释+PR 说明）。
- 计数：counts 补 `finite`/`nodata`（全网格口径，与姊妹引擎一致）——workflow 血缘
  `finite_cells/nodata_cells` 不再恒 null（读取段零改动即通）。
- 测试：`qcParametersCoverEffectiveInputs`、`qcCountsFiniteNodata`（与 structural.json
  独立核对、nodata>0 场景）。**预期影响**：新 structural 成果 parameter_hash 改变
  （从漏键到完整的诚实化变化）；同参数两跑 hash 仍相同。

### U6 约束 reason 词表（constraintpage_constraints.cpp）

- labels 补 6 个域层实际发出的 token（unknown_type/other_horizon/ignored/unknown_method/
  directions_disabled_by_request/interpretive_strength_zero_or_barriers_disabled）；
  `unknown_type:<type>` 前缀拼「未识别的约束类型：<type>」，`invalid_params:<detail>`
  直显域层详情；提示区补「N 条未类型化」计数（此前 semantic 为空的行不进任何桶）。
- 结构面三开关经 `paleo.page.structural*` 动态属性注入计划上下文（缺省 true 与引擎
  参数缺省一致）——仍只喂唯一解释器 `planConstraintUse`，不另立判定。
- 测试：`untypedConstraintRowShowsChineseReason`（真实主链 addConstraint type="line"）、
  `reasonVocabularyCoversDomainTokens`（参数化覆盖 other_horizon 等）。

### U7 解释性成果表（constraintpage_sections.cpp / constraintwellfactors.cpp）

- `cartographicWorkGenerated`/`interpretiveContoursGenerated` 回执即时 refreshFactorResults
  （此前只 setText，新版本要等切页才进「已保存成果」）；savedFactorResults kind 白名单
  补 `single_factor_cartographic_work` →「制图工作场」。显示/引用按钮经
  `layerForVersion` 的 raster 分支解析，无需 kind 守卫（核实）。
- 测试：`interpretiveResultsShowInFactorResultsTable`（表行 + 信号后即时刷新）。

### U8 等值线血缘键（constraintfactorjobs_contours/interpretive.cpp 写侧 + lineage 读侧）

- 写侧：两类等值线版本 extra 统一 `contour_feature_count`（OGR 只读计数，打开失败
  -1 哨兵、不改发布语义）；解释性等值线补 `analysis_sha256`（= 分析场 sha，与
  cartographic_work 版本同源）。成功语义不变（0 条仍成功发布）。
- 读侧：factorResultLineage 读 interval→「等值距 X」、levels_used→「N 级」、
  count==0→「0 条等值线」、unresolved_crossings>0→「未解交叉 N」、SHA 进 details。
  缺键不臆造（旧版本兼容）。
- 测试：`fixedLevelsPublishContourLayer`/`interpretiveCrossingKeepsAnalysisSha` 增补 +
  `interpretiveRampCountsContourFeatures` + 两个常数字 0 条用例 +
  `factorResultLineageContourExtras`。

### U9 FIXED_LEVELS 上限（constraintpage_internal.h + 两个 prepare 闸）

- parseLevels 超 10000（=kMaxContourLevels，UI 侧独立常量对齐）按 dropped 计数并截断
  保留前 10000（复用脏 token 机制，不静默截断）；两个 prepare（分析/解释性）在进
  GDAL 之前拒绝 >10000（防脚本直调）。服务层（src/qgis/factorcontour.cpp）按任务
  写域约束不改，递延。
- 发现并记录：QLineEdit 默认 maxLength=32767 会先截断超长粘贴——紧凑 token 格式
  （如 "1 1 1…"）才真正能越过 UI 层到 parseLevels 上限，测试按此构造。
- 测试：`fixedLevelsAboveCapRejectedAtPrepare`、`interpretiveLevelsAboveCapRejectedAtPrepare`
  （边界：恰=cap 通过）、`parseLevelsAboveCapCountsDropped`（纯函数 + 页面拒绝发请求）。

### U10 字段缓存（constraintpage_wellfactors.cpp）

- 缓存键 = 层位 + `paleo.page.fieldsgen`（wellAttributesChanged 世代）+ wells* 图层声明
  集合（layerId+source）。勾选切换命中缓存不再全要素扫描；层位切换/属性表变更/重声明
  正确失效。`setWellFactorFields` 每次仍执行（默认选中逻辑不缓存）。
- 测试：`wellFactorFieldFetchesAreCached`（ctor 计数 1；3 次勾选仍 1；
  wellAttributesChanged 后 2）。
- 限制：井点图层文件被外部直接编辑（不触发声明/信号变化）时勾选不再顺带重读字段
  ——主流程失效路径已覆盖，残余与原「顺带刷新」语义等价。

## 独立 Review（S5，未参与实现）

- 首轮（dbb03269..d05fbd2d）：**需修后可合**——1 Med（U10 缓存键不覆盖既有 wells
  图层就地编辑的失效路径）+ 3 Low（invalid_params detail 截断风险 / 裸 token 回退 /
  helper 两文件重复）+ Info 若干；状态机、边界、线程亲和、历史兼容、伪断言抽查
  （4 例改前红成立）、夹具真实性全部核过。
- 作者修正（commit 8dc14c65）：缓存键追加 wells source 文件 size+mtime（与 U1 守卫
  同配方）+ 新失效测试；section(1,-1)；词表回退；helper 收编 internal 头。
- 二次复审：**可合**——四处修正全部闭合、无新引入问题；残余仅理论性同毫秒等长
  改写窗口（无生产路径，与全项目守卫配方同界）。

## 验证汇总（全部实跑，持跨 worktree 锁 -j4/-j2）

- R1 干净基线（主 checkout @ dbb03269 Debug）：域内 15 测试 14 绿 + 1 环境性超时
  （gridScaleRatioGate；RelWithDebInfo 下 19/19、22s 复核确证环境因素）。
- 实现+修正后：域内 15 项关键测试两遍复跑均 100%（21/21 含外延一遍、15/15 两遍）；
  全量 ctest 408/408（修正前树）；S5 修正后全量复跑见 PR body 终值。
- 静态检查：`git diff --check`、`check_layering --strict`、`check_ui_invariants --strict`、
  `check_i18n` 全部通过（修正后复跑）。
- 数值对拍：structural power=2 golden 逐位不变；K=64 vs K=100 逐位一致；
  #457 语义（display_only 非硬屏障、引擎回执==计划）零触碰。

## 明确不做（与排除矩阵对齐）

- Open Issues #460–#485 任何子项（含同根因顺手修）；Open PR #486/#487 文件。
- #457 已交付能力不重写、不重包装。
- 新地统方法/新算法家族；SGS 组合内存闸（#468）；随机分布口径（#470）；wavelet（#471）。
- src/qgis/factorcontour.cpp（非约束专属，写域受限）——U9 服务层闸递延。
- S1-4 百分号单位换算语义、S1-6 layer_id 死代码、S2-2 undo/redo 视图重同步、S2-3 双控制器、
  S2-4 reveal、S3-4 三引擎 LOO、S4-5 陈旧标记：理由见账本「未入选候选」。
