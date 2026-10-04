# Goal-Loop 方向 27：沉积相自动编图辅助链——优势相→相界→合成→QA

## 背景（实测事实，勿再勘察）

产品核心是「辅助编图」而非替代人工。现状锚点：远程井道相预测（`src/ai`，
`PALEO_WELL_FACIES_URL`/`_API_KEY` env 可配）、单因素链 `sf::`（surfer_idw/
structural_idw 与上游位级对拍过）、`FactorContourService::generateStructuralContours`
（field_contours 语义，禁降 GDAL）、约束线体系、catalog 版本管线、GEOS 可用。
**全部产出走 catalog 版本 + `declare/instantiate` 正常图层管线，不产游离图层；
算法一律落 `src/algorithms` 数据层，禁 QtWidgets。**

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/facies-automapping -b goal/facies-automapping-20261004 origin/master
cd .worktrees/facies-automapping
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

1. **优势相分析**（`src/algorithms` 纯计算）：井点相代码 → 层位×井优势相/频率统计表 + 覆盖率指标；产出可挂属性表的结果资产。
2. **候选相界提取**（`src/algorithms`+GEOS）：单因素等值线（field_contours 语义）× 约束线求交/裁剪 → 候选相边界多边形，带来源证据标记（哪条等值线/约束贡献）。
3. **证据合成面板**（`src/workflow`+`src/ui`）：多来源（井相/单因素图/预测相/专家约束线）权重配置 → 合成候选编图单元；权重持久化进 job params。
4. **编图一致性 QA**（`src/algorithms` 检测器 + `src/ui` 报告面板）：边界不闭合/重叠/孤岛小面（阈值可配）/与约束线交叉冲突/缺井覆盖区——逐条可定位到要素。
5. **快速成图向导**：层位 → 选输入 → 一键生成草稿相图图层（正常 declare 进层位组，可继续人工编辑）；动作走 JobRunner 三段式。
6. **版本对比视图**：两版相图叠加差异高亮（新增/删除/变形分类着色），报告可导出。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；算法层零 UI 依赖是重点审计项。
- **诚实语义**：证据不足时如实标置信度/缺省，**不造确定性**；QA 报告可复现。
- **GEOS 资源**：句柄释放是重灾区，自审必查。
- **资源**：构建/测试一律 `-j8`。
- **vendor**：改 vendor 件登记 `PATCHES.md`。
- **UI**：对照 `DESIGN.md`；i18n 过两门。
- **性能断言**：禁绝对毫秒墙钟——比率门或 `PALEO_REAL_PROJECT_AREA` 类 env 门控。
- **ledger**：`.goal-loop-ledger-facies-automapping.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/诚实语义/版本管线/GEOS 释放/i18n/DESIGN.md 六维）→ 修复 → 再 review，**至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 优势相统计：合成井相输入 → 频率/覆盖率逐格断言。
2. 相界提取几何正确性：合成等值线×约束 → 边界多边形面积/顶点断言 + 证据标记完整。
3. QA 检测器召回：人工构造不闭合/重叠/孤岛缺陷各一，全部检出且可定位。
4. 草稿相图资产 catalog 版本/SHA 正常，经 organizer 落正确层位组。
5. 全量绿：新算法用例 + `tst_factorworkflow`、`tst_workflows`、`tst_wellfacies`、`tst_constraintstore`；`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（`## Summary`/`#### Test plan`/自审结论清单/遗留项）。**不合并、不推 master、不等 CI。**
