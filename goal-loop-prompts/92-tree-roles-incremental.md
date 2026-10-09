# Goal-Loop 方向 92：数据树角色词表单源化 + 树增量通道——一批小手术收两个结构债

## 背景（实测事实，勿再勘察；行号为 2026-10-09 master `32851a1e` 口径）

四轮盘点的两个「一天级改动、一直没立项」项合并收口：

1. **角色词表两处私榜，权威已经存在**（不要再造第三份）：
   - 树角色序是 ui 局部 lambda（`src/ui/pages/
     datalist_tree.cpp` 的 `roleOrder`：well_log=0/tops=1/
     time_depth=2/well_head=3，其余落默认桶；
     cuttings/lab_analysis/core 不在这张序里）；
   - AI 工具描述里另一份手写串
     （`src/ai/chat/domaintools.cpp` 的 role 参数说明：
     `well_head/well_log/tops/time_depth/horizon/
     seismic_volume/reference 等`）；
   - `datacatalog.h` 里 `EntityAssetLink.role` 旁边的角色
     列举只是注释，**不是权威**。
   - **唯一权威是** `src/catalog/roleregistry.h` 的
     `RoleRegistry`：内置表序、`RoleDef.display` 中文名、
     `entityTypes`，以及工程覆盖 `RoleRegistry::fromJson`
     （`project_area.json` 的 roles 节；无匹配定义则追加为
     自定义角色，表序尾部）。`DataCatalog::roleRegistry()`
     已暴露它（`open()` 时装载）。**禁止发明平行的
     `catalogRoles()` 常量。**
2. **树零增量通道**：`datalist_tree.cpp` 全文件 dataChanged
   零命中——`refreshAssetTree`（`:50`）全量重建模式；
   方向 52 交付的 dataChanged 增量通道只接了 FlatAssetModel；
   #270 修展开态保留正是全量重建之痛的补偿——树的增量
   通道一直空着。

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

1. **消费并按需扩展 RoleRegistry（禁止 `catalogRoles()`）**：
   树排序与 AI 描述都读当前工程的
   `DataCatalog::roleRegistry()`（同一 `RoleRegistry` 实例），
   **包含** `fromJson` 追加的工程自定义角色（显示名、表序、
   挂接实体类型都走覆盖，不在 ui/ai 再抄一份静态表）。
   若树需要的分组键是 `RoleDef` 还没有的（例如
   well/seismic/mapproduct/misc），扩展 `RoleDef` /
   `RoleRegistry` 与工程覆盖 schema，让自定义角色走同一条
   `fromJson` 路径。不要把 `datacatalog.h` 的注释升格成
   第二份词表。
2. **树改查表**：`roleOrder` lambda 改为按 `roleRegistry()`
   的表序（及扩展后的分组键）排序；工程自定义角色按词表落组；
   词表外（`isKnown` 为假，且不是本工程追加定义）落「未分组」
   可见桶（不静默藏匿）；综合柱状图启发式识别保持双轨
   （类型字段优先 + displayName 兜底——若类型字段已存在则
   直接切类型）。
3. **AI 描述动态化**：`domaintools.cpp` 手写角色串改为从
   当前工程的 `roleRegistry()` 生成（角色名、显示名、自定义
   角色都在）；出参 schema 不变（tst_aichatprojectquery
   零改动通过为验收线）。
4. **树增量通道**：`refreshAssetTree` 增 diff 能力——资产级
   增/删/改 → 节点级更新（QTreeWidgetItem 增删或模型化，
   按 R0 勘察择一）；全量重建保留为兜底（变更数超阈值 or
   结构性重排）；选中/展开态语义不变（#270 语义对拍）。
5. **测试**：词表单测（新角色落组/序稳定/未分组桶）；树
   增量三类更新（增/删/改）+ 兜底切换阈值；#270 展开态
   回归零改动；AI 出参对拍（改前/改后描述语义一致）。
6. **文档**：`RoleRegistry` 契约注释写明「新增角色走词表 /
   工程覆盖，禁止平行常量表」；方向 82 已立的 datanav 文档
   增补（树序与 AI 描述消费 `roleRegistry()`，含自定义角色）。

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

1. 单源：树角色序与 AI 描述都走 `RoleRegistry` /
   `DataCatalog::roleRegistry()`；rg 确认没有新建
   `catalogRoles()`；旧 `roleOrder` lambda 与 domaintools
   手写角色串删除。
2. 自定义角色：`project_area.json` roles 追加的角色出现在
   树分组与 AI 描述中（显示名/序与覆盖一致）；词表外角色
   落未分组可见桶。
3. 树增量：三类更新 QSignalSpy/结构断言绿；兜底切换
  （阈值）测试；#270 展开态回归零改动通过。
4. AI 出参：tst_aichatprojectquery 零改动通过（schema 稳定）；
   描述语义改前改后一致（对拍记录）。
5. 全量 ctest 对照 R0 红集合 diff 为空；layering 三档绿；
   check_i18n 绿。
