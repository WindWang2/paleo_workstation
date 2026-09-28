你在 paleo_workstation 项目把三个编图页——预测编图（PredictPage）、
单因素图（ConstraintPage 重定位）、智能编图（ComposePage）——从占位
面板升级为完整的生产级工作流。大体量任务（多交付物、多阶段），按
ZCode agent 工作流执行。

【执行方式】在 ZCode 中以 agent/teamwork-preview 模式执行：自行拆解
子任务并行开发。子任务建议划分（可调整）：
  A) 预测编图页（层位 chip + 算法 + 参数 + 任务化运行 + 结果图层）
  B) 单因素图页（单因素注册表 + 生成链 + 等值线 + 色带样式）
  C) 智能编图页（融合 + 矢量化 + 相界编辑接入 + 版本/发布 + 导出）
  D) 页面图层激活联动（随页切换应用页面图层档案）
  E) 测试贯通
Lead 负责分派、合并冲突裁决、最终验收。

【环境】仓库 /home/kevin/projects/paleo_workstation。C++20，Qt 6.11.2，
QGIS 4.2.2（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui
-lqgis_analysis），moc=/usr/lib/qt6/moc，测试 QT_QPA_PLATFORM=offscreen
+ QTest。**编译资源上限：cmake --build -j4（不得超过 4 并行），ctest
不加并行参数或 -j2。**

QGIS 4.2 原生能力（逐头核实后用，没有就报告降级）：
  - QgsProcessingAlgorithm/QgsProcessingRegistry/QgisProcessingService
    （本仓库 src/qgis/qgisprocessingservice.* 已封装）——IDW/TIN/
    Contour/RasterCalculator 走 processing，不自写插值管线；
  - QgsSingleBandPseudoColorRenderer / QgsColorRamp（色带样式）/
    QgsHillshadeRenderer；
  - 等值线两条路按用途分开：出矢量图层走 processing `gdal:contour`
    → 真 QgsVectorLayer（PALEO_QGIS_PLAN.md §12：等值线是 GIS
    LineString，不是 canvas 临时线）；仅渲染不产出图层可用
    QgsRasterContourRenderer（头已装）；
  - QgsMapToolCapture/QgsMapToolEdit + 拓扑编辑
    （src/ui/edittools/ 已有 z3 交付的工具集，复用不重写；
    QgsVertexTool 在本构建是 app-only——顶点编辑用现成的
    PaleoVertexTool，vertexeditortools.h 头注释有证据）；
  - QgsMapThemeCollection（页面图层档案机制由 m1-layer-platform
    交付；本任务消费它的 applyPageProfile，接口见下「接缝依赖」）。

现有接缝（先读再动）：
  - src/ui/pages/pagepanels.{h,cpp} —— PredictPage(:2246+)/
    ConstraintPage(:2458+)/ComposePage(:2619+) 现状面板；
  - src/workflow/workflows.{h,cpp} —— PredictionWorkflow::
    runPrediction / ConstraintWorkflow::runConstraintIDW /
    CompositionWorkflow::fuseFactors/deriveFaciesPolygons 信号齐备；
  - src/workflow/mappingworkflow.{h,cpp} —— 厚度链 runThicknessChain/
    computeThicknessSamples；
  - src/workflow/mapversioncontroller.{h,cpp} —— 版本保存/发布状态机；
  - src/qgis/qgislayerservice.h —— declare/instantiate/setActiveHorizon；
  - src/metadata/layermanifest.h —— LayerDeclaration{layerId,horizon,
    type,source,styleRef,group,title}，组词表 01_Base…07_Validation；
  - src/ui/edittools/ —— z3 已交付的 add/reshape/move/delete/vertex/
    undo 工具集；
  - src/ui/layoutdesignershell.* + src/ui/layout/ —— z1 布局设计器；
  - src/ui/paleomainwindow.cpp —— 壳：chipsRow、canvasPane、
    attachWorkflows 接线点、showPage 页切换。

【第一步：建 worktree】
cd /home/kevin/projects/paleo_workstation
# 若 wave/layer-platform 已并入 master：基于 master
git worktree add ../pw-mappages -b wave/mapping-pages
# 否则基于图层平台分支开发（在其上叠加，合流时先合 m1）
# git worktree add ../pw-mappages -b wave/mapping-pages wave/layer-platform
cd ../pw-mappages

【产品边界】三页的 QGIS 画布共用壳内单一 QgsMapCanvas；页内控件只发
意图信号，GIS 操作落 workflow/qgis 封装层。所有业务图层必须先经
manifest declare（LayerDeclaration）再 instantiate——不允许绕过
manifest 直建 QgsMapLayer 塞进 project。图层可见性/主题/属性面板复用
m1 平台，本任务不重造。

【分层契约（docs/UI_LAYER_PLAN.md 已批准，提前遵守）】
- src/ui/** 只渲染+发意图信号；不 include src/io/* 解析器头、不写
  catalog/工程文件；
- 业务编排放 src/workflow/*；Qgs* 封装放 src/qgis/*；
- 每个新 src/ 文件头注释加「// 层：视图 | 功能 | QGIS 封装」对应行。

【接缝依赖】m1 交付 QgisLayerProfileService::applyPageProfile(pageId)。
本任务按以下契约消费：predict/constraint/compose/validate 四页
pageId 固定为这四个字符串；若 m1 未合并，在 src/qgis/qgislayerprofile.h
同路径补最小实现（页面档案表 + QgsMapThemeCollection 封装），冲突时
以 m1 版本为准做 merge。同时保留老「horizon chips」行为：层位 chip
行仍驱动 setActiveHorizon。

【交付物】
1. 预测编图页升级（pagepanels.cpp PredictPage 段 + 可拆
   src/ui/pages/predictpage.{h,cpp}）：
   - 顶部页内参数区：层位 chip（沿用现有 chipsRow 联动，显示当前
     horizon）、预测类型选择（沉积相/地震相/测井相，映射算法词表）、
     算法下拉（setAlgorithms 现有）、参数表单（parseInputParams
     扩展为按算法 schema 动态建控件——schema 由算法注册表给出）；
   - 运行任务化：runRequested → workflow 跑 taskSvc worker（沿用
     PaleoTaskService 模式），按钮运行期禁用 + 进度条 + 可取消；
   - 结果落地：predictionDone(layerId) → declare 进 manifest
     group=02_Prediction（预测相面栅格 + 置信度栅格双层），instantiate
     后通知壳刷新图层树；默认样式走 styleRef；
   - 历史结果：当前层位已存在的预测图层列「已有结果」小清单，
     可一键重新加载显示。
2. 单因素图页升级（ConstraintPage 重定位为「单因素图」主页）：
   - src/services/singlefactordef.{h,cpp} —— SingleFactorDefinition
     注册表（数据层）：factorId/标题/输入资产类型/算法/processing
     alg id/输出 styleRef/默认参数；内置词表=砂体厚度、砂地比、
     地层厚度、孔隙度、渗透率、距井距离、预测置信度
     （PALEO_QGIS_PLAN.md §10）；
   - 页内改为「单因素清单 + 生成」双区：因素表（checkbox 行：名称/
     输入/状态「未生成|已生成·layerId」）+ 参数行 + 生成按钮；
   - 生成链：runIdwRequested 扩展为 generateFactorRequested(factorId,
     horizon, params) → workflow 走 QgsProcessing（IDW→GDAL grid），
     输出 declare 进 group=04_SingleFactor；
   - 等值线生成行：「间距 N m」输入 + 按钮 → processing contour →
     QgsVectorLayer declare 进 04_SingleFactor 子组 Contours；
   - 色带：每因素默认 QgsColorRamp 预设（manifest styleRef 引用
     styles/ 落盘 .qml）；单因素图层互斥显示开关（同组单选态，
     业务上同时只看一张单因素图）；
   - 约束要素编辑保留（物源线/展布线/控制点 = 03_Constraints 组的
     QgsVectorLayer，承接 z3 编辑工具集；本页提供「画物源线/展布线/
     控制点」入口按钮 → 壳切换 canvas map tool）；
   - 厚度样本表（现有 refreshThicknessSamples/动态属性机制）保留，
     挪进单因素页「厚度样本」折叠区。
3. 智能编图页升级（ComposePage 段 + 可拆 composepage.{h,cpp}）：
   - 因素融合区：04_SingleFactor 已声明图层 checkbox 清单（refreshFactors
     现有信号驱动）+ 融合按钮 → fuseFactors → 初始相面栅格进
     05_PaleoMap；
   - 矢量化区：polygonizeRequested（现有）+ 参数（minArea/simplify）
     → 相界 QgsVectorLayer 进 05_PaleoMap；矢量化成功后自动进入
     相界编辑态（壳接线：canvas setMapTool 到 z3 编辑工具，
     snapping 拓扑编辑已开）；
   - 相属性表：相界图层选中要素 → 编辑 faciesCode/type/comment 属性
     （QgsAttributeForm 或简化表单元件）；
   - 参考图区：06_Reference 组图层清单（勾选叠加）；
   - 版本/发布区保留现有契约（setVersionState/setPublishState/
     saveVersionRequested/publishRequested 不动）；
   - 导出行：「导出 PDF」→ layoutdesigner 打开或复用 compose 版面，
     版面地图项钉本页主题（m1 setLayoutMapTheme 接缝，m1 缺席时
     调 QgsLayoutItemMap::setFollowVisibilityPresetName 直写）。
4. 页面图层激活（src/ui/paleomainwindow.cpp showPage 接线点，小改）：
   - 每页 showEvent/showPage 调 applyPageProfile(pageId)：predict 显
     01_Base+02_Prediction，constraint 显 01_Base+03_Constraints+
     04_SingleFactor，compose 显 01_Base+03_Constraints+04_SingleFactor+
     05_PaleoMap+06_Reference（档案表定义在 m1，此处只调用）；
   - 层位 chip 切换 = setActiveHorizon + 重应用当前页档案；
   - 画布 resize/主题切换后刷新（renderComplete 节律沿用）。
5. tests/tst_mappingpages.cpp + 既有测试扩展：
   - 预测：runRequested 参数组装、任务化禁用/取消、结果双层 declare
     到 02_Prediction、重跑幂等（同名 layerId 更新不重复）；
   - 单因素：注册表词表完整、生成链 declare 到 04_SingleFactor、
     contour 产出 vector layer、互斥单选行为、厚度样本折叠区渲染；
   - 编图：融合/矢量化 declare 到 05_PaleoMap、相属性编辑回写
     edit buffer、发布门既有断言不回归（setPublishState 语义）；
   - 页面切换档案应用顺序断言（m1 服务或桩）。

【纪律】TDD 每子任务红→绿；worktree 内多 commit；objectName 与既有
信号签名全保留（runRequested/runIdwRequested/fuseRequested/
polygonizeRequested/thicknessChainRequested/exportPdfRequested/
saveVersionRequested/publishRequested 签名不动）；右 dock 面板结构
不变（仍是 per-page 栈页）。
不碰：src/qgis/qgislayerservice.* 公共 API（可加成员不改签名）、
layermanifest schema、workflows.* 既有信号签名、mapversioncontroller.*
状态机语义、layout/ 内部、edittools/ 内部、datapreview/、correlation*/、
wellcomposite/、seismic*/、src/{domain,catalog,io,algorithms,ai}/、
vendor/、m1 独占文件（layertreepanel/layerpropertiesdialog/
layerprofilebar），既有 tst_*（可新增不可改断言）。
完成后：git commit -a（仓库惯例 trailer），gh pr create 提交 PR，
汇报测试数、文件清单、与 m1 的合流注意、降级项（若有）。
