# 任务框架统一 + 巨型文件拆分（JobRunner）

状态：**轮 0–3 完成**（框架 + 2 处迁移面）。`tst_jobrunner` 11/11、
`tst_propworkflow` 15/15、`tst_factorworkflow` 21/21 绿。轮 4–5 未开始。
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
| 1 | 行为保留断言（信号序/取消/失败态） | **部分通过**（1/4 组已迁） | 轮 2：`tst_propworkflow` 3 条异步断言 + 11 个既有用例原样绿 |
| 2 | `tst_jobrunner` 5 条框架断言 | **通过** | 11/11 PASS，见「轮 1 实测」 |
| 3 | 拆分后既有测试原样绿 | **部分通过** | 轮 2 已迁面的既有测试原样绿；拆分发生在轮 4 |
| 4 | `datapreviewtabs` 拆后预览测试全绿 + 分发覆盖 | 未开始（轮 4） | — |
| 5 | 探测面每条绿或红进 TODOS | 未开始（轮 5） | — |
| 6 | 文件规模断言（单文件 ≤1500 行） | 未开始（轮 4） | — |
| 7 | 全账 + 本文档收口 | 进行中 | 轮 0–2 已记账 |

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

**处置**：这是环境/构建配置问题，**不在本 PR 修**——按方向 20 的禁区「重构不改
行为、不夹带无关改动」，修它属于另一个主题（要么给 QGIS 依赖补 QScintilla 的
include 接线，要么把 `qgscodeeditor.h` 从不需要它的 TU 里断开）。

**对本轮结论的影响**：无。迁移改动的两个 TU 均已在本轮成功编译成 obj
（`propertymodelworkflow.cpp.obj` 09:47、`paleomainwindow_attach.cpp.obj` 09:51），
且迁移的运行期行为由 `tst_propworkflow` 15/15 绿证明。**但 `paleo_ui` 的完整
链接未验证**——故 Oracle 1 记为「部分通过」而非「通过」。

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
