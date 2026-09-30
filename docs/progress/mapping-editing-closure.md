# goal/mapping-editing-closure — 编图/编辑/算法收敛工作包

> 分支 `goal/mapping-editing-closure-20260930-215309`（base master `ac882cf`，
> worktree ../pw-mapping-editing-closure-20260930-215309）。
> 范围：预测—约束—单因素—综合编图—编辑—验证—发布既有链路的正确性与
> 一致性收敛。不新增算法方向、不新增产品页、不做多 realization。

## Phase 0 对账（2026-09-30）

- **BASE_SHA**: `ac882cfcfb93f606fdcc6c8813b71380759f86bd`（PR #64 合并后）
- **open PR**: 0。
- **merged PR 摘要**（近 12 个）：#64 deepen-perf（ConstraintIDW typed 语义/
  distance transform/paleoAssetId/fault_cut——本包直接前置）、#63 Windows CI、
  #62 地震链路、#61 时深、#60 IO 性能、#59 预览地图化、#58 数据页操作、
  #57 井综合柱状、#56 相修订/地图改相、#54 horizon-aware workbench、
  #53 测试布线、#51 词表/拓扑/会话加固、#50 地震引擎。
- **Issue 真伪对账**（open ≠ 现状，逐条在 ac882cf 复核）：
  - `already-fixed-on-master`：#24（QPointer+finalizeSession+tst_edittools:3127）、
    #26（appcontext.cpp:190-205 取锁四落盘面）、#27 主路径
    （commitEdit 走 enqueueWrite；残留见下）、#28（PaleoAlgorithmWidget
    closeEvent/析构 cancel+waitForFinished）、#32（attributetablepanel destroyed→
    clearTable）、#33（horizonbinner 100M 上限+rotated 拒绝）、#37（audit §3a
    与代码一致）、#38（origin well_map/seismic_map 分立+settle 4 代上限）、
    #42 后半（publish isSafePathSegment）。
  - **仍真实存在（本包修复）**：
    1. 栅格算法无像元总数上限（IDW/welldist/DT 三处 double→int 截断 UB +
       整网格前置分配失控，bad_alloc 不转 Processing 错误）——#33 的算法侧同款。
    2. 取消粒度不全：DT 的 Dijkstra/屏障栅格化、IDW 的 BFS 标号/屏障栅格化
       无 isCanceled。
    3. 地理 CRS 无原因态（度 ≠ 米静默直算）。
    4. ANISO_RATIO 无上界（极端值静默全 nodata/NaN 面）。
    5. enqueueWrite/saveAll 内异常裸穿（busy 释放/writeFailed 信号被跳过）。
    6. 定位器（Ctrl+K）切层位无编辑拦截——静默回滚丢成果 + busy 滞留
       （releaseHorizon 直连 rollBack 不经服务）。
  - 非本包域：#25（地震，WP1）、#29/#30/#34/#35（Windows/CI，WP4）、
    #31/#36/#39（架构，WP4）、#40/#41（IO 卫生，WP2/WP4）、#43/#44/#45
    （UX/i18n/token）、#46（构建卫生）。
- **远程分支重叠矩阵**：全部 wave/* 与 codex/prediction-facies-editing、
  codex/sections-time-depth ahead=0（历史分支）。`codex/simplify-duplication`
  ahead 1（fc64457 去重重构：mappingsamples.h 采样收敛/metastore 快照公共化/
  maptools capturehelpers）——与本包文件域重叠（mappingworkflow/
  mapversioncontroller），**不 cherry-pick**；其重复消除价值在 WP4（generic
  去重）裁决，本包只在顺手处收口。
- **基线**：本地 QGIS 4.2.0 vendored 环境（paleo-workbench qgis-vendor +
  pwb-sdks SDK 组装，见「构建环境」节）。全量 ctest 基线见「验证」节。

## 交付（逐轮）

### Round 1 — 三算法引擎输入域加固（fix）

`src/algorithms/paleoalgorithms.{h,cpp}` / `welldist.cpp` / `distancetransform.cpp`：

- **网格预算守卫**（新 `PaleoAlgoGuards::gridDimsForExtent`，三引擎同口径）：
  像元总数 > 1 亿（horizonbinner 同口径）或维度超 INT_MAX → 显式
  QgsProcessingException；消灭 double→int 截断 UB 与整网格前置分配失控。
- **地理 CRS 原因态**：`PaleoAlgoGuards::geographicCrsWarning` 经 feedback
  pushWarning（不拒绝——历史兼容；三引擎同文案）。
- **ANISO_RATIO ∈ [1,1000]**：<1/非有限钳 1（既有），>1000 显式拒绝。
- **非有限输出 → nodata**：IDW 权重和产出的 inf/NaN 不再进栅格。
- **取消粒度**：DT 屏障栅格化（逐段）+ Dijkstra（每 4096 松弛查取消+报
  进度）；IDW 屏障栅格化（逐段）+ BFS 标号（每 1024 格查取消+报进度）。

测试（tst_algorithm_harness +6）：gridBudgetRejection /
geographicCrsWarningSurfaced / anisoRatioUpperBoundRejected /
dtCancellationHonoredBeforeRowWrite / idwBfsCancellationHonoredBeforeRowWrite
（+harness run() 扩展：feedback 覆盖与文本回传）。**修复前失败证据**
（BASE 生产码 + 新测试，/tmp/before_fix.log 留档）：预算测试在 BASE 上
走到 GDALCreate 失败（无预算报错）、CRS 告警缺失、2000 比值被静默接受、
两个取消测试第 0 行已写出（BFS/Dijkstra 不可取消）。

### Round 2 — 编辑会话生命周期收口（fix）

- `paleoprojectstore.cpp`：enqueueWrite / saveAll 的 gpkgCommit 异常转
  WriteResult 失败（裸异常会跳过调用方 busy 释放并丢 writeFailed 信号；
  saveAll 的 abort-before-qgz 排序对异常同样成立）。
- `qgislayerservice.{h,cpp}`：`setEditingService`（可选注入）；
  releaseHorizon 遇编辑中图层经服务回滚（busy 随会话释放）——未注入维持
  旧行为（测试裸用路径）。组装根接线（appcontext）。
- `paleomainwindow_attach.cpp`：定位器（Ctrl+K）切层位补编辑拦截（与
  HorizonChipBar 同口径 + QgsMessageLog 原因文案）——原先绕过拦截，经
  releaseHorizon 静默回滚丢成果且 busy 滞留。

测试（tst_runtime +2：异常→失败结果/队列不腐蚀/saveAll qgz 零调用；
tst_layerservice +1：releaseHorizon busy 释放 + editRolledBack 信号 +
声明权威保持）。BASE 无法编译（API 新增型），泄漏面为代码事实。

### Round 3+4 — 合成几何边界矩阵（test-only）

tst_algorithm_harness +8：多 break_line（共线两段各自栅格化）、四面墙
口袋隔离（凹形/孔洞 + ROI 语义钉板）、重合井（格心精确命中取要素序第
一 + 非命中格等权混合 + 双跑逐位一致）、极近井全格有限、单井常数面、
非有限 z 跳过/全坏拒绝、DT 种子重合+确定性、DT 分辨率一致性（cell 1 vs
0.5 同世界点距离一致）、DT 不可达口袋 nodata。

### Round 5 — lineage E2E（test-only）

tst_factorworkflow `welldistLineageEndToEndReopen`：井点声明 → welldist
因素生成 → 派生登记（DERIVED/managed/sha256/extra engine+kind+
manifest_layer_id 反链）→ 实例化（paleoLayerId/paleoAssetId 双向链）→
重跑幂等补章（含 declare replace 分支的实例替换口径）→ saveVersion →
全新 manifest/catalog/version 实例重开后逐跳完整。

### 语义钉板（不是缺陷，冻结声明）

- ≥2 条不共线 break_line 的凸包=多边形 ROI（屏障即边界，凸包外含井全
  nodata）；单条线退化不裁剪。已写入 ALGORITHM_AUDIT §3b。
- declare() 的 replace 分支：重生成换版本文件路径时销毁旧实例重建——
  E2E 测试按「重取实例」口径断言（lineage 无损）。

## 构建环境（本机复原记录）

宿主 Arch 无系统 QGIS/Qt6SerialPort/GDAL——按 BUILDING.md 的 vendor 路线
组装（deb 闭包路需 apt，Arch 不可用）：

- QGIS 4.2.0 库：`~/project/paleo-workbench/native/qgis_render_bridge/build/
  qgis-vendor/output/lib/libqgis_{core,gui,analysis}.so`（本机既有源码构建）
- 头：third_party/qgis src/{core,gui,analysis} 展平 + 构建生成头
  （qgsconfig/qgsversion/qgis_{core,gui,analysis}.h/ui_*.h/nlohmann）
- GDAL/PROJ/GEOS/Qt6SerialPort：`~/pwb-sdks/root/usr`（CMAKE_PREFIX_PATH +
  QT_ADDITIONAL_PACKAGES_PREFIX_PATH）
- srs.db/qgis.db/svg：qgis-vendor resources + output/data
- 组装脚本：`~/paleo-env/env.sh`（QGIS_PREFIX_PATH/LD_LIBRARY_PATH/GDAL_DATA/
  PROJ_LIB）——worktree 外，不进版本库

注：QGIS 4.2 的 OGR provider 编入 qgis_core（无独立插件），provider 面齐全。

## 验证

（最终两轮数字后填充）
