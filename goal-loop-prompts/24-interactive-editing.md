# Goal-Loop 方向 24：交互式约束编辑与地图数字化工具链

## 背景（实测事实，勿再勘察）

PLAN §39 E2 决策定了 embed 路线，代价是**数字化/编辑能力需自研**。现状锚点：

- 约束语义三件套（type/params/horizon）在 `src/workflow/constraintworkflow.cpp` + `src/workflow/constraintimport.cpp`，声明组 `03_Constraints`（`PaleoLayerVocabulary::kConstraintsGroup` 单常量）。
- 已有绘制件：`maptools/PaleoDrawConstraintTool`（QgsMapToolCapture）、`TypedConstraintDrawController`（类型化绘制入口）、`editingundostack`/`vertexeditortools`/`editingtools` 框架件（方向23 落地过部分编辑能力——勘察轮先盘点复用面再写新的）。
- 约束页 `src/ui/pages/constraintpage.cpp`、预览地图工具 `src/ui/datapreview/previewmaptools.h`。
- **编辑回路必须走 store 持久化**——改内存图层不碰 store = 数据不一致，判废。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/interactive-editing -b goal/interactive-editing-20261004 origin/master
cd .worktrees/interactive-editing
# vendor 三件为 gitignored——须从主仓绝对路径 symlink（../../ 相对路径会自环，勿用）
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序落地，每个可独立原子提交）

1. **顶点编辑器**：约束线/边界多边形节点级编辑——拾取节点、拖动、加点、删点；写回 `QgsVectorLayer` editBuffer **并同步 constraint store**（type/params/horizon 三件套不丢）。
2. **形状数字化工具**：手绘折线/多边形、矩形/圆/椭圆规则形；实时橡皮筋预览；ESC 取消、双击/回车闭合。
3. **捕捉系统**：井点、层位边界、既有约束线顶点吸附；容差可配（px）；捕捉命中可视化反馈。
4. **undo/redo 栈**：复用 `editingundostack`——编辑操作可撤销重做，跨 layer+store 一致回滚，栈深可配。
5. **属性就地编辑**：选中约束改类型（方向线/打断线/软边界等）、方向角、blockMode；多选批量改型。
6. **编辑态 UX**：工具条进/出编辑模式；编辑中要素虚线高亮；退出未保存提示；只读/外链缺失资产禁编。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；新顶层模块先 `scripts/new_module.sh` 登记；`tools/check_layering.py --strict` 必绿。
- **资源**：构建/测试一律 `-j8`，禁 `$(nproc)`。
- **vendor**：改 `vendor/sbm`/`vendor/qgis` 必须登记对应 `PATCHES.md`（P 编号递增）。
- **UI**：视觉决策对照 `DESIGN.md`；i18n 新字符串过 `i18n`/`i18n_selftest`。
- **性能断言**：禁绝对毫秒墙钟——比率门或 env 门控。
- **ledger**：worktree 根建 `.goal-loop-ledger-interactive-editing.md`，每轮记「改动/验证/判定/下一步」。
- **多轮 review（硬要求）**：每批落地 → 构建+受影响测试全绿 → **diff 全量自审**（分层违规 / store 一致性 / 语义漂移 / 资源泄漏 / DESIGN.md / i18n 六维出报告）→ 修复 → 再 review。**至少两轮零 High/Medium 发现**才收口；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀，对齐 git log 风格。

## Oracle 验收（逐条需验证证据）

1. 顶点编辑 round-trip：改点后 store 内坐标/type/params 与图层一致，`tst_constraintstore` 原样绿。
2. undo 一致性：编辑-撤销-重做后 store 与图层状态逐字段一致。
3. 批量改型：多选改类型后语义落库，参与下次单因素计算类型词表正确。
4. 全量绿：`tst_constraintstore`、`tst_layertreepanel`、`tst_ui` + 新编辑用例；`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（标题 conventional，body 含 `## Summary`/`#### Test plan`/自审结论清单/遗留项）。**不合并、不推 master、不等 CI。**
