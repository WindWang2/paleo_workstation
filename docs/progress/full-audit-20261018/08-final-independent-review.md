# Independent Final Review（full-audit-20261018 · 终审）

- 终审员：Independent Final Reviewer（未参与 Fix 1/Fix 2 任何编写，第三轮独立复核）
- 复审对象：`git diff 366aef97...HEAD` 全量（59 文件，+4423/-61；**实际 15 个 commit**——11 个
  Fix 1 修复 + 1 个审计交付件 + 1 个行尾清理 + Fix 2 的 1 个窄字符修复 + 1 个二次处置；
  任务书口径「13 个」与 git log 实数有出入，以 git log 为准）
- 方法：从 HEAD 出发独立通读全量 diff 与修复落点周边源码（含未改动消费方、测试夹具、
  CMake 接线、main.cpp）；前两轮报告（06-*-r2*.md）仅用于避免重复劳动，其结论均独立重验。
- 限制（如实声明）：按铁律未编译、未跑 ctest（主 Agent 持锁）。执行绿灯以
  07-fix-r2-results.md 验证汇总节的主 Agent 数据为准；本报告对每项测试独立核验的是
  「测试函数在位 + 断言实质有效 + 红绿方向逻辑成立」。静态检查器
  `tools/check_layering.py --strict` 已实跑通过（非编译动作）。

## 结论速览

| 分类 | 数量 | 编号 |
|---|---|---|
| 新 MUST_FIX | **0** | — |
| 新 SHOULD_FIX | 0 | — |
| 新 NOTE/DOC 级 | 4 | FF-N1…FF-N4（均不阻断） |
| 终审结论 | **PASS-WITH-NOTES** | 见文末条件 |

17 项修复簇（FC-001…FE-005 中 15 项有代码 + FC-004/FD-005 RECORD-ONLY）全部在位、
治根因、带真实回归测试；Fix 2 的 1 处 MUST_FIX + 5 处 SHOULD_FIX 逐处核实已落。
本轮四组对抗挑战（a-d）全部推翻失败——未发现任何被前两轮遗漏的功能性缺陷。

---

## 一、终审清单逐项结果（1-10）

### 1. 整体 diff 复核 ✓

全量 diff 按 commit 分组通读：11 个 Fix 1 修复 commit 意图与登记表一一对应；
Fix 2（7a5d6f41）与窄字符修复（52cb4717，d0a23078 自引入的 `QLatin1Char('、')`
窄字面量编译错，clang-tidy 门禁拦截后收口为 `QChar(0x3001)`）自洽；
2 个 docs commit 只动本目录交付件。删除的断言共 4 行，全部属「永真/自证 → 真值」
收紧方向（FE-001/FE-002），无既有断言被弱化。新增 src 改动无窄字符多字节字面量残留。

### 2. 已修复 Finding 逐项验证（对 HEAD 源码 + 测试）✓ 全部 15+2 项

| ID | 修复在位（HEAD 源码） | 测试在位且断言实质 |
|---|---|---|
| FC-001 P1 | predictionworkflow.cpp:264-275 冻结（source/provider/crs.toWkt/isRaster 四元组快照 + `params.remove("INPUT")`）；:391-411 worker 以 `QgsRasterLayer/QgsVectorLayer(uri,name,provider)` 回放 + `setCrs(fromWkt)`，与 runFactorAlgorithm（constraintfactorjobs_factor.cpp:1194-1210）逐参同款 | tst_workflows.cpp:360-410：prepare 后 `!params.contains("INPUT")` + `!inputUri.isEmpty()` + 真 worker 线程全链成功 + `threadViolationCount` 零新增——断言实质有效 |
| FB-001 P2 | faciespolygonize.cpp:169-186 新裁决（最高计数胜/并列保中心/挑战者并列取小码）——本轮独立重推迭代序无关性（不变式论证与 R2B 一致，另行手工重算 3×3 夹具逐格结果 `{3,3,3/3,3,7/7,7,7}`，面积 {3:5,7:4} 与测试 golden 逐格吻合） | tst_algorithms.cpp:826-899 六种子逐位一致（图斑码序/面积/WKT/归一化 PROVENANCE）+ golden 面积集（Fix 2 R2-SF-01 后方向已钉死） |
| FA-001 P2 | outsourceworkbook.cpp:102 函数级 cellBudget + finishRow 计账；wellcompositexml.cpp:93/937 两读口同口径；xlsx 路径预算已上移到 sheet 循环外（Fix 2 R2-MF-01） | tst_io_outsource::rejectsCellFillAmplification（300 行 × ss:Index=16384）+ rejectsMultiSheetXlsxAmplification（3 表 × 244 行 XFD，zipWriteArchive 真构造，断言「整簿拒收」）+ tst_wellfacies::comprehensiveXmlRejectsCellFillAmplificationAndOversize |
| FA-002 P3 | 三处 readAll 前置 512MB 闸（outsourceworkbook.cpp:751-757、wellcompositexml.cpp:69-77/916-924）；xlsx 由 zip 层三重闸把关（核实在案） | rejectsOversizeSpreadsheetMl + tst_wellfacies 同款（稀疏 resize，闸在 readAll 前） |
| FB-002 P2 | sgs.cpp:378-385/sgs3.cpp:688-695 以 `workersForPeakBudget` 收缩；sgs_internal.h:33-49 kPeakBudgetBytes=6GiB + 纯函数；峰值=输出张量+workers×工作区（2D 24B/cell、3D 80B/target 保守上估） | tst_geostat_sgs::parallelWorkerBudgetShrinksWorkers 四档直测（{4,1e8}→1、{4,400}→4、{4,5.2e7}→3、请求 1 恒 1）——数值经手算复核 |
| FB-003 P3 | sgsfill.cpp:123-139 聚合闸在 `out->assign` 之前；全程 int64 乘法无溢出面 | tst_sgsfill::aggregateOutputBudgetRejected（1024×1024×2047×R2≈17GB>4GiB，断言文案+out 空） |
| FD-001 P2 | evolutionplayerpanel.cpp:106-120 reloadFrames（含 Fix 2 的集合外/无激活统一复位 m_index=-1 + 标签清空）；paleomainwindow.cpp:492-495 与 chip 条同点位 | tst_evolutionplayer::reloadFramesFollowsProjectVocabulary（AreaRules::setProjectDir 换装第二套词表 → frameOrder 跟随 + 步进不越出 K1/K2） |
| FA-003 P3 | segyreader.cpp:1111-1128 分片失败按 `sh.cancelled` 分流：取消落 checkpoint（语义原样），io 错误不置 partial | tst_segy.cpp:670-719 截断注入：首进度回调 compare_exchange 确定性触发（各分片第 0 道即回调，时序已独立推演成立），断言 openCached 成功 + `!lastScanPartial` + traceCount=2400 |
| FA-004 P3 | catalogstore_json.cpp:282-289 `ord > links.size()+4096` 判损坏跳过（int64 比较无溢出；ord==size+4096 边界值经推演安全，resize 上界 size+4097） | tst_catalogstore::corruptLinkOrdSkippedNotHugeResize（真建库 + SQL 直插 ord=2e9，断言开库成功且好行完好） |
| FC-002 P3 | mapversioncontroller.cpp:336-346 retain 失败改 qWarning，不再「成功返回值+非空 error」；全仓确认同步 saveVersion 无生产调用方 | 既有 tst_versions retain 族回归（不依赖被删除的自相矛盾态） |
| FC-003 P3 | mapversioncontroller_async.cpp:416-456 legacy 来源十信号守卫 + :459-462 commit 前 stale 复查先于一切；busy 闸（:406 `snapshotBusy()`）保证 startPublish 重入被拒、`disconnectSourceWatches` 无误拆在飞任务窗口 | tst_versions.cpp:1141-1194 worker-gate（QSemaphore 持唯一线程）注入确定性编辑窗，断言发布失败 + 文案点名 + isPublished false |
| FD-002 P3 | previewmaptools.cpp:123-129 deactivate 藏带 + 基类调用 + m_points 保留（与 identify/profile 清理契约同款） | tst_previewmap_tools::measureDeactivateHidesBandAndKeepsPoints（真 QGIS 切换链 activate(pan) 触发 deactivate，续测三点齐） |
| FD-003 P3 | previewmapcanvas.cpp:108-119 置换摘除三层记账（仍在新表的层经 contains 守卫保留）；`std::as_const(m_layers)` 遍历中只改三个 QHash，无迭代器失效 | tst_previewmap_canvas::setLayersReplacementDropsReplacedBookkeeping（置换出→回表取默认 + 同表重排保持两翼） |
| FD-004 P3 | nativeattributetablehost.h:41-48 seam（std::function，无 ui/ include）；.cpp:98-115 seam 判定先于 offscreen 早退（Fix 2 R2-SF-04） | tst_edittools::attributeTableHostAskSeamDrivesSaveDiscardCancel（Save/Discard/Cancel 三分支真驱动 + 未注入 offscreen 直存回归） |
| FE-001 | tst_factorworkflow.cpp:429-443 六枚举双向往返 + 期望字面量（与 litholexicon 生产值逐一核对） | 即测试本体 |
| FE-002 | tst_perf.cpp 两槽绑生产真值：`workerPool()->maxThreadCount()==4`（paleotaskservice.cpp:220 实证）、`gridDimsForExtent` 2048 精确整除 + 1e10>1e8 必抛（paleoalgorithms.cpp:216-221 throw 实证）；CMakeLists.txt:1116 增链 paleo_services/paleo_algorithms 两目标均在（:422/:558） | 即测试本体，断言可变异变红 |
| FE-003 | make_appimage.sh:32-45 钉 20251108 + sha256sum -c（`set -euo pipefail` 下失败即断）；R2 已独立下载核验摘要逐位一致 | 脚本静态自检（URL 非 continuous + 校验行在场）——合理 |
| FE-004 | dataimport_document.cpp:300-311 QTemporaryDir 随机名 + isValid 失败 `finishConversion(-1)`（R2-SF-05）；dataimportservice.h:324-327 成员保活 | tst_import::documentConversionProfileDirIsRandomAndCleanedUp（断言不含 pid 可预测名 + 析构零残留） |
| FE-005 | resources/translations.qrc 物理删除；基线无任何 CMake 引用（本轮重新 grep 证实）；extra-ux.cmake:64-70 防再犯注释；Q_INIT_RESOURCE(translations)（main.cpp:168）与 qt6_add_resources(paleo_ui "translations") 目标名对应正确 | configure 无回归（主 Agent 构建数据） |
| FC-004 / FD-005 | RECORD-ONLY 确认未动：compositionworkflow.cpp/attach_compose 不在 diff 文件清单；paleomainwindow.cpp 仅动 include 与 onProjectOpened 两处 | —（按登记表口径） |

### 3. Fix 2 六处二次修改复核 ✓

| ID | HEAD 现状核对 |
|---|---|
| R2-MF-01 | cellBudget 声明位于 sheetTargets 循环**外**（outsourceworkbook.cpp:481-482），三读口同为整簿累计口径；多表回归夹具在位且单表 3,997,696<4M 恰在旧缺陷放行界内（红绿方向独立核算成立） |
| R2-SF-01 | golden 面积断言（3 面 5.0 / 7 面 4.0）在位；本轮逐格重算 3×3 夹具 SMOOTH=1 输出栅格，与 golden 完全一致 |
| R2-SF-02 | reloadFrames 的 else 分支统一 `m_index=-1` + `m_frameLabel->setText(QString())`（evolutionplayerpanel.cpp:113-119），覆盖 selection 在场（生产路径）与不在场两分支 |
| R2-SF-03 | publish 的 jobCompleted（:483）与 `!task`（:492）两分支均已补 `disconnectSourceWatches()`，与 save 路径 :382/:392 对称 |
| R2-SF-04 | seam 判定 `if (m_saveDiscardAsk && allowCancel)` 排在 offscreen 早退之前（nativeattributetablehost.cpp:98）；未注入时 offscreen 直存语义不变；allowCancel=false 保持原生两键路径（防语义漂移的既定取舍） |
| R2-SF-05 | `if (!m_convProfileDir) m_convProfileDir = make_unique<QTemporaryDir>(...)` + `if (!isValid()) return finishConversion(-1)`（dataimport_document.cpp:303-311） |

### 4. 文件生命周期检查 ✓

- 增删清单：A=16 个审计文档、D=resources/translations.qrc（唯一删除，基线零引用）、M=42；
  **无其他意外增删**，无二进制/临时文件混入。
- connect/lambda 生命周期（本轮新增面逐一核对）：
  - FC-003 守卫 10+ 连接：接收者恒 `this`（控制器）——控制器析构自动断连；lambda 只捕
    `stale` shared_ptr；layer 先亡则 destroyed 既标记又自清连接。`m_async` 惰性创建后
    永不 reset（全仓核对），jobCompleted lambda 内解引用安全。
  - FD-001 reloadFrames：无新连接（onProjectOpened 接收者=主窗，findChild 判空）。
  - FD-004 seam：std::function 存于 host（panel 子对象，同生共死），lambda 捕 this=panel，
    PaleoNotify 父窗口即 panel——无悬垂面。
  - FE-004：`m_convProfileDir` 成员先于 QObject 子（m_convProc）析构的瞬态窗口仅在
    「析构时转换在途」出现，~QProcess 随即终止进程，无功能影响（既有 documentTeardown
    测试钉住析构不阻塞）。
  - tst_edittools seam lambda 捕局部引用，作用域内使用——安全。

### 5. 跨模块调用检查 ✓

- FC-001 调用方兼容：生产唯一入口 paleomainwindow_attach_predict.cpp:79 只发标量参数
  （页面无 INPUT 输入面），INPUT 由 prepare 内自动解析（instantiate 的文件层）→ 冻结；
  worker lambda 调 `computePredictionJob(job.get())` 与新契约吻合；ONNX 分支在冻结段之前
  早退不受影响；父版本溯源改读 `job->inputUri.section('|',0,0)` 与旧口径同段。
- FD-004 两个消费点：生产 attributetablepanel.cpp:50-64（注入 PaleoNotify::askSaveDiscard，
  枚举映射 switch 全 case 无 default——漂移即 -Wswitch 红）；测试 tst_edittools 注入点。
  host 的 stopEditing 另经 QgsVectorLayerTools 接口（nativeattributetablehost.cpp:24-25）
  被原生属性表视图回调——allowCancel=true 时同样走 seam，口径统一。

### 6. 性能热点对照 ✓（无新 O(N²) 退化）

- setLayers 摘除循环 O(n·m)（`std::as_const(m_layers)` × `layers.contains`）——预览画布
  层数几十量级，且循环体只改三个 QHash、被遍历的 m_layers 循环后才整表赋值，无迭代器失效。
- 预算检查 O(1)/行；FB-001 计数循环 O(9)/格（原样）；FB-002 预算纯函数 O(1)；
  golden 比较只在新测试内（有界）；reloadFrames O(frames)。
- xlsx 整簿预算使多表恶意簿在表 2 中途即拒收——拒收路径本身 O(已读)。

### 7. 架构分层抽检 ✓

改动 24 个 src 文件头三行层标记齐全（本轮逐一核对 + `tools/check_layering.py --strict`
实跑「检查通过：无层违规」）。重点五文件：predictionworkflow.cpp（功能层新增
qgsrasterlayer/qgsvectorlayer/qgscoordinatereferencesystem——非 QtWidgets、非 ui/，
与 constraintfactorjobs_factor.cpp 既有先例同款）；nativeattributetablehost.h（QGIS 封装
仅增 `<functional>`，无 ui/ include）；attributetablepanel.cpp（视图引 qgis/ 不在禁令表）；
evolutionplayerpanel.*（视图）；sgs_internal.h（数据）。FE-002 测试增链不破坏
add_paleo_test 沙箱/LABEL 契约（paleo_services 属 `^paleo_[a-z]+$` 正则命中面）。

### 8. PR/Issue 冲突复查 ✓（详见挑战与风险节）

#518/#521 文件交集 6 文件，hunk 级逐一比对：除 tst_edittools.cpp（对 #521）为
**真文本冲突**（双方同锚点加 include/加 slot 声明，加法对加法，手工解法显然）外，
其余全部错开可自动合并。另发现基线后新开 PR #522/#523（#523 与本分支仅 CMakeLists.txt
交集且 hunk 错开：523 在 :492/:557/:596/:689/:1239，本分支在 :1113）。

### 9. 仓库卫生 ✓

`git status` 干净；`git diff --check` 零输出；diff 无 vendor/ 路径；新增文件全为
.md/.txt 审计交付件（issues 快照为公开仓库数据，无隐私）；无意外生成物。

### 10.（并入上文 2/3 项的逐 Finding 验证，无遗留）

---

## 二、挑战结果（a-d，全部推翻失败）

### a. 构造极端输入推演

- **FC-001 带 CRS 变换需求的算法**：worker 自建层的 `setCrs(QgsCoordinateReferenceSystem::fromWkt(wkt))`
  覆盖「层自身 CRS」；需要坐标变换的 paleo 算法（distancetransform.cpp:121、
  mincurvature.cpp:162、paleoalgorithms.cpp:427）全部经
  `QgsCoordinateTransform(from, to, context.transformContext())` 取变换上下文，而
  QgisProcessingService::run 在起点统一注入 `m_transformSnapshot`（工程 transformContext
  快照，qgisprocessingservice.cpp:645-660，注释即 #235-2 成文契约）。**层 CRS 走快照、
  变换走 context 注入——两条链都闭合，推翻失败**。无效 CRS（toWkt()==""）时跳过 setCrs
  回落文件内嵌 CRS（更合理方向，R2 已记 D3）；memory provider 不等价面在库不可达
  （OBS-02 口径维持）。
- **FB-001 的 kNodata 参与并列**：任务书假设 kNodata=-1——实际 kNodata=-9999
  （faciespolygonize.cpp:54），且中心 nodata 在计票前 `continue`（:153）、邻居 nodata
  在计票处 `if (v != kNodata)` 排除（:164）——**nodata 既不投票也不参选，无并列面；
  挑战前提不成立，推翻失败**。
- **FA-004 ord 恰为 links.size()+4096**：`ord > size+4096` 为严格大于——恰等边界不跳过，
  `resize(ord+1)` 至 size+4097（int64 比较、int 上界内）；ord=INT_MAX 必被拦。**边界行为
  有界且确定，推翻失败**。

### b. 快速取消/重试推演

- **FC-003 startPublish→立即再 startPublish**：startPublish 入口 `snapshotBusy()`
  （preparation || runner.busy()，:154/:406）——首个任务在飞（含排队）即拒第二个，
  「拆掉别人 watches」的窗口不存在；`!task` 启动失败分支也已补 disconnectSourceWatches
  （R2-SF-03）不悬挂。**推翻失败**。
- **FA-003 取消路径回归（openCached 全流程重读）**：取消分片 → `m_lastScanPartial=true`
  + 连续前缀 `m_scannedOffset`（分片序合并、首个失败分片按文件序裁决——与线程完成时序
  无关，本轮重读合并循环证实）；openCached :1431-1437 对 partial 快照照旧存 checkpoint
  返回 false。io 错误分片 → 不置 partial → :1438-1441 `resetState()+file.seek(0)` 落
  顺序 `open()`——此前死码分支现在真实可达（本轮新证）。取消与 io 混合时按文件序首个
  失败分片分类，两口径各自确定自洽。**checkpoint 语义未被改坏，推翻失败**。

### c. 输入空数据/损坏数据

- **FA-001 空 Table**：无 Row 事件 → 不计账、sheet 空完成（既有语义）；**无 Data 子节点的
  Cell**：SpreadsheetML 分支在 Cell 元素上按 ss:Index padTo（:288-291，空文本入格）——
  驻留即计费，预算口径与内存严格一致；xlsx 分支同理（`<c r="XFD1"><v>x</v></c>` 无 t 属性
  → 非共享串非错误 → padTo 16384 槽，测试夹具正是此形态且已被整簿预算拦截——本轮沿
  readXlsx :559-657 逐行核实）。**截断 XML（无 EndDocument）**：readSpreadsheetMl 循环外
  只有 finishSheet（丢弃在途行、不计账）——错误路径零过冲；readXlsx 循环外 finishRow
  在 hasError 分支前计账，最坏过冲一行（≤16384 格≈128KB）且预算跨 sheet 存续、下表
  首查即拦。**推翻失败**。
- **FB-003 nk=1**：cellCount64=ni·nj·1，int64 乘法，闸值语义不变；无除零/特判面。**推翻失败**。
- **FD-001 空词表工程（sequence_boundaries: []）**：mappingHorizons() 空 → reloadFrames
  走 else 复位（m_index=-1、标签清空）；stepBy 的 `next>=m_frames.size()` 守卫使步进
  无操作；applyFrame 的 indexOf==-1 早退。**空集合全链安全，推翻失败**（若 AreaRules
  schema 本身拒绝空数组，setProjectDir 失败、词表不变——同样安全）。

### d. 旧行为对比

- **FE-002 tst_perf 因环境失败？**：tst_perf 链 paleo_qgis/paleo_services/paleo_algorithms
  ——三库均不嵌翻译资源（qt6_add_resources 只挂 paleo_ui），测试体无任何 i18n 依赖；
  PaleoTaskService 构造不触 QSettings（且每测试独立 XDG/HOME/TMPDIR 沙箱）；
  gridDimsForExtent 的 2048 精确整除（10/2048 二进制精确）与 1e10>1e8 必抛
  （paleoalgorithms.cpp:216-221 throw QgsProcessingException 实证）均环境无关。**推翻失败**。
- **FE-005 删 qrc 后 i18n 初始化**：main.cpp:168 `Q_INIT_RESOURCE(translations)` 对应
  extra-ux.cmake:67 `qt6_add_resources(paleo_ui "translations" ...)` 的
  qInitResources_translations——目标名对应正确；被删 qrc 基线零 CMake 引用（本轮
  `git grep` 基线复核），删除无构建影响。残留面：lrelease 缺失时 extra-ux 不注册该资源
  而 main.cpp 无条件调用 Q_INIT_RESOURCE → 应用链接期 undefined reference——**该风险
  基线即存在（同构），非本轮引入**，记入剩余风险（FF-N3）。

**四组挑战合计：推翻成功 0 项；新 MUST_FIX 0 项。**

---

## 三、新发现（本轮终审独立提出，均 NOTE/DOC 级）

| ID | 级别 | 位置 | 摘要 |
|---|---|---|---|
| FF-N1 | DOC（审计文档更正） | 07-fix-r2-results.md OBS-06 / 06-r2b OBS-06 | 「tst_import 扫共享 /tmp、TMPDIR 不在每测试沙箱之列」与事实不符：paleo_test_sandbox 对非 Windows 每测试注入 `TMPDIR=${_home}/tmp`（CMakeLists.txt:885-890），ctest 下 QDir::temp() 指向私有沙箱，`after.size()==0` 无互扰；脏 /tmp 假红仅限**裸跑测试二进制**的开发机场景。R2 的 FE-004 节（正确版）与 R2B OBS-06（错误版）相互矛盾，应以前者为准 |
| FF-N2 | NOTE（合并序风险） | tests/tst_edittools.cpp | 本分支与 PR #521 在同两处锚点各自追加（include 块 :34-39、private slots :130-136）——**真文本冲突**（非语义冲突：双方均为加法，保留两侧即解）。本分支先合则 #521 rebase 手解；反之亦然。其余 5 个交集文件（CMakeLists/paleomainwindow/tst_import/tst_versions 对 #518，CMakeLists 对 #521）hunk 全部错开可自动合并 |
| FF-N3 | NOTE（既有环境债，非本轮引入） | src/app/main.cpp:168 × cmake/extra-ux.cmake:38-73 | 无 lrelease 环境下 `Q_INIT_RESOURCE(translations)` 无定义符号 → 应用链接失败。基线同款结构（qrc 从未被引用），审计未引入也未恶化；后续可用条件编译或强制 lrelease 依赖收口 |
| FF-N4 | DOC（归属口径） | 本目录台账 | 任务书/台账称「13 个修复/文档 commit」，git log 实为 **15** 个（11 修复 + 2 docs + Fix 2 的 2）；另基线后新开 PR #522（codex/facies-display-lock，9 文件零交集）与 #523（codex/data-map-export，42 文件，仅 CMakeLists 交集且 hunk 错开）——登记表「开放外部工作」应补充 |

（曾怀疑 readSpreadsheetMl 截断 XML 末行计账逃逸预算——循代码证实循环外仅 finishSheet
丢弃在途行，错误路径零过冲，自查撤销。）

---

## 四、终审结论

### **PASS-WITH-NOTES**

条件（非阻断，随常规迭代处理）：

1. FF-N1/FF-N4 两处文档口径更正随手即可（各一行）。
2. FF-N2 在合并 PR #521 或本分支时按「两侧都保留」手工解 tst_edittools.cpp 冲突
   （include 各取所需 + 两组 slot 声明并列）。
3. 执行面验收以主 Agent 汇总为准（定向套件全绿 + clang-tidy/layering/ui_invariants 绿）；
   本终审按铁律未重复执行，但已对全部 15 个新测试函数逐一核验在位性与断言实质
   （含红绿方向的独立逻辑推演），未发现永真/mock 绕过/文案假匹配。

理由：17 项簇修复全部在位且治根因；Fix 2 六处二次修改逐处核实落位；四组对抗挑战
（CRS/边界值/取消重入/空数据/环境对比）全部推翻失败；分层、卫生、性能、生命周期
四横切面零违规；新发现无一达 SHOULD_FIX 级。

## 五、剩余风险清单

| # | 风险 | 等级 | 出处/口径 |
|---|---|---|---|
| 1 | **环境阻断：本终审未编译未 ctest**——绿灯数据依赖主 Agent（07 验证汇总：定向套件 14/22 项全绿 + 三门禁绿）；静态层面未发现可致红的缺陷 | 流程性 | 铁律约束 |
| 2 | PR #521 与本分支 tst_edittools.cpp 文本冲突（加法对加法，手工可解）；#518 全部自动合并；新 PR #523 仅 CMakeLists 错开 | 低 | FF-N2/FF-N4 |
| 3 | R2B-OBS-01：fillSgs 恒带回调 → sgs3 走串行档，FB-002 worker 收缩对生产充填链不生效；串行档工作区（~65B×单带目标数）无上界——修复自披露的设计选择（串行=并行化前行为） | 中（既有） | 07 OBS-01 DOCUMENT_ONLY |
| 4 | R2B-OBS-02：FC-001 冻结回放对 memory provider 层不等价（schema 回放、数据丢失、isValid 仍 true）——当前无在库调用方，接新调用方时的纪律项 | 低（潜在） | 07 OBS-02 |
| 5 | FF-N3：无 lrelease 环境应用链接失败（Q_INIT_RESOURCE(translations) 无定义）——基线即有 | 低（既有） | 环境债 |
| 6 | R2B-OBS-05：make_appimage.sh 对已缓存 runtime 跳过 sha 校验（`if [ ! -x ]`）——打包机本地信任面 | 低 | 07 OBS-05 |
| 7 | FC-001-D1：预测 INPUT 冻结只覆盖单键——未来给预测页加 MASK/参考图层参数会静默重蹈跨线程活层违约（当前只收标量，安全） | 低（前瞻） | 06-r2 FC-001-D1 |
| 8 | 裸跑 tst_import::documentConversionProfileDirIsRandomAndCleanedUp 在脏 /tmp 上可假红（ctest 沙箱内不受影响）；512MB 稀疏夹具在旧码路径真实 readAll（受限 runner 以 OOM 代红——已是修复后无关面） | 低 | 07 OBS-06（口径经 FF-N1 修正） |
| 9 | SUSPECTED S-1/S-2/S-3（Isopach 符号约定、lowfreq nearestCell 复杂度、fillSgs 硬数据均值 float）按登记表维持记录不修 | 信息 | 04 登记表 |
