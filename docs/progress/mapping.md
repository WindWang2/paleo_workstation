# wave/mapping-editing — 进度与契约（2026-09-28）

分支 `wave/mapping-editing`（base `origin/master` 027524d）。八条主线全落地，
原子提交序列见 git log（feat(mapping)/feat(edittools)）。

## 完成项

### 1. 词表单一权威化（7b7ae6e）
- **canonical 组名词表单一权威 = `src/qgis/layervocabulary.h`**（PaleoLayerVocabulary
  命名空间）：canonical 七组、页面档案表（页→组集合）、组→编图页映射、
  旧名→canonical 别名，全部收口于此。
- 旧名→canonical：`01_Prediction`/`03_Predict`→`02_Prediction`；
  `02_Constraints`→`03_Constraints`；`03_Composite`→`05_PaleoMap`。
  `00_Data`（数据页组）与 `04_SingleFactor/Contours`（子组路径）原样透传。
- 档案摆树（qgislayerprofile stageTreeVisibility）经 `profileContains` 吸收
  旧组名——**旧 .qgz 的 `01_Prediction` 产层应用 predict 档案不再被表外隐藏**
  （现象级修复）。layertreepanel 组→页、predictpage/composepage 兼容清单
  改走 `groupFamily`；layermanifest.h/layertreepanel 悬空注释指向新权威。
- **旧名产点不改**（workflows.cpp 536/642/774/807/881/901/1401、
  io/dataimportservice.cpp、appcontext.cpp、registration.cpp——不都在本版图），
  消费面统一折算。
- 测试：tst_layerplatform `vocabularySingleAuthority`、
  `legacyGroupAliasKeepsOldPredictionVisible`。

### 2. QgsPointLocator 拓扑索引 + 跨层拓扑（3d8c447）
- `src/qgis/topologicalindex.{h,cpp}`（QgisTopologicalIndex）：每参与层一个
  QgsPointLocator（层 CRS 索引），`verticesNear`/`edgesNear` R-tree 邻域候选
  替换 PaleoVertexTool 的三处全要素线性扫描（共点集、落点焊接、共享边）。
- **失效策略（自建，不依赖 locator 内部跟踪）**：每层独立 dirty 位，
  geometryChanged/featureAdded/featureDeleted/afterRollBack/editingStarted/
  editingStopped 任一信号整层标脏；查询时惰性重建（销毁旧 locator 重建，
  `init(-1, relaxed=true)` 走 QgsPointLocatorInitTask 任务线程建 R-tree 后
  有界等待）。重建按手势粒度（press/dblclick/right-release 时），鼠标移动
  途中零重建。
- 精确共点判定（层坐标 qgsDoubleNear）与 map 空间 searchRadiusMU 门保持
  旧语义；`layerUnitRadius` 局部仿射探针 ×2 松弛折算 CRS 差异（宁可多候选）。
- 共享边插点位置：**不依赖 `Match::vertexIndex` 的边语义**（实测为
  firstVertex−1，跨几何类型不承诺）——以命中要素几何精确回查相邻顶点对定位。
- **跨层拓扑选项**：同 CRS 且处于编辑会话的相邻矢量层自动入写集（参与面
  每手势收集）；编辑命令按层一组——undo 按层独立（QGIS 无跨层 undo 栈，
  与 native 语义一致）。工具条「跨层」开关（仅拓扑开启时可用）镜像工程条目
  `paleo/crossLayerTopologicalEditing`（writeEntry，随 .qgz）。
- 新源文件注册：`cmake/extra-mapping.cmake`。

### 3. 编辑会话生命周期加固（2c68094）
- PaleoEditingToolbar 工程边界收尾：`setProject` 到不同工程时
  `finalizeSession`——提交优先、provider 拒绝则回滚，绝不把悬挂编辑缓冲留给
  图层析构；`watchProject` 监听 `layersWillBeRemoved`/`cleared`（编辑层即将
  被移除时在图层仍存活时收尾）。
- undo 跨边界：工程边界收尾即版本边界（§34）——提交后 undo/redo 全关，
  边界后触发为无害 no-op。
- HorizonChipBar 编辑中拦截不再静默：`horizonSwitchRefused(原因)` 信号带
  编辑层名文案（「正在编辑「%1」——先保存或放弃编辑，再切换层位」）；
  恢复路径 = 会话结束后 chip 照常可点（无记忆态）。

### 4. 属性表编辑入管线（b453274）
- AttributeTablePanel 显式编辑会话三键（attrEditStart/Save/CancelButton）+
  `beginEditing/saveEditing/cancelEditing` public slots。
- 提交语义与 MapVersionController 一致：注入 QgisEditingService 时走
  beginEdit（markLayerBusy）/commitEdit（enqueueWrite 单写者+markLayerFree）；
  无服务直连降级（同编辑工具条）。
- 单元格值提交路径 = `QgsAttributeTableDelegate::setModelData` 的
  beginEditCommand("Attribute changed")+changeAttributeValue+endEditCommand
  （libqgis_gui 反汇编证实；QGIS 4.2 模型 setData 不直写图层）——进图层原生
  undo 栈，面板不绕 edit buffer 直写 provider。

### 5. 图层平台递延三项（edc892c）
- **主题重命名**：`QgisLayerProfileService::renameTheme`——insert-then-remove
  记录复制（QgsMapThemeCollection 无原生 rename API；先建新名失败即中止、
  成功才删旧名）。管理对话框加「重命名」按钮；`page:*` 页面档案约定名不改。
- **图层创建时间元数据**：`QgisLayerService::instantiate` 首次实例化落
  `paleoCreatedAt` 图层自定义属性（ISO UTC，随 .qgz 持久化，复用不刷新）；
  属性面板业务页「创建时间」行从降级占位接真值。
- **「删除选中」消歧**：图层树工具条/默认动作文案改「删除所选图层/组」+
  tooltip 指明画布要素删除走编辑工具条；工具条按钮经守卫包装——编辑会话中
  的图层拒删并发 `layerRemovalRefused(原因)`。

### 6. 非 IDW 单因素引擎分级（a528f7a）
- **strathick 接入**：注册表项改 `paleo:paleo_isopach`（顶−底构造面相减），
  参数键 `topLayerId`/`baseLayerId`/`negativeToNodata`（默认 true）。
  `ConstraintWorkflow::generateFactor` 按引擎分派参数整形：isopach 走
  INPUT_TOP/INPUT_BASE 双栅格（父版本=两面栅格源）；ConstraintPage 勾选
  strathick 时展开顶/底构造面选择行（按当前层位列声明栅格），payload 携带两 id。
- **welldist/confidence 标 blocked**（契约见下节）：生成链按 id 显式拒绝
  （不静默降级 IDW——那会产出语义错误的栅格）。

### 7. 编图页边界 case（f00afe0）
- 档案应用/回滚：页面主题被删后 applyPageProfile 按档案表重建（自愈）。
- 页面切换 dirty 守卫：预测页忙碌态跨 hide/show 保持，取消路径完整。
- 约束捕获中断恢复：打断后画布工具卸下、无幽灵约束、序号不虚耗，新捕获全新开始。
- 预测页 schema 校验失败路径：`AlgorithmParamField.required`（constraint_idw
  的 FIELD 必填）；空必填 String 收集侧拒收 + statusLabel 说明 + 不发意图；
  校验通过清陈旧错误文案。

### 8. 测试扩容
每主线 ≥2 用例，共 +26：tst_edittools 65（+11）、tst_layerplatform 17（+4）、
tst_mappingpages 33（+3）、tst_factorworkflow 10（+2）、tst_attrpanel 7（+3）、
tst_chips 5（+1）、tst_drawctl 6（+1）、tst_layerprofilebar 13（+1）、
tst_layerproperties 11（改 1 断言）、tst_layertreepanel 15（+1）。

## 跨方向契约（引擎 id / 参数键——冻结）

实现侧（数据方向 `src/algorithms/` / services）按此注册即可点亮，工作流侧
整形分派已就位（`ConstraintWorkflow::generateFactor`）：

| 因素 | 引擎 id（processingAlgId） | 参数键 | 语义 | blocked 原因 |
|------|---------------------------|--------|------|--------------|
| welldist | `paleo:paleo_distance_transform` | INPUT=井点图层；CELL_SIZE=正数；OUTPUT=栅格目的地 | 逐像元到最近井点的绕障距离（距离变换；障碍=约束线） | 需 src/algorithms/ 新距离变换算法（数据方向版图） |
| confidence | `paleo:paleo_confidence_surface` | INPUT=预测结果栅格；OUTPUT=栅格目的地 | 预测置信度面（模型置信度通道直取） | onnxpredictionservice 只读首个输出张量（GetOutputCount 仅下限检查），无真实置信度通道 |

- 契约常量面：`SingleFactorContracts::welldistEngineId()/confidenceEngineId()`
  （src/services/singlefactordef.h）。生成链按 id 拒绝（错误文案含「尚未接入」）；
  **tst_factorworkflow::registryAlgorithmsAvailableAtRuntime 反向断言这两个 id
  尚未注册**——实现侧接入时该断言与拒绝用例同步翻转。
- strathick 已接入（`paleo:paleo_isopach`，INPUT_TOP/INPUT_BASE/
  NEGATIVE_TO_NODATA/OUTPUT）——参数真值见 src/algorithms/paleoalgorithms.h。

## Seam 请求（壳面触点，合并时集成——本波未碰 paleomainwindow*）

| Seam | 触点 | 动作 |
|------|------|------|
| S-1 | AttributeTablePanel::setEditingService | paleomainwindow_attach 装配属性表面板处注入 m_editSvc（与编辑工具条同一服务实例），busy 标记/释放时机即全 app 一致 |
| S-2 | HorizonChipBar::horizonSwitchRefused | 壳接状态栏/消息条展示拦截文案（现信号已发，无订阅者时静默降级为原弹回行为） |
| S-3 | LayerTreePanel::layerRemovalRefused | 同上，状态栏/消息条展示拒删原因 |
| S-4 | PaleoVertexTool::setCrossLayerTopologyEnabled | 已由 PaleoEditingToolbar 内部接线（无需壳面动作）；跨层开关的持久化键为 paleo/crossLayerTopologicalEditing，壳侧无感知 |
| S-5 | QgisTopologicalIndex 超大层分帧 | 若未来相图层规模显著增长（>数万要素），把 ensureReady 的有界等待换成查询期降级（索引未就绪时回退线性扫描）——接缝已在 topologicalindex.h 预留说明 |

## 遗留

- QgsPointLocator 索引构建为「relaxed 任务线程 + 有界等待」：数百要素的相图层
  毫秒级；超大层的完全异步/分帧见 S-5。
- 跨层拓扑写集要求参与层**处于编辑会话**（native 语义：只写可写层）——用户需
  同时开两层编辑；多入口开会的 UX 引导（如自动为同 CRS 邻层开编辑）递延。
- 跨层手势的 undo 按层独立（QGIS 无跨层 undo 栈）；若产品要求「一键全撤」，
  需在编辑工具条聚合各层 undoStack 的 undo() ——递延（涉及 UX 决策）。
- 主题重命名 insert-then-remove 在 mapThemesChanged 上发两次（insert+remove），
  档案工具条下拉会闪一次重建——接受（QgsMapThemeCollection 无原子 rename）。
- paleoCreatedAt 在图层**首次实例化**时落：从未实例化的声明无创建时间
  （manifest 无时间戳列，属 metadata 版图；如需声明级时间戳，数据方向加列后
  在 layerpropertiesdialog 接第二来源）。
- welldist/confidence 实现侧（见跨方向契约表）；接入口在
  ConstraintWorkflow::generateFactor 的引擎分派处 + singlefactordef.cpp 词表项。
