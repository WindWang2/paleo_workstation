# Goal-Loop 方向 40：断面 Y 型/分叉成面与自动生长

## 背景（实测事实，勿再勘察）

成面核只接受沿走向单调的单支断棒条带（`src/algorithms/faultsurface/
faultsurface.{h,cpp}`）——同剖面 trace 与纵向同时重叠判分叉并拒绝、
走向转折 >75° 拒绝、单剖面断层不外推、不从断层多边形反插。
**分叉断层目前只有棒没有面**。

- Y 型/多分支是另一套拓扑：主干条带 + 分支条带的共棱/分叉点结构，
  非条带三角剖分平移——先勘察核内条带→mesh 的中间结构再定数据模型。
- 自动生长（trace→面边界外推）：单剖面断层向上下外推需边界生长
  规则（外推距离/终止条件/不确定性标注）——勘察核内现有条带端点
  结构可挂点。
- SurveyFrame/断层投绘（方向 26/33 已落：faultProjection 断面 mesh ∩
  井径 curtain、wellsection 断层投绘）是消费面——分支/外推结果须
  在投绘路径如实呈现，断块语义不丢。
- 失败语义：分叉不支持的分支拓扑如实拒绝报因（同现行「重叠判分叉
  拒绝」口径），不画假面。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/fault-branch -b goal/fault-branch-20261004 origin/master
cd .worktrees/fault-branch
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **分叉拓扑模型**：主干+分支的共棱结构（勘察核内条带中间结构定
   数据模型）；Y 型交接处几何缝合规则（分叉点容差/棱共享）写死
   口径进注释。
2. **分叉成面**：双支/多支条带成面——各支独立三角剖分 + 共棱缝合；
   不支持的分支形态如实拒绝列原因。
3. **边界自动生长**：单剖面/端部条带的有限外推——外推步长/总距/
   终止条件参数化；外推区段 mesh 可辨（额外字段或分组），不混充
   实测段。
4. **消费面一致**：断面投绘（wellsection faultProjection）、3D 显示
   对新 mesh 结构如实呈现；分叉点/外推段视觉可辨（样式不混充）。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；拓扑/成面/外推归
  algorithms，投绘消费归 qgis/workflow，视图只发信号。
- **诚实面**：不支持的分支形态/外推无数据基础如实拒绝；外推段
  与实测段可辨（字段或样式）。
- **资源**：构建/测试一律 `-j8`。
- **性能断言**：禁绝对毫秒墙钟；成面用合成条带 + 比率门。
- **几何正确性**：分叉缝合处无自交/洞/重复顶点；外推边界连续。
- **ledger**：`.goal-loop-ledger-fault-branch.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/几何合法/
  拒绝语义/外推可辨/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. Y 型合成条带：主+一支分叉断面成面，共棱缝合处无洞/自交，
   分叉点在容差内对齐。
2. 拒绝语义：不支持的三分支/环状分叉如实拒绝并列出具体形态原因。
3. 自动生长：单剖面断层两端外推 N 步，外推段 mesh 带可辨标记，
   与实测段边界连续。
4. 投绘一致：分叉断面的 wellsection faultProjection 各支分别出
   trace，井径 curtain 无缺失段。
5. 75° 转折边界：转折恰在阈值两侧的条带行为确定性（一侧成面/
   一侧如实拒绝），不抖动。
