# Goal-Loop 方向 26：连井剖面深化——编辑交互、基准面与栅状图

## 背景（实测事实，勿再勘察）

连井剖面刚并入（`56d7780`）：`src/ui/wellsection/`（domain 数据核 + workflow 编排 + 图件面板），`tst_wellsection`/`_workflow`/`_ui` 三测全绿。本方向**在其上深化，不推翻现有架构**——剖面编辑产物走 catalog/manifest 版本，禁只落内存。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/wellsection-deep -b goal/wellsection-deep-20261004 origin/master
cd .worktrees/wellsection-deep
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

1. **基准面控制**：海拔/深度/拉平三模式；任一标志层作拉平基准（其余层按相对高程重排）；模式切换保数据不变形。
2. **剖面编辑**：井列拖拽重排、增/删井、井距按比例-等距切换；层位连线交互——拾取两井同名层位连线、断开重连；断层投绘开关。
3. **曲线与充填**：测井曲线道叠加（GR/DT/AC 可配道序）、相代码/岩性充填段；道宽/颜色按 `PaleoTheme`。
4. **栅状图（fence）**：多剖面井网自动布点（最小交叉启发式 + 手工指定）；交点井跨剖面共享联动（改一处全刷新）。
5. **剖面-平面联动**：剖面线位主地图高亮；平面/图层树选井列一键生成剖面；点剖面井反向定位闪烁。
6. **导出**：剖面 SVG/PNG + 层位井深表 CSV（含基准面模式标记）。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；domain 数据核**禁 QtWidgets 渲染依赖**，渲染归 `src/qgis`/`src/ui` 面。
- **资源**：构建/测试一律 `-j8`。
- **vendor**：改 vendor 件登记 `PATCHES.md`。
- **UI**：对照 `DESIGN.md`；i18n 过两门。
- **性能断言**：禁绝对毫秒墙钟。
- **ledger**：`.goal-loop-ledger-wellsection-deep.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/渲染正确性/版本一致性/联动一致性/DESIGN.md/i18n 六维）→ 修复 → 再 review，**至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 拉平不变量：拉平模式切换前后井深表数值一致，仅视图基准变化。
2. 井序/井列编辑 round-trip 写回 manifest 且版本号正确推进。
3. 栅状交点共享：交点井在一剖面改动后其余剖面同帧刷新。
4. 全量绿：三 `tst_wellsection*` 原样 + 新用例（拉平不变量/井序 round-trip/交点共享）；`tst_ui`、`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（`## Summary`/`#### Test plan`/自审结论清单/遗留项）。**不合并、不推 master、不等 CI。**
