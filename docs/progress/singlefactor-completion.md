# 单因素语义补完 + 约束线交互绘制收官

状态：方向23 全轮次（轮0 勘察、轮1-4 内核、轮5 交互面、轮6 吸附/对拍/收口）完成，
分支 `goal/singlefactor-completion-20261003`。本文是 `single-factor-native.md`（#109/#110）
的续篇：上游 `haiyou-visualization@27fdb99` 单因素模块剩余成建制语义的移植收官。

## 范围

- 工作目录：`.worktrees/sf-completion`
- 基线：`origin/master 4741115`
- 参考：`WWX9/haiyou-visualization` `27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f`
- 任务书：`goal-loop-prompts/23-singlefactor-completion.md`

## 移植映射表（续 #109）

| 上游文件 | 本侧落点 | 说明 |
|---|---|---|
| `direction_corridor.py` | `src/algorithms/singlefactor/corridor.{h,cpp}` | (s,n) 曲线坐标、core/influence 双半径指数包络、沿线尖端锥形收敛、多方向线归属 + 双角切向混合、椭圆搜索邻域、沿脊值输运与走廊混合。纯内核，未接 surface 管线（见递延） |
| `partition.py` | `partition.{h,cpp}` + `localidw.cpp` 分派 | 自由端延界 + node-safe 洪泛 + 井专属归属；`interpretation_partition_v1` 经 `ResolvedParameters.hardBarrierModel` 进 BarrierGrid，默认 `grid_connectivity_v1` 行为不变 |
| `continuous_metric.py` FaultPathMetric + `surfer_idw.py` | `faultpath.{h,cpp}` + `SurferIdwAlgorithm` + `method=surfer_idw` | 可见性判定（相切豁免）、图上最短路绕行距离替代「不可见即丢弃」；Processing 壳复用 LocalDirectionIdw 输出契约（QC/支持栅格/资产指纹），workflow 三段式经 `LocalDirectionJob.engineId` 分派 |
| `regional_contours.py` | `regionalcontours.{h,cpp}` | 分区独立井控等值线：逐区子样本独立解析 + 逐格心插值 + 区域掩码 marching squares（鞍点消歧、穿级含等值）；分区内无井拒绝外借 |
| `buffer_transition.py` | `buffertransition.{h,cpp}` | 显式低值缓冲 + 五次 smoothstep 肩部，重叠取独立候选较低包络；floor 只来自显式规则 |
| `contour_avoidance.py` | `contouravoidance.{h,cpp}` | 几何绕行核（改线不改值）：缓冲外环双弧绕行 + 弧长上限 + 截断回退。原 `CartographicDetour` 的值域绕行（`buildCartographicWork`）不动，本模块补几何层 |
| （参考工具） | `tools/reference/singlefactor/generate_completion_reference.py` | 完成版对拍黄金值生成器（corridor/partition/faultpath） |

不移（禁区确认）：`fast_grid.py`、`methods/gridded.py`、`constrained_engine.py` 历史兼容段、
`workflow.py` UI 组装段。

## 交互面说明

- **前提修正**：任务书「约束页仅两入口」已过时——#110 后 `constraintpage` 已有五个
  类型化绘制按钮（方向线/打断线/软边界/等值停止/制图绕行，五种 `Semantic` 全覆盖）。
  本轮补的是编辑面与反馈链，并把「两入口」注释腐化（5 处）修正。
- **编辑面**：`ConstraintWorkflow::removeConstraint`（store 落盘删除 +
  `constraintRemoved` 信号）、`switchConstraintSemantic`（读回既有逐线参数只换语义，
  走 `updateConstraintLine` 同一持久化通道改写 type 列 + params_json）；约束页新增
  「编辑约束线」（壳侧进编辑会话 + `PaleoVertexTool`，撤销/重做走编辑条原生 undo 栈）、
  「删除选中约束」（未选行禁用）、列表右键五语义切换/删除；删除后已实例化的
  `constraints.<horizon>` 图层 reload。类型化控制器共享编辑条 CAD dock
  （`paleo-editing-cad-dock`，`shareCadDock` 死代码接线）。
- **吸附**：勘察定案用 QGIS snapping（非自研）——画布创建即配 AllLayers +
  顶点/段 + 10px（`QgisCanvasController::nativeSnappingConfig`），所有
  `QgsMapToolCapture` 派生工具自动吃到；井位/已有约束线端点可吸。断层棒是域模型
  （`FaultStick`，project.sqlite）非地图图层，不参与 QGIS 吸附（见差异清单）。
- **反馈链**：绘制入库 → `constraintAdded` → 列表刷新（既有）→ 下次单因素计算按
  type 列 + params_json 语义消费（`tst_constraint_draw` 五类型回读断言闭环）。

## 语义差异清单（相对上游）

1. **faultpath 绕行节点**：上游 shapely `unary_union(buffer(ε))` 外环顶点；本侧解析
   生成——中间顶点放双测地 mitre 对角点 `v ± ε·(n₁+n₂)/(1+t₁·t₂)`，自由端放侧偏点 +
   切向越端角点，闭合折线（首尾重合）按中间顶点处理因此密封。实测与上游黄金值最大
   差 `4.2e-13`（口径 1e-9×S 内）。**未合成**：相交/重叠墙的 union 交点节点（上游靠
   unary_union 合并），各墙独立绕行——验收规模（离散断层）不受影响。
2. **partition 的 y 方向守卫**：上游 `assign_well_regions` 越界守卫只认升序 y 坐标
   （生产路径 `build_global_surface` 传升序 ys；传降序会把域内井全判 pending）。本侧
   北置 GridSpec 实现正确处理双向。对拍夹具按上游真实用法（升序 y）生成，标签 id 因
   种子扫描顺序可置换，断言用置换不变量（region_count + 探针格/井「同区」矩阵）。
   上游 `near_barrier_mask` 性能掩码未移植（恒做几何判定，结果等价）。
3. **corridor**：`build_legacy_direction_field`（NumPy 平滑/填充管线的粘合段）未移植；
   `estimate_mean_well_spacing` 复用本侧 `meanNearestSpacing`（n>400 不抽样口径，#109
   同款）。
4. **regional_contours**：上游走 shapely 区域多边形 + `LocalInterpolator` +
   `field_contours` 全管线；本侧用分区标签 + `evaluateAt` 逐区插值 + 自研 marching
   squares。分区边界处四格不全的像元不提线（截断而非外推）。
5. **contour_avoidance**：移植几何核子集；未移植高斯圆角（`_round_near_buffer`）、
   尖端桥接（`_bridge_shaped_gap`/`_turn_stopped_tips`）、场值复核（`value_check`——
   本侧调用方不提供标量场）、按级别错开车道（`order*step` lane）；STRtree 换线性成对
   相交检查。口径「沿缓冲边改道，保不住拓扑即在缓冲处截断」写死在头注释。
6. **surfer_idw 网格化**：上游 `build_global_surface` 的 strict 分区模式
   （`barrier_extend_to_boundary`）未并入 method=surfer_idw 首版——解释分区语义经
   `hardBarrierModel=interpretation_partition_v1` 在 localidw 侧提供。各向异性
   `ANISOTROPY_RATIO/ANGLE` 进 Processing 面。
7. **吸附断层棒**：断层棒非地图图层（域模型存储），QGIS snapping 覆盖不到；如需
   吸断层需先把棒发布为图层（递延）。

## 本机命令与台账（Oracle 逐条）

构建目录是 worktree 内 Ninja `RelWithDebInfo`，`QGIS_PREFIX` 指向本机
`vendor/superbuild/prefix`。

```text
ctest --test-dir build -R '^tst_singlefactor_(corridor|partition|faultpath|buffer|regional|avoidance)$' --output-on-failure
ctest --test-dir build -R '^tst_singlefactor_parity' --output-on-failure
ctest --test-dir build -R '^tst_constraint_draw$' --output-on-failure
ctest --test-dir build -R '^tst_factorworkflow$' --output-on-failure
python3 tools/check_layering.py --strict
python3 tools/check_i18n.py
python3 tools/check_ui_invariants.py --strict
```

- **O1 走廊**：`tst_singlefactor_corridor` 10 用例——弯曲线 (s,n) 直线化（弧长单调、
  法距左右号）、core 全拉伸/influence 外零影响/过渡带指数单调、多方向线归属 + 竞争
  双角混合（30° 断言）、椭圆邻域拉伸与线长扩展、沿脊剖面端点锚定与 NaN 填充。通过。
- **O2 分区**：`tst_singlefactor_partition` 6 用例——两侧分区井不共享（exclusive 归属
  无 -2）、自由端延到域边界（L 形两端、贴边端免延、无边界记冲突）、分区 id 稳定（重
  跑逐格一致）、node-safe 不渗漏、BarrierGrid 适配对照（同输入旧路径单区绕行/新路径
  两区分井）。通过。
- **O3 FaultPathMetric**：`tst_singlefactor_faultpath` 9 用例——直墙绕行 = 经端点折线
  长且 > 直线 + 绕端量级、无障碍 = 欧氏、相切可见、各向异性度量压缩、密封环全
  NaN、与硬屏障对照（`barrierGridAdapterSplitsWhereHardBarrierWraps`：旧路径同区、
  新路径分井）。通过。
- **O4 surfer_idw**：`surferIdwKeepsDetourSamples`（跨墙井按绕行距离参与：值 < 无墙
  2.6 且 > 单井 1.1，并与 FaultPathMetric 距离手算值 1e-12 一致）+ 端到端
  `tst_factorworkflow::surferIdwUsesFaultPathEngineEndToEnd`（method=surfer_idw 出图、
  QC 记 `paleo:paleo_surfer_idw`/`paleo_surfer_idw_v1`/`fault_path_metric_v1`、约束行
  不记 unknown_type）+ 方法面（约束页下拉新增「Surfer IDW（断层绕行）」）。通过。
- **O5 分区等值线**：`tst_singlefactor_regional`——两区低/高级别线各只在各自区出现、
  每条线整体落单一分区（逐点 region 断言不跨区）、空区拒绝外借（文案「不能跨隔断
  借用」）。通过。
- **O6 缓冲过渡**：`tst_singlefactor_buffer`——按 |距离| 排序过渡带单调平滑、带外不
  动、核内常值 floor、非零值（10）显式 floor（5）不被误判为零厚度、重叠较低包络且线序
  无关、非法 spec 拒绝。通过。
- **O7 交互绘制**：`tst_constraint_draw` 6 用例——五语义入口 `onDrawn` 确定性驱动 →
  `ConstraintStore` type 列与 `params_json.semantic` 回读逐行对号（hard_barrier↔
  break_line 同义词口径）、语义切换改写 type 列 + 冻结语义词表拒绝第六词、删除落盘 +
  未知 id 拒绝、捕获期间画布吸附 AllLayers+顶点/段生效。顶点编辑/撤销重做走编辑条
  原生链（`tst_edittools` 既有覆盖 + 本轮接线），列刷新经 `constraintRemoved`。通过。
- **O8 对拍**：`tools/reference/singlefactor/generate_completion_reference.py` 生成
  `tests/fixtures/singlefactor/kernel_parity_completion.json`（3 case：corridor 投影 8
  点 × 6 量、faultpath 距离 5×3 矩阵、partition 双模式置换不变量）；
  `tst_singlefactor_parity_completion` 按 1e-9×S 口径全过（faultpath 实测 maxAbs
  4.2e-13）。旧 13 case `kernel_parity.json` 不动，`tst_singlefactor_parity` 仍过。
- **O9 ledger**：本文档即台账；提交序列 18c00aa（corridor）→ a2bd19d（partition）→
  b36cc4f（faultpath+surfer）→ 36109e6（buffer/regional/avoidance）→ c2049fb（UI
  编辑面）→ 本轮（对拍+吸附+收口）。

全量回归：`QT_QPA_PLATFORM=offscreen ctest --test-dir build` 全绿（见 PR 描述）；
`check_layering --strict`/`check_i18n`/`check_ui_invariants --strict` 通过；vendor 前缀
全量构建零新警告（基线 421 条全部来自 vendored QGIS 头的 QMetaType 弃用告警，未新增）。

## 仍递延项

- 真工区验收：`PALEO_REAL_PROJECT_AREA` 未设置（同 #109 O12），不记通过。
- corridor 未接 surface 管线：内核 + 测试 + 对拍已就位，`method=corridor_idw` 网格化
  待后续方向。
- surfer_idw 的 strict 分区模式（`barrier_extend_to_boundary` 同款）与解释分区的组合。
- contour_avoidance 的高斯圆角、尖端桥接、场值复核、按级别车道错开。
- 相交/重叠断层的 union 绕行节点合成（当前各墙独立绕行）。
- 断层棒吸附（需先把 FaultStick 发布为地图图层）。
- #109 遗留 O7/O11/O12/O13 项不变。
