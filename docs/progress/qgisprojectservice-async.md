# qgisprojectservice-async — 工程后台打开与测区范围恢复（实战系补账立账）

本文为补账文档：`src/qgis/qgisprojectservice_async.cpp`（40398d20 新增
+268）与 `src/services/previewdoc_survey.cpp`（同提交 +58）此前零文档。
行号为 origin/master `f31ee461` 口径。

## What（交付面）

| 面 | 内容 |
|----|------|
| 异步打开 | `QgisProjectService::openProjectAsync(input)`（qgisprojectservice_async.cpp:54-280）：worker 线程整读工程，GUI 线程接管——大工程打开不再冻结 UI |
| 取消 | `cancelOpen()`（:40-52）：原子标志 + 代际号自增 + openAborted/openFinished 信号，拒绝态不碰现有工程 |
| 测区范围 | `PreviewDocService::requestSurveyBounds(assetId, done)`（previewdoc_survey.cpp:9-58）：SEG-Y 四角点后台恢复，预览页不卡扫描 |

## 口径（语义决策）

1. **代际号防串台**：每次打开 `++m_openGeneration`，进度/完成回调只对
   当前代际生效（:73, :78, :88）——连开两次时旧任务的迟到结果被丢弃，
   不会把旧工程盖到新会话上。
2. **worker 读什么**：后台 `QgsProject` 独立实例整读，读旗
   `DontLoadLayouts | DontLoad3DViews`（:255-256，QGIS 契约允许后台读）；
   同时挂 readProject 信号克隆工程 XML 文档（:247-248）、loadingLayer/
   layerLoaded 进度映射到 10-75 区间（:249-253）。图层树在引用解析后
   冻结成独立 XML（:263-268），避免接管后旧树被改。
3. **成功才接管（单事务语义）**：worker 失败/取消 → 现有工程原样不动
   （:88-92, :204-207）。成功路径 GUI 线程：主工程实例以
   `DontResolveLayers | DontLoadLayouts | DontLoad3DViews` 读原路径
   （保留 QGZ 辅助存储/附件/相对路径语义，:104-108 注释），再用后台已读
   层替换占位层（:113-142，附件数据源重定位 + 辅助存储恢复），回放
   图层树/主题/标注/剖面/GPS 引用解析（:146-158），恢复布局（:160-161）。
4. **防读取期间变更**：worker 记录 size+mtime，接管前复核不一致即拒
   （:96-99「读取期间工程文件已变化，请重新打开」）。
5. **清单优先**：.paleo 输入先读清单定位 qgz 成员（:221-241）；清单坏
   → 裸 qgz 仍可开但成员缺失进 warnings（:230-231）；georeference/map
   配置随清单进状态、清单缺失时回退 .qgz 自定义属性再回写清单
   （:165-194 收养）。
6. **线程搬运**：QgsLayerTree 由 unique_ptr 持有非 QObject 子对象，
   显式 `moveToThread(ownerThread)` 后再搬 QgsProject（:273-275 注释）。
7. **requestSurveyBounds 同款纪律**（previewdoc_survey.cpp）：代际号
   `m_surveyGeneration` 防串台（:12, :39-40）、SegyReader 实例按 assetId
   复用缓存（:17-18, :43）、走 PaleoTaskService 后台任务（:47-53）、
   无任务服务时同步回退（:54-57）；四角点缺任一即报「完整测区角点
   不可用」（:33-36）。

## 证据

- 交付提交 40398d20（qgisprojectservice_async.cpp +268、
  previewdoc_survey.cpp +58，随「完善测区、地震聚类、时深对齐与工程
  导入加载」合入）；测试接线见 CMakeLists.txt:445/:503-504。
- 异步打开的既有行为面测试在 tst_projectsvc（createProject/save 侧）；
  openProjectAsync 全链自动化测试缺（真实 QGZ 夹具 + QEventLoop 驱动，
  成本高）——记 TODOS，不在本方向强补。

## 对账与递延

- 与同步 `openProject`（qgisprojectservice.cpp）：并存双入口——同步版
  仍是内部/测试路径，异步版是 UI 主路径；两者对「清单收养、成员缺失
  warning」语义必须同步演化的约束靠 review 纪律（记 TODOS：抽公共
  清单决策段）。
- 与方向 59 header-hygiene 的 previewdoc 重头扇出：previewdoc_survey.cpp
  新增 TU 加重了 previewdoc.h 扇出（当时 +1 TU），方向 59 已立收口任务。
- 递延：打开进度区间 0-100 的数值（0 清单/10-75 图层/80 就绪/85-100
  接管）是 UI 文案契约，无测试锚（低风险，文案级）。
