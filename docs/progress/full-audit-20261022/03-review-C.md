# 审查报告 C（2026-10-22 轮）— Workflow / AI 推理 / 井震联动 / 跨模块调用关系

只读深审。基线 b301f2e8（工作树 `/home/kevin/projects/paleo_workstation-full-review-20261022`）。
增量区间 `093346e6..b301f2e8`（20 个 PR），本模块实际触达 src/workflow 的提交：

| 提交 | PR | 主题 | workflow 侧改动 |
|------|----|------|----------------|
| 49dbe95e | #603 | 统一测井读取 + 预览-确认-提交安全链 | folderimport（CancelImport/wellTargets/expectedHashes/skippedPaths/previewedPaths）、wellpredictionservice、wellsectionworkflow、wellpredictionreview、propertymodelworkflow、sectionworkbench、mappingworkbench_predict |
| 4f63cf05 | #610 | 线程生命周期/深度归一/断层图层泄漏 | aiassistworkflow（horizonGridFetch 快照化）、wellpredictionservice、faultinterpretationcontroller |
| 70b2589a | #613 | 空工程层位过滤 + 在线底图 | projectareasummary |
| 07f74fc3 | #619 | 岩心照片放大/文案 | mappingevidencereview（文案） |
| 9bfdb883 | #618 | 单因素 IPO 三段式 | mappingworkbench_edit（文案） |

src/ai 与 src/linkage 在本区间**零代码变更**（增量清单为空），按简报做全目录
接口/契约面复覆盖。#616（井位可视化底图）的 workflow 层编排实际落点即
#613 的 projectareasummary + qgis/ui 侧（非本 Agent 主责）。

上轮（full-audit-20261021）C-1..C-6 六项**已全部核实修复**（见 §3.1）。

## 1) 覆盖清单

### 逐行级精读（增量文件全文 + 调用链交叉）
| 文件 | 方式 | 风险级 | 测试对应 |
|------|------|--------|---------|
| src/workflow/folderimport.{h,cpp} | 全文 + 新取消机制/提交链逐分支 | 高 | tst_folderimport.cpp（cancel-after-worker-before-commit、cancelFirst/cancelSecond 隔离、cancelled 批——已核对断言面） |
| src/workflow/wellpredictionservice.cpp | prepare/plan/next/finish 全文 + readErrors 索引链 | 高 | tst_wellfacies.cpp（normalizeDepthCurve 族）、tst_wellprediction*（间接） |
| src/workflow/mappingworkbench_predict.cpp | predict() 全流程 + preparationError 新路径 + begin/endPredictionTask | 高 | tst_mappingpages.cpp（MD/单位拒绝路径**无**覆盖，见 C-9） |
| src/workflow/wellsectionworkflow.cpp | loadCurveBodies/request 代次守卫段 + C-5 现状复核 | 高 | tst_wellsection*（部分） |
| src/workflow/wellpredictionreview.cpp | predictionLog 全文 | 中 | 无（C-7） |
| src/workflow/propertymodelworkflow.cpp | requestFromCatalog 增量段 | 中 | tst_propertymodel*（间接） |
| src/workflow/sectionworkbench.cpp | sectionWells 增量段 + m_logs 生命周期 | 中 | tst_sectionworkbench*（间接） |
| src/workflow/aiassistworkflow.cpp | horizonGridFetch 全文 + 信号订阅面 | 高 | tst_aichattoolloop.cpp:50+（#610 加的生命周期测试） |
| src/workflow/faultinterpretationcontroller.cpp | ensureMapLayer 全文 | 低 | tst_faultinterpretation* |
| src/workflow/projectareasummary.cpp | snapshot() 全文 | 中 | tst_projectareasummary*（间接） |
| src/workflow/mappingevidencereview.cpp / mappingworkbench_edit.cpp | diff 逐行（文案级） | 低 | 既有文案断言 |

### 契约/接口级复核（增量交互方）
- io/dataimportservice.{h,cpp}、dataimport_session.cpp（commitImport 的
  cancelled/intent/恢复门）、dataimport_folder.cpp（produceFromPlan F1 门 +
  skippedPaths/forceImportPaths）、dataimport_file.cpp（expectedSourceSha256
  复核）、ingestplan.cpp（executeIngestPlan 取消口径、rollbackFailedRow）。
- domain/welllogfacies.cpp（normalizeDepthCurve #610 重写）、
  domain/wellnumeric.h（depthScaleToMetres 严格词表）、io/lasparser.cpp
  （depthUnitToMeters 三变体 + LAS indexBasis=MD 惯例 :220/:381）、
  io/welllogread.cpp（detect/parse 全文）、io/lascache.h（线程安全契约）。
- services/paleotaskservice.h（PaleoTask 状态机/failPublication/
  discardPublication/cancelAsked 契约）。
- ui/paleomainwindow_project.cpp（CancelImport/hooks 接线）、
  ui/paleomainwindow_workbench_wells.cpp（predictionLog 消费）、
  ui/pages/wellpredictionpanel.cpp（setLog 深度轴消费 :290-292）、
  ui/pages/constraintpage_generate.cpp（#618 IPO 门控→ConstraintWorkflow 契约）。
- app/aiwiring.cpp（horizonGridFetch 装配线程面）。

### src/ai 接口/线程面（本区间无 diff，契约面复覆盖）
- tileinference.cpp 全文（plan/softmax/stitch/runTileInference：输入签名门、
  #143 NaN 净化、输出形状校验、类数上限、取消契约）。
- onnxpredictionservice.h/.cpp 关键段（双锁 #479、会话池引用计数 +
  retire/release、m_inFlight 排空 #402、runTensorOn 模型绑定 #144）。
- remotepredictrouter.cpp 全文（runId 纪律、降级单线程池、C-15 关停守卫、
  响应体上限 #469）。
- wellfaciesservice.cpp 全文（generation 守卫、密钥脱敏、响应上限 #133、
  acceptedJob LRU #479、15 分钟轮询上限）。
- chat/llmclient.cpp finished 处理器段（C-6 修复验证 :444-445/:517）。
- aichatcontroller.cpp retryLast/sendRound（C-1 修复验证 :349-352/:448）。

### src/linkage（无 diff，全量四件复读）
- wellmaplink.cpp 全文（身份索引 + 失效钩子族 + rebind 恢复）、
  selectioncontext.cpp 全文（广播深度/合并 settle 有界 4 轮）、
  seismicmaplink.cpp 全文（双向选择同步 + 光标标记生命周期）、
  threewaylocator.cpp 全文。

### 跨模块/六页主线核查点
- 数据管理→catalog：folderimport staging→commitImport（epoch/baseSeq/冲突
  重produce ≤ kMaxImportAttempts；Cancelled/Failed → discard/failPublication
  分流）。
- 智能预测：mappingworkbench.predict →（m_wellRemote 在场）WellPredictionService
  prepare（代次守卫）→ finishPrediction 冻结请求校验 → 版本落库 →
  catalog.changed → synchronizeCatalogLayers（queued 合并）→ product.* 图层。
- 单因素图：constraintpage_generate（IPO 门控）→ ConstraintWorkflow
  （本区间未改，上轮已行级精读）。#618 的 mappingworkbench_edit 仅文案。
- 验证：wellpredictionreview predictionLog → wellpredictionpanel；
  consistencyImpl 后台报告（C-2/C-3 修复复核）。
- 上轮 C-4 复核：inversionworkflow.h:141 reclaimTempVolume + dock 三分支调用。

### 未核查区（如实声明）
- workflow 的 evolution/realization/datagraph/faciesclassify/derivation/
  faultsurface/welltopseditor/pythonconsole/officepreview/xmlpreview/
  storagegovernance/wellsiting/surfacegridding/paleomap* 家族：本区间零
  diff，上轮（03-review-C 20261021）仅做异步面/头文件盘点，本轮未重读
  （继承上轮「未逐行审」结论；其中 B.8/B.9/C-10 排除域不变）。
- ai/onnxpredictionservice.cpp 的模型注册表/加载全路径仅读关键段（#589
  EXCLUDED_EXISTING_ISSUE 域）；onnxfixture/modelregistry/horizonsuggest 未重读。
- ai/chat/domaintools.cpp、chatsession.cpp 未重读（上轮已覆盖，无 diff）。
- 未构建、未运行任何测试；全部结论基于静态读码 + 调用链论证。

## 2) Findings

本轮编号接续上轮 C-1..C-6（已修复，见 §3.1），自 C-7 起。

---

### C-7 predictionLog 对 LAS 文件不做米制归一——英尺井的预测对比井道与米制预测井段错轴
- **ID/Severity/Confidence**: C-7 / P3 / VERIFIED(机制) · HIGH(整体，触发需英尺 LAS 井)
- **Module**: src/workflow（wellpredictionreview）× 验证页
- **File:line/Function**: `src/workflow/wellpredictionreview.cpp:35-50`
  `MappingWorkbench::predictionLog()`；消费面 `src/ui/pages/wellpredictionpanel.cpp:290-292`；
  对照归一化权威 `src/workflow/wellpredictionservice.cpp:300`（normalizeDepthCurve）
- **Root Cause**: #603 重写 predictionLog 时给**非 LAS**（explicitAxis：XML/CSV/TXT
  等）格式加了「MD 基准 + 米/英尺单位强校验 + ×scale 归一到米」；LAS 分支
  保持 `WellLogRead::parse` 原样返回——深度单位不换算（FT 井保持英尺原值）。
  但同 PR 里 WellPredictionService 的输入管线对 LAS 与非 LAS **都**经
  normalizeDepthCurve 归一到米（`depthUnitToMeters`：FT→0.3048，
  welllogfacies.cpp:584-589），落库的 `facies_intervals`/formation 区间是米。
- **Trigger**: 任一预测井的测井版本是英尺 LAS；用户在验证页（井预测复核面板）
  打开该井的曲线对比。
- **Observed/Expected**:
  - Observed: `wellpredictionpanel.cpp:290-292` 用 `m_log.curves.first().values`
    （英尺）与区间 top/bottom（米）同轴过滤——英尺深度恒大于米区间界，
    曲线样本全部落在区间外，井道空白/错位。
  - Expected: 与 explicitAxis 分支同口径：LAS 深度道 `depthScaleToMetres`
    归一（LAS 基准按行业惯例已是 MD，lasparser.cpp:220/:381，仅需补单位换算）。
- **Reproduction**: 构造 DEPT 单位 FT 的 LAS 井 → 智能预测（成功，米制区间）→
  验证页打开井预测复核并请求曲线（`paleomainwindow_workbench_wells.cpp:57`
  → setLog）→ 曲线道与相带条错轴。
- **Impact**: 英尺 LAS 井在验证页的「曲线 × 预测相带」复核不可用（视觉级错误，
  不损坏数据）。LAS 英尺半边是**既有缺口**（旧 LasParser::parse 同样不换算），
  但本 PR 重写该函数且已为另一半补齐，缺口成为函数内显性不对称。
- **IssueMatch/PRMatch**: 无冲突（上轮 C 系无此条；#603 diff 即本函数）。
- **FixStrategy**: LAS 分支补三行：`depthScaleToMetres(curves.first().unit)`
  为 nullopt 时按现有文案拒收；否则 `values × scale`、unit 置 "m"（与
  explicitAxis 分支共用同一小段，顺带消掉 detect 二次调用，见 C-8）。
- **RegressionTest**: FT-LAS fixture 断言 predictionLog 返回 unit=="m" 且
  首道值 ×0.3048；再断言 panel 区间过滤命中样本数 > 0。

### C-8 WellLogRead 全部新调用点双重内容嗅探——每文件每轮多 4-6 次文件打开/头读
- **ID/Severity/Confidence**: C-8 / P3 / VERIFIED
- **Module**: src/workflow（#603 接线面）× src/io 契约
- **File:line/Function**（六处同型，外层 detect + parse 内部再 detect）:
  - `wellpredictionservice.cpp:274-280`（worker 读曲线）
  - `wellsectionworkflow.cpp:377-381`（loadCurveBodies）
  - `mappingworkbench_predict.cpp:518-520`（prepare worker 读深度范围）
  - `wellpredictionreview.cpp:35-37`（predictionLog）
  - `propertymodelworkflow.cpp:~1163-1166`（requestFromCatalog）
  - `sectionworkbench.cpp:414-416`（sectionWells）
  - 根因面：`io/welllogread.cpp:164`（parse 内部 `detect(path)`）+
    `detect()` 自身做 DlisParser::sniff + LisParser::sniff（各再开一次文件）
    + 64KB 读。
- **Root Cause**: #603 的接线模式是「先 `detect(path) != Las` 判 explicitAxis，
  再 `parse(path, ...)`」——而 parse 内部第一步又是 detect。每份井曲线文件
  每次读取做两趟完整嗅探（≈2×(2-3 次 open + 64KB read)），之后才是真正解析。
- **Trigger**: 任何连井剖面取数、测井预测准备、属性建模装配、剖面叠加、
  预测对比曲线加载——含多井后台批量路径（wellpredictionservice worker 按
  井×曲线引用逐文件）。
- **Observed/Expected**:
  - Observed: 嗅探成本 ×2；对 DLIS/LIS 文件 sniff 还要读带标头（两趟）。
  - Expected: 调用方已持有 format 时不再重复嗅探——建议 WellLogRead::parse
    增加 `WellLogFormat` 入参重载（或 detect 结果透传），六个调用点改用。
- **Impact**: 纯性能（后台池为主，GUI 线程的 predictionLog/
  requestFromCatalog 每次也多一趟 64KB+嗅探 IO）；无正确性影响。
- **IssueMatch/PRMatch**: 无冲突（io 层属 Agent B 域，此处只登记 workflow
  接线侧放大）。
- **FixStrategy**: 见上；与 C-7 修 predictionLog 时顺带收敛其一。
- **RegressionTest**: 现有 parse/detect 单测不回归即可；可加计数钩子断言
  单次读文件 open 次数。

### C-9 #603 五条 MD/单位硬拒绝路径（workflow 层）零测试覆盖
- **ID/Severity/Confidence**: C-9 / P3 / VERIFIED
- **Module**: src/workflow × tests
- **File:line**: `wellsectionworkflow.cpp:383-387`、`mappingworkbench_predict.cpp:522-525`、
  `propertymodelworkflow.cpp:1166-1181`、`wellpredictionreview.cpp:40-45`、
  `sectionworkbench.cpp:417-421`（五处新 `indexBasis != MD`/未知单位拒收文案）
- **Root Cause**: io 层（tst_welllogtext/tst_dlisparser/tst_lisparser）覆盖了
  indexBasis 解析，但 workflow 层「拒收→如实报错→UI 可见」的接线无任何断言
  （`grep -r "声明米或英尺\|MD 测深" tests/` 零命中）。这些路径是 #603 的
  核心行为变化（此前非 LAS 曲线被 LAS 解析器静默吞掉）。
- **Trigger**: 回归风险——后续重构（如 C-7 修 LAS 归一时顺手改分支）无护栏。
- **Observed/Expected**: Expected 每条路径至少一例「坏输入 → 错误文案可达
  （warnings/skip/errorOccurred 通道）」断言。
- **IssueMatch/PRMatch**: 无冲突。
- **FixStrategy**: 五处各加一例（fixture 已有 io 层样例可复用）。
- **RegressionTest**: 即本条本身。

### 观察项（不计入 Findings，供收敛决策参考）

1. **文件夹导入取消语义翻转（#603 有意变更，建议产品侧确认）**：
   旧口径「取消后已处理的行保留入库」（旧 folderimport.h 注释、旧
   executeIngestPlan「已取消（已入库的行保留）」）；新口径 `session.cancelled`
   门（dataimport_session.cpp:426-433）+ finished 复核（folderimport.cpp:140-149）
   = **取消/worker 失败一律整批丢弃**，文案「已取消，本批数据未入库」。
   有意设计（窄窗复核注释 + tst_folderimport 三例新测试背书），非缺陷；
   但用户可见行为变化（长批次中途取消丢已处理行）值得 UX 侧过目。
   另注：`importFailed` 信号全仓无 UI 消费者（仅 io/workflow 内部 emit），
   取消时 failSession 逐行补发的 importFailed 实际无外部副作用。
2. **aiwiring.cpp:74-75 注释过期**：仍写「horizonGridFetch 每次取数重解析
   声明」；#610 后实为信号驱动的路径快照（layerDeclared/horizonReleased
   刷新），快照过期由闭包内 `QFile::exists` 兜底如实报「未找到」。建议改注释。
3. **空单位 LAS 的模块间接受面分歧（既有、代码内有按模块注明的口径）**：
   wellpredictionservice 按 米（welllogfacies 变体空单位→1.0）、
   propertymodelworkflow 旧口径按米（#166 注释）、wellsectionworkflow
   拒收（depthScaleToMetres nullopt）。模块内自洽、模块间不一致——非本轮
   引入，登记备查。
4. **importActiveChanged 队列翻动（既有）**：makeImportJob 入队即 emit(true)、
   finish emit(false)——两作业排队时 true,true,false,false，中间窗口
   m_folderImportActive=false（仅影响 paleomainwindow_attach_data.cpp:305
   预览标签门，无功能后果）。若后续做导入并发收紧可改计数口径。
5. **SeismicMapLink::onContextSelection 全表扫描**（seismicmaplink.cpp:185-193）
   对照 WellMapLink 的身份索引（wellmaplink.cpp:106-140）未做索引化——
   测线层基数小（百级）可接受，若未来接入大道数层需同构索引。
6. **LasParser::depthUnitToMeters / paleo::wellnumeric::depthScaleToMetres /
   welllogfacies.cpp depthUnitToMeters 三个近同名函数三种空单位/未知单位
   语义**（0.0 / nullopt / 1.0+NaN）——本区间新增了跨三者的调用组合
   （mappingworkbench_predict 用 LasParser 变体、wellpredictionservice 用
   wellnumeric 变体），语义选择在各调用点均正确，但三体并存是后续错配
   温床，建议收敛到 wellnumeric 权威词表。

## 3) 模块健康结论

### 3.1 上轮 Findings 修复复核（full-audit-20261021 → b301f2e8）

| 上轮 ID | 现状 | 证据 |
|---------|------|------|
| C-1 retryLast 漏 tools | ✅ 已修 | aichatcontroller.cpp:349-352 收编 sendRound()（:448 带 tools()） |
| C-2 一致性报告 /tmp rename EXDEV | ✅ 已修 | mappingworkbench_consistency.cpp:230-247（报告目录内暂存 + 同 FS rename，带审计注记） |
| C-3 导出 mutationSeq 误陈旧 | ✅ 已修 | mapexport.cpp:403、horizonbatchexport.cpp:529、mapbookqueue.cpp:280 三处判据摘除（session/目录守卫保留） |
| C-4 反演临时体滞留 /tmp | ✅ 已修 | inversionworkflow.h:141 reclaimTempVolume + seismicsectiondock_inversion.cpp:239/244/261 三终态分支回收 |
| C-5 岩性查询路径写 catalog | ✅ 已缓解 | datacatalog_versions.cpp:305 map 重载补 sameValue no-op 短路（写点本身保留在 wellsectionworkflow.cpp:865，重复刷新不再抬 mutationSeq） |
| C-6 llmclient 非流式 tail | ✅ 已修 | llmclient.cpp:444-445（tail 并入 m_body）+ :517 |

### 3.2 本轮增量质量评价

- **folderimport（#603 重构面）**：取消句柄（weak_ptr + atomic_bool +
  QPointer<PaleoTask>）线程模型正确（GUI 建句柄/worker 只读 atomic/终态
  finished 标志防迟调用）；Conflict→重 produce 有界（kMaxImportAttempts）；
  提交失败现在分流 discardPublication/failPublication（旧版提交失败任务页
  仍显成功——顺手修复的一处静默）；early-cancel、服务关闭、代次切换各分支
  均有诚实收口。测试覆盖良好。
- **wellpredictionservice（#603+#610）**：worker 读曲线的 readErrors 索引
  链正确（data.size() 即当前井序，单线程写 + QFutureWatcher 同步读）；
  normalizeDepthCurve（排序/去非有限/同深度去重/单位别名）带 6 例单测；
  代次守卫完备（prepare finished/keystore 回调/start 三处 generation 比对）。
- **aiassistworkflow horizonGridFetch（#610）**：池线程闭包只持
  mutex+shared_ptr 快照，主线程信号刷新，服务析构自动断连——上轮点名的
  「池闭包触主线程对象」类风险在该点已消除；aiwiring 装配在主线程（核实）。
- **faultinterpretationcontroller（#610）**：无工程时 setParent(this) 堵
  memory 图层泄漏；m_mapLayer 是 QPointer，工程清理防悬垂。注：QgsProject::
  instance() 单例几乎恒非空，else 分支现实近似死代码（防御性，无害）。
- **projectareasummary（#613）**：seen 集大小写不敏感（toUpper），configured
  短路正确；`maps.value(h)`/`stages[h]` 仍按原大小写查（写入侧为规范
  层位名，理论面一致，登记为低风险备注）。
- **ai/ONNX（无 diff 复覆盖）**：tileinference 契约面（输入签名门/NaN 净化/
  输出形状校验/类数上限/取消）健壮；onnxpredictionservice 双锁 + 会话池
  引用计数/退休/排空设计正确；router runId 纪律与降级单线程池、
  wellfaciesservice 密钥脱敏与响应上限均到位。#589（锁外读模型名）
  维持 EXCLUDED_EXISTING_ISSUE。
- **linkage（无 diff 复覆盖）**：SelectionContext 广播合并有界（4 轮上限 +
  超限 qWarning）；WellMapLink 身份索引带失效钩子族；SeismicMapLink 双向
  同步 QSignalBlocker 防乒乓。未见新问题。
- **六页主线**：导入→预测→单因素→编图→验证的状态流转与页面间数据一致性
  在契约层成立（冻结请求校验、catalog.changed 合并同步、staging-journal
  事务、代次守卫）；本轮唯一的主线级数据一致性缺口即 C-7（验证页英尺井错轴）。

### 3.3 总体结论

**增量面（20 PR、13 个 workflow 文件）无 P0/P1/P2 级新发现**；上轮四项 P2
全部带测试与审计注记落地。新立案 3 项 P3（C-7 英尺井对比错轴 / C-8 双重
嗅探 / C-9 拒收路径零测试），另 6 条观察项供收敛决策。workflow/ai/linkage
三模块在「失败诚实、代次守卫、线程边界」三条主纪律上处于本仓最好水平；
最值得跟进的收敛项是三个深度单位换算函数的三体并存（观察 6）与取消语义
翻转的产品确认（观察 1）。

---

*Agent C，2026-10-22。方法：Read 精读 + git diff/调用链追踪；未修改任何源文件，
未执行构建/测试命令。*
