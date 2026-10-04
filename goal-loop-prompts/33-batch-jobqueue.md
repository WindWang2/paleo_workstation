# Goal-Loop 方向 33：批处理与作业队列编排——多层位批量计算、恢复与报告

## 背景（实测事实，勿再勘察）

JobRunner 统一三段式刚并入（方向20：`services/jobrunner.{h,cpp}`，
prepare/compute/publish + 代次丢弃 + 协作取消），`paleotaskservice` 承载
QThreadPool 调度。但单因素/相图/等值线目前都逐个 horizon 手点——多层位
全工区批量跑没有队列编排。约束：**批作业复用既有 JobRunner 工作流路径，
不新造计算通道**；队列状态可持久化，崩溃后可恢复。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/batch-jobqueue -b goal/batch-jobqueue-20261004 origin/master
cd .worktrees/batch-jobqueue
# vendor 三件为 gitignored——须从主仓绝对路径 symlink（../../ 相对路径会自环，勿用）
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **批量作业定义**：按「层位集合 × 方法集合 × 参数模板」声明批次（JSON 可序列化）；单作业=既有工作流调用（generateFactor/等值线/相预测等），参数逐作业可覆写。
2. **队列调度**：FIFO + 并行度可配（默认保守并发，不与用户交互作业抢线程）；优先级插队、暂停/恢复/取消整批或单作业。
3. **断点恢复**：批次状态（完成集/失败集/待定集）持久化到工程；应用重启后可续跑，已完成作业不重复（幂等键=参数指纹）。
4. **进度与报告**：批次进度面板（逐作业状态/耗时/失败原因）；批后汇总报告（成功/失败/跳过+失败原因聚合）可导出。
5. **失败策略**：单作业失败不炸批——记录原因继续；失败作业可单独重试；致命错误（catalog 不可用）熔断全批。
6. **编排 UI**：批次编辑器（选层位/方法/参数模板）+ 队列面板；与既有 JobRunner 进度信号集成，不绕进度面。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；队列调度归 `src/services`/`src/workflow`，UI 只观察信号。
- **幂等**：重跑不产生重复资产/版本——参数指纹判重是硬约束。
- **资源**：构建/测试一律 `-j8`；批量测试用小批次夹具，不真跑大计算。
- **vendor**：改 vendor 件登记 `PATCHES.md`。
- **UI**：对照 `DESIGN.md`；i18n 过两门。
- **性能断言**：禁绝对毫秒墙钟。
- **ledger**：`.goal-loop-ledger-batch-jobqueue.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/幂等/取消语义/崩溃恢复路径/熔断边界/i18n 六维）→ 修复 → 再 review，**至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 幂等：同批次连跑两遍 → catalog 资产/版本零增量。
2. 崩溃恢复：批次中途强杀（夹具内模拟）→ 重启续跑只补未完作业。
3. 失败隔离：批次内造一单作业失败 → 其余照常完成，失败原因聚合进报告。
4. 取消语义：批中取消 → 在跑作业协作停止、待跑作废、状态如实回写。
5. 全量绿：新用例 + `tst_jobrunner`、`tst_workflows`、`tst_factorworkflow`、`tst_ui`；`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（`## Summary`/`#### Test plan`/自审结论清单/遗留项）。**不合并、不推 master、不等 CI。**
