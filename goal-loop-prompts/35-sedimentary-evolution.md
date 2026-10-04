# Goal-Loop 方向 35：沉积体系多期演化分析——层位叠置、相带迁移与演化视图

## 背景（实测事实，勿再勘察）

古地理编图的核心问题是「演化」：本期相对上期的进积/退积/侧移方向与幅度
决定下一期编图约束。现状：层位序有 `mappingHorizons()`，相图/单因素图按
层位组织（organizer 层位组），但无跨期分析面。约束：**演化指标如实标口径**
（像素/几何近似、井控密度加权与否注明），跨期对比只在同格网/同坐标域
下进行，不同源直接拒算不近似。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/sedimentary-evolution -b goal/sedimentary-evolution-20261004 origin/master
cd .worktrees/sedimentary-evolution
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

1. **相邻期对比引擎**（`src/algorithms`+GEOS）：相邻层位相图/单因素图叠置——相带面积增减、边界位移方向量（前缘线位移矢量场/中位数）、重叠率矩阵；不同格网/坐标域直接拒算。
2. **演化指标表**：逐层位对的量化指标（各相面积、质心位移、边界进退中位距、优势相变更率）——可导出表格资产。
3. **迁移矢量图**：相带前缘位移矢量可视化图层（箭头场），进层位组正常 declare 管线。
4. **演化剖面视图**：选定剖面线上多期相叠置的纵向对比（与方向 26 wellsection 面联动则复用其容器，无联动期先独立面板）。
5. **多期动览**：层位序连续切换的演化动览（帧间淡入/步进按钮），任意帧可定格导出；步进顺序按 `mappingHorizons()`。
6. **演化报告**：多期指标汇总报告页（面积曲线图+迁移表+结论摘要区可编辑），报告可导出版本化。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；对比/指标纯计算归 `src/algorithms`，视图归 `src/ui`。
- **诚实面**：不同源（格网/坐标域/井控集不同）直接拒算并说明原因，不插值硬对。
- **资源**：构建/测试一律 `-j8`。
- **vendor**：改 vendor 件登记 `PATCHES.md`。
- **UI**：对照 `DESIGN.md`；i18n 过两门。
- **性能断言**：禁绝对毫秒墙钟；大格网对比用比率门。
- **ledger**：`.goal-loop-ledger-sedimentary-evolution.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/同源校验/指标口径/GEOS 释放/i18n/DESIGN.md 六维）→ 修复 → 再 review，**至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 拒算诚实面：格网尺寸/坐标域不同的两期输入 → 明确拒算且原因可读。
2. 合成验证：构造已知位移的相带多边形对 → 位移方向/幅度断言。
3. 面积矩阵：三相连三期合成输入 → 面积增减/重叠率逐格断言。
4. 视图联动：迁移矢量图层进层位组且样式规范；动览步进顺序与 `mappingHorizons()` 一致。
5. 全量绿：新用例 + `tst_factorworkflow`、`tst_workflows`、`tst_layertreepanel`；`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（`## Summary`/`#### Test plan`/自审结论清单/遗留项）。**不合并、不推 master、不等 CI。**
