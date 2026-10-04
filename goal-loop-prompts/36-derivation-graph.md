# Goal-Loop 方向 36：版本衍生血缘图谱——parentVersionIds 知识图谱可视化

## 背景（实测事实，勿再勘察）

血缘数据已闭环，缺的是图：

- `CatalogVersion::parentVersionIds`（`src/catalog/datacatalog.h:82`）——
  DERIVED 版本 → 源版本 id 列表；注册面 `DerivedAssetRegistrar::commit`
  （`src/workflow/derivedassets.cpp`，外部产物走 `commitExternal`）。
- **反查闭包已就绪，本方向无需新增数据接口**：
  `DataCatalog::downstreamClosure(versionId)`（BFS、环安全、不含种子）、
  `markDownstreamStale`（上游字节变 ⇒ 下游记 `extra["stale"]`/`staleReason`）。
  上游=逐版本读 parentVersionIds 展开，下游=downstreamClosure，两向都能取。
- 图谱视觉先例：`TopologyGraph`（`src/ui/pages/dataopspanelextra.h:354`，
  QGraphicsView 二部图：椭圆节点+连线+`nodeClicked` 定位信号）、
  `VersionTimeline`（entitypanel D4.3 版本时间线段）、
  `CollapsibleSection` 折叠段（entitypanel.cpp:359/380 两处装配先例）。
  衍生图是同族部件——布局、信号命名、主题 token（PaleoTheme）口径对齐，
  **不引第三方图布局库**。
- 挂点候选（勘察后二选一，ledger 记取舍）：实体页新增「衍生血缘」
  CollapsibleSection（与「关联拓扑」「版本时间线」并列）；或数据页
  右侧栏。stale 高亮与方向 30 体检面板语义互通。

**一切读数经 DataCatalog API；视图层只画不查库（经 services/workflow 层
转发数据）。**

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/derivation-graph -b goal/derivation-graph-20261004 origin/master
cd .worktrees/derivation-graph
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **谱系图核心 `DerivationGraph`**（QGraphicsView，视图层新部件）：
   - 节点=版本：形态区分 kind（RAW 方形 / DERIVED 圆形 / external 菱形），
     `extra["stale"]==true` → 警示色描边 + tooltip 展示 staleReason；
     标签 = 版本短名 + 所属资产名，超长截断。
   - 有向边=衍生关系：箭头方向取一并在注释里写死口径（建议 parent→child，
     「源流向下游」）；环安全（数据侧已环安全，渲染侧也要防重复入图）。
   - 选中态：点击节点高亮其祖先+后代两向闭包子图，其余降透明度。
   - 规模防御：深度滑块（默认上游 2 层/下游 2 层）+ 节点数超阈值时
     折叠并给「已折叠 N 个上游/下游节点」明示——禁无限展开卡 UI。
2. **挂点与联动**：实体页「衍生血缘」折叠段或数据页侧栏（二选一）；
   `nodeClicked` → 版本定位（复用 `absolutePathForVersion`/预览链路，
   只发信号不直查库）；当前选中资产/版本变化 → 子图跟随重建。
3. **检索过滤**：按实体/资产/kind 过滤；`staleOnly` 开关只显过期子图
   （与方向 30 体检联动语义：图中能定位体检列报的 stale 版本）。
4. **空态与退化**：无衍生关系 → 空态文案（「该版本无衍生记录」），
   不空白屏；孤立节点群如实平铺；无 catalog/未开工程 → 禁用态说明。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；图部件归视图层，
  闭包取数走 catalog API（数据层已有接口，勿在 UI 里直拼 BFS）。
- **诚实面**：取不到/无数据如实空态，不猜不补。
- **资源**：构建/测试一律 `-j8`。
- **UI**：对照 `DESIGN.md`；节点配色/描边走 PaleoTheme token；
  i18n 过 `check_i18n.py` 两门。
- **性能断言**：禁绝对毫秒墙钟；大图用节点数阈值折叠 + 抽样式计时。
- **ledger**：`.goal-loop-ledger-derivation-graph.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/旁路读数/
  内存所有权/环安全/空态/i18n 六维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 三层链 RAW→D1→D2：图中呈链式三节点，父子方向正确，点击节点定位到
   正确版本（预览链路生效）。
2. 一父多子 + 一子多父（D61/D62→厚度栅格类先例）：分叉与合流形态正确，
   共享父节点不重复绘制。
3. stale 高亮：`markDownstreamStale` 标过的版本节点警示描边 +
   staleReason tooltip 与体检口径一致。
4. 大图防御：构造 ≥200 版本工程，默认深度折叠下渲染不卡、
   「已折叠 N」计数如实，逐层展开可用。
5. 环安全：人工构造 parentVersionIds 环 → 不栈溢、不重复节点、
   环成员如实显示。
6. 空态：无 parentVersionIds 的版本 → 「无衍生记录」明示文案，
   不出现空白画布。
