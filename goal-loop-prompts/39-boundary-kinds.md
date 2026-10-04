# Goal-Loop 方向 39：相界地质语义类型——整合接触/尖灭/相变的编辑与图面表达

## 背景（实测事实，勿再勘察）

相界四类词面已冻结（`src/workflow/boundarysemantics.h`）：整合接触、
尖灭、相变、断层切割——**仅 fault_cut 单类型已跑通**（boundary_kind
属性 schema → composepage 下拉 → `QgisStyleService::applyFaciesBoundaryStyle`
断层红粗边）。其余三类缺差异化行为定义与图面。

- 落点现状：kind 已落要素级属性；逐弧段差异化需 boundary-graph 线层
  （TODOS P2 注记，勘察后定深浅）。
- 断层切割先例链：`boundarysemantics.h` 词表 → composepage 下拉编辑 →
  `applyFaciesBoundaryStyle`（`src/qgis/qgisstyleservice.cpp`）分类渲染——
  三类扩展沿同链走，不新开数据通道。
- 编辑语义差异化是核心工作量：不同边界类型的约束编辑行为不同——
  尖灭边界允许单边相带终止（拓扑上不等价于闭合环）、整合接触边界
  不切割两侧相、相变边界允许渐变带（勘察 `constraintworkflow`/
  `typedconstraintdrawcontroller` 的可挂点）。
- 图式：三类边界线型/符号对照地质图惯例（方向 31 词表面已有
  facies_definite/inferred/transitional 线型——勘察可否复用，
  不复用则登记新线图式）。
- 验证面：`faciesqa`/证据合成（方向 27 已落）可按边界类型核查——
  勘察 QA 规则的可挂点，只接不重写。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/boundary-kinds -b goal/boundary-kinds-20261004 origin/master
cd .worktrees/boundary-kinds
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **三类图面**：整合接触/尖灭/相变的线型与符号进样式服务
   （复用方向 31 facies_* 线型或登记新图式）；composepage 下拉按类型
   着色 round-trip（fault_cut 先例全链复刻）。
2. **编辑行为分化**：按边界类型约束编辑——尖灭允许开放端、整合接触
   禁切两侧、相变允许带域；非法编辑如实拒绝报因，不静默修正。
3. **逐弧段（勘察定深浅）**：boundary-graph 线层若工程量可受则立；
   否则保持要素级并 ledger 记递延理由。
4. **QA 挂接**：faciesqa 按边界类型出核查项（如尖灭端点无落位、
   相变带无渐变范围）——只接既有规则框架。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；语义/编辑规则归
  workflow/domain，图式归 qgis 封装，下拉/交互归视图层。
- **诚实面**：类型缺省/未知 kind 如实走中性样式；非法编辑给原因。
- **资源**：构建/测试一律 `-j8`。
- **UI**：对照 `DESIGN.md`；i18n 两门；地质词面用冻结词表原文。
- **性能断言**：禁绝对毫秒墙钟。
- **冲突**：qgisstyleservice.cpp/composepage 为多点挂接热区——
  与他方向撞时按语义并集合。
- **ledger**：`.goal-loop-ledger-boundary-kinds.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/词表一致/
  编辑语义/样式 round-trip/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 三类着色 round-trip：下拉赋类 → 线型/颜色按类型渲染 → 工程存取
   后样式不回退。
2. 编辑语义：尖灭开放端可过、整合接触切两侧被拒（有原因文案）、
   相变带可画渐变域——三场景各有证据。
3. 词表一致：UI/属性/样式三处的四类词面与 boundarysemantics.h
   冻结词面逐字一致。
4. 未知 kind：无 boundary_kind 或表外值的要素走中性样式，不 crash
   不猜类。
5. QA 项：按类型的核查规则至少一条生效（报告有名有因）。
