# 07 后台任务中心·验证发布·图件导出·交付可靠性 — 分阶段 ledger

- 分支：`goal7/tasks-validation-release-20261014`
- worktree：`../paleo-wt-goal7-task-release`
- base SHA：`b0520429c21f858a1c053b876f511680088564a9`（= origin/master，2026-10-14 经 gh api 复核）
- 写权限白名单：jobrunner/paleotaskservice/batchjobqueue/validationworkflow(+验证发布实际文件)/batchjobpanel/validatepage/releasestore/notifications(协商)/make_appimage/selfcheck + 对应测试
- 硬围栏：PR #437 的 84 文件（实时 `gh pr view 437 --json files`，快照存 /tmp/pr437_files.txt）；Open Issues #352–#444 全部不修
- 特别注意：**根 CMakeLists.txt 在 #437 保护区** → 新测试一律加进既有测试文件，不新建 target/源文件

## 排除指纹（本方向名义白名单内但被 Issue 占用的路径）

| Issue | 路径/故障 | 本方向处置 |
|---|---|---|
| #362 | validatepage.cpp repopulate 不释放 cell widget（HEAD 已部分修复 populateResult 路径；clearResults() 同族残留） | 不动 validatepage 的 cell widget 处理；clearResults 残留记「留给 Issue Agent」 |
| #374 | batchjobpanel refresh() 不回收行（幽灵行） | 不动 batchjobpanel 行回收逻辑 |
| #378 | batchjobqueue retryFailed 运行中不清熔断态 | 不动 retryFailed/熔断路径 |
| #395 家族 | store 读路径 read-write 开库（faultset/wellsiting/wellsection 三 store） | releasestore 无只读实例概念，不加只读支持（避免同故障改名）；记「留给 Issue Agent」 |

## R0 — 真实基线（2026-10-14）

### 领域调用图（文件:行号证据见 S1 审计，要点）

- **PaleoTaskService**（src/services/paleotaskservice.*）：任务注册中心 + 专用 QThreadPool(≤4)。4 态状态机（Running/Succeeded/Failed/Cancelled），终态收敛在 owner 线程 applyFinish（幂等闸 :192），worker 只读原子 cancel 位。会话号 #153/#124、关停排空 #416 补偿、保留策略 #164。
- **JobRunner**（src/services/jobrunner.*）：三段契约 prepare(owner)→compute(worker)→commit(owner, finished 槽内同步 #159)。代际 Claim 防陈旧回包；stale= 代际过期或会话不等（jobrunner.h:329-330）。compute 失败仍进 commit（失败如实上 UI）。
- **BatchQueue**（src/workflow/batchjobqueue.*）：编排层，槽位=并发度[1,8]，每槽一个 JobRunner；批次状态 JSON 落 `<projectDir>/artifacts/metadata/batch_jobs/<batchId>.json`（QSaveFile 原子写）。**生产零接线**（仅测试消费；batchjobpanel 也未被窗口实例化）。
- **ValidationWorkflow**（src/workflow/validationworkflow.cpp）：同步 validate() + 异步 startValidation()（快照 catalog/declarations，三重陈旧守卫：session+mutationSeq+QPointer；stale 走 discardPublication）。问题码：MANIFEST_READ_FAILED/SRC_MISSING/DUP_HORIZON/BUSY/TIME_RESIDUAL/RASTER_MISSING。
- **ValidatePage**（src/ui/pages/validatepage.cpp）：问题表+残差表双表、三视图联动载荷、剖面导航 armed 载荷、工程边界 clearResults()。
- **ReleaseStore/ReleasePanel**（src/metadata/releasestore.*、src/ui/releasepanel.cpp）：releases 表（同 project.sqlite）、不可变快照、diff(A,B)。无只读实例概念。
- **导出管线**（只读审计）：qgis/layoutexport（方向05 只读）→ workflow/mapexport、mapbookqueue（staging 树+owner 守卫 journal+SHA256+manifest 失败明细）、horizonbatchexport（staged+原子登记+会话/世代检查）。ui/layout/layoutexportactions 是 UI 壳。
- **部署验证**：src/selfcheck（qgis 组 PASS/FAIL + perf 组 JSON 预算）、tools/make_appimage.sh（全量捆绑+不动点闭包收集）。

### S1 审计发现的候选缺口（全部避开 Issue/PR 围栏后核实）

| # | 缺口 | 严重度 | 状态 |
|---|---|---|---|
| G1 | layer busy 注册表无引用计数：同层位两并发任务，先终态者提前解锁（paleotaskservice.cpp:283/359 + store QHash 覆盖语义）——§35 工具门/编辑门/校验门(BUSY)全部误放行 | P1 | 已核实代码，进 Round A |
| G2 | BatchQueue 会话零感知：工程切换后 pump() 继续出队，新项带新会话号照常 commit，writeState 写旧工程目录 | P1 | 已核实，进 Round B |
| G3 | failPublication/discardPublication 零测试；头文件 :85-87 契约「Finished observers must see that failure」对早于 runner 注册的观察者不成立（#159 口径只保晚注册者） | P2 | 进 Round A（测试锁定+头注释精确化） |
| G4 | 重复 itemId 折叠：expand 不去重 + m_indexById 覆盖 → 面板行/取消/记账错位 | P2 | 进 Round C（先读 duplicateFingerprint 测试） |
| G5 | pump 让位判据把批次自身任务计入 runningCount → 池饱和时 GUI 线程排队自旋 | P2 | 进 Round C |
| G6 | runner 析构跳过 cb.cleanup 钩子（在飞 Job 临时产物仅两处靠 Job 析构兜底） | P2 | 记录，视轮次余量 |
| 观察 | releasestore ensureOpen 读路径建表（#395 同族模式）、releasepanel diff 失败无细节、createdUtc 生 ISO 串 | P3 | 记「留给 Issue Agent」/低优先 |

### 本机基线构建/测试

- 依赖接线：vendor/{onnxruntime,prefix} → symlink 共享 pw-seismic-runtime-closure-20260930-215353（与兄弟 worktree 同源，不并发写共享 prefix）。
- configure/build/ctest 全部经 `/tmp/paleo_workstation_global_build_j4.lock`，-j4。（基线构建结果见下方轮次记录）

## 轮次记录

### Round A — 任务状态语义（G1+G3）

**可观察现有行为（3 条，测试锁定）：**
1. 终态只落一次：`applyFinish` 幂等闸（paleotaskservice.cpp:192-193），排队丢弃补偿与 runner 回包双到只生效一次（既有 tst_taskservice「终态后迟报忽略」+ 新增发布翻转一次性断言）。
2. compute 失败仍通知 owner 并进 commit（失败如实上 UI）：jobrunner.h:350-370，既有 tst_jobrunner::computeFailureReachesCommitAsFailedState。
3. stale 会话/代际在 commit 入口被丢弃（jobrunner.h:329-330 + #153/#159/#235）：既有 sessionResetDropsInFlightJob + 新增发布失败分层可见性测试。

**真实反例（G1，先红后绿）：** 同层位两个并发任务，先终态者无条件 `markLayerFree`（paleotaskservice.cpp 旧 :359），§35 工具门/编辑门/校验门在第二个任务仍在写产物时误放行。
- 修复：`m_layerHolds` 引用计数（createTask ++，onTaskFinished -- 归零才解锁）；不改 store 侧（paleoprojectstore 不在本方向白名单，且 qgiseditingservice/toolavailability 各有独立持有语义，不越界）。
- 测试：`overlappingLayerTasksHoldBusyUntilLastFinishes`（红→绿）、`cancelledOverlappingLayerTaskStillCounts`（取消路径计数对账）。

**G3（契约精确化+零测试补齐）：** `failPublication`/`discardPublication` 全仓零测试。新增 3 个语义测试（翻转/幂等/互斥/默认文案/finished 不重发）+ 1 个观察者分层测试（早观察者 taskAdded 期连接见 Succeeded；晚观察者 start() 后连接见 Failed；jobCompleted(gen,false)）。头注释 :85-88 改为 #159 精确口径（早观察者须在 changed() 复读终态）。

### Round B — 会话切换与批次停批（G2）

**反例（G2）：** BatchQueue 无会话感知——工程切换后 pump() 继续出队，新项带新会话号照常 commit（旧工程输入写进新工程）、writeState 写旧工程目录。
- 修复：`resume()` 钉 `m_sessionAtStart`；`pump()` 顶部会话守卫（单一强制点）：待跑项作废（Cancelled/「工程已切换或关闭」）、在跑项协作取消、状态落盘、收尾。retryFailed 的重启路径同样被守卫拦截（会话号不重钉）；批后 Cancelled 项非 Failed → retryFailed 无对象，返回 false。
- 与 #371 边界：该 Issue 是 `src/ui/seismicsection/seismicsectiondock_inversion.cpp` 反演面板的 generation/复位缺失，不同文件不同组件，无重叠。
- 测试：`sessionSwitchStopsBatchInsteadOfCrossingProjects`（executor 恰 1 次调用、剩余全 Cancelled、状态落盘、retryFailed false）、`sessionStoppedBatchIsResumableInNewSession`（停批不是卡死：新会话重起同批全跑完）。

### Round C — 批处理身份与调度（G4+G5）

**G4 反例：** 重复声明（methodIds/horizons 含重复）→ itemId 撞车 → m_indexById 覆盖、面板行折叠、cancelItem 只命中最后一份。
- 修复：`expand()` 确定性 "#2" 后缀去重（覆写表仍按规范键查；同 spec 重展开可重现 → 续跑对账不变；指纹判重语义不变，既有 duplicateFingerprintInSameBatchSkipped 的 skipped==2 断言原样保留）。
- 测试：`duplicateDeclarationKeepsItemIdentityUnique`（2 项 2 id、计数不缩水、起批判重即 Skipped）。

**G5 反例：** pump 让位判据 `m_tasks->runningCount() >= poolCap` 把批次自身任务计入——池容量 1、并发 2 时第二项永不出队，pumpLater 以事件循环频率空转自旋。
- 修复：让位判据改为「非本批任务」`(m_tasks->runningCount() - running) >= poolCap`（running = 本批 Running 项数，含本轮已出队者）。
- 测试：`ownPoolSaturationDoesNotBlockDequeue`（taskAdded==2、runningCount==2、放闸后全 3 项成功）。

### Round D — 验证页面完整性（结论：不改代码）

- ValidationWorkflow 异步路径三重陈旧守卫（session+mutationSeq+QPointer，validationworkflow.cpp:311-328）健康且与 #285/#160 语义一致；验证记录来源/时间/复验入口（运行按钮 busy 门+失败解释文案）齐备。
- validatepage.cpp 的 cell widget 处理全部落在 #362 修复半径内（populateResult 已有 clearCellWidgets；clearResults() 的同族残留属 Issue Agent 收尾范围）——本方向不动，避免同故障改名重复修。
- 无其它未占用缺口具备事实证据，不为凑数硬造。

### Round E — 发布对比可诊断性

**反例（轻）：** `ReleaseStore::diff` 失败只有 bool——库打不开/版本不存在不可区分，面板笼统报「版本对比失败」。
- 修复：diff 增尾插 `QString *error = nullptr`（向后兼容）；缺哪个版本点名（"release 不存在：rel-404"）；ReleasePanel 显示细节。
- 测试：`diffErrorNamesMissingRelease`（单缺/双缺/成功不写 error）、`diffFailureSurfacesActionableReason`（真实 UI 路径：下拉塞 rel-404 → 点对比 → statusMessage 带版本号与「不存在」）。
- 注：releasestore 的 ensureOpen 读路径建表与 #395 同族（该 Issue 名单外但同模式），不加只读实例支持——记「留给 Issue Agent」。

### Round F — 导出失败验证（结论：既有覆盖良好，补 1 个缺口）

- 已核实（3 条可观察行为）：错误路径诚实（null layout/父路径是文件/空页选/缺模板 → !ok + error，无假成功）；成功产物可校验（PNG 魔数+尺寸、PDF 头、SVG 标签、多页兄弟文件 files 报告与目录计数）；缺数据源 → warnings 非静默（missingSnapshotLayerSurfacesWarnings）+ 活图层移除隔离（asyncExportSurvivesLiveLayerRemoval）。
- 缺口：重复导出同路径无测试。补 `reexportToSamePathOverwritesCleanly`（第二次成功、files 只报本趟、无残留兄弟、魔数合法）。layoutexport.* 源码零改动（方向05 只读）。

### 留给 Issue Agent 表（本方向发现但不修）

| 发现 | 关联 | 说明 |
|---|---|---|
| validatepage `clearResults()` 只 setRowCount(0) 不走 clearCellWidgets（工程关闭路径的孤儿胶囊控件） | #362 修复半径 | Issue Agent 收尾 clearResults 时一并处理 |
| releasestore 读路径（releases()/manifestAt()/diff()）经 releaseEnsureOpen 建表，无只读实例概念 | #395 同族模式 | 若 #395 修法推广到全部 store，releasestore 应跟进 readOnly 形参 |
| JobRunner 析构只取消+推进代际，在飞 Job 的 cb.cleanup 永不执行 | 无 Issue | 临时产物兜底目前靠 Job 析构（仅 mappingworkbench 两处）；建议未来开 Issue |
| BatchQueue 队列容量/内存/磁盘目录无上界（项数随 层位×方法 增长） | 无 Issue | 生产接线前需要背压/配额设计，建议开 Issue |
| releasepanel 显示原始 ISO UTC 时间串 | 无 Issue | 界面合理性小项，依赖 DESIGN.md 时间显示口径统一 |

### Round G — 本地稳定性门禁（命令集）

```bash
flock -x /tmp/paleo_workstation_global_build_j4.lock cmake --build build --target paleo -j4
flock -x /tmp/paleo_workstation_global_build_j4.lock ctest --test-dir build -R "tst_taskservice|tst_jobrunner|tst_batchjobqueue|tst_release|tst_releasepanel|tst_layoutexport" -j4 --output-on-failure
python3 tools/check_layering.py --strict
git diff --check
```
（结果见下方「构建与测试记录」；不修改 GitHub Actions，不新增 CI 相关文件。）

## 构建与测试记录（2026-10-14/15 实跑）

### 环境事实

- 项目盘 `/home/kevin/project`（/dev/nvme0n1p2）在本轮中途 100% 满（兄弟 worktree 构建占用；非本分支行为）——本方向 build 迁至 `/home/kevin/paleo-builds/goal7-task-release`（/dev/sda2），同时回收本 worktree 旧 build 1.3G。
- 依赖接线（对 BUILDING.md 的本机补齐）：`-DQGIS_PREFIX=<vendor>/prefix/usr`、`-DGDAL_INCLUDE_DIR=<vendor>/prefix/usr/include/gdal`、`-DGDAL_LIB=<vendor>/prefix/usr/lib/x86_64-linux-gnu/libgdal.so`、`-DGEOS_C_LIB=<vendor>/prefix/usr/lib/x86_64-linux-gnu/libgeos_c.so`、`-DGEOS_INCLUDE_DIR=<vendor>/prefix/usr/include`、`-DCMAKE_PREFIX_PATH=<vendor>/prefix/usr/lib/x86_64-linux-gnu/cmake;<goal7-deps>/qt6-5compat/extracted/.../cmake`、`-DQt6Keychain_DIR=/home/kevin/project/paleo-goal7-keychain-overlay`（方向02 overlay，vendored 0.15.0）。
- 构建/测试运行期环境：`LD_LIBRARY_PATH=<vendor>/prefix/usr/lib/x86_64-linux-gnu:<vendor>/prefix/usr/lib`（Qt 工具 rcc/lrelease 混载系统 Qt 会 GNU_PROPERTY 拒载）+ `QT_PLUGIN_PATH=<vendor>/prefix/usr/lib/x86_64-linux-gnu/qt6/plugins`（QSQLITE 驱动）。

### 结果（全部走 /tmp/paleo_workstation_global_build_j4.lock，-j4）

1. configure（新位置）退出码 0。
2. `cmake --build … --target paleo tst_taskservice tst_jobrunner tst_batchjobqueue tst_batchjobpanel tst_taskpanel tst_release tst_releasepanel tst_layoutexport -j4` → 退出码 0。
3. `ctest -R "…8 套件…" -j4` 第一遍 → **100% tests passed, 0 failed out of 8**。
4. 同命令第二遍 → **100% tests passed, 0 failed out of 8**（两遍均含全部新用例）。
5. `python3 tools/check_layering.py --strict` → 通过；`git diff --check` → 通过。
6. 中途实测红（先红后绿证据）：tst_release diffErrorNamesMissingRelease（B1，S5 实跑 7 passed/1 failed）；tst_jobrunner publicationFailure…（悬垂捕获 earlyState==Running）；tst_batchjobqueue sessionSwitch…（修复前 computeCalls==2——双槽出队，直接暴露 G5b）；G5b 修复后 4 个既有用例超时（夹具门超时 4s×串行 >10s）→ 门超时校准 300ms 后全绿、套件 108s→9s。

### Round C 增补（G5b，测试实跑暴露的新生产缺陷）

`BatchQueue::pump()` 出队循环按 `m_slots.size()` 遍历，而槽位在构造/setConcurrency 只扩不缩——`setConcurrency` 下调后实际并发仍是历史峰值，头文件「运行中调只影响后续出队」契约失真。修复：出队上限 `qMin(槽位数, m_concurrency)`（32a6b949）；会话守卫/熔断的 requestCancel 仍遍历全槽（超额在飞项也必须可取消）。测试价值：本修复由本方向新增 sessionSwitch 用例实测暴露（修复前 computeCalls==2），属「反例先红」链条的自然延伸。
