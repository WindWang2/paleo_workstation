# Goal-Loop 方向 23：单因素语义补完 + 约束线交互绘制（上游全量移植收官）

## 背景（实测事实，勿再勘察）

参考工程 `WWX9/haiyou-visualization@27fdb99`，本地克隆 `/home/kevin/projects/_refs/haiyou-visualization`，模块 `Drawing/drawing/single_factor/`（18.3k 行 Python）。#109/#110 已移植内核子集（localidw/localdirectionalgorithm/curvekernel/support/types + 等值线 + 制图工作场 + 井群权重），但**语义面没移全**。本方向把剩余成建制模块补齐，并把约束线交互绘制做全。

### A. 未移植的内核语义（按优先级）

| 上游文件 | 职责 | 现状 |
|---|---|---|
| `direction_corridor.py` (1192 行) | Surfer 式方向走廊：折线 (s,n) 曲线坐标、core/influence 双半径衰减、**多方向线分区独立控制**、邻域椭圆拉伸 | 无 corridor 代码——现有 DirectionGuide 语义不含走廊拉伸 |
| `partition.py` (495 行) | 解释分区模式：屏障自由端延到域边界 + flood-fill，两侧**不共享井**（比硬屏障更强） | 无——只有 `grid_connectivity_v1` 硬屏障 |
| `continuous_metric.py` FaultPathMetric 段 + `surfer_idw.py` (244 行) | 有限断层绕行**加长距离而非丢弃样本**；标准全局 IDW + 各向异性 + 断层路径 | 硬屏障直接丢弃绕行样本；默认仍是 `paleo_constraint_idw` |
| `regional_contours.py` (53 行) | 分区独立井控等值线 | 无 |
| `buffer_transition.py` (58 行) | 显式低值缓冲 + 有限宽平滑过渡 | `bufferHalfWidth`/`displayBuffer`/`cartographicBuffer` 字段在 `types.h` 已声明，过渡实现未落 |
| `contour_avoidance.py` (361 行) | 制图绕行：等值线绕断线缓冲的几何处理（改线不改值） | `Semantic::CartographicDetour` 枚举已落地消费，几何绕行实现待核（勘察轮第一件事） |

`fast_grid.py`/`methods/gridded.py`/`constrained_engine.py` 历史兼容段/`workflow.py` UI 组装段**不移**（NumPy/CUDA 不适用，历史包袱不背）。

### B. 约束线交互绘制（半套已有）

- **已有**：`maptools/PaleoDrawConstraintTool`（QgsMapToolCapture 线捕获）、`TypedConstraintDrawController`（约束页**仅两入口**：方向线/打断线）、CAD dock 共享、编辑框架（`editingtools`/`vertexeditortools`/`editingundostack`）、`ConstraintDrawController`（§42 通用约束）。
- **缺口**：`Semantic` 五值里只两值可画；已画约束线无顶点编辑/删除/语义切换面；无吸附（断层/井位/层位边界）；绘制完成→入库→参与计算的反馈链不完整。

## 目标形态

### 内核补完（`src/algorithms/singlefactor/` 加件）

1. **`corridor.cpp/h`**：移 `direction_corridor.py`——(s,n) 曲线坐标变换、core/influence 双半径衰减场、多方向线分区归属（网格单元归属最近方向线）、搜索邻域按局部轴拉伸成椭圆。
2. **`partition.cpp/h`**：移 `partition.py`——屏障自由端延到域边界（延长线几何）+ node-safe flood-fill 分区；分区结果进 `ResolvedParameters`/`BarrierGrid`。
3. **`faultpath.cpp/h`**：移 `continuous_metric.py` 的 FaultPathMetric 段 + `surfer_idw.py` 的可见性/绕行逻辑——图上最短路（绕过障碍的测地距离）替代「不可见即丢弃」；`method=surfer_idw` 新方法枚举。
4. **`regionalcontours.cpp/h`**：分区独立等值线（分区编号→各区独立井控等值线组）。
5. **`buffertransition.cpp/h`**：显式低值缓冲 + 有限宽平滑过渡（值域过渡非几何过渡，注释口径写死「不推断任意断线是零厚度」）。
6. **`contour_avoidance`** 勘察轮核实 `CartographicDetour` 现状几何实现深度，缺则补。

### 交互绘制补全（`src/ui/`）

7. **约束页绘制入口五全**：硬屏障/方向线/解释边界/停线/制图绕行五个可画类型（`TypedConstraintDrawController` 扩 semantic 参数）。
8. **编辑面**：已绘约束线顶点编辑（`vertexeditortools` 复用）+ 删除 + 语义切换（右键或属性面板改 type）+ 撤销/重做走 `editingundostack`。
9. **吸附**：绘制时吸附断层棒/井位/已有约束线端点（QGIS snapping 或自研就近吸附，勘察轮定）。
10. **反馈链**：绘制入库 → 约束页列表刷新 → 参与下次单因素计算的语义确认（类型词表落 `ConstraintStore` type 列）。

## Oracle 验收（全部须实测通过并记账本）

1. **走廊**：合成弯曲方向线 → (s,n) 坐标下直线化断言；core 内全拉伸、influence 外零影响、过渡带单调衰减断言；多方向线分区归属正确。
2. **分区**：合成 L 形屏障 → 两侧分区井数断言（不共享）；自由端延长到边界断言；分区 id 稳定。
3. **FaultPathMetric**：直墙断层两侧绕行距离 > 直线距离（差值 ≈ 端点绕行长度）；无障碍 = 欧氏；**与硬屏障对照断言**（同输入旧路径丢弃、新路径保留）。
4. **surfer_idw**：跨断层网格点估值用绕行距离的样本参与断言；`method=surfer_idw` 入方法面。
5. **分区等值线**：两分区各自等值线组独立、不跨区断言。
6. **缓冲过渡**：过渡带值单调平滑、带宽外不受影响断言；非零值不被误判为零厚度断言。
7. **交互绘制**：五类型可画断言（`onDrawn` 确定性驱动先例）；顶点编辑/删除/撤销重做断言；入库后 `ConstraintStore` 读回 type 正确。
8. **对拍**：新增点查询进 `kernel_parity.json` 夹具族（corridor/partition/faultpath 各若干点，容差沿用 1e-9×S 口径）。
9. ledger 全账 + `docs/progress/singlefactor-completion.md`（移植映射表续 `#109` 文档、语义差异清单、交互面说明、仍递延项）。

## 勘察指引

- 上游：`/home/kevin/projects/_refs/haiyou-visualization/Drawing/drawing/single_factor/` 逐文件读（`direction_corridor.py`/`partition.py`/`surfer_idw.py`/`continuous_metric.py`/`regional_contours.py`/`buffer_transition.py`/`contour_avoidance.py`）
- 本侧内核：`src/algorithms/singlefactor/`（types.h 语义枚举、curvekernel、localidw、support——新模块照其形态）
- 交互：`src/ui/typedconstraintdrawcontroller.*`、`src/ui/maptools/paleomaptools.*`、`src/ui/edittools/`（vertex/undo 框架）、`src/ui/pages/constraintpage.*`、`io/constraintstore.*`（type 列词表）
- 编排：`src/workflow/workflows.cpp` 三段式接新 method 枚举；`tests/fixtures/singlefactor/` 对拍夹具格式
- `docs/progress/single-factor-native.md`（#109 移植口径——本文档是它的续篇）

## 禁区

- **不动已移植内核的语义**：localidw/curvekernel/硬屏障行为不变——新语义走新枚举/新 method 值。
- 不移 `fast_grid`/`methods`/`workflow.py` UI 段/`constrained_engine.py` 历史段；不背 Python 包袱。
- FaultPathMetric 与方向 18 去重：**本方向是唯一落点**（18 已删该项）；若 18 先行落地也不许改它的文件，语义名冲突按上游 `FaultPathMetric` 为准。
- 不碰 `src/catalog`/`welllogset`；与方向 20（workflows.cpp 拆分）有文件交集——勘察轮确认其收官状态，在飞则不动 `workflows.cpp` 结构、只加 method 分支。
- 交互绘制不引入新 map tool 框架——复用 `QgsMapToolCapture`/`editingundostack` 现状。
- 语义词表禁止再造新词——五种 `Semantic` 枚举已冻结，交互入口按枚举映射。

## 迭代协议

- **轮0**：勘察定案——`contour_avoidance` 现状深度、`Semantic`↔type 词表映射、CAD dock 共享点、吸附实现选型（QGIS snapping vs 自研）；各模块 API 签名进 ledger。
- **轮1**：corridor + `tst_singlefactor_corridor`。
- **轮2**：partition + `tst_singlefactor_partition`。
- **轮3**：faultpath + surfer_idw method + `tst_singlefactor_faultpath`/`surfer`。
- **轮4**：regional_contours + buffer_transition + contour_avoidance 补齐 + 各测试。
- **轮5**：约束页五入口绘制 + 顶点编辑/删除/语义切换/撤销重做 + `tst_constraint_draw` 扩面。
- **轮6**：吸附 + 对拍夹具扩 + 真工区 + progress 收口。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/sf-completion -b goal/singlefactor-completion-<date> origin/master`
2. 每轮原子提交，ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**：`git diff origin/master...HEAD` 全量自审；无调试残留；层标记齐；`check_layering --strict` 绿；vendor 前缀全量构建零新警告；ctest 全绿；Oracle 每条有命令+输出证据。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
