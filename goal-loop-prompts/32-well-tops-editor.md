# Goal-Loop 方向 32：井分层数据编辑与质量管理——分层表、批量修正与版本化

## 背景（实测事实，勿再勘察）

`well_stratification` 资产已能导入（DC.dat 等）并在预览页表格呈现，但分层
**编辑**面缺位——改一个层要回源文件重导。它是层位格架（方向28 消费）、连井
剖面（wellsection 消费）、单因素井控（`wells` 实体顶深表）的共同上游，改动
必须走 catalog 版本管线并触发下游失效标记。**编辑落库≠改预览内存态。**

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/well-tops-editor -b goal/well-tops-editor-20261004 origin/master
cd .worktrees/well-tops-editor
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

1. **分层表编辑器**：按井打开分层表——行级 CRUD（层名/顶深/底深/备注）、插入/删除/排序、单元格校验（顶<底、深度域合法、层名非空）；脏行高亮+保存原子提交。
2. **批量修正**：跨井批量操作——层名重命名/统一、按新基准整体位移、按层删除；差异计算/校验可下 worker，**落库提交必走 GUI 线程单事务**（produce-then-commit，与 `FolderImportWorkflow` 同口径，禁 catalog 跨线程写）。
3. **校验器**：与格架单元归属一致性（有方向 28 格架则联动校验，无则跳过）、叠置/空洞/倒置检测、与井轨迹 MD/TVD 域冲突检测——逐条定位到行。
4. **版本化与回滚**：编辑产生新版本（语义等价 catalog DERIVED），差异摘要（增删改行数）入版本元数据；回滚=新版本指向旧内容。
5. **导入融合**：再导入同井分层时走「差异对比+选择合并」而非覆盖——逐冲突行给出保留旧值/用新值选项。
6. **下游失效**：编辑提交后触发消费面失效标记（wellsection/格架视图/单因素井控）——复用 `markInputsStale`/版本 bump 机制，不直接刷 UI。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；编辑模型归数据/功能层，表格视图归 `src/ui`。
- **诚实面**：校验失败逐条行级定位，不汇总模糊报错；合并冲突不默认覆盖。
- **资源**：构建/测试一律 `-j8`。
- **vendor**：改 vendor 件登记 `PATCHES.md`。
- **UI**：对照 `DESIGN.md`；i18n 过两门。
- **性能断言**：禁绝对毫秒墙钟；万行级表编辑用比率门。
- **ledger**：`.goal-loop-ledger-well-tops-editor.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/版本管线/校验完备/下游失效链/合并诚实面/i18n 六维）→ 修复 → 再 review，**至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 编辑 round-trip：改分层→保存→重开逐字段一致，catalog 新版本/SHA 正常。
2. 校验召回：人工植入叠置/空洞/倒置/悬空层名各一，全部行级检出。
3. 导入合并：同井二次导入 → 冲突行逐条可分别取舍，合并结果符合预期。
4. 下游失效：编辑提交后消费面出现失效标记（版本 bump 断言）。
5. 全量绿：新用例 + `tst_constraintstore`、`tst_datapreview`、`tst_wellsection*`；`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（`## Summary`/`#### Test plan`/自审结论清单/遗留项）。**不合并、不推 master、不等 CI。**
