# Goal-Loop 方向 28：层序地层格架工作台——格架树、标志层与编图单元校验

## 背景（实测事实，勿再勘察）

「格架先行」是编图正确性的前提。现状锚点：catalog 已有 `sequence_boundary`
实体类型与 `well_stratification` 资产类型（`datacatalog` 角色词表负责校验，
测试输出里的「角色与实体类型不符」警告即此机制）；`mappingHorizons()` 提供
层位序；约束声明走 `03_Constraints`。约束：**`sequence_boundary` 既有语义不得
破坏**；格架数据走 catalog 实体+版本，禁止旁路存储。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/sequence-framework -b goal/sequence-framework-20261004 origin/master
cd .worktrees/sequence-framework
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

1. **格架树管理**（`src/catalog` 模型 + `src/ui` 面板）：层序→体系域二级格架 CRUD/排序/重命名；与 `mappingHorizons()` 映射关系显式建模。
2. **标志层管理**：标志层 CRUD、绑定所属格架单元；与 `well_stratification` 分层名做引用完整性校验（悬空引用报警）。
3. **井间格架建议**（`src/algorithms`）：按各井分层顶深/厚度最近邻一致性半自动建议井间标志层归属——**建议只出候选，人工确认后才写库**。
4. **编图单元校验**：horizon ↔ 格架单元映射检查——无归属 horizon、格架单元无编图数据、一 horizon 多单元歧义，出诊断报告面板。
5. **格架柱状视图**（`src/ui`）：单元层级色带柱状图（厚度/层级按格架树）；与剖面/平面联动高亮当前单元。
6. **一致性诊断**：孤立井分层（无格架归属）、格架空洞（单元全工区无井覆盖）、跨单元重名层——可复现诊断报告 + 导出。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；格架模型属 `src/catalog`/`src/domain`，视图只做渲染。
- **兼容**：catalog 角色词表新增类型先登记，不得让既有 `sequence_boundary`/`well_stratification` 断言误报。
- **资源**：构建/测试一律 `-j8`。
- **vendor**：改 vendor 件登记 `PATCHES.md`。
- **UI**：对照 `DESIGN.md`；i18n 过两门。
- **性能断言**：禁绝对毫秒墙钟。
- **ledger**：`.goal-loop-ledger-sequence-framework.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/角色词表兼容/实体-资产引用完整性/版本管线/i18n/DESIGN.md 六维）→ 修复 → 再 review，**至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 格架 CRUD round-trip：新建-保存-重开逐字段一致；与 `mappingHorizons()` 映射可双向解析。
2. 归属校验：人工构造悬空分层/格架空洞/重名层各一，诊断器全检出且可定位。
3. 建议面只出候选：未确认前 catalog 零写入断言。
4. 全量绿：新用例 + `tst_constraintstore`、`tst_arearules`、`tst_datapreview`、`tst_workflows`；`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（`## Summary`/`#### Test plan`/自审结论清单/遗留项）。**不合并、不推 master、不等 CI。**
