# 方向 04 ledger：单因素计算、已有地质约束与插值工作流的数值和交互质量提升

- 分支：`goal7/singlefactor-constraints-20261014` · worktree `../paleo-wt-goal7-singlefactor`
- base SHA：`b0520429c21f858a1c053b876f511680088564a9`（= origin/master，2026-10-14 核实）
- 开放 PR：仅 #437（80 文件保护区，含 CMakeLists.txt / constraintfactorjobs_geostat.cpp /
  constraintfactorjobs_internal.h / constraintwellfactors.cpp / tst_factorworkflow.cpp /
  cmake/extra-prediction-facies.cmake——全部 write-deny）
- 开放 Issue 指纹排除：#352 #379 #392 #393 #415 #417（详见下）

## R0 真实基线（2026-10-14）

### 环境接线（实测）

- 主仓无 vendored prefix（vendor/superbuild/prefix、vendor/prefix、vendor/onnxruntime 均不存在）；
  系统无 QGIS 开发包。
- 共享依赖来自 `../pw-seismic-runtime-closure-20260930-215353`（deb 闭包 QGIS 4.2.3 +
  onnxruntime），以相对 symlink 接入：`vendor/src_wt -> pw-seismic worktree 根`、
  `vendor/prefix -> src_wt/vendor/prefix`、
  `vendor/onnxruntime -> src_wt/vendor/onnxruntime`。
- **勘误（2026-10-14）**：symlink 目标生成期间多次出现路径串重复污染（本会话工具链现象），
  最终以 `readlink -f` 验证 `vendor/prefix/usr/lib/libqgis_core.so.4.2.3` 可解析为准；
  symlink 不入库，此为一次性环境事实。
- configure 实测命令（直接 cmake，非 paleo-dev——paleo-dev 的 env 注入在裸 shell 下不发生）：
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
   -DQt6_DIR=$PWD/vendor/prefix/usr/lib/x86_64-linux-gnu/cmake/Qt6
   -DQGIS_PREFIX=$PWD/vendor/prefix/usr`
  （必须显式 Qt6_DIR：Arch 无 multiarch 惯例，CMAKE_LIBRARY_ARCHITECTURE 检测为空，
  deb 闭包 Qt6 的 cmake config 在 lib/x86_64-linux-gnu 下，不带 Qt6_DIR 会落到系统 Qt6
  并因缺 Qt6SerialPort 失败。）
- 构建/测试一律 `flock -x /tmp/paleo_workstation_global_build_j4.lock` + `-j4`。

### 已有交付消重（避免重做）

- #430 四步操作流（约束线→提取井点→计算→保存引用）已交付：S3 审计确认四步门控链完整
  （generateFactorButton 七级门、contourButton 三级门、extract 四级门），跨层位引用被
  preparedWellFactorVersion（horizon+factor+params 三重全等 + 文件指纹）拦截，无陈旧引用通路。
- #435（井震/预测/单因素深化）、#436（扫尾）已合并：constraintfactorjobs.cpp 的
  startConstraintJob 已用 QPointer（#379 修复）、cb.commit 已带错误原因（#415 修复）。
- JobRunner 三段式（prepare/compute/publish + cleanup 钩子）为既有框架，本方向只在其分文件内迭代。

### 域依赖图（S1-S4 审计汇总，全文见各 subagent 报告，主 Agent 存档于本文件附录节选）

调用链：`constraintpage*（视图）→ ConstraintWorkflow::generateFactor/generateContours*
（constraintfactorjobs_factor/contours/interpretive/localdirection.cpp）→ 算法引擎
（singlefactor/localidw、krigingsurface、structural、geostat/{kriging,variogram,sgs,cokriging}）→
linsolve（唯一线性求解器，列主元 LU，全局 1e-12·max|A| 奇异阈）`。
持久化：commitExternal（derivedassets，#437 保护区）→ catalog 版本行 + registrar 声明。

### 发现分组（R0 要求的消重结论）

**已有 Issue 占用（本方向不做，留给 Issue Agent）**：
- #379 startConstraintJob 回调裸捕 this（master 已修该位置；S2 发现 runFactorWork 面的
  m_proc.data() 按值捕获为同型新实例 F:972/L:545/F:1176——机理相同，避让不修）
- #417 src/algorithms 8 项：structural.cpp:451 取消泄漏 GEOSPreparedGeom、
  fieldcontours.cpp:270 preparedDomain 泄漏、support.cpp:376 空 component 越界、
  cokriging.cpp:615 usedSamples 契约矛盾、gridsolver.cpp:761 CV 非有限 z、
  faultpath.cpp:63 pointInRing 空环（master 已含部分防护）等
- #392 fieldcontours 空 boundaries 交空域、#393 FaultPathMetric 节点/段预算
- #352 协克里金工作线程 QgisLayerService::instantiate()（修复面在保护区 workflow 层）

**PR #437 保护区（零接触）**：见文件头清单；含 tst_factorworkflow.cpp——本方向测试只能
扩展其他既有 tst 文件或在非保护 extra-*.cmake 追加。

**低价值/口径差异（文档化不修）**：
- C7 隔断档实验变差丢弃同格近距对（<cellSize）——口径差异
- C8 MM1 协克里金协变量无量纲标准化——cokriging.h 注释已声明已知病态面
- C9 OK 单样本远点方差渐近 2×基台——kriging.h 注释已声明
- S2-13 discardTempTree 子串判据——防御深度弱点，非现症

**递延（接口提案，不动保护区）**：
- S2-3 declare 失败留「伪已保存版本」（commitExternal 无回滚）——修复需 derivedassets.h
  （#437 保护区），本方向在 PR 描述中给接口提案不实现
- S2-11 stale 判定以取消面貌上报——涉及任务面板语义，下一 PR

**经核实的未占用任务（进入开发列表，按 R 值排期）**：

| # | 缺陷 | 位置 | 触发 | Oracle |
|---|------|------|------|--------|
| D1 | sgs/sgs3 模型校验漏 NaN nugget/sill → 全场 NaN 且 Status::Ok | sgs.cpp:132-141、sgs3.cpp:428-437 | NaN 变差参数直入 | 1 |
| D2 | kriging modelUsable 放行零信号模型（nugget=sill=0）→ 全格 solverFailures + Status::Ok | kriging.cpp:91-95 | 零信号裸 API | 1 |
| D3 | variogram 隔断档取消被洗成 InvalidInput（faultPathMetric Cancelled → InvalidInput 文案） | variogram.cpp:160-166 | 障碍感知拟合中途取消 | 1/4 |
| D4 | azimuthDeg=NaN + ratio>1 时 γ 静默塌缩为 0 → 矩阵奇异 | variogram.cpp:44-49、kriging.cpp:93 | 方位角未初始化 | 1 |
| D5 | sgs 2D 查询点与静态样本重合无特判（sgs3 有）：数值残差 sqrt 放大 ~1e-7 抖动 | sgs.cpp:258-270 | 格心与井位重合 | 1 |
| D6 | factor publish 的 results.isEmpty() 裸 return false 不设 error（4 处） | constraintfactorjobs_factor.cpp:257/389/515/808 | 引擎失败未设错时 | 2 |
| D7 | 同步路径 localdirection/surfer staging 段失败不清 paleo-sf- 临时目录 | constraintfactorjobs_localdirection.cpp:316-341 | m_factorTasks 空时取消/拷贝失败 | 4 |
| D8 | structural/fixed 等值线提取无取消钩子（regular 路有） | constraintfactorjobs_contours.cpp:259-265、interpretive.cpp:370 | 长提取中取消不生效 | 4 |
| D9 | contourIntervalSpin「自动(0)」残留：换勾非 structural 因素不复位 → interval=0 被服务层拒；structural 生成后不补置 0 | constraintpage.cpp:180-183 | 因素类型切换 | 2/3 |
| D10 | parseLevels 静默丢弃非法 token（「10, abc, 30」→[10,30] 无提示） | constraintpage_internal.h:58-70 | 脏输入 | 2 |
| D11 | 「（旧输入）」stale 标记被 layerDeclared 重声明静默抹掉 | constraintpage.cpp:280-283 vs markInputsStale | 任意层重声明 | 3 |
| D12 | localidw engine.solver 构造后从未使用（每次建引擎白做 dedupe+桶格索引） | localidw.cpp:250-253 | 每次 kriging 引擎构造 | 6（性能） |

测试配对（全部落在非保护测试文件，零 CMakeLists.txt 改动）：
- tst_geostat_sgs.cpp（extra-geostat.cmake:16）+`nanModelParamsRejected`（D1，先红后绿）
- tst_geostat_kriging.cpp（extra-geostat.cmake:15）+`zeroSignalModelRejected`（D2）
- tst_geostat_variogram_barrier.cpp（extra-sf-completion.cmake:9）+`cancelledBarrierTrackPropagates`（D3）
  +`oversizeBarrierGridShrinksToBudget`（4M 缩格现状钉）
- tst_geostat_variogram.cpp（extra-geostat.cmake:14）+NaN azimuth 拒绝（D4）
- tst_singlefactor_kriging.cpp（extra-sf-kriging.cmake:30）+取消直通断言（D3 编排面）

### 性能测量方案（E 方向）

- 基线：ctest -R "tst_geostat_perf|tst_singlefactor_perf"（比率门既有）；D12 修复前后
  tst_singlefactor_kriging 全文件 wall time + tst_singlefactor_perf 中位数对比（同机同条件两遍）。
- 400 万像元预算：variogram 隔断档 kCellBudget=4e6 缩格路径补测（现状无测试）。

### 风险与排除

- 黄金锚风险：D5 改 sgs 2D 抽样路径必须保持随机流推进不变（照常求值 gaussianRandom、
  乘 sqrt(0)），否则 tst_geostat_sgs 的 goldenAnchorPinsRefactoredKernel（2048 格量化张量）红。
- D9/D10/D11 属 UI 状态：修前先写可复现的失败断言（扩展非保护 UI 测试或以现状钉注释），
  遵守 DESIGN.md（禁用按钮必须带 reason tooltip）。
- 本方向不修 Issues、不动 #437 保护区、不新增算法家族。

## 轮次记录

### 轮 1（2026-10-14 晚）：D1-D12 全部实施

- 生产代码（14 文件，464 insert/42 delete）：
  - geostat：sgs.cpp（D1 NaN 拒绝 + D5 重合特判）、sgs3.cpp（D1 + verticalRangeRatio 有限性）、
    kriging.cpp（D2 零信号 + D4 azimuth/ratio 有限性，modelUsable 与网格入口双面）、
    variogram.cpp（D3 取消透传：faultPathMetric Cancelled 原样上报）
  - singlefactor：localidw.cpp（D12 移除从未消费的 engine.solver 构造——每分量引擎省一次
    dedupe+桶格索引）
  - qgis：factorcontour.h/cpp（D8 generateFixedContours 透传取消到 GDAL 回调；
    generateStructuralContours 阶段边界轮询——提线内部无检查点，注释如实声明粒度）
  - workflow：constraintfactorjobs_factor.cpp（D6 四处空 results 设默认 error）、
    constraintfactorjobs_localdirection.cpp（D7 staging 段三失败分支自清 paleo-sf- 临时目录）、
    constraintfactorjobs_contours.cpp + _interpretive.cpp（D8 三路调用接 cancelled）
  - UI：constraintpage.cpp（D9 等值距双向复位 + noteFactorLayer 补置；D11 stale 属性化
    paleo.page.factorstale.<horizon>，refreshFactorLayers 重写时恢复、noteFactorLayer 清除）、
    constraintpage_internal.h（D10 parseLevels dropped 出参）、constraintpage_generate.cpp
    （D10 点击拒绝 + tooltip 脏 token 提示）
- 测试（5 文件 +198 行，零 CMakeLists.txt 改动——全部扩展现有注册）：
  - tst_geostat_sgs.cpp + nanVariogramParamsRejected（D1 先红后绿）
  - tst_geostat_kriging.cpp + zeroSignalAndNonFiniteModelRejected（D2/D4）
  - tst_geostat_variogram_barrier.cpp + cancelledInsideBarrierFieldPropagates（D3）+
    oversizeBarrierGridShrinksToCellBudget（4M 缩格现状钉）
  - tst_singlefactor_kriging.cpp + cancelledBarrierFitPropagatesWithoutIdwFallback
    （D3 编排面：取消不冒充 IDW 面，surfaceFallbacks==0）
  - tst_mappingpages.cpp + contourIntervalResetsAcrossFactorTypes（D9）+
    parseLevelsDroppedTokensAreRefused（D10）+ staleSuffixSurvivesLayerRedeclare（D11）
- 边界复核：19 个改动文件对 PR #437 80 文件清单零命中；tools/check_layering.py --strict 绿；
  git diff --check 干净；vendor/src_wt symlink 入 worktree 私有 info/exclude（不共享、不入库）。
- 环境事实：全局构建锁与并行方向（data-catalog-preview / task-release / well-sections /
  seismic-ai）轮流竞争，本方向构建/测试按契约排队执行。

### 轮 2：先红后绿 + S5 独立 review 闭环

- **先红后绿（Oracle 3）**：git stash 撤 4 个 geostat 源修复保留新测试 → 重编 →
  3/4 红（tst_geostat_kriging/sgs/variogram_barrier，singlefactor_kriging 因 256 分辨率
  取消落在 #327 既有检查点而仍绿——S5-m2 已在测试注释中如实澄清）；stash pop 恢复 →
  5/5 绿（含 tst_mappingpages 4.4s、tst_singlefactor_kriging 34.9s）。
- **S5 独立 review（未参与实现）**：PASS-WITH-FIXES——0 BLOCKER、1 MAJOR、9 MINOR。
  S5 实跑 8 个套件（5 目标 + sgs3/contours/perf 邻接）全绿；核对 D5 随机流契约
  （每格恰一次 gaussianRandom）、D2 默认模型仍通过、D8 尾参默认值 ABI 兼容、
  D12 零残留引用、层标记齐全。
- **S5-M1（MAJOR）已修**：sgs/sgs3 补 isfinite(azimuthDeg)/isfinite(anisotropyRatio)
  （ratio>1 + NaN azimuth 的 γ(dx,dy) 塌缩路径与 kriging 同型，此前「同口径」注释不实）；
  cokriging.cpp 的 ConstrainedKrigingSolver 同款顺手补齐；tst_geostat_sgs 补两断言。
- **S5-m2/m3/m4 已修**：m2 编排面测试注释改为如实口径（256 分辨率取消落在 #327
  逐样本检查点，真正钉 D3 的是 variogram_barrier 的 512 分辨率用例）；m3 撤销
  noteFactorLayer 的等值距压回（无法区分残留与用户显式输入，静默吞输入更糟——
  跨类型残留已由勾选切换回调覆盖，取舍记录在案）；m4 interpretive 取消检查提前到
  失败分支前（与 contours 同序，避免 GDAL rc 误导文案）。
- **未修 MINOR（记录在案）**：m1（D7 注释论据修正为防御性双清——实测该路径
  Pending 析构已兜底）、m5（factorcontour 非 tr 与文件既有约定一致）、m6（零基台
  文案口径分叉）、m7（stale 属性只增不减，量级受层位数约束）、m8（20.0 魔数两处）、
  m9（localidw isVariogramUsable 未同步收紧——有 IDW 回落兜底）。
- 全量回归：S5 修复后完整 ctest -LE perf（见 PR 描述回填）。
