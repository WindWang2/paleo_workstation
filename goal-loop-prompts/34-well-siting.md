# Goal-Loop 方向 34：新井部署与井网辅助——覆盖诊断、候选点位与方案对比

## 背景（实测事实，勿再勘察）

工区井网合理性直接影响编图质量（方向 27 QA 的「缺井覆盖区」即此需求的
另一面）。现状：井位在 `wells` 实体（catalog），工区边界受 `arearules` 米制
契约管，GEOS 可用；无任何部署辅助面。约束：**建议只出候选不自动落井**
（虚拟井实体单独标记 `planned`，绝不混进 `wells` 实井计算输入）。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/well-siting -b goal/well-siting-20261004 origin/master
cd .worktrees/well-siting
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

1. **覆盖诊断**（`src/algorithms`+GEOS）：现有井距统计（最近邻/分位数）、工区内覆盖空洞检测（距最近井>阈值的连续区）→ 诊断图层（正常 declare 管线）。
2. **候选点位生成**：在覆盖空洞内按规则网格/最大空洞圆心生成候选点位；避开约束线/断层/边界缓冲（距离参数可配）。
3. **方案评估**：候选井加入后的覆盖改善指标（空洞面积差、平均最近邻距差、按层位加权的井控密度差）——逐候选井出贡献分。
4. **交互布点**：地图上手工放候选井（吸附网格/自由放），评估指标实时刷新；候选井可增删改名。
5. **方案对比视图**：多套布点方案（方案=候选井集合+指标快照）并列对比表；方案随工程持久化（实体类型 `planned`，与实井隔离）。注意 catalog 实体词表现为 `well|seismic_survey|sequence_boundary|auxiliary`——`planned` 是新词，须走角色/词表登记面正式扩展（catalog 有角色校验，硬编码字符串过不了）。
6. **导出**：方案点位表 CSV + 覆盖对比图导出。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；诊断/评估纯计算归 `src/algorithms`，交互归 `src/ui`。
- **诚实面**：指标如实标口径（欧氏近似/工区边界裁剪）；覆盖评估不声称地质最优。
- **planned 隔离**：`planned` 实体绝不进单因素/编图/剖面计算输入——这是 High 级红线，测试必断言。
- **资源**：构建/测试一律 `-j8`。
- **vendor**：改 vendor 件登记 `PATCHES.md`。
- **UI**：对照 `DESIGN.md`；i18n 过两门。
- **性能断言**：禁绝对毫秒墙钟；空洞检测大工区用抽样式比率门。
- **ledger**：`.goal-loop-ledger-well-siting.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/planned 隔离/GEOS 释放/指标口径诚实面/版本管线/i18n 六维）→ 修复 → 再 review，**至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 空洞检测：合成规则井网+人工空洞 → 检出空洞面积/位置断言。
2. planned 隔离：含 planned 井场景跑单因素/编图 → 计算输入实体集断言不含 planned。
3. 方案评估：加一候选井 → 覆盖指标改善方向与幅度断言（合成夹具可解析验证）。
4. 方案持久化：方案保存-重开点位/指标一致。
5. 全量绿：新用例 + `tst_arearules`、`tst_workflows`、`tst_constraintstore`；`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（`## Summary`/`#### Test plan`/自审结论清单/遗留项）。**不合并、不推 master、不等 CI。**
