# WP4 — Architecture / Shell / DevEx / Duplication Closure

> 分支 `goal/architecture-devex-closure-20261001-005639`（worktree
> ../pw-architecture-devex-closure-20261001-005639）。本文是该工作包的
> 审计底账与进度记录；`codex/simplify-duplication` 的逐 hunk 处置见
> §2。`.goal-loop-ledger.md` 为执行账本（不入库）。

## 1. Phase 0 对账（BASE_SHA = ac882cf）

- **open PR = 0**；最近 merged 波次 #52–#64 已读（ux-polish / devex-infra /
  mapping / prediction / wellcomposite / dataops / preview-map / io-perf /
  sections-time-depth / seismic-chain / ci-stabilize / deepen-perf）。
- **远程分支**：除 `codex/simplify-duplication`（ahead 1 / behind 18）外
  全部 ahead=0（历史遗留，建议后续清理，不在本 PR 动）。
- **基线测试**：PR #64 报告 127/127 全绿；本 worktree Debug 全量构建 +
  ctest 结果见 §5。

### 1.1 Open Issue 真伪对账（2026-10-01 复现）

| Issue | 复现结论 | 说明 |
|---|---|---|
| #25 P1 SEG-Y 连续换测线竞态 | **残留（WP1 领地）** | `seismicsectiondockwidget.cpp:1047/1402` 相邻线 prefetch 仍裸 `QThreadPool`；TODOS P2「剖面 dock 取数迁 SeismicTaskService」同源。本 lane 不抢修，seam note 记档 |
| #26 P1 ProjectDirLock 未取锁 | already-fixed | `appcontext.cpp` 打开工程经 ProjectDirLock 排他 |
| #27 P1 保存版本绕写队列 | already-fixed | `mapversioncontroller.cpp:169` 走 `enqueueWrite`；edit 经 `m_editSvc->commitEdit` |
| #28 P1 算法对话框 UAF | already-fixed（结构性） | widget 成员裸 `m_context/m_feedback` 模式已不存在；任务走 PaleoTaskService shared_ptr |
| #29 P1 MSVC 缺 /utf-8 | already-fixed | CMakeLists `add_compile_options(/utf-8 /W4)` |
| #30 P1 tst_boot 硬编码 /usr | already-fixed | `tst_boot.cpp:55` 读 `QGIS_PREFIX_PATH` |
| #31 P1 QGIS vendor 被绕过 | **部分残留** | linkage 已无 ui/ include；workflow 直连 `<qgs*>` 是既定布线（分层表未禁），全面收口超本 lane 预算，记档 |
| #32 P2 属性表面板 horizonReleased 零消费 | 未验证（WP3 领地） | mapping page 主逻辑 |
| #33 P2 horizonbinner 无上界 | 未验证（算法） | 非本 lane |
| #34 P2 Windows 不 vendor ONNX | **残留（build hygiene）** | 大项，本 PR 只记档 |
| #35 P2 OSGeo4W 未钉版+CI 零缓存 | **残留（CI 侧）** | manifest.json 已有安装器摘要；包闭包钉版与 CI 缓存不本地可证，记档 |
| #36 P2 PaleoMainWindow 上帝对象 | **部分残留** | 已拆 5 文件（1567+1780+627+248+313），attach 仍 1780 行；本 PR 评估再拆（见 §4） |
| #37 P2 ALGORITHM_AUDIT 失真 | 未验证 | PR #64 C1 已实装 ConstraintIDW typed；待核对文档 |
| #38 P2 联动互吞 | already-fixed | origin 已分化 `well_map`/`seismic_map`；settle 有代数上限（4） |
| #39 P3 分层纪律 | 大部分 already-fixed | domain→io include=0；AreaRules 非进程全局；paleo_core 已改 INTERFACE 伞+分层静态库；AppContext 编排待评估 |
| #40 P3 落盘纪律 | 大部分 already-fixed | .bak 走 tmp+`paleoReplaceFile`；LO 独立 UserInstallation profile（dataimportservice.cpp:1717） |
| #41 P3 .running 单文件 | already-fixed | `.running-<pid>`（crashreport.cpp:330） |
| #42 P3 静默容错缺口 | MapVersionStore 侧 already-fixed（`isSafePathSegment` :382）；SegyReader 侧 WP1 领地 |
| #43 P2 DESIGN 控件映射 | already-fixed（裁决入 DESIGN.md 2026-09-28 决策日志） |
| #44 P2 i18n 三面板失守 | already-fixed（主体） | 残留 QStringLiteral 均 objectName/符号；CI 门禁仍缺（本 PR 评估补轻量扫描） |
| #45 P3 token 纪律 | **部分残留** | PaleoTheme token 已立；ui 内裸 hex 仍多（数据符号色与 chrome 混杂），全量翻牌超本 PR 预算，记档 |
| #46 P3 构建卫生四小项 | 大部分 already-fixed | OGR2OGR 死变量/幽灵脚本已删；fonts/OFL.txt 已备；文档计数漂移本 PR 修 |
| #47 评审汇总 | 元 issue，按上述逐项对账 |

### 1.2 远程分支重叠矩阵

| 分支 | ahead/behind vs master | 处置 |
|---|---|---|
| codex/simplify-duplication | 1/18 | §2 逐 hunk 处置（本 lane 特殊职责） |
| 其余 8 个 wave/*/codex/* | 0/N | 历史已合并；建议维护者清理（不在本 PR） |

## 2. simplify 分支 hunk disposition 表

`fc64457`（基点 a8c009c）→ master ac882cf。**14 个目标文件中 13 个与该
分支基点逐字节一致**；唯一漂移 `src/workflow/workflows.cpp`（+185/−6：
C4 stampLayerAssetLink 盖章 + C5 generateDistanceFactor 第三引擎）。

| # | hunk（simplify 版） | master 现状 | 处置 |
|---|---|---|---|
| 1 | `capturehelpers.h` 新增（resolveCadDock/transformToCanvas），paleomaptools/paleoshapetools 收口 | 两文件与基点一致；`editingtools.cpp` 另有第 3 份本地 `resolveCadDock`（注释自认「kept local because sharing … would mean including paleomaptools.cpp」——正是本抽取要解的） | **移植 + 扩展**：editingtools 一并收口（3 消费者） |
| 2 | `mappingsamples.h` 新增（timeForTop/pickSamplePoint/sampleRasterAt），mappingworkflow/mapversioncontroller 收口 | 两文件与基点一致 | **移植** |
| 3 | `MetaStore::openConnection`，三 store ensureOpen 收口 | 三文件与基点一致 | **移植** |
| 4 | `PaleoProjectStore::backupQgz`，saveAll/commitAll 收口 | 与基点一致 | **移植**（+分支同款失败路径测试） |
| 5 | `SelectionContext::settlePending` | 与基点一致 | **移植**（+分支同款 settle 测试） |
| 6 | geojsonaffine `CoordinateBounds` | 与基点一致 | **移植**（+bounds 回读断言） |
| 7 | `PaleoVertexTool::updateDragPreviews`（press/move 两份预览循环） | 与基点一致；release 提交路径第 3 处循环语义不同（真提交 vs 预览），不并 | **移植**（仅预览两份） |
| 8 | `WellCompositeCanvas::refreshScaleTracks`（zoom/reset/setRatio 三份） | 与基点一致；`setTracks` 第 4 处尾不同（无信号），不并 | **移植**（仅同尾三份） |
| 9 | `ConstraintWorkflow::declareFactorResult`（两引擎声明尾） | **漂移**：尾新增 `stampLayerAssetLink`；新增第三引擎 `generateDistanceFactor` 同尾 | **移植适配**：helper 增 assetId 参数；覆盖三调用点 |
| 10 | `residualSummaryJson` 形参名 pd→projectData（tidy） | 已随 master 演进吸收 | drop |
| 11 | tst_linkage +2、tst_runtime +1、tst_import +1 断言 | 测试文件与基点一致 | **移植** |
| 12 | docs/progress/simplify.md | 该分支自证文档 | drop（本文档替代对账） |

**不移植**（分支「审查后保留的差异」仍成立）：曲线自动分组、连续/离散
XML 曲线、地震抽取/解释注册、道绘制/轴绘制框架——语义差异或抽象收益不足。

## 3. 实现记录（提交序列）

| commit | 内容 | 验证 |
|---|---|---|
| 4d52b76 | fix(devex)：闭包 prefix 测试环境两根因——tst_onnx* 的 `LD_LIBRARY_PATH` 整变量替换改 `ENVIRONMENT_MODIFICATION path_list_append`（保留继承的闭包库路径，ORT 追加不抢）；`paleo-dev test` 自动补 `GDAL_DRIVER_PATH`（闭包 GDAL 插件不在编译期搜索路径）+ BUILDING.md 记档 | tst_onnx 2/2、tst_previewmap_assets 29/29 两遍绿 |
| de0f930 | test(previewmap)：zoomHistoryBackForward 精确相等改 1e-9 相对容差（QgsMapCanvas::setExtent 纵横比重排 ulp 噪声，实测 xmax 差 2.8e-13） | tst_previewmap_canvas 25/25 两遍绿 |
| 66aa37f | refactor(metadata)：`MetaStore::openConnection`（三 store ensureOpen 收口）+ `PaleoProjectStore::backupQgz`（saveAll/commitAll 备份块收口）+ 分支同款备份失败回归测试 | 6 store 套件两遍绿 |
| cd0e56e | refactor(linkage)：`SelectionContext::settlePending` 收口 + 层位合并/runaway 上限测试 | tst_linkage 9/9 两遍绿 |
| d40ff0e | refactor(io)：geojsonaffine `CoordinateBounds` 收口 + bounds 回读一致断言 | tst_import 45/45 两遍绿 |
| —（R4）| refactor(workflow)：`mappingsamples.h` 三原语收口（T25 同口径承诺） | 5 mapping 套件两遍绿 |
| —（R5）| refactor(ui/maptools)：`capturehelpers.h` 收口 **+ editingtools 第 3 份 resolveCadDock 一并收编**（超出 simplify 版：其注释自认 kept local 因无处共享） | tst_edittools/tst_ui/tst_mappingworkbench 两遍绿 |
| —（R6）| refactor(ui)：`updateDragPreviews`（press/move 两份）+ `refreshScaleTracks`（zoom/reset/setRatio 三份；setTracks 尾不同不并；release 提交路径语义不同不并） | tst_edittools + wellcomposite 10 套件两遍绿 |
| —（R7）| refactor(workflow)：`declareFactorResult` **适配 master 演进**（三引擎调用点 + assetId 形参承载 C4 盖章；旧 idw 路径声明形状不同不并） | 5 workflow 套件两遍绿 |
| —（R8）| feat(devex)：`tools/check_i18n.py` 门禁 + ctest `i18n`/`i18n_selftest` + 清零 4 处真文案（main.cpp 崩溃提示标题、工厂页签名、SVG/PDF 元数据标题×2） | 门禁绿 + 4 受影响套件两遍绿 |
| 701db6b | chore(i18n/docs)：lupdate 同步 3 新 context；BUILDING.md 测试计数改「以 ctest -N 为准」（#46 残留项） | lupdate 3 new/3019 existing |
| b48d6ed | test(devex)：tst_procdialog/tst_ui/tst_uxtheme 的固定 /tmp setPath 改每运行 QTemporaryDir（TODOS B5 遗留；根治跨运行陈旧状态） | ctest + 直跑两遍绿 |

净行数：生产源 −169 行（重复块删除 > 新公共头），测试 +~150 行（新增等价/回归用例）。

## 4. Shell/DevEx 附加项处置

- **#36 PaleoMainWindow 上帝对象**：前序 wave 已实质拆解（mainwindow 主
  TU 1567 + attach 1780 + workbench 627 + sections 248 + appcontext 436，
  paleoribbon/paleotheme 承载样式知识）。attach 内部已按 10 个 `attach*`
  方法分段清晰；进一步机械拆文件只挪行数不降复杂度，且 mapping 接线段
  与 WP3 活跃文件域重叠——**记档递延**，不做 vanity split。
- **#31 workflow 直连 `<qgs*>`**：分层表未禁（qgis 封装层豁免只约束
  QtWidgets/ui include）；全面收口是 DAG 级工程，超本 PR 预算——记档。
- **#34 Windows vendor ONNX / #35 OSGeo4W 钉版 + CI 缓存**：CI/平台侧
  事项，本地不可证——记档待 CI 属主。
- **#45 token 纪律**：PaleoTheme 双主题 token 已立（#52/#55）；ui 内裸
  hex 442 处混有数据符号色（DESIGN.md:93 域配色不属 UI token）——全量
  翻牌需逐处判定 chrome vs 数据语义，超本 PR 预算，记档。
- **测试计数文档**：BUILDING.md 改「以 ctest -N 为准」（本 PR 后 131 项）。

## 5. 验证底账

- Debug 全量构建绿（ninja -j4，699 目标）；零新增自有文件警告。
- `tools/check_tidy.py --base ac882cf`：18 个改动 TU 全绿（clang-tidy 22.1.8，
  仓库 .clang-tidy 配置）。
- `check_layering.py --strict` 绿（每轮提交前各跑一次）。
- `paleo_selfcheck` 绿（providers/srs.db/fixture/非均匀渲染）。
- 全量 ctest 结果见下（两遍 + 性能门复测口径）：
| 轮次 | 结果 | 红集 |
|---|---|---|
| BASE_SHA 基线（本 worktree，负载 ~12） | 120/129 | tst_onnx×2（LD_LIBRARY_PATH 覆盖）、tst_previewmap_assets（GDAL 插件）、tst_previewmap_canvas（ulp 比较）、性能×4 + correlation_full |
| 本分支终轮 1（负载 ~2.5） | 126/131 | tst_cache_las（50.2/50ms）、tst_perf_segyindex（13.3/5ms）、tst_perf_regress（0.51/0.42）、tst_perf_las（50.2/50ms）、tst_correlation_full（3100/3000ms） |
| 本分支终轮 2（负载 ~2.5） | 128/131 | tst_cache_las、tst_perf_segyindex、tst_perf_regress |
| 修缮后终轮 3 | 125/131 | 终轮 2 的三性能红 + tst_perf_las/correlation_full（负载回升至 ~3.3）+ tst_relocate 单次 setup flake（makeStack nullptr；3 连直跑 + ctest 复跑全绿，与本分支改动无调用面交集，WP3 账本同款负载 flake 记录） |

性能门三红的处置依据：与 BASE_SHA 在本机的基线红完全同集（非本分支
引入；本分支不动 LAS/SEGY IO 热路径），且三个并行 WP worktree
（seismic/data-io/mapping）各自基线记录同款红集 + simplify 分支当年在
第三台机器的基线也是同三红（cold 70.42ms / index 21.27ms / ratio 0.44）。
门限值不改（阈值须同机 A/B 证据，本机对 BASE 无劣化）。

- `git diff --check` 绿；`git status` 干净（账本除外，按惯例不入库）。
- 独立只读 review：无 P0/P1；1×P2（i18n 脚本 UTF-8 守卫）+ 3×可安全修
  P3 已修（eae504a），其余 P3 记录在案（paleo-dev 探测目录覆盖面、
  i18n 扫描器跨行首参 >3 行的漏报——门禁定位是挡增量回归，非全量清算）。
