# 影响分析/版本对比后台化 + 万条资产导航树瓶颈修复（2026-10-28）

- 基线：origin/master `d7abab8dec20ec4138b8a1cc34fcd68bd871f342`（PR #793 后）
- 分支：`codex/perf-runtime-20261027`（独立 worktree `../paleo-perf-runtime`）
- 环境：CachyOS Linux x86_64（6.18.55-cachyos-lts）、GCC 16.2.1、Qt 6.12.0、
  vendored QGIS/superbuild prefix（RelWithDebInfo，-j8）；系统 poppler 26.10
  /draco 1.5.9 与 vendored libgdal 需求（poppler 163/draco 9）漂移，从 pacman
  缓存解出旧库到 /tmp 并以 rpath 接入（仅本 worktree 构建，未动共享 prefix）。

## 任务 A：影响分析与成果版本对比后台化（TODOS P1/R04，本批关闭）

### 问题

`MappingWorkbench::analyzeImpact`（含 `analyzeLayerVersionImpact` 入口）与
`compareVersions`（含 `compareWithRecomputeSource` 入口）在 GUI 线程同步执行：
≤64 产品 × 2 次 `replayGrid`（各至 400 万像元）+ `QgsSpatialIndex` 建索引 +
栅格 block 全读 + 图斑归属。UI 侧 `runWorkbenchQueryFamily` 用等待光标 +
状态栏挡住（代码注释自认「异步任务化在 full-audit 台账记为剩余工作」）。

### 实现（三段式，复用 JobRunner/PaleoTaskService 既有模式）

- `ImpactJob`/`CompareJob` 纯数据快照作业；`prepare`（GUI 线程）抓目录修订、
  before/after 输入版本快照、下游闭包产品快照（含 frozen inputs 替换表、
  相面/人工编辑子层、frozenGrid、facies/thresholds）；`compute`（worker 线程）
  重放 + diff + 图斑归属，**不触碰 live catalog**（目录跨线程读被
  `noteRead` 断言拒绝），图层经作业局部缓存独立打开
  （`openVersionLayer`：路径解析 + symlink 安全校验 + CRS 兜底，与
  `attributionLayer` 同口径抽出共享）；`commit`（GUI 线程）核对
  catalog 指针/工程目录/catalogRevision/输入版本逐字段快照后才写报告
  （原子写）并 emit `impactAnalysisReady`/`versionCompareReady`。
- 失效边界：取消、切工程（`bindCatalog` 主动取消 + commit 双保险）、目录
  修订前进、输入版本 extra 就地更新（`updateVersionExtra` 同版本变化）→
  过期结果一律不发布、不写报告、不登记成成功。
- UI（`paleomainwindow_workbench_commands.cpp` 查询族）：`impactAnalysis`
  /`compareRecompute` 改调 `startImpactAnalysis`/`startVersionCompare`，
  结果呈现（统计摘要/转移矩阵 Top3/报告路径）平移到
  `attachMappingEditFlow` 的 ready 信号处理器；无任务服务时同步入口
  （`analyzeImpact`/`compareVersions`/`analyzeLayerVersionImpact`/
  `compareWithRecomputeSource`）契约原样保留（测试壳与既有测试直驱）。
- 语义守恒：直接依赖/潜在影响/可确定影响三级、冻结版本、像元网格、人工
  解释保护与报告字段逐字保留；同/后台结果等价由测试逐字段断言
  （除 `analyzed_at`/`report_path`）。

### 附带修复（基线红项，实测 3/3 复现确认先于本批存在）

`tst_impactrecompute::controlledRecomputePreservesHistoryAndIsIdempotent`
在 pristine 基线确定性失败：幂等续跑扫描把算法参数里的 `ordered_inputs`
（计划携带的冻结旧输入快照）当参数与成果存档的新输入逐键比对，永不命中。
修复：该键跳过参数比对——输入身份已由既有的 `actual/plannedInputs`
按序列表比对（PW-145 顺序语义不变）。

### 验证（`tst_impactrecompute` 10/10 绿）

`backgroundImpactAnalysisLifecycle` / `backgroundVersionCompareEquivalenceAndCancel`：
同步/后台结果等价（小输入 + 200×200 网格大输入）；`start` 非阻塞
（<500ms）；忙拒绝；排队期协作取消（占满池 blocker 确定性同步点）；
排队期目录修订前进（旧请求晚到不发布）；切工程重绑不发布；对象销毁
（在飞作业 + runner + workbench 全销毁不崩溃）；GUI 响应性以可控同步点
证明——排队期与 worker 执行期间 `processEvents` 持续处理事件（busy 态
可见），不依赖 sleep。取消/过期路径不写报告文件。

## 任务 B：万条资产访问瓶颈（PALEO_CATALOG_SCALE=10000）

### 方法

`makeSyntheticCatalogDir` 公开夹具（10k 井/资产/受管版本/链接，确定性），
同机同配置串行运行；测试新增分阶段绝对计时（fixture/open/legacy/refresh/
filter/sort/locate/restoreRefresh 毫秒值）；产品代码加 `PALEO_PAGE_PERF=1`
env 门控的阶段计时器（缺省零开销）。旧实现对照 = 测试内同 10k 目录的
全行/全单元格控件 QTableWidget 循环（既有 baseline 通道，非分页实现成本）。

### 瓶颈定位（改前，两次采样）

`refreshAssetTable` ~380ms/次：行快照 25ms、过滤 1–3ms、排序 19–24ms、
**导航树重建 `refreshAssetTree` 245–310ms/次（首次 2559ms）**——且表格/
图标/虚拟/分组视图下无条件执行，过滤/排序/定位/每次目录变更全部被它
主导。GUI 线程文件 stat 为 0（既有断言保持：`refreshStats 0`、
`baselineStats 120000`），路径解析不是当前瓶颈（`buildAssetRow` 在 >200
资产时已跳过 stat）。

### 修复（失效边界显式）

树视图不可见（viewStack 当前页 ≠ 0）时 `refreshAssetTree` 挂脏标记返回；
`setViewMode(0)` 切回树视图强制补建并按持久选中身份（`m_selKeep`）还原
树选中；`selectAssetInViews` 在树不可见时跳过树全项迭代/clearSelection
（选中身份持久在 `m_pageSelection`，切回时还原）。**树可见期间从不陈旧**
（视图切换即失效边界，无时间维度缓存）；无视图栈（树单视图形态）保持
立即重建。行为守卫测试 `treeDeferredRefreshRecoversOnViewSwitch`：
表格视图期间目录前进（新增资产）→ 切回树视图补建到最新状态且选中还原。

### 改前/改后交错测量（同机、同 10k 夹具、B/A/B/A/B/A 串行）

| 阶段 | before 中位 | after 中位 | 提升 |
|------|------------|-----------|------|
| refreshAssetTable | ~402ms | ~52ms | **7.7×** |
| filter（applyListFilter） | ~359ms | ~10ms | **36×** |
| sort（两次列头点击） | ~727ms | ~67ms | **10.8×** |
| locate（selectAssetInViews） | ~359ms | ~13ms | **27×** |
| restoreRefresh | ~394ms | ~57ms | **6.9×** |

样本见 `/tmp/ab-measure.log`（3 轮交错，各阶段改善远超运行噪声；
机器背景负载 ~24/16 核，两版本交错同承受）。全部既有断言保持绿
（分页/全局排序/过滤/跨页定位/选择恢复/渲染对象上限/零 GUI stat）。

### 与历史口径的关系

TODOS P2（Windows 万条基准分阶段测量）的 Linux 侧分阶段计时已由本批
测试内建（含旧实现对照通道）；Windows 运行仍待做（该项不关闭）。
历史采样点 `resolvedVersionPath → canonicalFilePath → stat` 在当前代码的
刷新路径上已为零（>200 资产跳过 stat + 上述断言），本次实测复证。

## 验证汇总

- 直接受影响：`tst_impactrecompute` 10/10、`tst_assetpaging` 13 过 1 跳
  （scale 需 env）、`tst_composeattribution`、`tst_mappingworkbench`、
  `tst_batchjobpanel`、`tst_previewdoc`、`tst_panels*`、`tst_datagraphpage`、
  `tst_projectsetup`、`tst_paleomaplibrary`、`tst_shortcuts_shell` 全绿。
- 源码闸门：layering/layering_strict/symbol_audit/qgs_audit、qgis_spread*、
  ui_invariants*、ui_size*（clean）、file_budget*（54=基线）、i18n、ui_tokens、
  qgis_versions、docs_drift 全绿；`layering_transitive` 的
  `ui_qtwidgets_notes 6>5` 为**基线预先存在的红**（pristine 源码复现，
  第 6 条边 `previewmappage.h:7 → previewmaptools.h → qgsmaptool.h →
  QGestureEvent` 与本批改动无关；本批前后计数相同 6）。
- 基线预先存在的测试红（pristine 复现，非本批引入，未修）：
  `tst_welllog_ui::wellLogPrimaryButtonPromotesLink`
  （`resolvedWellLogPrimary("ast-secondary")` 失败，属 R14 数据操作域）。
- `PALEO_PAGE_PERF` 计时器缺省零开销（env 查询一次 + 纳秒计时），
  `PagePerfStage` 在 pageshared.h（视图层公共助手，无新依赖）。

## 文件清单

- `src/workflow/mappingworkbench.h/.cpp`：ImpactJob/CompareJob 三段式声明
  与 bindCatalog 取消；`mappingworkbench_impact.cpp` 重写为
  prepare/compute/commit + 后台入口 + 幂等键修复；
  `mappingevidencereview.cpp` 抽出 `openVersionLayer`。
- `src/ui/paleomainwindow_workbench_commands.cpp`（命令改后台启动）、
  `paleomainwindow_workbench_editflow.cpp`（ready 信号接线与结果呈现）。
- `src/ui/pages/`：datalist.h（m_treeStale）、datalist_tree.cpp（挂脏延迟）、
  datalist_table.cpp（setViewMode 补建+还原选中）、datalist_select.cpp
  （树不可见跳过树操作）、datalist_filter/datalist_undo（阶段计时）、
  pageshared.h（PagePerfStage）。
- `tests/tst_impactrecompute.cpp`（两个后台生命周期测试）、
  `tests/tst_assetpaging.cpp`（分阶段计时 + 挂脏行为守卫）。
- `TODOS.md`：关闭 P1/R04。
