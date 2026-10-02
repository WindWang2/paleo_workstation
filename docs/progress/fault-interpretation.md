# goal/fault-interpretation — 断层解释数据框架与拾取工具（收口记录）

- 分支：`goal/fault-interpretation-20261002`（自 `origin/master` e6e95af）
- 目标：断层解释最小可用闭环——剖面拾取断层棒 → FaultSet 入库 → 层位图
  切割多边形 → 管理面板 → 工程保存重开完整还原（全链 offscreen 可测）。

## 交付面

| 件 | 位置 | 说明 |
|----|------|------|
| 数据模型 | `src/domain/faultset.{h,cpp}` | FaultSet（命名断层实体）/FaultStick（剖面定位棒：IL/XL/任意线 + TWT 范围）/FaultHorizonCut（层位切割多边形 + 上盘方向）；JSON 整体往返（types.h toMap/fromMap 约定）；留 extra 扩展位（Oracle #6 边界） |
| 持久化 | `src/metadata/faultsetstore.{h,cpp}` | project.sqlite 新表 `fault_set`（单行 JSON 文档，MetaStore user_version 门共库）；写一律经 `PaleoProjectStore::enqueueWrite`（EnqueueFn 注入，ConstraintStore 同式）；只读降级如实拒绝 |
| 编排 | `src/workflow/faultinterpretationcontroller.{h,cpp}` | 唯一权威模型 + FaultEditStack（闭包式命令栈：QUndoStack 属 QtWidgets 词表禁项，功能层不可用）+ SelectionContext 联动（`fault:<id>` 前缀 id 空间，协议形状零改动）+ catalog `fault` 角色链接 ensure（词表既有角色，幂等）+ 切割镜像内存图层（模型整刷） |
| 剖面拾取 | `seismicsectiondockwidget.*` / `seismicsectioncanvas.*` | dock 注入编排器后拾取走 FaultSet（undo 入编排栈；不双写会话伴生文件）；任意线剖面身份 = IL/XL 路径点串（同路径重提取可复现）；顺带修复任意线不设 SectionRef 导致拾取误归属上一条线的旧缺陷；画布新增 FaultSet 棒回显 + 选中白 halo 高亮 |
| 层位图切割 | `FaultManagerPanel` 复用 `PaleoDrawPolygonTool` | 多边形绘制 → `setCut(fault, horizon, wkt, side)`（同断层同层位替换）；活动层位取 `SelectionContext::activeHorizon()` |
| 管理面板 | `src/ui/faults/faultmanagerpanel.{h,cpp}` + `paleomainwindow_faults.cpp` | 断层树（显隐勾选/改名/删除/子节点棒与切割）、撤销/重做镜像、上盘方向设置、选中 → 三视图联动；壳接线 `main.cpp → attachFaults` |

## Oracle 验证证据

1. **闭环 offscreen**：`tst_faultsectionui::fullLoopSaveAndReopenRestores` ——
   IL 120 剖面拾取 → FaultSet（自动落盘）→ 切割多边形 → 改名 → 销毁视图后
   全新 store/controller 重开：断层名/棒点列/剖面归属/切割+上盘方向完整还原，
   新 dock 同剖面自动回显棒，新面板树显示改名后断层。
2. **编辑语义 undo/redo 逐拍**：`tst_faultinterp::pickAndUndoRedoStepByStep`
   （建→拾×2→删→改名 5 命令逐拍回退/重放，含被删棒 id 原样恢复）、
   `stickPickAutoCreatesFaultAndUndoRemovesBoth`（自动建断层随首棒撤销/重放）、
   `cutsIndependentPerHorizonViaController`（切割链逐拍）、
   `tst_faultsectionui::pickUndoRemovesStickAndOverlay`（剖面侧撤销含画布回显）。
3. **联动信号载荷**：`tst_faultinterp::selectionPayloadAndEcho`
   （QSignalSpy 断言 `["fault:f-1"]` + origin；未知 id 过滤；回声剥前缀）、
   `mapLayerMirrorFeatures`（地图要素选中 → origin=fault_map 广播；面板选择 →
   地图要素回声选中）、`tst_faultsectionui::selectionHighlightOnCanvas`
   （剖面画布高亮位断言）。
4. **断层-层位独立存取**：`tst_faultset::cutsPerHorizonAreIndependent` +
   `tst_faultinterp::cutsIndependentPerHorizonViaController`（H1/H2 关系互不
   影响、同层位替换不增行、独立撤销）。
5. **门禁**：`tools/check_layering.py --strict` 绿；全量构建零新增警告
   （本方向文件零警告）；串行 ctest **143/144**（含新增 4 套断层测试全绿，
   108–111 号）；唯一失败 `tst_startup_trace` 为启动时序预算闸门，在
   **未含本方向改动的主仓 master 基线 checkout 上以同类断言复现失败**
   （realStartupRatioGatesHold/injectedDegradationMustTripGate 份额超标）
   ——机器负载态敏感的既有闸门，非本方向回归。`-j4` 并行下另有
   tst_seismic_perf/tst_panels 共享夹具目录并行冲突偶红，串行复跑全绿。
6. **边界**：无封闭性/断距计算代码；`Fault.extra`/`FaultHorizonCut.extra`
   前向扩展位按原样往返。

## 测试账

- `tst_faultset`（paleo_domain）10/10：实体 CRUD/命名去重、棒剖面查询、
  JSON 往返、坏 JSON 拒绝、insertFault 恢复计数器。
- `tst_faultsetstore`（paleo_store）7/7：直写往返、空工程、写队列注入、
  只读拒绝、二次编辑重开可见。
- `tst_faultinterp`（paleo_workflow/linkage/store）10/10：见 Oracle 2/3/4 +
  catalog 角色 + 镜像层。
- `tst_faultsectionui`（paleo_ui/workflow/linkage/store）9/9：IL 拾取落账回显、
  时间切片拒拾、撤销、任意线身份稳定（同路径回显/异路径不回显）、高亮、
  面板意图、全闭环还原。

## 过程发现（防回归）

1. Qt6 `QJsonValue::fromVariant` 会把 QVariant 嵌套列表拍平——点列序列化
   用平铺 `[x0,y0,x1,y1,…]` 口径（faultset.cpp pointsToList 注释）。
2. `QUndoStack`/`QUndoCommand` 在 layering checker 词表属 QtWidgets 禁项，
   功能层不可用——自建 QtCore 闭包命令栈 `FaultEditStack`。
3. 任意线剖面旧实现从不设 SectionRef（stale ref 拾取误归属）；本方向
   起 arbitrary 入口作废旧 ref 并以路径点串为身份。
4. `QgsGeometry` QGIS 4.2 无 `isValid()` 成员——空/坏 WKT 判 `isNull() ||
   isEmpty()`。
5. UI 测试 main 用 QApplication + initTestCase 内 `initQgis()` 成对调用；
   QgsApplication 栈实例在「SeismicTaskService + 面板」组合下 exitQgis 段错误。

## 留待后续（TODOS 候选）

- 切割多边形顶点级编辑（当前为重画替换语义；可挂 edittools 原生编辑会话）。
- 断层棒地图投踪（stick 上平面图显示；镜像层目前只上切割多边形）。
- 断层自动识别/蚂蚁追踪、断距/封闭性分析（明确禁区外）。
