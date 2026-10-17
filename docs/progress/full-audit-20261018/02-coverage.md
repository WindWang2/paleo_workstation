# 审查覆盖清单（full-audit-20261018）

完整逐文件覆盖清单见五份分报告（同目录 reviews/agent-{A,B,C,D,E}-findings.md §1），
本文件汇总口径与统计。审查基线 366aef97，自研源码约 19 万行。

## 模块 × 审查方式汇总

| 模块 | 规模 | 逐行/重点函数深审 | 接口+调用链/模式扫描 | 报告 |
|---|---|---|---|---|
| src/domain | 83 文件 12.3k 行 | deviationsurvey、wellsection(520-700)、constraintsemantics(300-450)、wellcompositemodel、faciesconsistency、nicestep | 其余（值对象族，first/last/除法/int 转换 grep 全查有守卫） | A |
| src/catalog | 28 文件 6.2k 行 | datacatalog、datacatalog_versions、catalogstore、catalogstore_json、catalogindex | CRUD/schema/entities/assets/links 等 11 文件 | A |
| src/io | 85 文件 25.1k 行 | segyreader、segyindexstore、dlisparser、lisparser、lasparser、welllogread、ziparchive、sfpkgreader、outsourceworkbook、wellcompositexml、streaming、partialread、constraintstore、cachecore、encodingdetect、wellfileparsers、attrgridout、timedeptool（18 文件 ~13k 行） | sattrio（#518 排除）、rasterpyramid、dataimport_* 8 文件、cuttingsdoc 等 14 文件 | A |
| src/metadata | 28 文件 5.5k 行 | paleoprojectstore、atomicfile.h、mapversionstore、wellsectionstore、paleoprojectfile、metastore、layermanifest、mapsnapshot | faultsetstore 等 6 文件 | A |
| src/algorithms | 144 文件 37.3k 行 | geostat 全 10 文件、inversion 3 文件、paleoalgorithms、mincurvature、gridsolver、distancetransform、welldist、velocitymodel、seismicattr、localidw、krigingsurface、curvekernel.h、faciespolygonize、faciessliver、petrophys（~26 文件） | cluster/som/supervised（#470 修复面核验）、stratgrid、singlefactor 其余、evolution/wellsiting/faultsurface 等 | B |
| src/services | 74 文件 21.3k 行 | paleotaskservice、jobrunner、futuretask.h、seismictaskservice(+_slice/_viz/_attr)、imagelod（逐行 8 文件） | evidenceindex、petrophyscomputeservice（重点）+ 其余 27 文件接口级 | B |
| src/workflow | 133 文件 46.9k 行 | mapversioncontroller(+_async)、mappingworkbench(+_predict/_edit/_compose/_polygonize/_consistency)、predictionworkflow、wellpredictionservice、batchjobqueue、assetops、mappingworkflow(前500)、compositionworkflow(fuseFactors)、constraintfactorjobs_factor(核心段)、evidencerecord、projectlayerrefresh（~20 文件） | 其余 ~40 文件（PR #521/#518 领地按排除矩阵处理） | C |
| src/ai | 34 文件 5.8k 行 | onnxpredictionservice、onnxlocalpredictor、remotepredictrouter、wellfaciesservice、modelregistry、horizonsuggest、wellfaciesskeystore、llmclient（8 文件） | 其余（#518/#460-#479 领地） | C |
| src/linkage | 8 文件 0.9k 行 | 全部 4 cpp | 头文件细节 | C |
| src/qgis | 90 文件 19.5k 行 | qgisconstrainteditsession、qgiseditingservice、qgiscanvascontroller、previewmapcanvas、previewmaptools、qgislayerservice、qgislayerorganizer、qgisprojectservice_async、evidencemapadapter、mapcanvaslink、mappingreferencecanvas、draftautosave、wellpointnavigation、nativeattributetablehost、qgisstyleservice（15 文件） | 其余（PR #521 facies* 排除；导出簇排除） | D |
| src/ui | 400 文件 114.8k 行 | paleomainwindow 本体+_pages+_docks+.h、validatepage、constraintpage_generate、predictpage、mappingworkbenchpage(ctor+predict)、horizonchipbar、evolutionplayerpanel、notifications 栈、taskpanel、paleodockmanager、horizonmarkers、welltopseditordialog | 其余按风险抽查 + 全目录 QTimer/异步/connect-lambda/裸new/QPointer 模式扫描；PR #521/#518/导出簇领地排除 | D |
| src/app | 12 文件 3.7k 行 | 全部 | — | E |
| tests | 404 文件 | 深读 22 个 + 全仓脚本化扫描（永真断言/QSKIP 191 处/msleep 79 处/守卫断言/赋值断言） | 其余 ~350 抽样 | E |
| CMakeLists+cmake | 49+1 文件 | 全部逐行 | — | E |
| tools+scripts | 31+ | 全/近全读 14 个，其余头部核对 | — | E |
| resources | geology.qrc 343 条目核对、facies-hierarchy.json schema vs 消费 | — | E |
| .github/workflows | ci.yml 逐段、deb-lock.yml | — | E |
| 分层横切 | 1122 文件批量实证（层标记 0 缺失、越层 include 0、QtWidgets 0 泄漏、白名单 0 违规） | — | E |
| 安全横切 | QProcess/SQL/XML/zip/临时文件定向扫描 | — | E |

## 深审占比与诚实声明

- 逐行/重点函数深审约 110 个文件（~60k 行，自研代码 ~1/3），选择标准：不可信输入解析、
  持久化/事务、并发/任务系统、数值核、Qt 生命周期高危、组装根与构建布线。
- 其余为接口+调用链+高危模式扫描（agents 各自 §4 未覆盖清单如实列出——未逐行 ≠ 无问题，
  后续审计不应假设其干净）。
- 未做运行时验证（subagent 只读约束）；所有 Regression Test 均为设计稿，红绿由本轮 Fix 阶段落地验证。

## 03-callgraph-and-contracts 摘要（关键契约链）

- prepare→compute→publish 三段式线程契约：prepare=GUI/catalog 线程（stage/父版本）；
  compute=worker（Processing/ONNX+落盘，禁 catalog/GUI 对象）；publish=GUI（commitExternal+声明+信号）。
  唯一违例：predictionworkflow INPUT 活层穿线程（FC-001/P1，本轮修复）。
- QgisProcessingService::run 自述契约（qgisprocessingservice.cpp:662-667）：worker 调用方不得传
  主线程拥有的图层对象——全仓正确范本 runFactorAlgorithm/computePolygonize/computeCompose，
  predictionworkflow 为唯一例外。
- DataCatalog 线程闸（checkWriteThread）+ staging copy 供 worker 独占；PaleoTaskService
  session/generation + JobRunner Claim 双换代守卫——链路核对闭合（C §3）。
- 任务系统（PaleoTaskService/JobRunner/futuretask/SeismicTaskService）逐行无新缺陷（B §6）。
- 分层契约：1122 文件批量实证干净（E），与 ctest layering 闸门互证。
