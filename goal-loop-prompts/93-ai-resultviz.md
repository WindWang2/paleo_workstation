# Goal-Loop 方向 93：AI 工具结果结构化呈现——血缘图/资产表进对话卡片

## 背景（实测事实，勿再勘察；行号为 2026-10-09 master `32851a1e` 口径）

方向 77（#306）交付 query_project/asset_lineage 只读工具后，
AI 助手能「答工程问题」，但**工具结果只是 JSON 文本回灌**：

- `toolFinished` 只把 JSON 塞进下一轮请求 + controller 信号
  （`src/workflow/aichatcontroller.cpp:69-77`）——query_project
  的实体清单、asset_lineage 的血缘闭包在对话里没有结构化
  呈现（表格/图），用户体感最直接的缺口。
- 血缘出参 schema 已备（`domaintools.cpp:207-228`——
  nodes/edges/selection{count,version_ids}）——数据形态
  天然适合图呈现。
- 现成渲染资产：derivationgraph 页面（方向 36 血缘图谱
  可视化，`src/ui/pages/derivationgraph.cpp`）——本方向
  复用其渲染而非重造。
- 工具卡占位已有（`src/ui/ai/aiassistdock.cpp` 工具卡片区，
  方向 51 立的骨架）——缺的是「结果结构化视图」而非
  「卡片存在」。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\ai-resultviz -b goal/ai-resultviz-20261010 origin/master
cd .worktrees\ai-resultviz
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。

## 目标形态（建议按序）

1. **结果视图类型分派**：按工具/topic 分派结构化视图——
   query_project(wells/horizons/assets)→紧凑表格卡；
   asset_lineage→血缘闭包小图；well_details→键值对卡；
   兜底=现状 JSON 折叠块。
2. **血缘小图**：nodes/edges → 轻量图呈现（复用
   derivationgraph 渲染核或 QGraphicsScene 简化版——
   R0 勘察其可复用度后定案；不做全功能图谱，闭包 ≤N 节点
   的快照图 + 节点悬停版本信息）；「在血缘页打开」按钮
   跳转 derivationgraph 页（全功能交互）。
3. **表格卡**：实体清单 → 可复制表格（井名/层位/资产类型/
   版本）；行点击 → 数据页定位（先例：assetActivated 通道）。
4. **交互红线**：视图只读——卡片内零写操作（AI 工具只读
   纪律延续）；「跳转定位」是导航不是数据修改。
5. **测试**：视图分派单测（工具→视图类型映射）；血缘小图
   节点/边数与出参一致（断言）；表格卡复制到剪贴板；
   兜底折叠块；offscreen 截图入 ledger。
6. **文档**：DESIGN.md 补「AI 结果卡片」节（若未有）——
   token/交互口径。

## 通用纪律（方向内全程有效）

- **分层**：视图归 src/ui/ai（消费 derivationgraph 渲染核
  属视图层内部复用，合法）；编排信号归 workflow（既有
  toolFinished 链不动）；`check_layering.py --strict` 绿。
- **行为红线**：工具执行/回灌/预算语义零变更（纯呈现层）；
  JSON 文本回灌保留（供模型下一轮）——结构化视图是**额外**
  用户呈现，不替换协议面。
- **DESIGN.md**：卡片视觉先提案 token 再实现。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行。
- **无人值守**：复用度与视图形态自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-ai-resultviz.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/只读红线/协议面零变更/DESIGN 一致/i18n 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 分派：工具→视图类型映射单测绿；未知工具兜底折叠块。
2. 血缘小图：节点/边数与出参一致（合成闭包断言）；
   「在血缘页打开」跳转正确（导航信号断言）。
3. 表格卡：内容与出参一致；复制到剪贴板（QClipboard 测试）；
   行点击定位（导航断言）。
4. 只读：卡片内零写路径（rg + 测试——catalog 快照前后一致）。
5. 协议零变更：aichattoolrunner/aichatcontroller 的执行与
   回灌测试零改动通过；全量 ctest 对照 R0 红集合 diff 为空。
6. check_i18n 绿；layering 三档绿；DESIGN.md 节与实现一致。
