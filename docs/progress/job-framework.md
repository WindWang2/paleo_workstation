# 任务框架统一 + 巨型文件拆分（JobRunner）

状态：**框架、2 处迁移面及 workflow/预览拆分已落地**。下文轮 0–4 的行号、
文件规模、Windows 环境故障及测试数均为当轮历史快照；不能当作当前路径索引。
现行路径与审查结果见文末 2026-10-03 对账。历史 `tst_jobrunner` 11/11、
`tst_propworkflow` 15/15、`tst_factorworkflow` 21/21 绿。轮 4 的拆分代码与轮 5 的探针已进入 master，旧窗口
未执行的测试由文末本轮 Linux 基线/重构对照补齐。
未跑完的 Oracle 不记为通过。

## 范围

- 目标分支：`goal/job-framework-20261003`
- 工作目录：`.worktrees/jobframe`
- 目标基线：`ede43ce`（PR #112 合并点）
- 相关历史：#85（版面导出与属性建模移出 GUI 线程）、#106（catalog owner-thread
  写守卫）、#111/#112（fault-surface 与 UI 工作流打磨）
- 交付形态：本文档即账本（沿用 `docs/progress/*.md` 内嵌账本先例，无独立 ledger 文件）

## 轮 0 勘察定案

### 任务书基线漂移修正

勘察首轮发现本地 master 落后 `origin/master` 43 个提交，同步后实测行数与任务书基线**一致**：

| 文件 | 任务书基线 | 实测（`ede43ce`） |
|------|-----------|------------------|
| `src/workflow/workflows.cpp` | 4086 | 4086 |
| `src/ui/datapreview/datapreviewtabs.cpp` | 5249 | 5249 |
| `src/workflow/propertymodelworkflow.cpp` | 767 | 767 |
| `src/ui/paleomainwindow_attach.cpp` | 2815 | 2815 |

### 三段式全集：实测 4 处 6 组（任务书记 5 处）

`prepare*` 符号全仓扫描结果——三段式契约的真实分布：

| # | 组 | prepare | compute | commit | 位置 | 现状编排者 |
|---|-----|---------|---------|--------|------|-----------|
| 1 | 局部方向单因素 | `prepareLocalDirectionJob` | `computeLocalDirectionJob` | `publishLocalDirectionJob` | `workflows.cpp:1685/1786/1887` | `paleomainwindow_attach.cpp:1427` |
| 2 | 分析等值线 | `prepareAnalysisContourJob` | `computeAnalysisContourJob` | `publishAnalysisContourJob` | `workflows.cpp:2131/2217/2271` | `paleomainwindow_attach.cpp:1665` |
| 3 | 解释性等值线 | `prepareInterpretiveContourJob` | `computeInterpretiveContourJob` | `publishInterpretiveContourJob` | `workflows.cpp:2798/2959/3121` | `paleomainwindow_attach.cpp:1567` |
| 4 | 属性建模 | `requestFromCatalog`（隐式 prepare） | `runCompute` | `commitComputed` | `propertymodelworkflow.cpp:434/573` | `paleomainwindow_attach.cpp:501/552` |
| 5 | 断层成面 | `build` | `produce` | `saveToStore` | `faultsurfaceworkflow.cpp` | **无生产调用面**（仅测试） |
| 6 | 版面导出 | 参数快照（内联） | worker lambda | 内联回包 | `layoutexportactions.cpp:263` | 同文件 |

**与任务书的差异**：
- 任务书说 `faultsurfaceworkflow`（#111 新增）是三段式之一——**实测不成立**。
  `FaultSurfaceWorkflow` 全仓**只有测试在用**（`tst_faultsurfaceworkflow.cpp`），
  `src/` 下无任何生产调用点，`faultinterpretationcontroller.cpp` 完全不引用它。
  它的 `build`/`produce`/`saveToStore` 是**纯同步 API**：`produce` 内部把
  「成面 → 写 mesh JSON → registrar.commit」三步串在一个函数里，没有任务池接线、
  没有取消、没有进度回包。因此它**不是可迁移的三段式**——迁移它需要先给它造一个
  异步编排面（属于新功能，不是重构）。**轮 2 迁移对象改为组 4（属性建模）**，
  组 5 记为「框架就绪但无生产调用面，迁移递延到它接入 UI 编排时」。
- 任务书说 `layoutexportactions` 是三段式之一——**部分成立**，`exportLayoutAsync` 的
  prepare/commit 是内联在同一个函数里，没有独立的 job 结构体。
- `propertymodelworkflow` 的 prepare 面**没有独立符号**（`requestFromCatalog` 兼作输入快照
  抓取），与任务书描述的 `runCompute`/`commitComputed` 二段式一致。

### 语义差异清单（框架必须能同时容纳的五种实际行为）

这是本方向最关键的勘察产出——**5 处三段式的语义并不等价，框架不能一刀切**：

| 维度 | 组 1/2/3（workflows.cpp） | 组 4（属性建模） | 组 5（断层成面） | 组 6（版面导出） |
|------|--------------------------|------------------|----------------|---------------|
| **generation 防陈旧** | **有**。`job->generation = ++m_publishGeneration`（prepare 段写入），publish 段 **14 处** `job.generation != m_publishGeneration` 早退。计数器 `m_publishGeneration` 是 `ConstraintWorkflow` 私有 | **无**。用 `m_propModelRunning` 布尔做「忙则拒绝」——**另一种形态** | 无显式互斥（且无生产调用面） | 无显式互斥 |
| **取消传播** | `requestCancel` → compute 内 `cancelRequested()` 轮询点。**publish 段是临界区**：任务已终态，不再接受取消 | 同形态，注释明写「发布是临界区」 | `saveToStore` 同步执行，无取消点 | 无取消接线 |
| **进度回包** | compute 内**节流 50ms** + 手工 stage 词表映射（`prepare`/`geometry`/`encode`），再经 `PaleoTask::changed` 回 GUI 翻译成中文标签 | 无节流，每次 `reportStage` + `QueuedConnection` 直推面板 | 无 | 无 |
| **失败通道** | compute 返回 `QString` 错误串 → `PaleoTask::State::Failed` → finished 回包读 `errorText()` → 页面 statusLabel + `QgsMessageLog` | `PropertyModelComputed::error` → `showResult(false, why)` + statusBar | `FaultSurfaceProduceResult::ok/error` 直接返回 | 内联错误处理 |
| **失败清理** | **额外义务**：失败/取消时 `dropTemp(job->outputPath)` 删除 `paleo-sf-` 临时目录 | 无临时目录 | 无 | 无 |
| **同步兜底** | `!m_taskSvc` 时走同步直连（`generateFactor`/`generateInterpretiveContours`），不泵事件 | `!m_taskSvc` 时同步 `runCompute` + 直接 `finishPropertyModelRun` | 纯同步 | `m_taskSvc` 为空时回落同步导出 |

**结论**：现状的 generation 防陈旧**是存在的**——三个 Job 结构体都自带
`quint64 generation` 字段，`ConstraintWorkflow::m_publishGeneration` 单调自增，
prepare 段写入 `job->generation = ++m_publishGeneration`，publish/commit 段共
**14 处** `if (job.generation != m_publishGeneration)` 早退校验（`workflows.cpp`
1695/1899/1945/1992/2203/2280/2324/2415/2611/2681/2943/3135/3192/3255）。
但它是**每类各自一份**的私有计数器：属性建模（组 4）没有这一套，用
`m_propModelRunning` 布尔互斥。所以现状是**两种防陈旧形态并存**（generation 号
vs 忙则拒绝布尔），这才是真正要收敛的分歧——框架要把 generation 变成唯一形态，
`bool busy()` 退化为 generation 之外的准入检查。

### owner-thread 现状

- `PaleoTask::finished` 信号在 GUI 线程发出（服务内部 `applyFinish` 走
  `QueuedConnection`），所以组 1–4 的 commit 段事实上已经在 owner 线程。
- 组 5（`faultsurfaceworkflow::saveToStore`）与组 6 是**纯同步调用**，owner 线程靠调用点
  纪律保证，无机制。
- 框架需把这个「事实」变成 `QThread::currentThread() == ownerThread` 的显式断言。

### JobRunner API 签名（定案）

```cpp
// 层：功能
namespace paleo::jobs {

// 三段式契约。Job 为迁移者自定义的输入快照 + 中间产物聚合体。
template <class Job>
class JobRunner {
public:
  // owner 线程构造。ownerThread 默认 QThread::currentThread()。
  explicit JobRunner(QObject *owner, QThread *ownerThread = nullptr);

  struct Callbacks {
    // owner 线程：抓输入快照。返回 false → 走 PrepareFailed 通道，compute/commit 不执行。
    std::function<bool(Job &, QString *)> prepare;
    // worker 线程：纯计算。抛异常由框架捕获 → 失败通道。返回 false → 失败。
    std::function<bool(Job &, const CancelFn &, const ProgressFn &)> compute;
    // owner 线程：登记/发信号。取消或陈旧时**不执行**。
    std::function<bool(Job &, QString *)> commit;
    // 未进 commit 的通知（取消/陈旧/prepare 失败）。cleanup 已先执行。
    std::function<void(Job &, DropReason)> onDropped;
    // 临时产物清理：未成功发布时在 owner 线程执行一次（替代各处的 dropTemp）。
    std::function<void(Job &)> cleanup;
    // 进度节流毫秒，0 = 每次都报。默认 50（对齐现状三处实际节流值）。
    int progressThrottleMs = 50;
  };

  // 启动一代任务。忙则返回 nullptr。generation 自增，旧代 commit 被丢弃。
  PaleoTask *start(const QString &title, std::shared_ptr<Job> job, Callbacks cb,
                   const QString &layerId = QString(), bool quiet = false);

  // 取消当前代（若有）。已在 commit 段执行的任务不受影响。
  void requestCancel();

  // 认领一代（prepare 成功后由框架自调；暴露供代际断言与将来 supersede 用）。
  Claim claim();

  quint64 currentGeneration() const;
  bool busy() const;
  PaleoTask *currentTask() const;
};

} // namespace paleo::jobs
```

关键设计点：

1. **commit 强制 owner 线程**：`start` 捕获 owner thread 指针，commit 回调经
   `QMetaObject::invokeMethod(owner, ..., Qt::QueuedConnection)` 调度，并在入口断言
   `QThread::currentThread() == m_ownerThread`。`#80` 守卫从纪律变成机制。
2. **取消后 commit 不执行**：框架在调度 commit 前查 `task->cancelRequested()`，为真则
   走 dropped 分支（触发迁移者注册的清理钩子，替代各处的 `dropTemp`）。
3. **失败通道如实**：`compute` 抛异常 → 捕获 `what()` → 与返回 `false` 等价，
   `commit` 收到失败态（如 `Job::ok=false` + error 串）后由迁移者如实上 UI。
4. **节流内建**：`ProgressFn` 内部做 50ms 节流（对齐现状三处实际节流值），迁移者只
   报语义化 stage 名。`reportStage` 走 `PaleoTask` 既有通道，不改公开契约。

### `workflows.cpp` 拆分边界

现状 4086 行，四个类混装。**按类边界拆**（不是按行数切）：

| 目标文件 | 内容 | 预估行数 |
|---------|------|---------|
| `predictionworkflow.cpp` | `PredictionWorkflow`（含 `runPrediction`/ONNX 伴生判定） | ~350 |
| `constraintworkflow.cpp` | `ConstraintWorkflow` 全部（含三组三段式） | ~2950 |
| `constraintcontour.cpp` | 三组三段式 + 制图工作场（从 ConstraintWorkflow 析出，同类实现） | 待实测 |
| `compositionworkflow.cpp` | `CompositionWorkflow`（融合/转面/属性写回/编辑准备） | ~450 |
| `validationworkflow.cpp` | `ValidationWorkflow` | ~200 |

`ConstraintWorkflow` 本身仍可能超 1500 行阈值，需在轮 4 实测后再定二次边界
（候选：约束 CRUD 与因子生成 vs 三段式作业面）。**头文件契约不动**。

### `datapreviewtabs.cpp` 拆分边界

现状 5249 行。照 `paleomainwindow_*` 前缀先例，按资产类型拆构建器，
`openAsset` 调度表集中到单一文件：

| 目标文件 | 内容 |
|---------|------|
| `datapreviewtabs.cpp` | tab 容器 + **`openAsset` 调度表**（集中）+ 公共装配 |
| `datapreviewbuilder_las.cpp` | LAS 预览构建器 |
| `datapreviewbuilder_segy.cpp` | SEG-Y 预览构建器 |
| `datapreviewbuilder_well.cpp` | 井/轨迹预览构建器 |
| `datapreviewbuilder_map.cpp` | 图版/制图预览构建器 |
| `datapreviewbuilder_other.cpp` | 其余资产类型（网格/点云/表格等兜底） |

具体类型清单待轮 4 实测 `openAsset` 现有分支后定案。

### 探测面扩展清单

`tst_ui_blocking.cpp` 现状只盖 2 条探针（LAS 预览解析、validate）。待扩展：

| # | 长操作 | 位置 | 预期 |
|---|--------|------|------|
| 1 | 实体批量删除 | 待定位 | 探测先于修复 |
| 2 | 元数据打开 | 待定位 | 探测先于修复 |
| 3 | 连接诊断 | 待定位 | 探测先于修复 |
| 4 | 综合图重绘 | 待定位 | 探测先于修复 |

**纪律**：探针不加宽松阈值。红的如实进 `TODOS.md` 标注「已知阻塞：xxx」。

### 禁区确认

- `single-factor` 方向已收官：PR #109 已合并（`38e4ed2` 等提交在 master 上），
  **无在飞冲突面**。`workflows.cpp` 相关段可自由重构。
- `goal/catalog-sqlite-2026-10-02`、`goal/well-logset-20261002` 两个 worktree 仍在飞，
  分别碰 `src/catalog` 与 `src/services/welllogset`——本方向**不碰这两个面**。
- 不改 `PaleoTaskService` 公开契约（只加适配层，不加方法到其类内）。
- 不引入新线程模型（沿用 `QThreadPool`）。

## Oracle 账本

| # | Oracle | 状态 | 证据 |
|---|--------|------|------|
| 1 | 行为保留断言（信号序/取消/失败态） | **通过**（2 处已迁面共 6 条） | 轮 2：`tst_propworkflow` 3 条；轮 3：`tst_factorworkflow` 3 条；两处既有测试均原样绿 |
| 2 | `tst_jobrunner` 5 条框架断言 | **通过** | 11/11 PASS，见「轮 1 实测」 |
| 3 | 拆分后既有测试原样绿 | **通过** | `workflows.cpp` 拆 4 刀后 `tst_workflows`/`tst_factorworkflow`/`tst_composeworkflow`/`tst_propworkflow` 全绿；`datapreviewtabs.cpp` 拆 5 刀后 `tst_previewdoc`/`tst_previewmap_tools`/`tst_previewmap_identify` 全绿，且 `tst_ui_blocking` 每刀后**逐用例完全一致** |
| 4 | `datapreviewtabs` 拆后预览测试全绿 + 分发覆盖 | **通过** | 5 刀已切 + 共享内部头；`tst_previewdoc` 46.30s / `tst_previewmap_tools` 4.64s / `tst_previewmap_identify` 4.80s 全绿。**分发覆盖断言本就存在**：`tst_datapreview::everyTypeOpensContent()` 逐一 openAsset 9 种类型并断言各自特征控件（well_log 的 curveCombo / tops 的 wellCombo+topsTable / well_head 的井下拉 / horizon 的 showOnMapBtn / seismic 的测线选择 / image / document 内嵌 / geojson 统计 / 未知类型兜底） |
| 5 | 探测面每条绿或红进 TODOS | **历史已记档；非全绿** | 3 条新探针：🔴 拓扑重建（如实红，已记 TODOS）· 🟢 批量软删（绿，且推翻了我的 O(N²) 假设）· 🔴 元数据打开（夹具未对齐，2026-10-03 确认为 nodeCount 断言失败）；「连接诊断」经全库勘察确认**不存在真实入口**（已记） |
| 6 | 文件规模断言（单文件 ≤1500 行） | **部分达成（历史快照）** | `workflows.cpp` 4295→153 ✓；`datapreviewtabs.cpp` 5249→1595，仍大于 1500。约束 CRUD / 作业面已二次拆分，但 `constraintfactorjobs.cpp` 与预览内部头仍超过此目标，不能记为全部达标。 |
| 7 | 全账 + 本文档收口 | 进行中 | 轮 0–5 已记账 |

## 轮 1：JobRunner 框架 + tst_jobrunner

### 落点与形态

框架落 `src/services/jobrunner.{h,cpp}`（**层：数据**），不落 `src/workflow`：
它只依赖 Qt + 同层的 `PaleoTaskService`，而 `paleo_workflow` 与 `paleo_ui` 都已
link `paleo_services`，落 services 层两个消费层都能直接用，测试侧也能用
`LIBS paleo_services paleo_qgis` 保持瘦链接面。

- `JobRunnerBase`（非模板）承载：owner 线程调度、generation 计数、取消标志、
  任务句柄。`scheduleOnOwner` 用 `QMetaObject::invokeMethod(..., QueuedConnection)`
  把 commit 段排队回 owner 线程；`assertOwnerThread` 在 commit 入口断言
  `QThread::currentThread() == m_ownerThread`（`#80` 从纪律变机制）。
- `JobRunner<JobT>`（模板）承载三段式契约。`start()` 签名：
  `PaleoTask *start(const QString &title, std::shared_ptr<Job> job, Callbacks cb, layerId, quiet)`。

### 两个实测得来的关键设计（都写进了注释）

1. **Job 必须以 `shared_ptr` 共享所有权过三段**。第一版按值传给 worker、再按值
   捕获给 commit，结果 compute 段写进 Job 的产物 commit 段**根本看不到**——
   两侧是互不可见的两份副本。改为 `shared_ptr<Job>` 后 commit 读到的就是
   compute 写的那份（`tst_jobrunner::computeFailureReachesCommitAsFailedState`
   就是钉这一条的：commit 断言读到 `job.error`）。
2. **跨线程状态放 `Shared`，worker 不捕获 `this`**。`Claim` 持
   `shared_ptr<Shared>`（与 JobRunner 内部**同一个对象**，不是副本）与本代专属的
   `cancel` 标志。第一版给 Claim 传了 generation 的副本，导致 `current()`
   在 worker 侧永远为真、陈旧检测形同虚设。worker 因此可以在 JobRunner 析构后
   继续安全收尾。

### 陈旧语义的可达性（修正了任务书的隐含假设）

`start()` 有 `busy()` 门控（同一 runner 同时只跑一代，对齐现状「忙则拒绝」），
因此**「第二代接管 → 第一代 commit 被丢弃」无法经 `start()` 制造**。陈旧
（`DropReason::Stale`）是 Claim 级的安全网，钉的是判据本身
（`staleGenerationDropsCommit` 直接对 Claim 断言代际），而不是构造一个假并发。
若将来要支持 supersede（新一代抢占而非拒绝），Stale 路径才真正被激活——届时
需要补一条经 `start()` 的端到端断言。

### 本机命令

```text
source build/jobframe-env.sh          # 本地构建环境（见下「环境注记」）
jobframe_configure                    # 首次配置
jobframe_build tst_jobrunner
ctest --test-dir build -R '^tst_jobrunner$' --output-on-failure
python tools/check_layering.py --strict
python tools/check_i18n.py
python tools/check_ui_invariants.py --strict
```

`build/jobframe-env.sh` 是本 worktree 的构建环境脚本，放在已被 gitignore 的
`build/` 下，**属本机操作便利，不在交付面内**：bash 侧没有 `vcvars64`
（`cmd.exe` 被沙箱阻断），故手工拼 MSVC 14.38 + Windows SDK 10.0.22621 +
Qt 6.8.0 + QGIS prefix 的 `PATH`/`INCLUDE`/`LIB`/`CMAKE_PREFIX_PATH`。

### 环境注记（三条实测结论，踩坑成本高）

1. **运行时 Qt 必须用 deps 那套**。`paleo-qgis-deps/Library/bin` 里带**自己构建
   的** `Qt6Core.dll`（5.7MB），与 `C:/deps/Qt/6.8.0` 的（6.0MB）**不是同一个
   构建**；QGIS/GDAL 依赖闭包是按前者链接的。Qt 6.8.0 排在 PATH 前面时，所有
   QGIS 链接的 exe 直接 `0xc0000139`（入口点找不到）。`tst_domain`（不链 QGIS）
   仍绿，所以「按链接面二分」是定位这类问题的最快手段。
2. **无控制台下 QTest 结果看不见**。ctest 拉起的 exe 把 QTest 结果行走
   `OutputDebugString`，而 `__fastfail` 会连带丢掉 stdout 缓冲 →
   `ctest --output-on-failure` 什么都没有。`main()` 里追加 `-o <file>,txt`
   让结果落盘，才看得见崩在哪个用例（本测试保留了这个手法）。
3. **PDB 并发写冲突**。`-j8` 下偶发 `C1090 PDB API 调用失败 错误代码 23`，
   编译单元实际没编进去，链接出的是**旧 exe**，症状极具误导性。踩到时用 `-j1`。

### 轮 1 实测输出

```text
$ ctest --test-dir build -R '^tst_jobrunner$' --output-on-failure
1/1 Test #14: tst_jobrunner ....................   Passed    3.75 sec
100% tests passed, 0 tests failed out of 1

$ cat build/tst_jobrunner-result.txt
PASS   : TestJobRunner::initTestCase()
PASS   : TestJobRunner::computeOffOwnerCommitOnOwner()
PASS   : TestJobRunner::staleGenerationDropsCommit()
PASS   : TestJobRunner::cancelInterruptsComputeAndSkipsCommit()
PASS   : TestJobRunner::cancelReleasesBusyForNextStart()
PASS   : TestJobRunner::exceptionCrossesWorkerToCommit()
PASS   : TestJobRunner::computeFailureReachesCommitAsFailedState()
PASS   : TestJobRunner::prepareFailureBuildsNoTask()
PASS   : TestJobRunner::rejectsStartWhenBusy()
PASS   : TestJobRunner::withoutTaskServiceDegrades()
PASS   : TestJobRunner::cleanupTestCase()
Totals: 11 passed, 0 failed, 0 skipped, 0 blacklisted, 253ms

$ python tools/check_layering.py --strict
检查通过：无层违规。
$ python tools/check_i18n.py
i18n 检查通过：文案入口无裸字面量。
$ python tools/check_ui_invariants.py --strict
ui invariants: clean (violations=0, baseline=0)
```

编译零新警告（`/W4`；`assertOwnerThread` 的 `where` 形参在 NDEBUG 下会变未引用，
已用 `Q_UNUSED` 显式吞掉，避免与文件其余部分不一致的新警告）。

## 轮 2：迁移 propertymodelworkflow（第一处迁移）

### 迁移面与等价点

`PropertyModelWorkflow` 新增 `startJob()`，把原来在 `paleomainwindow_attach.cpp`
里手写的「起任务 → 判忙 → 连 finished → commit」编排换成框架调用。
`runCompute`/`commitComputed`/`run` 三个既有符号**一行未动**——它们是同步契约，
老调用点与既有测试照旧（`run()` 内部仍是两段直连）。

| 维度 | 迁移前（attach.cpp 手写） | 迁移后（框架承担） | 等价性证据 |
|------|------------------------|------------------|-----------|
| 忙则拒绝 | `m_propModelRunning` 布尔 | 框架 `busy()` 门控（`start` 返回 nullptr） | `asyncJobRejectsWhenBusy…` (a) |
| 取消传播 | `m_propModelCancel` 布尔 + `task->requestCancel()` | 框架 `requestCancel()`（置本代 cancel + 任务取消位） | `asyncJobCancelSkipsCommit` |
| commit 线程 | finished 回调恰在 GUI 线程（事实） | 框架排队回 owner + `assertOwnerThread` 断言（机制） | `asyncJobSucceedsAndRegisters…` |
| 失败态 | `computed.error` → `showResult(false, why)` | 同（失败仍进 commit，迁移点读 `job.error` 上 UI） | `asyncJobRejectsWhenBusy…` (b) |
| 进度回包 | 手工 `reportStage` + `invokeMethod` 推面板 | 框架 50ms 节流 + `ProgressFn` → 面板 `updateProgress` | 面板侧槽位不变 |
| 无池兜底 | `!m_taskSvc` 同步直连 | 同（`startJob` 返回 nullptr → 走老同步路径） | 由 (a) 同形覆盖 |

### 三处需要判断的地方（都不是「顺手改语义」）

1. **二次登记必须消掉**。迁移前 `finishPropertyModelRun` 自己调
   `commitComputed`；迁移后 commit 段已登记，若 UI 段再调一次就是对同一份
   staging 登记两次。判据用 `job->registered`（commit 段成功时置位），
   **不用 `runner.busy()`**——commit 段在发 `finished` 之前就 `clearTask()` 了，
   拿 busy 判会误判成「没登记过」。无池兜底路径不建 job，仍走直连登记。
2. **`m_propModelJob` 每代必须 reset**，否则 UI 段会读到上一代的登记结果。
3. **失败信号跨线程的现状照旧**。`runCompute` 的失败路径里有
   `emit modelFailed(why)`，迁移后它仍从 worker 线程发出。生产代码无连接点
   （只有 `tst_propworkflow` 用 QSignalSpy 听 `modelStored`），但这是**既有行为**，
   重构不改——记录在此，不在本 PR 修正。

### 实测输出

```text
$ ctest --test-dir build -R '^tst_propworkflow$'
1/1 Test #158: tst_propworkflow .................   Passed   37.45 sec

$ grep asyncJob build/tst_propworkflow-result.txt
PASS   : TestPropWorkflow::asyncJobSucceedsAndRegistersOnOwnerThread()
PASS   : TestPropWorkflow::asyncJobCancelSkipsCommit()
PASS   : TestPropWorkflow::asyncJobRejectsWhenBusyAndReportsFailure()
```

同一次运行里 11 个既有用例（含 `chainRegistersDerivedAndIsReproducible`、
`cancelAndProgressAreHonest` 等同步契约）**原样绿、断言未改**——
这是 Oracle 1「行为保留」与 Oracle 3「不丢面」在本迁移点上的证据。

顺带记一笔测试手法：`tst_propworkflow` 原先也没有 `-o` 落盘，失败时看不到
是哪条红。已同样加上（ctest 无控制台下 QtTest 结果走 `OutputDebugString`）。

### 已知阻塞：paleo_ui 全量链接缺 QScintilla 头（环境问题，非本方向引入）

`paleo_ui` 目标在**我没碰过的** TU 上编译失败：

```text
qgsrasterattributetable.h(289): warning C4996 ...
include/qgis/qgscodeeditor.h(30): fatal error C1083:
  无法打开包括文件: “Qsci/qsciapis.h”: No such file or directory
FAILED: CMakeFiles/paleo_ui.dir/src/ui/layers/layerpropertiesdialog.cpp.obj
```

诊断（`layerpropertiesdialog.cpp` 与本方向无关，`src/` 内无人引用
`QgsCodeEditor` / `Qsci`）：

- `qgscodeeditor.h` 于 2026-10-02 00:18 被更新，新引入 `#include <Qsci/qsciapis.h>`；
- QScintilla 头只存在于 `C:/deps/qscintilla-install/include`，而 `CMakeLists.txt`
  与 `cmake/*.cmake` **完全没有 QScintilla 接线**——`CMAKE_PREFIX_PATH` 里有
  `qscintilla-install`，但那不会变成编译期 `-I`；
- 既有 worktree（`fault-surface`）能编出 `paleo_ui.lib`，靠的是它没重编这个 TU
  的陈旧 obj，不代表配置正确。

**处置（已解决）**：修在**独立分支** `fix/qgis-qscintilla-include`（提交
`d43123f`，37 行纯新增），**刻意不夹带进方向 20**——它属于环境修复而非本方向的
重构。做法与 QGIS 既有探测同风格：`QSCINTILLA_PREFIX` 走 cache/env 解析，未给出
时从既有 `CMAKE_PREFIX_PATH` 逐个前缀自动发现（不给调用方增加传参负担），再挂到
`paleo_qgis_core_deps`（传播 QGIS 头的同一目标）。只需头不需链库——已核实 QGIS 头
里 Qsci 只以继承基类与 `INDIC_MAX` 编译期常量的形式出现（0 处 `Qsci::` 调用）。

阻塞解除的实测证据（移植到 jobframe 的已配置 build 上增量验证）：

```text
$ cmake -S . -B build
-- QScintilla headers: C:/deps/qscintilla-install/include
-- Configuring done (3.5s)

$ cmake --build build -j1 --target paleo_ui
[272/273] Linking CXX static library paleo_ui.lib     # 33m01s

$ ls -la --time-style=+%H:%M build/CMakeFiles/paleo_ui.dir/src/ui/layers/layerpropertiesdialog.cpp.obj
1275421 12:18 .../layerpropertiesdialog.cpp.obj      # 正是此前失败的那个 TU
$ ls -la build/paleo_ui.lib
219982748 12:22 build/paleo_ui.lib
```

故 `paleo_ui` 整库链接已验证，方向 20 第一次拿到该层完整的编译证据。

顺带记一个 configure 坑：新 worktree 的**首次** configure 会漏掉 4 条 `vendor/`
include（glm/saribbon/sbm/segyio），症状同样是后续 TU 报 QGIS 头找不到。**再
configure 一次即恢复**。已在 skill `paleo-workstation-windows-build-env` 记录。



## 轮 3：三组单因素作业的统一异步面

### 形态

`ConstraintWorkflow` 上新增统一面，9 个 `prepare*`/`compute*`/`publish*`
**一行未改**（它们是既有契约，既有测试直接钉它们）：

```cpp
using ConstraintJobVariant = std::variant<LocalDirectionJob, AnalysisContourJob,
                                          InterpretiveContourJob>;
enum class ConstraintJobKind { LocalDirection, AnalysisContour, InterpretiveContour };
struct ConstraintJob { ConstraintJobKind kind; ConstraintJobVariant payload;
                       QString title; std::function<QString(double)> stageOf; };

bool prepareConstraintJob(ConstraintJob &, const QVariantMap &params, QString * = nullptr);
bool computeConstraintJob(ConstraintJob &, const std::function<bool()> &, const std::function<void(double)> & = {});
bool publishConstraintJob(const ConstraintJob &, QString * = nullptr);
static QStringList constraintJobTempPaths(const ConstraintJob &);
PaleoTask *startConstraintJob(paleo::jobs::JobRunner<ConstraintJob> &, ConstraintJob, QObject * = nullptr);
```

用 `variant` 是因为三组 job 的中间产物路径各不相同（本方向最需要收敛的
「同一协议写三遍」那部分正是这些路径的分派）。`kind` 即分派索引。

### 收敛掉的重复

| 重复项 | 现状 | 收敛后 |
|--------|------|--------|
| 忙则互斥 | 三处各判 `m_factorTask && running()` | 框架 `busy()` |
| 取消接线 | 三处各连 `runCancelRequested` → `requestCancel` | 框架 `requestCancel()` |
| 进度节流 | 三处各写一遍 50ms 节流 | 框架统一（`stageOf` 只留阶段词表映射） |
| 阶段词表 | 局部方向自映射 prepare/geometry/encode，另两处各一份 | `stageOf` 声明式，阈值原样保留 |
| 临时产物清理 | **三处逐字复制的 `dropTemp` lambda** | 一条共用判据（仍只清 `paleo-sf-` 前缀） |
| publish 临界区注释 | 三处各写一遍「发布是临界区」 | 框架一处（commit 段不接受取消） |

### 两个设计错误（实测打出来的）

1. **这 9 个 prepare/compute/publish 是非静态成员函数**（要访问 catalog /
   layers / projectStore），我第一版把统一面的三个辅助函数声明成了 `static`，
   编译报 C2352。改为非静态成员，并在 compute/commit 的 lambda 里捕获 `this`。
2. `QVariant` 没有 `toVector()`——levels 需从 `QVariantList` 逐元素 `toDouble()`
   转换。

### 实测输出

```text
$ ctest --test-dir build -R '^tst_(workflows|factorworkflow)$'
1/2 Test #30: tst_workflows ....................   Passed  121.95 sec
2/2 Test #35: tst_factorworkflow ...............   Passed  194.69 sec
100% tests passed, 0 tests failed out of 2
```

新加 3 条统一面断言（`unifiedJobRunsAnalysisContourThroughRunner` /
`unifiedJobRejectsWhenBusy` / `unifiedJobCancelSkipsPublish`），加断言后
`Totals: 21 passed, 0 failed, 1 skipped`。第一条与既有
`contourDeclaresVectorLayer` **同口径**比对（声明存在、类型 vector、组
`04_SingleFactor/Contours`、成果信号三段参数）——这是「统一面与裸接口行为
等价」的直接证据。

三项门禁全绿。`/W4` 零新警告。

### 修正对任务书的第三处基线偏差：拆分边界与编译成本

- `ConstraintWorkflow` 单独约 **2860 行**，超出任务书定的 1500 行阈值 →
  轮 4 需二次拆分（约束 CRUD 与因子生成 / 三段式作业面）。已记入拆分边界表。
- **`workflows.cpp` 单 TU 编译需 12 分钟以上**（4000 行 + 大量 QGIS 头），
  `-j1` 如此。拆分恰好解决这个——两者互为因果，轮 4 的收益比原计划大。

### 拆分边界（轮 4 实测，替代轮 0 的估计）

`workflows.cpp` 4086 行的实测分布：

| 区段 | 行区间 | 行数 |
|------|--------|------|
| 头部（include + 词表 + 辅助） | 1–437 | ~437 |
| `PredictionWorkflow` | 438–755 | ~320 |
| `ConstraintWorkflow` | 756–3620 | ~2860 |
| `CompositionWorkflow` | 3621–4070 | ~450 |
| `ValidationWorkflow` | 4071–4086 | ~200 |

`datapreviewtabs.cpp` 5249 行的实测分布（**与轮 0 估计差异很大**）：

| 区段 | 行区间 | 行数 |
|------|--------|------|
| tab 容器 + 公共装配 | 1–2300 | ~2300 |
| **`buildContent`（一个函数）** | 2301–4867 | **~2567** |
| 井体构建 + 回包槽 | 4868–5249 | ~380 |

真正的巨兽是 `buildContent` **这一个函数**（占全文件 49%），比整个
`workflows.cpp` 的三分之二还大。它内部有天然切分线（按 `asset.type`）：
document / well_log / well_head / horizon / seismic / image_reference /
geojson / 兜底参考。轮 4 按这些切点拆构建器文件，`openAsset` 调度表集中到
`datapreviewtabs.cpp` 顶部。


## 轮 4 补充：本机 UI 测试的既有 DLL 阻塞（非本方向引入）

`tst_datapreview` / `tst_previewmap_page` 在本 worktree 报 `0xc0000139`
（运行时 DLL 入口点缺失）。已用**未被我碰过**的 `.worktrees/fault-surface`
做对照：同一个 `tst_datapreview` 同样 `0xc0000139` ⇒ 与方向 20 的拆分无关。

根因是本机 Qt 双份（`paleo-qgis-deps/Library/bin` 自建的一套 vs
`C:/deps/Qt/6.8.0`），QGIS GUI 侧链的是前者；跑测试时 PATH 只能满足一套，
于是涉及 `qgis_gui.dll` 的测试链起不来。`tst_previewdoc` / `tst_previewmap_tools` /
`tst_previewmap_identify` / `tst_taskpanel` 不依赖那套，能跑且全绿。

**待办**（不夹带进方向 20，另行立项）：给测试运行准备一套一致的 DLL 解析环境
（参照 paleo-dev.ps1 的 PATH 排序，或用 Qt6::Core 前置的部署脚本）。

**因此 Oracle 4 的直接执行证据受限**：`everyTypeOpensContent` 本身没跑过，
但它是**既有测试**、本方向未改动其断言，且 `tst_datapreview` 在 fault-surface
上同样跑不起来。拆分后能跑通的四个预览侧测试全绿。


## 2026-10-03：master 基线与全库去重/文档/测试对账

基线为 `3b22a9c`，工作目录 `/home/kevin/projects/paleo_workstation-refactor`，
分支 `refactor/dedup-docs-tests`。主仓当时已有两处未提交 UI 探针修复，
本 worktree 从提交创建，未带入这些修改。共享 gitignored prefix/ONNX 依赖，
独立 RelWithDebInfo/Ninja 构建目录；所有构建/ctest 并行度 ≤8。
以下是本轮审查现状，上文 Windows/DLL 受限及单体行号均保留为历史证据。

### 去重结果与保留边界

| 主题 | 当前落点 | 真实消费者与保持的契约 |
|---|---|---|
| 等值线发布 | `src/workflow/constraintfactorjobs.cpp` 私有 `analysisShaMatches` / `contourMetadata` / `cartographicHashParameters` / `inheritMockFlag` | SHA 比较 4 处、等值线元数据 2 处、制图 QC 指纹 2 处、mock 继承 6 处；generation、declared-path、报错、清理顺序与 fixedLevels 门保持 |
| 失败文案赋值 | `src/workflow/workflowerrors_internal.h::setError` | workflows_internal 的拆分消费者及 depthconversion/propertymodel/faultsurface 共用；只引 QString，避免三套独立 workflow 为一个赋值 helper 引入 GDAL/ONNX/QGIS 头闭包 |
| 约束组 | `src/qgis/layervocabulary.h` 的 `kConstraintsGroup` | CRUD 四产点、workbench、树排序/档案及普通夹具引用 03_Constraints 常量；词表冻结值断言保持独立字面量，旧 02_Constraints 输入/兼容断言保留 |
| 栅格预览 | `datapreviewtabs_internal.h::addRasterPyramidHint` | horizon/image 两处；原提示条时机、任务信号、QPointer 与 host 生命周期保持 |
| 同目录叠加 | `DataPreviewTabs::addSiblingOverlayButton` | geojson/horizon 两处；菜单项顺序、objectName、tr 上下文、连接归属保持 |
| SEG-Y | `vendor/sbm/src/Data/Sgy/SgyRegularFile.h::IsRegularFile` | IndexCache 两处、SequentialScan 四处；普通文件非抛异常守卫统一，各路径打开/检查顺序及错误原因仍由调用方保留；IndexService 不直接读文件，非重复点 |
| 欧氏井距 | `src/algorithms/welldistance_internal.h` | welldist 与 distance_transform 无屏障分支，共用点收集与逐行计算；坐标/浮点运算顺序、取消/失败关 GDAL、进度与成功关闭时机保持 |
| workflow 夹具 | `tests/helpers/workflowfixture.h` | workflows/factorworkflow/asynccontour/faults/geostat 五套同序真实栈；前两套共用三点 GeoJSON 与 DERIVED 断言助手 |
| preview 夹具 | `tests/helpers/previewfixture.h` | datapreview/previewmap_assets/previewmap_perf 三套同序真实栈与 staging；前两套共用 importAll；删除仅作构造临时量的 metaPath 成员 |
| 测试链接 | CMake `tst_jobrunner` / `tst_propmodelperf` | jobrunner 用 services + ui_deps（QgsApplication 需要 QtWidgets），移除 paleo_qgis 静态库依赖；propmodelperf 的 io 已由 workflow 传递，不重复列；测试标签、沙箱、注册不变 |

既有跨 TU helper 已集中在 `workflows_internal.h` 与约束专用
`constraintworkflow_internal.h`；setError 经前者包含的轻量
`workflowerrors_internal.h` 单点定义。inversionworkflow 的 bool setError 还承担
返回 false 的短路语义，保留其不同签名。wellsLayerIdFor 只有一个定义，hashParams 是局部值。
未找到点名五件内 sf:: 内外双套同功能实现。host+QVBoxLayout、loadingLabel、
stateLabel 等已由集中构建入口/内部头复用，未再造抽象。清掉 preview 内部头与
geostat 测试的重复 include；本轮扫描未发现 `src/` 永久 `#if 0` 块。

SHA 字节读取本已有 catalog 的 `sha256FileHex` 单点实现。
PreviewDocService 经 `verifyExternalVersionSha` 复用它，并额外处理 managed/空 SHA、
会话缓存、大小写兼容与下游 stale；分析场守卫比较计算前后的快照 SHA（大小写精确，
代次/路径保护）；structural sidecar 读取由 `qgis/factorcontour.cpp::generateStructuralContours`
校验 JSON、FieldContourSurface 与栅格尺寸；发布时虽记录 structural_sha256，
读取路径尚未复验此 SHA（递延项见 TODOS.md），不能把结构校验误报成完整性复验。
这些拒绝/放行策略不同，未把它们塞进一个带策略开关的通用校验器。

constraintstore 的锁/元数据写夹具、previewdoc 的 store 路径/销毁次序与上述栈
不相同，保留；previewmap 工具/画布测试的 CRS、要素数量和所有权也不强合并。
没有合并测试文件、删除用例或调整任何预算/容差。改动测试的 void 函数清单及
QtTest 断言数与 master 对照一致；测试注册仍为 243 项。

### 拆分文件覆盖核对（调用面审查，不代表分支覆盖率）

| 被测文件 | 现有证据 |
|---|---|
| validationworkflow.cpp | tst_workflows 的缺源/重复层位/busy 验证，tst_mapping 的域/残差链 |
| compositionworkflow.cpp | tst_workflows / tst_composeworkflow 的融合、分相、声明与拒绝路径 |
| predictionworkflow.cpp | tst_workflows / tst_onnxworkflow / tst_mapping 的处理/ONNX/声明链 |
| constraintworkflow.cpp | tst_constraintstore / tst_workflows 的约束 CRUD、持久化与 IDW |
| constraintfactorjobs.cpp | tst_factorworkflow / tst_singlefactor_asynccontour / tst_singlefactor_faults / tst_geostat_workflow 的三段发布、陈旧/取消/被改写守卫与成果 |
| datapreviewtabgeojson.cpp | tst_datapreview::everyTypeOpensContent 与 previewmap_assets 的图例/标注/TOC |
| datapreviewtabhorizon.cpp | 同上 + 版本切换、等值线、统计、极值、剖面与缓存 |
| datapreviewtabimage.cpp | previewmap_assets 的有/无 world file 两路，datapreview 类型覆盖 |
| datapreviewtabseismic.cpp | datapreview 的解码任务服务、井震/失败原因与测线选择 |
| datapreviewtabwelllog.cpp | datapreview 的曲线/单位/缩放/多井，UI blocking 的异步读 LAS 探针 |
| jobrunner.h/.cpp | tst_jobrunner 的 owner/worker 线程、陈旧、取消、异常、失败清理、prepare 拒绝、busy 与同步兜底 |

点名 11 组均有调用面证据（JobRunner 头/实现合计一组），无整组未测者。仍建议另补轻量回归：两个资产页的
同目录菜单实际触发叠加（不只测 sibling 枚举）、金字塔完成后 host 已销毁/其他
asset 信号隔离，以及 structural sidecar 被改写后的拒绝与清理组合。现有小夹具
不触发 >50MB 金字塔门，不能把类型覆盖等同于此分支已测。

规模限制也如实保留：constraintfactorjobs.cpp 3580 行、datapreviewtabs.cpp 1627 行、
datapreviewtabs_internal.h 1553 行仍超过方向20 的 1500 行目标；本 PR 不放宽预算、
不为凑行数再次合并/分裂文件。FaultSurface 与 layout 的 JobRunner 迁移、调用级
分层审计及首次 configure include 根因仍递延。

### 实际验证与失败归属

所有构建均为 `cmake --build build -j8`，基线全量及最终代码重编均零 error。
以下计数是 CTest 套件数，不是 QtTest 函数数；QSKIP 真工区用例没有被当成已执行。

| 执行 | 实际结果 | 本 worktree 的日志 |
|---|---|---|
| 动手前 master `3b22a9c` 全量 `ctest --test-dir build -j8 --output-on-failure` | 238/243，通过项保持；5 红，660.00s | `build/refactor-baseline-ctest.log` |
| 等值线 helper，6 组相关测试 + layering/strict | 8/8 | `build/refactor-workflow-ctest.log` |
| 约束组/预览/SEG-Y + 8 门禁 | 22/22 | `build/refactor-groups-preview-segy-ctest.log` |
| 欧氏井距/共享夹具 + 8 门禁 | 17/17 | `build/refactor-fixtures-algorithms-ctest.log` |
| JobRunner/属性建模性能链接集 | 2/2 | `build/refactor-link-ctest.log` |
| 追加错误赋值收口，8 组相关测试 + layering 三项 | 11/11 | `build/refactor-errors-ctest.log` |
| 首次重构全量 `ctest --test-dir build -j8 --output-on-failure` | 238/243；启动比率探针本轮红，catalog 本轮绿，744.68s | `build/refactor-final-ctest.log` |
| 启动探针隔离 `ctest --test-dir build -j1 -R '^tst_startup_trace$' --output-on-failure` | 1/1，16.24s；没有改门限/断言 | `build/refactor-startup-recheck.log` |
| 最终代码再次全量，同一 `-j8` 命令 | 239/243；4 个基线红，525.14s，无新增失败 | `build/refactor-final2-ctest.log` |
| layering ×3、ui_invariants ×3、i18n ×2 | 8/8；最终全量也全过 | `build/refactor-final-gates.log` |

基线失败的五个可执行文件在改源前保存在 `build/refactor-master-tests/`，
BASE_COMMIT 记录完整 `3b22a9c` SHA；隔离 CTest 沿用原环境/沙箱/超时。
这比使用主仓后续被其他会话修过的二进制更能证明原基线归属。

| 红项/波动 | 原基线与隔离复核 | 最终代码结果与处理 |
|---|---|---|
| `tst_ui_blocking` | topology 的 laps ≥2 与 metadata 的 nodeCount >0 两断言失败；保留 master 二进制复跑同样 7 过/2 败 | 两条仍红（7 过/2 败）；原探针/预算一字未改。后续 master 的 `41feecf` 另行修复，不属于本 PR |
| `tst_singlefactor_perf` | 原 L 中位 21671ms；保留 master 二进制隔离复跑 20089ms，均超 15000ms | 最终 L 18980ms，同一道预算失败；不改门限，不由本轮引入 |
| `tst_perf_catalog` | SHA 查询重开后实际 ver-2/期望 ver-1；保留 master 二进制隔离复跑同一断言红 | 两次重构全量均绿；这是结果/顺序差异，不能只叫性能噪声。后续 master 的 `e8da8cf` 已修行序，本轮只记录历史对照 |
| `tst_cache_las` | 原 cold 3.929ms/disk 3.014ms；保留 master 隔离 cold 3.865ms/disk 2.470ms，均不满足 disk <0.5×cold | 最终 cold 2.859ms/disk 1.806ms，同一比率断言红；属于任务书列举之外的已证实基线红，发布例外需确认 |
| `tst_wellcomposite_visual` | 原与保留 master 隔离均 17/48 点超容差（允许 4，色差门 22）；vendored 字体已注册、Fusion 已钉，具体渲染根因未定 | 最终仍同样 17/48；不重生成 golden、不调容差，额外基线红的发布例外需确认 |
| `tst_startup_trace` | 动手前全量通过 | 首次重构全量两条比率/判别力断言失败；隔离复跑及最终全量均过。与共享机器负载相符，没有据此改实现或预算 |

隔离四项复核日志为 `build/refactor-master-recheck.log`；L 性能单独隔离日志为
`build/refactor-master-perf-recheck.log`。UI 探针使用 QtTest 的文件输出，另存
`build/refactor-master-ui-blocking-result.txt` 与 `build/refactor-final2-ui-blocking-result.txt`。
两次全量耗时受共享机器负载影响，不用它们宣称重构带来性能提升。

`git diff --check` 每次提交前均过。13 个改动测试文件的 void 函数清单、QtTest
断言数量对照原基线一致；注册仍为 243 项，标签、环境、沙箱与超时属性未变。
初次 configure 的 JSON inventory 尚无未构建可执行文件的 command 字段，
命令注册由未变的 add_paleo_test/target 名称及 CMake diff 核对，而不是把缺字段当作一致。
未改 `.github`/CI/ruleset、安全策略、vendor/qgis 补丁或 wellsection；未新增文档。

初轮 native 对抗、测试与维护性审查未找到新运行时缺陷；CRS 文档对无效非空
crs 的表述及 correlation 同步状态残留已修。审查为本机同模型覆盖，未运行外部
模型进程；未测菜单激活/大栅格生命周期等可选补强仍如上列明。最终复核单独记录
在交付/PR 中，不用基线红或静态审查声称全绿、百分比分支覆盖率或跨模型验证。

任务期间 `origin/master` 前进到 `2ce97f8`（catalog 行序、UI 探针修复、include 去重、
TODOS 对账）；本分支保留指定 `3b22a9c` 基线的行为/测试，账本明确后续修复归属。
TODOS 同时保留已进入 master 的修复注记，避免本 PR 合入后重新标回未修。
