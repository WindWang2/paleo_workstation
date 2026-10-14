# refactor(07): 后台任务中心、验证发布、图件导出与现有交付可靠性

- 分支：`goal7/tasks-validation-release-20261014` → `master`
- base：`b0520429c21f858a1c053b876f511680088564a9`（origin/master，2026-10-14 经 gh api 复核）
- 方向：07 后台任务中心·验证发布·图件导出·交付可靠性（七个并行方向之一）
- ledger：`docs/progress/parallel-7/07-tasks-validation-release.md`

## 范围与所有权

只改本方向白名单内文件：`src/services/paleotaskservice.*`、`src/workflow/batchjobqueue.*`、`src/metadata/releasestore.*`、`src/ui/releasepanel.cpp`（发布面板）与对应既有测试文件（`tst_taskservice/tst_jobrunner/tst_batchjobqueue/tst_release/tst_releasepanel/tst_layoutexport`）。`src/qgis/layoutexport.*` 方向05 只读——本轮零改动。根 CMakeLists.txt 在 PR #437 保护区——未动，全部新测试进既有测试文件。

## 改动摘要（6 个生产修复 + 13 个新测试）

1. **同层位并发任务 busy 引用计数**（G1，P1）：`onTaskFinished` 不再无条件 `markLayerFree`——同 layerId 多任务（批队列同层多方法、同文件预取+解码重叠）最后一个终态才解锁；否则 §35 工具门/编辑门/校验门在第二个任务仍在写产物时误放行。
2. **批次会话守卫**（G2，P1）：`BatchQueue` 钉起批会话号，`pump()` 单一强制点——工程切换/关闭后待跑项作废、在跑项协作取消、批次收尾（向旧工程目录写最后一份全终态记档，不再有后续增量写入）；不再把旧工程输入快照的产物 commit 进新工程。停批可在新会话重起续跑（非永久卡死）。
3. **itemId 身份唯一**（G4，P2）：重复声明（层位/方法表重复项）确定性 `#2` 后缀——`m_indexById` 不再覆盖、面板行/单项取消/报告记账不错位；指纹判重语义不变（既有 `skipped==2` 断言原样保留）。旧记档恢复的 last-wins 折叠由批内指纹判重兜底（语义安全，不做旧档迁移）。
4. **pump 让位判据排除自身任务**（G5，P2）：池被「非本批」任务占满才让位交互——旧判据把批次自身任务计入，池容量≤并发度时第二项永不出队、`pumpLater` 以事件循环频率自旋。
5. **pump 出队上限按并发度**（G5b，实测暴露）：构造默认 2 槽只扩不缩，pump 却按槽位数出队——`setConcurrency` 下调对出队完全无效（与头文件「运行中调只影响后续出队」契约不符）。出队循环改 `qMin(槽数, 并发度)`；会话守卫/熔断的取消仍遍历全槽。
6. **发布对比可诊断**（E）：`ReleaseStore::diff` 尾插可选 `error` 出参（入口清空、成功不带细节、查询失败/版本不存在/两侧同版本分开报因，向后兼容）；面板提示带细节。

测试锁定既有契约（A/F）：`failPublication`/`discardPublication`（此前全仓零测试）的翻转/幂等/互斥/finished-不重发；早/晚观察者对发布失败的分层可见性（#159 口径文档化）；重复导出同路径干净覆盖。

## 本地验证（实际执行，2026-10-15）

- `flock … cmake --build …/goal7-task-release --target paleo tst_taskservice tst_jobrunner tst_batchjobqueue tst_batchjobpanel tst_taskpanel tst_release tst_releasepanel tst_layoutexport -j4` → **退出码 0**
- `ctest -R "tst_taskservice|tst_jobrunner|tst_batchjobqueue|tst_batchjobpanel|tst_taskpanel|tst_release|tst_releasepanel|tst_layoutexport" -j4` **×两遍 → 两遍均 100% tests passed, 0 tests failed out of 8**（含全部新用例；tst_batchjobqueue 由修复前 108s 降回 9s）
- `python3 tools/check_layering.py --strict` → 通过（退出码 0）
- `git diff --check` → 通过
- 独立 review（S5）：首轮 **FAIL-blockers**（B1 红测试/M1 查询失败误报/m2 重复点名/文档计数措辞）→ 全部修复（ef291929/32a6b949）→ 复验 8/8 绿
- 反例先红后绿实测：G1（overlappingLayerTasksHoldBusy…）；G2（sessionSwitch…，修复前 computeCalls==2 实测红）；G4（ids.size()==2）；G5（taskAdds==2）；G5b 由 sessionSwitch 用例实测暴露（双槽出队）；B1 由 S5 实跑确认红。另修一处自产测试缺陷（早观察者内层 connect 悬垂捕获 task 参数）。
- 环境波折（不影响仓库内容）：项目盘 `/home/kevin/project`（nvme0n1p2）一度 100% 满 → 本方向 build 迁 `/home/kevin/paleo-builds/goal7-task-release`（sda2）；Qt6Keychain 复用方向02 overlay（vendored 0.15.0；系统 0.17.0 与 vendored Qt 混链必败）；测试运行需 `LD_LIBRARY_PATH=<vendor>/lib/x86_64-linux-gnu:<vendor>/lib` + `QT_PLUGIN_PATH=<vendor>/lib/x86_64-linux-gnu/qt6/plugins`（ctest 的 ENVIRONMENT 不透传外部路径变量，见 CMakeLists.txt:844 注释）。

## 排除与围栏核查

- PR #437（84 文件）：零交集（`comm` 实测）。
- 不修任何 Open Issue：#362（validatepage cell widget——本方向完全未动 validatepage）、#374（batchjobpanel——未动）、#378（retryFailed 熔断——master 已修，本分支未碰该路径，非重复修）、#371（seismicsection 反演——不同组件）等开放 Issue 逐类核对；同族新发现记入 ledger「留给 Issue Agent 表」而非顺手修。
- validatepage 经审计主体健康（异步三重陈旧守卫+失败解释+busy 门），无未占用缺口，不硬造改动。

## 与并行 PR 的文件交互（发布前复查）

- PR #448（方向05，本轮新开）也向 `tests/tst_layoutexport.cpp` 纯新增用例（+160/-0，主题为截断 PDF 校验/异步信号契约）；本分支对该文件同为纯新增（重复导出用例）——语义独立，文本级相邻插入，后合并方需重解一次琐碎 git 冲突。`src/qgis/layoutexport.*` 源码本方向零改动。
- 其余新开 PR（#445/#446/#447/#449/#450/#451）与分支文件零交集。

## 已知风险与限制

- BatchQueue/BatchJobPanel 目前无生产接线（库+测试先行）；会话守卫/身份唯一/并发度门按接线即用口径实现，接线本身不在本轮范围（属新业务接线，递延）。
- G1 修复只覆盖「任务-任务」同键并发；任务与编辑会话同键并发仍为既有覆盖语义（qgiseditingservice 独立持有），行为不变。
- 未处理 Issue 摘要：全部开放 Issue 未认领未修改；本方向新发现但递延的 5 项见 ledger「留给 Issue Agent 表」。

## 下一阶段建议

- BatchQueue 生产接线前的队列容量/磁盘配额设计（ledger 已记）。
- JobRunner 析构时在飞 cleanup 钩子的补偿语义（建议开新 Issue）。
