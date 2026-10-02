# Goal Loop — goal/fault-interpretation：断层解释数据框架与拾取工具（新功能）

你接手一个自治迭代循环。方向：**断层解释的最小可用闭环**——剖面上拾取断层棒
（fault sticks）、层位图上落断层多边形、断层命名与管理、断层-层位切割关系
的数据模型。这是解释工作流的核心骨架之一，新功能、纵深大。

## 背景事实

- 编辑基建已厚：`src/ui/edittools/`（vertexeditortools/editingundostack/
  editingtoolbar）+ `src/qgis/qgiseditingservice.*`——编辑会话/撤销栈/顶点编辑
  先例齐全；`tst_edittools` 92s 用例面是参照。
- 剖面交互：`src/qgis/seismicsectiontool.*`（刚从 linkage 迁入 qgis 层）、
  `src/ui/seismic3d/` 剖面拾取与任意剖面能力已就位。
- 拓扑：`src/qgis/topologicalindex.*`、`distancetransform`、`faciespolygonize`
  空间算法先例；catalog/`layermanifest` 管图层角色。
- `faciespolygonize` spike 证明多边形化管线通；断层多边形可复用其思路。

## 范围（建议子项，按需取舍/排序）

1. **断层数据模型**（数据层）：FaultSet —— 命名断层实体 + 断层棒集合
   （剖面拾取的线段，带 IL/XL/任意剖面定位 + TWT 范围）+ 层位多边形（切割线）。
   落 catalog 角色，可序列化进工程。
2. **剖面断层拾取工具**：剖面视图上的断层棒绘制工具（折线拾取，挂编辑撤销栈），
   属 QGIS 封装/视图边界按层规走（工具挂 qgis 层画布 hook，UI 只发信号）。
3. **层位图断层多边形**：层位平面图上画/编辑断层切割多边形；断层与层位的
   切割关系（上盘/下盘边）建模——至少存关系，求交裁剪可作后续。
4. **管理面板**：断层树/列表（命名、显隐、删、重命名），选中断层在三视图联动
   高亮（复用 linkage selectioncontext）。
5. **测试**：FaultSet 序列化往返、拾取→模型落账、撤销栈语义、断层-层位关系
   一致性断言；编辑工具用例风格对齐 `tst_edittools`。

## Oracle

1. 闭环：剖面拾取断层棒 → FaultSet 入库 → 层位图画多边形 → 面板管理 → 
   保存工程重开完整还原，全链 offscreen 可测。
2. 编辑语义：拾取/删除/改名全部入撤销栈，undo/redo 逐拍断言。
3. 联动：选中断层经 SelectionContext 在三视图高亮（断言信号载荷，不截屏）。
4. 断层-层位切割关系数据模型断言：同一断层对多层位的关系独立存取。
5. 分层绿（`--strict`）；ctest 全绿；ledger；push + `gh pr create`。
6. 边界清晰：本方向不做断层封闭性分析/断距计算——模型留扩展位即可。

## 勘察指引

- `src/qgis/qgiseditingservice.*` + `src/ui/edittools/`：编辑会话与撤销栈形状。
- `src/qgis/seismicsectiontool.*`：剖面画布工具挂点；`src/linkage/selectioncontext.*`
  联动协议；`catalogindex`/`layermanifest` 角色登记先例。
- 序列化：`paleoprojectstore` 写队列/`mapversionstore` 版本化先例——FaultSet
  落盘走既有存储语义，不自开平行存储。

## 禁区

- 不做断层自动识别/蚂蚁追踪等算法方向（手动解释框架优先）。
- 不改 SelectionContext 协议形状，只在其上挂断层类型。
- 不绕写队列直写存储；工具进编辑会话而非旁路改图层。

## 迭代协议

- 轮0-1：勘察编辑/联动/存储接口面 → 数据模型定型入 ledger。
- 中段：FaultSet 核→剖面拾取工具→多边形编辑→管理面板→联动，每件一轮。
- 完成定义 = Oracle 6 条全绿，ledger 轮次齐全。

## 交付协议（必守）

1. **新建 worktree 分支开发**：`git worktree add .worktrees/fault-interp -b goal/fault-interpretation-<日期>`
   从最新 `origin/master` 起，全程不在主 checkout 写代码。
2. **完成后先仔细 review 再开 PR**——逐条执行，全部通过才准推送：
   - `git diff origin/master...HEAD` 自审全部改动：删掉调试残留、printf、注释掉的死代码；
     每个文件头三行层标记在；分层 `tools/check_layering.py --strict` 绿；
   - 全量构建无新警告；ctest 全绿（含新增测试）；
   - Oracle 每条在 ledger 里有对应验证证据（命令+输出摘要），不是「应该没问题」；
   - review 发现的问题先修再验，直到干净为止。
3. 推送 + `gh pr create`（标题 conventional，body 写 `## Summary`/`#### Test plan`/
   自审结论清单）。
