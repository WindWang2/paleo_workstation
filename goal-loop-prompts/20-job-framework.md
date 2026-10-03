# Goal-Loop 方向 20：任务框架统一 + 巨型文件拆分（重构大方向）

## 背景（实测事实，勿再勘察）

- **三段式任务模式已散落重造 5 次**：prepare/compute/publish 各自为政——
  `workflows.cpp`（4086 行，含 `prepareLocalDirectionJob`/`computeLocalDirectionJob`/`publishLocalDirectionJob` + 局部方向 + 解释等值线两组三段）、
  `propertymodelworkflow.cpp`（767 行，`runCompute`/`commitComputed`）、
  `layoutexportactions`（`exportLayoutAsync` worker 路径）、
  `faultsurfaceworkflow`（#111 新增）、
  `paleomainwindow_attach.cpp`（2815 行，finished 回包编排）。
  每处各自实现：generation 防陈旧、任务取消接线、进度回包、失败诚实——**同一协议写 5 遍，语义各有偏差**。
- **两座巨兽文件**：`src/workflow/workflows.cpp` 4086 行（多工作流挤一文件）、`src/ui/datapreview/datapreviewtabs.cpp` 5249 行（全部资产类型预览构建器挤一文件）。`paleomainwindow*.cpp` 已拆 5 片（attach/faults/sections/workbench 前缀拆分先例）。
- **任务服务**：`PaleoTaskService`（`src/services` 或 `src/app`）已有 `runTask`/`requestCancel`/`reportStage` + finished 回包槽——框架的锚已存在。
- **GUI 线程残留**：`tst_ui_blocking` 只盖了 LAS 预览 + validate 两探针；其余长操作（实体批量删除、元数据打开、连接诊断、`openAsset` 非 LAS 类型、综合图重绘）**无系统性探测**。
- **#80 owner-thread 守卫**已立：catalog 写面必须 GUI 线程——框架需原生尊重此约束（commit 段强制 owner 线程）。
- **已知边界**：分层检查器只挡 include 层，「不带 include 直接 new」挡不住——本方向顺带把跨层构造面做一次审查。
- 纪律：读 DESIGN.md；每 `src/` 文件三层标记；`add_paleo_test`；`check_layering --strict` 绿；**重构不改行为**——每个重排都配行为保留断言。

## 目标形态

两件大事：**抽出统一 JobRunner，5 处散落三段式收敛到框架**；**拆两座巨兽文件**。

1. **`JobRunner` 框架**（`src/workflow/jobrunner.*` 或 `src/services`）：
   - 统一三段式契约：`prepare`（owner 线程，抓输入快照）→ `compute`（worker，纯计算）→ `commit`（owner 线程，登记/发信号）；
   - 框架原生提供：generation 防陈旧序列化、取消传播（`requestCancel` → compute 轮询点）、`reportStage` 进度回包（QueuedConnection）、失败通道（异常捕获→commit 段收到失败态如实上报）；
   - **owner-thread 约束内建**：commit 段强制回 GUI 线程（`QMetaObject::invokeMethod` + 断言 affinity），#80 守卫从纪律变成机制；
   - 现状 5 处各自的三段式全部迁到 JobRunner——行为等价断言集（每处迁移前后信号序/失败态/取消语义一致）。
2. **`workflows.cpp` 拆分**：按工作流边界拆 `constraintworkflow.cpp`/`contourworkflow.cpp` 等（现状 4086 行单文件 → 按职责分文件，头文件契约不动）。
3. **`datapreviewtabs.cpp` 拆分**：按资产类型拆构建器（LAS/SEG-Y/井/图版/其他各一片，照 `paleomainwindow_*` 前缀先例）；`openAsset` 调度表集中。
4. **GUI 线程探测面扩展**：`tst_ui_blocking` 同款「事件循环探活 + 计时」探针铺到每个已识别长操作（实体批量删除/元数据打开/连接诊断/综合图重绘）——**探测先于修复**，红的进 TODOS，能修的修。

## Oracle 验收（全部须实测通过并记账本）

1. **行为保留断言**（重构纪律）：每个迁移的工作流——信号序（started→progress→finished/failed 顺序）、取消语义（cancel 后 commit 不执行）、失败态（错误经既有通道上 UI）三条断言。
2. **框架测试** `tst_jobrunner`：compute 在 worker 线程断言（`QThread::currentThread` 比较）；commit 在 owner 线程断言；generation 过期任务 commit 被丢弃断言；cancel 中断 compute 断言；异常穿越 worker→commit 断言。
3. **拆分不丢面**：`workflows.cpp` 拆后所有既有测试（`tst_workflows`/`tst_constraint*`/`tst_propmodel*` 等）原样绿——不改断言。
4. **`datapreviewtabs` 拆后**：`tst_preview*`/`tst_ui` 全绿；`openAsset` 各类型分发覆盖断言。
5. **探测面**：新增 UI 线程探针每条要么绿要么红进 TODOS（红的如实标注「已知阻塞：xxx」）。
6. **文件规模**：`workflows.cpp` 拆分后单文件 ≤1500 行断言（阈值记文档）；`datapreviewtabs` 同。
7. ledger 全账 + `docs/progress/job-framework.md`（三段式契约形态、迁移映射表、拆分边界、探测覆盖面、递延清单）。

## 勘察指引

- `src/workflow/workflows.cpp` 的三段式全集（prepare/compute/publish 命名形态）+ `propertymodelworkflow.cpp` 的 `runCompute`/`commitComputed` 是**最干净的参考形态**（#85 修的）
- `src/services/paleotaskservice.*` 或 `src/app`（任务服务实际落点、`runTask`/`reportStage`/finished 回包签名）
- `src/ui/paleomainwindow_attach.cpp`（finished 回包编排现状）、`paleomainwindow_*.cpp` 拆分前缀先例
- `src/ui/datapreview/datapreviewtabs.cpp`（`openAsset` 分发 + 各类型构建器边界）
- `tests/tst_ui_blocking.cpp`（探针先例：事件循环探活 + 比率门 + 热身模式）
- `#80` 守卫：`datacatalog.*` owner-thread 断言面、`entitystore` AttachLink 契约
- `docs/progress/single-factor-native.md` 的「编排三态」段（模式文档先例）

## 禁区

- **重构不改行为**：不许在拆分/迁移时顺手改语义——要改的进 TODOS，不混进重构 PR。
- 不碰 `src/catalog` 持久层；不碰 `src/services/welllogset`（#108 已合）；不碰方向 18/19 的在飞文件面（`algorithms/geostat`、测斜域模型）。
- `single-factor` agent 可能仍在迭代 `workflows.cpp` 相关段——**合并冲突风险自知**，勘察轮先 `git log` 确认该方向收官状态，在飞的段不碰。
- 不引入新线程模型（QThreadPool/QtConcurrent 现状沿用）；不改 `PaleoTaskService` 的公开契约（只加适配）。
- 探测探针**不加宽松阈值**——红的如实红进 TODOS，不许调阈值过关。
- 不为拆而拆：拆分按职责边界，不机械按行数切。

## 迭代协议

- **轮0**：勘察定案——5 处三段式的语义差异清单（各自 generation/cancel/progress 实际行为）、`workflows.cpp`/`datapreviewtabs` 拆分边界、single-factor 方向收官状态确认；JobRunner API 签名进 ledger。
- **轮1**：`JobRunner` 框架 + `tst_jobrunner`（5 条断言集）。
- **轮2**：迁移 `propertymodelworkflow` + `faultsurfaceworkflow`（最干净两处先试）+ 行为保留断言。
- **轮3**：迁移 `workflows.cpp` 三段式 + `layoutexport` + `paleomainwindow_attach` 回包收敛。
- **轮4**：`workflows.cpp` 拆分 + `datapreviewtabs.cpp` 拆分 + 不丢面断言。
- **轮5**：探测面扩展（每条探针红/绿如实记账）+ progress 收口。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/jobframe -b goal/job-framework-<date> origin/master`
2. 每轮原子提交，ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**：`git diff origin/master...HEAD` 全量自审；**逐迁移点核对行为等价**；无调试残留；层标记齐；`check_layering --strict` 绿；vendor 前缀全量构建零新警告；ctest 全绿；Oracle 每条有命令+输出证据。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
