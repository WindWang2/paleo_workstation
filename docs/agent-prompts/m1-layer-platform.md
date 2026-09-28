你在 paleo_workstation 项目建造「图层平台」：图层管理面板、图层属性对话框、
按页面/布局激活图层的档案系统。这是三个编图页（预测编图/单因素图/智能编图）
共用的底座，也是一个大体量任务（多交付物、多阶段），按 ZCode agent 工作流执行。

【执行方式】在 ZCode 中以 agent/teamwork-preview 模式执行：自行拆解子任务
并行开发。子任务建议划分（可调整）：
  A) 图层树面板升级（layertreepanel：工具条+筛选+全量右键菜单+图例节点）
  B) 图层属性对话框（layerpropertiesdialog：原生属性壳 + Paleo 业务页）
  C) 图层档案/主题服务（qgislayerprofile：QgsMapThemeCollection 封装 +
     页面档案表 + 打印布局联动）
  D) 档案工具条 + 层位联动排序（layerprofilebar + setActiveHorizon 时序）
  E) 测试贯通（每个子任务配 QTest，最后全量）
Lead 负责分派、合并冲突裁决、最终验收。

【环境】仓库 /home/kevin/projects/paleo_workstation。C++20，Qt 6.11.2，
QGIS 4.2.2（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui
-lqgis_analysis），moc=/usr/lib/qt6/moc，测试 QT_QPA_PLATFORM=offscreen
+ QTest。**编译资源上限：cmake --build -j4（不得超过 4 并行），ctest
不加并行参数或 -j2。**

QGIS 4.2 已装头文件（逐个核实后再用，头里没有就报告降级方案）：
  - QgsLayerTreeView / QgsLayerTreeModel / QgsLayerTreeViewDefaultActions
    / QgsLayerTreeViewIndicator / QgsLayerTreeModelLegendNode
  - QgsVectorLayerProperties(QgsMapCanvas*, QgsMessageBar*, QgsVectorLayer*,
    parent, flags) —— 矢量图层原生属性对话框
  - QgsRasterLayerProperties(QgsMapLayer*, QgsMapCanvas*, parent, flags)
    —— 栅格图层原生属性对话框
  - QgsMapThemeCollection —— 命名可见性主题，存在 QgsProject 内、随 .qgz
    持久化；createThemeFromCurrentState(root, model) / applyTheme /
    insert / removeTheme / mapThemes / mapThemesChanged
  - QgsMapLayerStyleManager —— 图层命名样式（save/restore 预设样式）
  - QgsLayoutItemMap::setFollowVisibilityPreset(bool) +
    setFollowVisibilityPresetName(name) —— 打印布局地图项钉主题
  - QgsMessageBar（属性对话框必需参数，无则自造一个实例持有）

现有接缝（先读再动）：
  - src/ui/paleomainwindow.cpp:455-517 现有 QgsLayerTreeView 裸挂左 dock
    （layerTreeDock / layerTreeView / layerTreeEmptyState 三个
    objectName 必须原样保留——tst_ui 依赖）；
  - src/qgis/qgislayerservice.{h,cpp} —— 层位粒度实例化服务
    （declare/instantiate/setActiveHorizon/releaseHorizon，图层归
    QgsProject 所有）；
  - src/metadata/layermanifest.h —— LayerDeclaration{layerId,horizon,
    type,source,styleRef,group,title}，group 词表即 01_Base/02_Prediction/
    03_Constraints/04_SingleFactor/05_PaleoMap/06_Reference/07_Validation；
  - src/qgis/qgiscanvascontroller.{h,cpp} —— canvas 与 map-tool 生命周期；
  - src/qgis/qgisprojectservice.{h,cpp} —— QgsProject 包装；
  - src/ui/layout/ —— 布局设计器壳（z1 已交付 QgsLayoutView 体系）。

【第一步：建 worktree（lead 建好，子任务共享）】
cd /home/kevin/projects/paleo_workstation
git worktree add ../pw-layers -b wave/layer-platform
cd ../pw-layers

【产品边界】图层显示/隐藏/排序/透明/样式/属性全部落到 QGIS 图层对象上
（PALEO_QGIS_PLAN.md §5：QgsLayerTree 是唯一图层状态源）。Paleo 只做业务
接线：manifest layerId ↔ catalog assetId 关联、层位归属、页面档案表。
禁止维护第二份图层状态（不允许 panel 内自建 QHash<layerId,bool> 可见性
台账——主题状态只读 QgsLayerTreeModel）。

【分层契约（docs/UI_LAYER_PLAN.md 已批准，提前遵守）】
- src/ui/** 只渲染+发意图信号；不 include src/io/* 解析器头、不直接写
  catalog/工程文件；业务编排走信号回壳/workflow。
- src/qgis/* 是 Qgs* 封装层（可 QtWidgets）；新 QGIS 封装类放这里。
- 每个新 src/ 文件头注释加一行「// 层：视图」或「// 层：QGIS 封装」
  （六值词表：数据/功能/QGIS 封装/视图/组装根/测试壳）。

【交付物】
1. src/ui/layers/layertreepanel.{h,cpp} — 图层管理面板（替换
   paleomainwindow 里裸 QgsLayerTreeView 挂接，壳只 new 这个面板）：
   - QgsLayerTreeView + QgsLayerTreeModel（AllowNodeReorder/Rename/
     ChangeVisibility 现有三 flag 保留）；
   - 顶部工具条：添加组、删除选中、展开全部、折叠全部、筛选输入框
     （按 layer title/id 过滤树）；
   - 右键菜单用 QgsLayerTreeViewDefaultActions 组全量：zoomToLayers /
     zoomToSelection / showFeatureCount / renameGroupOrLayer /
     removeGroupOrLayer / addGroup；Paleo 自加项：「属性…」「复制图层」
     「导出样式 .qml」「加载样式 .qml」「在新页打开所属编图页」；
   - 图例符号节点默认展开（QgsLayerTreeModel legend nodes，
     不要关 ShowLegendAsTree 现有行为除非有理由）；
   - indicator：未实例化层位图层灰显 + tooltip「该图层属于层位 X
     （未激活）」；缺源图层（provider 读不到）加警示 indicator；
   - 空态 label 保留（layerTreeEmptyState 复用现有文案）。
2. src/ui/layers/layerpropertiesdialog.{h,cpp} — 图层属性：
   - openLayerProperties(QString layerId)：经 LayerResolver 解析
     QgsMapLayer*，按 type 分发 QgsVectorLayerProperties /
     QgsRasterLayerProperties（canvas + messageBar 实参从注入拿，
     messageBar 缺省时 new 一个持有）；
   - 追加「Paleo 业务」页：layerId、所属 group、层位 horizon、关联
     catalog assetId/资产名、来源 provenance、创建时间——只读展示 +
     「在数据页查看资产」跳转信号 assetInspectionRequested(assetId)；
     实现可走 QgsMapLayerConfigWidget 挂页，若原生挂点在桌面对话框
     壳不可达，改为在原生对话框外层 QTabWidget 包一层（原生对话框
     setParent 进 tab——先验证可行，不可行就在原生对话框旁并排
     PaleoPanel，报告里写证据）；
   - 样式管理：QgsMapLayerStyleManager——「保存当前样式为预设」「从
     预设恢复」「导入/导出 .qml」三个动作，预设名随 manifest styleRef
     约定；
   - 属性改动落地 QgsMapLayer 对象（QGIS 持久化进 .qgz），Paleo 侧只
     记业务字段到 manifest/catalog，不复制路径/CRS/provider。
3. src/qgis/qgislayerprofile.{h,cpp} — `QgisLayerProfileService`
   （QGIS 封装层，「不同布局激活不同图层」的核心）：
   - 底部机制 = QgsMapThemeCollection（挂在
     QgsProject::mapThemeCollection()，随 .qgz 持久化——与 project
     绑定不用自建存储）；
   - API：applyPageProfile(pageId)、captureCurrentAsTheme(name)、
     applyTheme(name)、removeMapTheme(name)、themes()、
     pageProfileFor(pageId)；信号 profileApplied(pageId,themeName)、
     mapThemesChanged 转发；
   - 内置页面档案表（默认映射，允许 manifest 扩展覆盖）：
       predict   → 组 {01_Base, 02_Prediction} + 当前层位约束图层
       constraint→ 组 {01_Base, 03_Constraints, 04_SingleFactor}
       compose   → 组 {01_Base, 03_Constraints, 04_SingleFactor,
                    05_PaleoMap, 06_Reference}
       validate  → 组 {01_Base, 07_Validation}
       data      → 不操作画布（数据页无地图）
     档案应用语义：表内组/层置可见+加入 canvas 图层集，表外组置隐藏
     （不删图层、不动 manifest——纯可见性主题）；用户手改可见性后
     档案不覆盖，除非显式 applyPageProfile；
   - 首次应用时若主题不存在：从档案表生成主题记录 insert 进
     collection（「页面档案」与「用户命名主题」共用同一存储，
     页面主题名加前缀 "page:" 区分）；
   - 与 QgisLayerService::setActiveHorizon 的时序钉死：先换层位
     （实例化/释放）再应用页面主题，防主题应用到刚释放的 layerId；
   - 打印布局联动：setLayoutMapTheme(QgsLayoutItemMap*, themeName)
     辅助封装 setFollowVisibilityPreset——智能编图导出的版面地图
     可钉 compose 主题。
4. src/ui/layers/layerprofilebar.{h,cpp} — 档案工具条（图层 dock 头）：
   主题下拉（QgsMapThemeCollection::mapThemes 实时）+「保存当前可见性
   为主题…」+「管理主题…」（重命名/删除小对话框）+当前页档案指示。
5. 接线点（壳侧小改）：m_leftDock 内容换成 layertreepanel + profilebar
   组合；showPage() 每页切换调 applyPageProfile；图层树「属性…」进
   layerpropertiesdialog。改完 paleomainwindow.cpp 净增长应 <100 行。
6. tests/tst_layerplatform.cpp — 覆盖：
   - 页面档案应用后树可见性与主题记录一致（逐组断言）；
   - 主题随 .qgz 往返（save→clear→load→主题仍在）；
   - captureCurrentAsTheme/applyTheme/removeTheme 生命周期；
   - setActiveHorizon + applyPageProfile 次序下无悬空 layerId；
   - 属性对话框对矢量/栅格/缺源三类图层的行为（offscreen 下对话框
     可构造、业务页字段正确）；
   - 筛选框过滤、组级显隐、复制图层。

【纪律】TDD 每子任务红→绿；worktree 内多 commit；objectName 兼容
（layerTreeDock/layerTreeView/layerTreeEmptyState 不动）；五页行为不变。
不碰：paleomainwindow.* 除交付物 5 列明的接线点、pagepanels.*、
workflows.*、qgislayerservice.* 的公共 API（可加成员不改签名）、
layermanifest.* 的 schema、layout/、edittools/、maptools/、
datapreview/、correlation*/、wellcomposite/、seismic*/、
src/{domain,catalog,io,metadata,algorithms,ai}/、vendor/、tests 里
既有 tst_*（可新增不可改断言）。
完成后：git commit -a（仓库惯例 trailer），gh pr create 提交 PR，
汇报测试数、文件清单、集成注意、降级项（若有）。
