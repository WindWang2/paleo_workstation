# Ledger — goal/sedimentary-evolution-20261004（方向35：沉积体系多期演化分析）

分支：`goal/sedimentary-evolution-20261004`（基于 origin/master @ 5424a97）

## 环境事实（实测）

- worktree `.worktrees/sedimentary-evolution`；vendor 三件 symlink 自主仓绝对路径。
- 基线构建：gcc 16.2.1 + 系统 Qt 6.11.2 + vendored QGIS/GEOS。**链接器在 8 路并发下
  段错误**（`collect2: fatal error: ld 以信号 11`，tst_seismicattrui / tst_inversion_workflow，
  内存充足仍崩）——workaround：`cmake --build -j8` 失败后用 `-j2` 收尾剩余链接，
  编译仍全程 -j8。非本方向引入，主仓同环境可复现。
- 基线 11 项 oracle 关联测试（tst_factorworkflow/tst_workflows/tst_layertreepanel/
  layering×3/ui_invariants×3/i18n×2）改前全绿。

## 交付物

| 层 | 文件 | 内容 |
|----|------|------|
| 数据 | `src/algorithms/evolution/types.h` | DTO（FaciesCoverage/FaciesChange/OverlapCell/BoundaryVector/EvolutionResult） |
| 数据 | `src/algorithms/evolution/compare.{h,cpp}` | 相邻期对比引擎：同源校验（CRS/尺寸/像元/原点逐项拒算）+ 面积/重叠矩阵/质心/边界进退（中位距、圆均方位、进比、合长度）+ 前缘位移矢量场（图幅框剔除双侧） |
| 数据 | `src/io/faciescoveragereader.{h,cpp}` | 相多边形 GPKG → DTO + PALEO_PROVENANCE 格网口径戳解析（缺戳拒读） |
| 数据 | `src/algorithms/faciespolygonize.cpp` | prov JSON 增 `grid` 戳（cols/rows/geotransform/crs_wkt） |
| 功能 | `src/workflow/evolutionworkflow.{h,cpp}` | analyzePair/analyzeSequence：解析 facies.<h> → 读取 → 内核 → 迁移矢量 GPKG（DerivedAssetRegistrar 版本化）+ declare `evolution.vectors.<h>`（05_PaleoMap 层位组）+ 指标表 CSV 资产 |
| QGIS 封装 | `src/qgis/qgisstyleservice.{h,cpp}` | `applyMigrationVectorStyle`：QgsArrowSymbolLayer + 规则渲染（进蓝/退红 × 前缘细/质心粗） |
| 视图 | `src/ui/evolution/evolutionplayerpanel.{h,cpp}` | 动览面板：步进序=mappingHorizons()、QTimer 功能步进（无编排动画）、编辑守卫、定格导出 intent |
| 视图 | `src/ui/paleomainwindow.cpp` + `_attach.cpp` | chip 行右端挂面板；attachMappingExport 接定格导出（画布抓帧 → catalog OUTPUT png 资产） |
| 布线 | `cmake/extra-evolution.cmake` + CMakeLists 清单一行 | 模块源/测试注册 |

测试：`tst_evolution_kernel`（12 用例：拒算×4/合成位移/三期矩阵/消失新生/取消/同形/口径注记）、
`tst_evolution_workflow`（6 用例端到端）、`tst_evolutionplayer`（8 用例）。

## 口径决议（诚实面）

1. **拒算不近似**：CRS/格网尺寸/像元/原点任一不同 → InvalidInput + 原因含两侧数值；
   工作流层同样拒（不重采样对齐）。旧产物无 grid 戳 → 拒（「需重 derive」文案）。
2. **指标口径**：面积/重叠 = GEOS 多边形求交（非像素计数）；边界进退 = 早期边界等距采样
   → 对期同相边界最近点，进/退 = 采样点是否落在对期同相内；图幅框（两期共有格网外缘）
   双侧剔除（采样点与目标线段都不落在框上）；位移方向 = 单位矢圆均值 + 合长度；
   未做井控密度加权。全部写入 `EvolutionResult::methodNote` 并随资产 extra 落库。
3. **质心位移 ≠ 前缘位移**：面积加权质心在单侧进积时移动幅度小于前缘（合成测试
   20 vs 40），两者都报、注释说明。
4. **GEOS 谓词返回约定**：1=true 0=false 2=异常（首版误当 2=true，探测修正）。
5. **动览 vs DESIGN.md Motion**：目标形态提到「帧间淡入」，DESIGN.md 禁编排动画
  （ui_invariants 检查器 + Motion 条款）——按 DESIGN.md 收敛为即时切换 + QTimer
   功能性步进，无淡入；定格导出走 catalog OUTPUT 版本化资产。

## 遗留（PR body 记录）

- 演化剖面视图（目标形态 4）未做——方向 26 wellsection 容器联动留待后续。
- 演化报告页（面积曲线+可编辑结论区）未做——指标 CSV/JSON 资产已可消费，
  报告 UI 留待后续方向。
- 单因素图（连续场栅格）跨期对比未纳入——本期只对相多边形覆盖；连续场的
  同格网差值面需要另立口径。

## 轮次记录

- R1（实现轮）：内核/读取器/工作流/样式/面板/测试落地，三套件绿。
- R2（第一轮自审，六维）：
  - 分层 ✓（新文件头三行词表齐全；workflow include io 为数据层依赖，无违禁）；
  - 同源校验 → 补 `isCartographicProductLayer` 守卫（解释性制图工作场不进跨期对比）；
  - 指标口径 → provenance 由手拼 JSON 改 QJsonObject（层位名含引号的注入面）；
    指标行改为 CSV 写出成功后才进会话累计（stage 失败重跑不重复行）；
  - GEOS 释放 → laterFront 线段聚合失败分支补手动回收；
  - i18n ✓（全量 tr，内核 std::string 中文按 singlefactor 先例）；
  - DESIGN.md → 定格导出反馈从 compose 页私有标签改主窗状态栏（全局动作）；
    instantiate 样式改 best-effort（不借 error 出口，避免「成功但 error 非空」）。
- R3（第二轮自审）：箭头符号补头部张开宽（只设杆宽无头宽）；其余六维复查零 High/Medium。

## 验证（终跑）

- 新套件：tst_evolution_kernel 12/12、tst_evolution_workflow 6/6、tst_evolutionplayer 8/8。
- oracle 既有面（tst_factorworkflow/tst_workflows/tst_layertreepanel/layering×3/
  ui_invariants×3/i18n×2）：全绿（见终跑日志）。
- 全量 255 项：**5 项存量失败与本方向无关**（stash 基线复跑证据）：
  tst_ui（mapBookButton 缺失 ×2 slot）、tst_cache_las（磁盘计时比率门）、
  tst_wellcomposite_visual（golden 渲染容差）、tst_projectsvc
  （openGateCancelKeepsCurrentProject）。origin/master @5424a97 基线同挂。
- `git diff --check` 干净。
- 环境注记：ld 在 8 路并发下段错误 → 编译 -j8 + 链接 -j2 收尾；ctest 全程 -j8
  且期间不得并行编译（曾致 tst_singlefactor_perf/tst_startup_trace 假失败，
  单独重跑均绿）。
