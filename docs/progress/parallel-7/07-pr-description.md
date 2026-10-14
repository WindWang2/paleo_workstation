# refactor(07): 后台任务中心、验证发布、图件导出与现有交付可靠性

- 分支：`goal7/tasks-validation-release-20261014` → `master`
- base：`b0520429c21f858a1c053b876f511680088564a9`（origin/master，2026-10-14）
- 方向：07 后台任务中心·验证发布·图件导出·交付可靠性（七个并行方向之一）
- ledger：`docs/progress/parallel-7/07-tasks-validation-release.md`

## 范围与所有权

只改本方向白名单内文件：`src/services/paleotaskservice.*`、`src/workflow/batchjobqueue.*`、`src/metadata/releasestore.*`、`src/ui/releasepanel.cpp` 与对应既有测试文件（`tst_taskservice/tst_jobrunner/tst_batchjobqueue/tst_release/tst_releasepanel/tst_layoutexport`）。`src/qgis/layoutexport.*` 方向05 只读——本轮零改动。根 CMakeLists.txt 在 PR #437 保护区——未动，全部新测试进既有测试文件。

## 改动摘要（4 个生产修复 + 12 个新测试）

1. **同层位并发任务 busy 引用计数**（G1，P1）：`onTaskFinished` 不再无条件 `markLayerFree`——同 layerId 多任务（批队列同层多方法、同文件预取+解码重叠）最后一个终态才解锁；否则 §35 工具门/编辑门/校验门在第二个任务仍在写产物时误放行。
2. **批次会话守卫**（G2，P1）：`BatchQueue` 钉起批会话号，`pump()` 单一强制点——工程切换/关闭后待跑项作废、在跑项协作取消、状态落盘收尾；不再把旧工程输入快照的产物 commit 进新工程、不再向旧工程目录续写状态。停批可在新会话重起续跑（非永久卡死）。
3. **itemId 身份唯一**（G4，P2）：重复声明（层位/方法表重复项）确定性 `#2` 后缀——`m_indexById` 不再覆盖、面板行/单项取消/报告记账不错位；指纹判重语义不变（既有 `skipped==2` 断言原样保留）。
4. **pump 让位判据排除自身任务**（G5，P2）：池被「非本批」任务占满才让位交互——旧判据把批次自身任务计入，池容量≤并发度时第二项永不出队、`pumpLater` 以事件循环频率自旋。
5. **发布对比可诊断**（E）：`ReleaseStore::diff` 尾插可选 `error` 出参（向后兼容），缺哪个版本点名；面板提示带细节。
6. **测试锁定既有契约**（A/F）：`failPublication`/`discardPublication`（此前全仓零测试）的翻转/幂等/互斥/finished-不重发；早/晚观察者对发布失败的分层可见性（#159 口径文档化）；重复导出同路径干净覆盖。

## 排除与围栏核查

- PR #437（84 文件）零交集（`git diff origin/master...HEAD --name-only` 逐一对过 /tmp/pr437_files.txt）。
- 不修任何 Open Issue：#362（validatepage cell widget——本方向完全未动 validatepage）、#374（batchjobpanel——未动）、#378（retryFailed 熔断——未动该路径）、#371（seismicsection 反演——不同组件）等 77 个开放 Issue 逐类核对；同族新发现记入 ledger「留给 Issue Agent 表」而非顺手修。
- validatepage 经审计主体健康（异步三重陈旧守卫+失败解释+busy 门），无未占用缺口，不硬造改动。

## 本地验证（实际执行）

- `flock -x /tmp/paleo_workstation_global_build_j4.lock cmake --build build --target paleo -j4` → 退出码 【待填】
- `ctest -R "tst_taskservice|tst_jobrunner|tst_batchjobqueue|tst_release|tst_releasepanel|tst_layoutexport" -j4` → 【待填】
- `python3 tools/check_layering.py --strict` → 通过（退出码 0）
- `git diff --check` → 通过
- 反例先红后绿：G1（overlappingLayerTasksHoldBusyUntilLastFinishes）、G2（sessionSwitch…executor 恰 1 次）、G5（taskAdds==2）在修复前语义下必红【回填实测】

## 已知风险与限制

- BatchQueue/BatchJobPanel 目前无生产接线（库+测试先行）；会话守卫/身份唯一按接线即用口径实现，接线本身不在本轮范围（属新业务接线，递延）。
- G1 修复只覆盖「任务-任务」同键并发；任务与编辑会话同键并发仍为既有覆盖语义（qgiseditingservice 独立持有），行为不变。
- 未处理 Issue 摘要：全部 77 个开放 Issue 未认领未修改；本方向新发现但递延的 5 项见 ledger「留给 Issue Agent 表」。

## 下一阶段建议

- BatchQueue 生产接线前的队列容量/磁盘配额设计（ledger 已记）。
- JobRunner 析构时在飞 cleanup 钩子的补偿语义（建议开新 Issue）。
