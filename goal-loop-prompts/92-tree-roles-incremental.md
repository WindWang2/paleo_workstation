# Goal-Loop 方向 92：数据树角色词表单源化 + 树增量通道——一批小手术收两个结构债

## 背景（实测事实，勿再勘察；行号为 2026-10-09 master `32851a1e` 口径）

四轮盘点的两个「一天级改动、一直没立项」项合并收口：

1. **角色词表三处私榜**（单源化缺口）：
   - 树角色序 ui 局部 lambda 硬编码（`src/ui/pages/
     datalist_tree.cpp:140-146`——well_log=0/tops=1/
     time_depth=2/…，cuttings/lab_analysis 落默认桶）；
   - AI 工具描述里的角色清单另一份手写串
    （`src/ai/chat/domaintools.cpp:192-193`）；
   - 权威定义点是 `src/catalog/datacatalog.h:59` 的**注释**。
2. **树零增量通道**：`datalist_tree.cpp` 全文件 dataChanged
   零命中——`refreshAssetTree`（`:50`）全量重建模式；
   方向 52 交付的 dataChanged 增量通道只接了 FlatAssetModel；
   #270 修展开态保留正是全量重建之痛的补偿。方向 90（perf
   round2）未发车，树的增量通道一直空着。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\tree-roles-incremental -b goal/tree-roles-incremental-20261010 origin/master
cd .worktrees\tree-roles-incremental
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。主回归面：tst_assetpaging
（树结构/展开态）+ tst_aichatprojectquery（AI 工具出参）。

## 目标形态（建议按序）

1. **词表升格**：`datacatalog.h:59` 注释升格为 `catalogRoles()`
   常量表（role→{分组, 排序序号, 显示名}——分组含
   well/seismic/mapproduct/misc 族，AI 工具描述与树共用）；
   domain/catalog 层定义（数据层），树与 ai 消费。
2. **树改查表**：`roleOrder` lambda 改查 `catalogRoles()`；
   新角色（如自定义 role）按词表落组，词表外落「未分组」
   可见桶（不静默藏匿）；综合柱状图启发式识别保持双轨
  （类型字段优先 + displayName 兜底——若类型字段已存在则
   直接切类型）。
3. **AI 描述动态化**：`domaintools.cpp:192-193` 手写串改
   从 `catalogRoles()` 生成（分组名与角色清单单源）；出参
   schema 不变（tst_aichatprojectquery 零改动通过为验收线）。
4. **树增量通道**：`refreshAssetTree` 增 diff 能力——资产级
   增/删/改 → 节点级更新（QTreeWidgetItem 增删或模型化，
   按 R0 勘察择一）；全量重建保留为兜底（变更数超阈值 or
   结构性重排）；选中/展开态语义不变（#270 语义对拍）。
5. **测试**：词表单测（新角色落组/序稳定/未分组桶）；树
   增量三类更新（增/删/改）+ 兜底切换阈值；#270 展开态
   回归零改动；AI 出参对拍（改前/改后描述语义一致）。
6. **文档**：`catalogRoles()` 契约注释（含「新增角色先入表」
   纪律）；方向 82 已立的 datanav 文档增补。

## 通用纪律（方向内全程有效）

- **分层**：词表归 catalog/domain（数据层），树/ai 消费；
  `check_layering.py --strict` 绿。
- **行为红线**：默认词表下树结构/顺序对拍零差异（等价
  重构）；AI 出参 schema 零变更；选中/展开态语义逐点不变。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行。
- **无人值守**：增量阈值与兜底口径自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-tree-roles-incremental.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/等价对拍/单源纪律/增量正确性/i18n 五维）→ 修复 →
  再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 单源：树角色序/AI 描述/未来消费全走 `catalogRoles()`
  （rg 三处旧硬编码清零——lambda 删除/手写串删除）。
2. 新角色：词表测试（自定义 role 落组 + 未分组桶可见）。
3. 树增量：三类更新 QSignalSpy/结构断言绿；兜底切换
  （阈值）测试；#270 展开态回归零改动通过。
4. AI 出参：tst_aichatprojectquery 零改动通过（schema 稳定）；
   描述语义改前改后一致（对拍记录）。
5. 全量 ctest 对照 R0 红集合 diff 为空；layering 三档绿；
   check_i18n 绿。
