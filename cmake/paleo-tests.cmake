# cmake/paleo-tests.cmake — 测试注册（主清单）
# 由根 CMakeLists.txt 按顺序 include()（纯结构拆分，语义与拆分前逐行一致）。
# 注意：include() 不开新作用域，CMAKE_SOURCE_DIR 仍是仓库根。
# 文件名刻意不用 extra- 前缀——extra-*.cmake 受 _paleo_extra_manifest 防漏对账。

# ---- 测试注册（devex T1 链接瘦身：LIBS = 各测试 include 面的最小闭包，见
# docs/progress/devex.md 对比表；单参 = 伞式旧式契约，保留 5 个作为兼容样本）----
add_paleo_test(tst_portableenvironment LIBS paleo_app)
add_paleo_test(tst_runtime LIBS paleo_qgis)                 # metadata,qgis
add_paleo_test(tst_projectsvc LIBS paleo_qgis)              # metadata,qgis
add_paleo_test(tst_basemap_project LIBS paleo_qgis)
add_paleo_test(tst_background_open LIBS paleo_workflow)
add_paleo_test(tst_officepreview LIBS paleo_ui)
add_paleo_test(tst_identifybrowse LIBS paleo_ui)
add_paleo_test(tst_layerservice LIBS paleo_qgis paleo_services) # catalog,metadata,qgis,services(#483 refreshBasemaps ErrorHub 断言)
add_paleo_test(tst_canvas_tools LIBS paleo_services paleo_qgis) # metadata,qgis,services
add_paleo_test(tst_domain LIBS paleo_domain)
add_paleo_test(tst_faciescatalog LIBS paleo_domain) # #409：formationPrediction 统一口径回归
add_paleo_test(tst_jobrunner LIBS paleo_services paleo_ui_deps)  # 方向20：JobRunner 三段式框架断言
add_paleo_test(tst_algorithm_spec LIBS paleo_services)  # algo-atom P0：Spec ↔ initAlgorithm / schema 对齐
add_paleo_test(tst_algorithm_adapters LIBS paleo_workflow)  # algo-atom P2：Processing/NativeCpp 适配器契约
add_paleo_test(tst_linkage LIBS paleo_linkage)
add_paleo_test(tst_algorithms LIBS paleo_algorithms paleo_ui_deps)
add_paleo_test(tst_faciessliver LIBS paleo_algorithms)  # A11：小图斑吸收新旧实现对拍 + 可取消 + 大网格耗时
add_paleo_test(tst_seismicattr LIBS paleo_algorithms)      # 地震属性核（纯数值，无 QGIS init）
add_paleo_test(tst_dsp_fft LIBS paleo_algorithms)          # #559：dsp radix-2 FFT 专属套件（解析恒等式 + 独立直求 DFT 对拍；纯数值，无 QGIS init）
add_paleo_test(tst_sattrio LIBS paleo_io)                 # goal/attr-volume：SATR/SATV 容器 round-trip
add_paleo_test(tst_seismicattrscan LIBS paleo_services)  # goal/attr-volume：时间切片/属性体扫描编排
add_paleo_test(tst_workerexceptionsink LIBS paleo_services) # PW-014：工作分片异常转运
add_paleo_test(tst_projectfile_safety LIBS paleo_store)  # PW-075/126：project.paleo 成员越界与版本缺失
add_paleo_test(tst_singlefactor_kernel LIBS paleo_algorithms)
add_paleo_test(tst_wellsiting_kernel LIBS paleo_algorithms)  # 方向34：井网辅助纯核（Oracle 1/3）
add_paleo_test(tst_singlefactor_corridor LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_partition LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_faultpath LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_buffer LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_regional LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_avoidance LIBS paleo_algorithms)
add_paleo_test(tst_constraint_draw LIBS paleo_ui)
add_paleo_test(tst_singlefactor_perf LIBS paleo_algorithms)
add_paleo_test(tst_contourlevels LIBS paleo_algorithms)   # #149 级别数上限
add_paleo_test(tst_singlefactor_contours LIBS paleo_qgis paleo_services)
add_paleo_test(tst_singlefactor_fieldcontours LIBS paleo_workflow)
add_paleo_test(tst_singlefactor_contourbudget LIBS paleo_algorithms paleo_qgis paleo_services)
add_paleo_test(tst_singlefactor_guilatency LIBS paleo_algorithms paleo_services)
add_paleo_test(tst_singlefactor_faults LIBS paleo_workflow)
add_paleo_test(tst_singlefactor_asynccontour LIBS paleo_workflow)
add_paleo_test(tst_singlefactor_parity LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_acquisition LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_structural LIBS paleo_workflow)
add_paleo_test(tst_singlefactor_parity_completion LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_contract LIBS paleo_domain)
add_paleo_test(tst_singlefactor_samples LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_store LIBS paleo_io)
add_paleo_test(tst_services2 LIBS paleo_qgis)               # algorithms,metadata,qgis
add_paleo_test(tst_sequenceframework LIBS paleo_services)  # 方向28：格架 CRUD/诊断/建议（零写入）断言
add_paleo_test(tst_geosymbols LIBS paleo_qgis paleo_domain) # 方向31：花纹/线型词表+入口（qrc 资源在 paleo_domain）
add_paleo_test(tst_semanticlegend LIBS paleo_qgis paleo_domain) # 方向31：图例自动生成 + 增删/renderer 联动
add_paleo_test(tst_symbolpicker LIBS paleo_ui paleo_qgis paleo_domain) # 方向31：符号选择器面板（缩略/过滤/patternPicked）
add_paleo_test(tst_workflows LIBS paleo_workflow)
add_paleo_test(tst_boundaryeditrules LIBS paleo_workflow) # 方向39：相界类型编辑规则（纯语义面）
add_paleo_test(tst_wellsiting_workflow LIBS paleo_workflow)  # 方向34：井网辅助编排（Oracle 2/4）
add_paleo_test(tst_wellsiting_panel LIBS paleo_ui)         # 方向34：布井辅助面板（视图锚点/意图信号）
add_paleo_test(tst_mappingworkbench LIBS paleo_core paleo_qgis_attribute_table) # 伞式 + #410 属性表宿主回归
add_paleo_test(tst_ui)                                      # 伞式（app 面全栈，兼容样本）
add_paleo_test(tst_panels)                                  # 伞式（全栈，兼容样本）
# RUN_SERIAL 独占机器跑：dataops_d2_filterPerformance10k 的 100ms 墙钟断言
# 在 -j8 并行负载下偶发超限（108ms 实测）——同 tst_perf/tst_correlation_full
# 的计时抖动关回串行基线先例（根治归 TEST-02 类递延）。
set_tests_properties(tst_panels PROPERTIES RUN_SERIAL TRUE)
# #503：tst_panels dataops 族（D1–D10，92 用例）拆分为独立目标——原 5337 行
# 巨型测试文件瘦 relink 面 + 失败定位；用例逐字搬运（共享夹具
# tests/helpers/dataopsfixture.h）。RUN_SERIAL 同 tst_panels（d2 计时断言）。
add_paleo_test(tst_panels_dataops)                     # 伞式（dataops 族，逐字搬运）
set_tests_properties(tst_panels_dataops PROPERTIES RUN_SERIAL TRUE)
add_paleo_test(tst_mappingpages LIBS paleo_ui)
add_paleo_test(tst_wellattributes LIBS paleo_ui)
add_paleo_test(tst_project_wellinputs LIBS paleo_ui)
add_paleo_test(tst_project_paleomaps LIBS paleo_ui)
add_paleo_test(tst_factorworkflow LIBS paleo_workflow) # m2(B)：单因素生成链真实栈（tst_workflows 模式）
add_paleo_test(tst_composeworkflow) # m2(C)：相属性回写 edit buffer + 壳自动编辑态（tst_ui 模式；伞式兼容样本）
add_paleo_test(tst_perf LIBS paleo_qgis paleo_services paleo_algorithms)
set_tests_properties(tst_perf PROPERTIES RUN_SERIAL TRUE)
add_paleo_test(tst_locator LIBS paleo_ui)
add_paleo_test(tst_layoutshell LIBS paleo_ui)
add_paleo_test(tst_maptools LIBS paleo_ui)
add_paleo_test(tst_correlation LIBS paleo_ui)
add_paleo_test(tst_decorations LIBS paleo_ui)
add_paleo_test(tst_manifestproj LIBS paleo_qgis)
add_paleo_test(tst_import LIBS paleo_io paleo_services paleo_qgis Qt6::Pdf) # 方向44：并集可达断言；方向94：转换 round-trip 页数断言用 QtPdf
# 方向 95：mini 夹具扩散段——整工程生产产物上的导入域不变量（受管 SHA
# 对拍/只读锁/外链 sha 冻结/A3 双 LAS 并集）。第二源 + 生成器表达式对齐
# tst_mkprojectfixture 手动注册口径；RUN_SERIAL 同先例（QProcess 子进程 +
# QGIS init 资源面，CI 并行负载下防抖）。
target_sources(tst_import PRIVATE tests/fixtures/mkprojectfixture.cpp)
target_compile_definitions(tst_import PRIVATE
  MKPROJECT_BIN="$<TARGET_FILE:paleo_mkproject>"
  MKPROJECT_MINI_DIR="${CMAKE_SOURCE_DIR}/tools/reference/mkproject/mini")
add_dependencies(tst_import paleo_mkproject)
set_tests_properties(tst_import PROPERTIES RUN_SERIAL TRUE)
add_paleo_test(tst_io_importrows_derivation LIBS paleo_io)  # L5.9：DataImportService::previewFiles 推导 FolderPreviewRow 显示类型/词表/可改性/实体预显
add_paleo_test(tst_imagelod LIBS paleo_services)              # 方向79：图片道 LOD/EXIF/LRU
add_paleo_test(tst_wellattachmentpanel LIBS paleo_ui)        # 方向79：井附件面板 CRUD offscreen
add_paleo_test(tst_drawctl LIBS paleo_ui)
add_paleo_test(tst_procdialog LIBS paleo_qgis)
add_paleo_test(tst_rehydrate)                               # 伞式（app 面全栈，兼容样本）
add_paleo_test(tst_release LIBS paleo_store)
add_paleo_test(tst_sequenceframework_ui LIBS paleo_ui)  # 方向28：格架面板/柱状视图无头断言
add_paleo_test(tst_releasepanel LIBS paleo_ui)
add_paleo_test(tst_taskpanel LIBS paleo_ui)
add_paleo_test(tst_notifications LIBS paleo_ui)   # 方向64：通知卡/模态单弹/风暴（offscreen；同 tst_taskpanel 取 paleo_ui 的 QtWidgets 头）
add_paleo_test(tst_attrpanel LIBS paleo_ui)
# ---- merged worktree waves ----
# 方向50：IO 容错、导入诊断、SDK 消费与工作簿边角回归。
add_paleo_test(tst_wellfileparsers LIBS paleo_io)
add_paleo_test(tst_io_importledger LIBS paleo_workflow)
add_paleo_test(tst_io_robustness LIBS paleo_io paleo_algorithms)
add_paleo_test(tst_io_seismicquality LIBS paleo_services paleo_algorithms)
add_paleo_test(tst_seismichorizoncluster LIBS paleo_services) # 取窗契约锚（f0e7b10d ±12ms）
add_paleo_test(tst_io_workbook_edges LIBS paleo_io)
# 方向 95：mini 数据集对拍槽（SpreadsheetML/岩屑 CSV 实文件直读，零 QProcess
#——不走 fixtures/mkprojectfixture.cpp，paleo_io 瘦链接面不动）。
target_compile_definitions(tst_io_workbook_edges PRIVATE
  MKPROJECT_MINI_DIR="${CMAKE_SOURCE_DIR}/tools/reference/mkproject/mini")
add_paleo_test(tst_segy LIBS paleo_io)
add_paleo_test(tst_las LIBS paleo_io)
add_paleo_test(tst_dlisparser LIBS paleo_io)          # 方向44：DLIS (RP66 v1) 全集读口
add_paleo_test(tst_lisparser LIBS paleo_io)           # 方向44：LIS79 全集读口
add_paleo_test(tst_lasperf LIBS paleo_io)    # 方向21：LAS 读面比率门探针（同进程参照实现）
add_paleo_test(tst_snapshot_dedup LIBS paleo_io) # #84：快照/活对象 dedup 复核同一语义（真重哈希）
add_paleo_test(tst_shacache LIBS paleo_io) # setDiskFile 换绑不得把在途哈希写入新 sha.json
add_paleo_test(tst_constraintstore LIBS paleo_workflow)
add_paleo_test(tst_constraintevidence LIBS paleo_io)
add_paleo_test(tst_constraintediting LIBS paleo_ui)
add_paleo_test(tst_shapetools LIBS paleo_ui)
add_paleo_test(tst_edittools LIBS paleo_ui)
add_paleo_test(tst_attributetablehost LIBS paleo_qgis paleo_qgis_attribute_table) # FD-004/R2-SF-04：ask seam 契约（独立文件避开 PR #521 冲突面）
add_paleo_test(tst_faciespartition LIBS paleo_qgis)
add_paleo_test(tst_correlation_full LIBS paleo_ui)
# tst_correlation_full 内嵌墙钟性能断言（10 次 warm reorder < 1000ms，测试
# .cpp 归 correlation 方向）——并行负载下偶发超时（本次 ctest -j4 实测捕获）。
# RUN_SERIAL 独占机器跑：计时抖动关回串行基线；根治（QTRY_/预算放宽）
# 归测试属主方向，见 docs/progress/devex.md T2 注记。
set_tests_properties(tst_correlation_full PROPERTIES RUN_SERIAL TRUE)
add_paleo_test(tst_layoutpalette LIBS paleo_ui)
add_paleo_test(tst_layoutitempanel LIBS paleo_ui)
add_paleo_test(tst_layoutexport LIBS paleo_ui)
add_paleo_test(tst_mapbook LIBS paleo_ui)              # goal/mapbook-reporting：格序列/模板/蒙太奇/批量队列
add_paleo_test(tst_layoutundo LIBS paleo_ui)
add_paleo_test(tst_layoutdesigner_full LIBS paleo_ui)
add_paleo_test(tst_standardelements LIBS paleo_qgis) # 方向25：标准图件元素工厂 + 三类内容模板骨架
add_paleo_test(tst_layoutpersistence LIBS paleo_qgis) # 方向25：布局随工程 QGZ round-trip
add_paleo_test(tst_layouttemplatelibrary LIBS paleo_workflow) # 方向25：模板库（qpt→catalog 版本 + 库表）
add_paleo_test(tst_horizonbatchexport LIBS paleo_workflow) # 方向25：按层位组批量出图 + catalog 登记
add_paleo_test(tst_horizonbatchexport_diag LIBS paleo_workflow) # #541 诊断套件（此前 582 行零注册覆盖幻影；2026-10-19 审计 E-04 接闸）
# ---- wave/data-foundation ----
add_paleo_test(tst_catalog LIBS paleo_store)
add_paleo_test(tst_catalogroles LIBS paleo_store)   # 方向92：角色词表单源
add_paleo_test(tst_projectparsers LIBS paleo_io)
target_compile_definitions(tst_projectparsers PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_horizonbinner LIBS paleo_io)
target_compile_definitions(tst_horizonbinner PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_segy_lines LIBS paleo_io)
target_compile_definitions(tst_segy_lines PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_seismic_core LIBS paleo_domain)
add_paleo_test(tst_sgycoordmapper LIBS paleo_domain)  # #135 仿射拟合数值稳定性（中心化最小二乘）
add_paleo_test(tst_welltracelocate LIBS paleo_domain) # #129 井口 XY → 井旁道（非单位线距/旋转测网）
add_paleo_test(tst_seismic_index LIBS paleo_services)
add_paleo_test(tst_fspath LIBS paleo_domain) # #291 QString↔filesystem::path UTF-16 契约（Windows 中文路径回归闸门）
target_compile_definitions(tst_fspath PRIVATE
  SEGY_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/testdata/project_area/mini_seismic.sgy")
add_paleo_test(tst_dataperf_index LIBS paleo_services) # 方向21：索引缓存发布修复回归面
add_paleo_test(tst_coldstart LIBS paleo_io) # 方向21：数据路径冷启动分段（catalog/索引/LAS 首触 vs 二触）
add_paleo_test(tst_seismic_3d LIBS paleo_ui)
target_compile_definitions(tst_seismic_3d PRIVATE
  SEGY_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/testdata/project_area/mini_seismic.sgy"
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_seismic_section LIBS paleo_ui)
target_compile_definitions(tst_seismic_section PRIVATE
  SEGY_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/testdata/project_area/mini_seismic.sgy"
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_timedeptool LIBS paleo_io)
target_compile_definitions(tst_import PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_previewmap_canvas LIBS paleo_qgis)
add_paleo_test(tst_previewmap_tools LIBS paleo_qgis)
add_paleo_test(tst_previewmap_identify LIBS paleo_qgis)
add_paleo_test(tst_previewmap_page LIBS paleo_ui)
add_paleo_test(tst_previewmap_assets LIBS paleo_ui)
target_compile_definitions(tst_previewmap_assets PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_wellbasemap LIBS paleo_ui)  # #14 井位类页面 GIS 底图（本地夹具，不联网）
add_paleo_test(tst_onlinebasemap LIBS paleo_qgis) # 在线预设/URI/主图叠加/工程持久化（不联网）
add_paleo_test(tst_basemap_rendering LIBS paleo_ui) # 可见预览/底图互斥与实际出图（不联网）
target_compile_definitions(tst_wellbasemap PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_previewmap_perf LIBS paleo_ui)
target_compile_definitions(tst_previewmap_perf PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
# 墙钟预算断言（D6.7）——共享机负载下抖动关回串行基线（tst_correlation_full 先例）。
set_tests_properties(tst_previewmap_perf PROPERTIES RUN_SERIAL TRUE)
add_paleo_test(tst_datapreview LIBS paleo_ui)
target_compile_definitions(tst_datapreview PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
# ---- W7 新接缝测试（docs/UI_LAYER_PLAN.md）----
add_paleo_test(tst_folderimport LIBS paleo_workflow paleo_ui)
target_compile_definitions(tst_folderimport PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
# ---- 方向 30：工区数据管线与健康管理 ----
add_paleo_test(tst_storagegovernance LIBS paleo_workflow)
add_paleo_test(tst_assetpaging LIBS paleo_ui)
target_link_libraries(tst_assetpaging PRIVATE ${CMAKE_DL_LIBS})
add_paleo_test(tst_cataloghealth LIBS paleo_services)   # 体检召回（缺失/SHA/未决/孤立/无版本）
add_paleo_test(tst_dataexport LIBS paleo_workflow)
add_paleo_test(tst_exportpipeline LIBS paleo_ui)
add_paleo_test(tst_paleomaplibrary LIBS paleo_ui)
add_paleo_test(tst_assetops LIBS paleo_workflow)        # 版本回滚/对比 + 物理清理 + 未决归位
add_paleo_test(tst_previewdoc LIBS paleo_services paleo_qgis)
target_compile_definitions(tst_previewdoc PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_wellcomposite LIBS paleo_ui)
add_paleo_test(tst_smoke_realdata LIBS paleo_workflow) # real-data smoke; skips unless PALEO_REAL_PROJECT_AREA is set
add_paleo_test(tst_stratigraphicweb LIBS paleo_ui)
add_paleo_test(tst_webviewpanel LIBS paleo_ui)   # offscreen: engine unavailable → fallback, host survives
# ---- wave3/ux-consistency ----
# (fonts qrc + theme singleton live in paleo_core below; UI-domain tests here)
add_paleo_test(tst_uxtheme LIBS paleo_ui)
if(PALEO_HAVE_ORT)
  add_paleo_test(tst_onnx LIBS paleo_ai)
  add_paleo_test(tst_onnxsessions LIBS paleo_ai)
  add_paleo_test(tst_tileinference LIBS paleo_ai paleo_services)
  add_paleo_test(tst_modelregistry LIBS paleo_ai)
  add_paleo_test(tst_aiassistworkflow LIBS paleo_workflow)
  add_paleo_test(tst_remotepredictrouter LIBS paleo_ai)
  add_paleo_test(tst_aiassist_perf LIBS paleo_ai paleo_sbm)
  add_paleo_test(tst_onnxworkflow LIBS paleo_workflow)
  target_compile_definitions(tst_onnxworkflow PRIVATE
    PROJECT_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")
  add_paleo_test(tst_predict_challenger_r2 LIBS paleo_ai paleo_services paleo_workflow)
  # ORT 运行库目录必须出现在 LD_LIBRARY_PATH：用 ENVIRONMENT_MODIFICATION
  # 的 path_list_prepend 前置，而不是 APPEND 一条整体 LD_LIBRARY_PATH 赋值
  # ——后者仍会替换外部注入的值（deb 闭包路 paleo-dev 注入的 vendored
  # QGIS/Qt 目录会被吞掉，间接依赖落到系统 Qt 上产生版本冲突——本机
  # Arch Qt6.11 + vendored Qt6.10 即此症状）。
  set_tests_properties(tst_onnx tst_onnxworkflow tst_onnxsessions tst_tileinference tst_modelregistry
    tst_aiassistworkflow tst_remotepredictrouter tst_aiassist_perf tst_predict_challenger_r2 PROPERTIES ENVIRONMENT_MODIFICATION
    "LD_LIBRARY_PATH=path_list_prepend:${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib")
endif()

# ---- wave/mapping-pipeline (阶段C+E 编图链路) ----
add_paleo_test(tst_projectdata LIBS paleo_services paleo_qgis)
add_paleo_test(tst_qgisruntime LIBS paleo_qgis) # #483 prefix 优先级契约
add_paleo_test(tst_mapping LIBS paleo_workflow)

# ---- wave2 async-ux (D1/D2 异步任务框架 + 任务页进度条) ----
add_paleo_test(tst_taskservice LIBS paleo_ui)
add_paleo_test(tst_futuretask LIBS paleo_services) # #479 futuretask.h 专测
add_paleo_test(tst_threeway)          # 伞式（全栈，兼容样本）
add_paleo_test(tst_chips LIBS paleo_ui)
add_paleo_test(tst_versions LIBS paleo_workflow)
add_paleo_test(tst_mapsnapshot_scale LIBS paleo_workflow) # 10k/100k 快照实测，只记 BASELINE，不断言墙钟

# ---- wave3/derived-publish (T26/D10/D15 + §40 速度模型与 CDP 映射) ----
add_paleo_test(tst_derivedassets LIBS paleo_workflow)
add_paleo_test(tst_seismicmapping LIBS paleo_ui)
target_compile_definitions(tst_seismicmapping PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")

# ---- application ----
add_executable(paleo src/app/main.cpp)
if(WIN32)
  # Qt's entry-point target follows this property; double-clicking opens only UI.
  set_target_properties(paleo PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_link_libraries(paleo PRIVATE paleo_core)

# ---- goal/perf-systematize 簇1：启动分段仪表 + 比率门 ----
# 真实 paleo 二进制 offscreen 起停（首帧即退），PALEO_STARTUP_TRACE 落盘
# JSON → 段完整性 + 机器无关比率门（startup_ratios.json ×1.5）；另起一次
# 注入 1500ms 劣化断言门必红（判别力）。计时面挂串行，对齐 tst_perfbudget。
add_paleo_test(tst_startup_trace LIBS paleo_services)
# #399：测试经 PALEO_APP_BINARY 启动真二进制——LIBS 闭包里没有 paleo，
# 单编 target 时二进制缺席必败；与其余 $<TARGET_FILE:…> 消费者同补依赖。
add_dependencies(tst_startup_trace paleo)
target_compile_definitions(tst_startup_trace PRIVATE
  PALEO_APP_BINARY="$<TARGET_FILE:paleo>"
  PALEO_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # 审计 01 M5：基线不靠 CWD 相对路径
set_tests_properties(tst_startup_trace PROPERTIES RUN_SERIAL TRUE)

add_executable(paleo_seismic_demo tools/seismic_showcase.cpp)
target_link_libraries(paleo_seismic_demo PRIVATE paleo_core)

add_executable(tst_boot tests/tst_boot.cpp)
target_link_libraries(tst_boot PRIVATE paleo_qgis_iface Qt6::Test)
paleo_test_link_sandbox(tst_boot)
target_compile_definitions(tst_boot PRIVATE FIXTURE_GPKG="${FIXTURE_GPKG}")
add_test(NAME boot COMMAND tst_boot
         -o "${PALEO_QTEST_FIRST_RUN_DIR}/boot.txt,txt")
paleo_test_sandbox(boot)

add_executable(tst_polygonize tests/tst_polygonize.cpp spikes/polygonize/paleopolygonize.cpp)
target_link_libraries(tst_polygonize PRIVATE paleo_qgis_iface Qt6::Test ${GDAL_LIB})
paleo_test_link_sandbox(tst_polygonize)
target_include_directories(tst_polygonize PRIVATE ${GDAL_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/spikes/polygonize)
add_test(NAME polygonize COMMAND tst_polygonize
         -o "${PALEO_QTEST_FIRST_RUN_DIR}/polygonize.txt,txt")
paleo_test_sandbox(polygonize)

# ---- wave3/model-hardening (数据模型与测试基建加固) ----
add_paleo_test(tst_metastore LIBS paleo_store)      # sqlite user_version：新库采纳/遗留升级/未来拒开
add_paleo_test(tst_test_sandbox LIBS paleo_domain)  # 方向81：测试沙箱契约（XDG / Windows 私有注册表 hive）
add_paleo_test(tst_layermanifest LIBS paleo_store)  # P1-11 / ARCH-04 LayerManifest 线程亲和性与写队列调度门禁
add_paleo_test(tst_segy_fixture LIBS paleo_io)   # 合成最小 SEG-Y 夹具：索引/单线/角点/范围门
target_compile_definitions(tst_segy_fixture PRIVATE
  TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")
# mkproject 夹具工厂消费测试（方向 78）：QProcess 驱动 paleo_mkproject
# --manifest mini 全链（生产导入路径），四断言面（导入/round-trip/配准残差/
# 复用性）。手动注册（非 add_paleo_test）：要带 fixtures/mkprojectfixture.cpp
# 第二源 + MKPROJECT_BIN 生成器表达式；RUN_SERIAL 对齐 tst_perfbudget 先例
#（QProcess 子进程 + QGIS init 的资源面，CI 并行负载下防抖）。LABEL services
#（core 段，不进 perf）。
add_executable(tst_mkprojectfixture tests/tst_mkprojectfixture.cpp
               tests/fixtures/mkprojectfixture.cpp)
target_link_libraries(tst_mkprojectfixture PRIVATE paleo_services paleo_qgis Qt6::Test)
target_compile_definitions(tst_mkprojectfixture PRIVATE
  MKPROJECT_BIN="$<TARGET_FILE:paleo_mkproject>"
  MKPROJECT_MINI_DIR="${CMAKE_SOURCE_DIR}/tools/reference/mkproject/mini")
add_test(NAME tst_mkprojectfixture COMMAND tst_mkprojectfixture
         -o "${PALEO_QTEST_FIRST_RUN_DIR}/tst_mkprojectfixture.txt,txt")
# #420：add_paleo_test 等价沙箱要两条腿——ENV 沙箱 + 注册表 hive 链接库
#（Windows NativeFormat 不走 XDG env，缺它 QSettings 会落真实注册表）。
paleo_test_link_sandbox(tst_mkprojectfixture)
paleo_test_sandbox(tst_mkprojectfixture)
set_tests_properties(tst_mkprojectfixture PROPERTIES LABELS services RUN_SERIAL TRUE)
add_dependencies(tst_mkprojectfixture paleo_mkproject)
add_paleo_test(tst_algorithm_harness LIBS paleo_algorithms paleo_ui_deps)  # AlgorithmTestBase harness + ≥3 算法用例
add_paleo_test(tst_algorithm_rasterout_crs LIBS paleo_algorithms paleo_ui_deps)  # QgsApplication 需 QtWidgets（同 tst_algorithm_harness）；BIZ-11（方向58）：无 CRS 输入经共享写口带规范 CRS
add_paleo_test(tst_perfbudget LIBS paleo_services paleo_qgis)
# 墙钟预算门禁在 CI 并行负载下必抖（实测 525ms>500ms，预热救不了 CPU
# 争抢）——计时测试串行跑，对齐 tst_correlation_full 先例。
set_tests_properties(tst_perfbudget PROPERTIES RUN_SERIAL TRUE)     # 非 UI 性能预算：工程打开/folder 枚举/save 200 版本
target_compile_definitions(tst_perfbudget PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
# 方向70：预算降档系数单测（tests/perfbudget_relax.h 契约——tst_correlation_full/
# tst_perfbudget 的墙钟预算经 relaxedBudgetMs 放宽）。默认二进制系数=1；下面
# 同源第二二进制强制 PALEO_SANITIZER_BUILD=1（系数=3）——on/off 两档预算差在
# 本机（含 MSVC，sanitizer 档仅 Linux）即可对拍验证。
add_paleo_test(tst_sanitizer_budget LIBS paleo_domain)
add_executable(tst_sanitizer_budget_on tests/tst_sanitizer_budget.cpp)
target_link_libraries(tst_sanitizer_budget_on PRIVATE paleo_domain Qt6::Test)
paleo_test_link_sandbox(tst_sanitizer_budget_on)
target_compile_definitions(tst_sanitizer_budget_on PRIVATE
  PALEO_SANITIZER_BUILD=1 FIXTURE_GPKG="${FIXTURE_GPKG}")
add_test(NAME tst_sanitizer_budget_on COMMAND tst_sanitizer_budget_on
         -o "${PALEO_QTEST_FIRST_RUN_DIR}/tst_sanitizer_budget_on.txt,txt")
paleo_test_sandbox(tst_sanitizer_budget_on)
set_tests_properties(tst_sanitizer_budget_on PROPERTIES LABELS domain)

# ---- data/role-registry (A 包：工程作用域角色词表) ----
add_paleo_test(tst_roles LIBS paleo_store)

# ---- data/commit-coord（D 包：journal + 幂等有序提交） ----
add_paleo_test(tst_commitcoord LIBS paleo_store)    # commitAll 记档生命周期/幂等重入/崩溃恢复/catalog→qgz 顺序

# ---- data/entity-view (B 包：实体视图 + link ordinal + staleness-lite) ----
add_paleo_test(tst_entityview LIBS paleo_store)

# ---- data/catalog-integrity（goal/core-deepening A-R1：装载/保存契约） ----
add_paleo_test(tst_catalog_integrity LIBS paleo_store)  # 格架保存失败回滚/坏段行序号吸收/JSON 列损坏告警/extra 保留字

# ---- wave4/runtime-resilience ----
# 崩溃本地转储（§38 留白按本地优先落地）+ 外链源重定位恢复路径。
# 新增源与测试集中在此块；并行包合并冲突时保留双方行。
add_paleo_test(tst_crashreport LIBS paleo_services)
add_paleo_test(tst_errorhub LIBS paleo_services)      # 方向64：去重窗口/聚合/环形逐出/并发（方向76 并入 d54 独值并发用例）
# 方向76 合流：54 系呈现件（notificationmanager/notificationcard/
# errorhistorymodel/errorhistorydock）及其测试（tst_errorhub_d54 /
# tst_notification_card / tst_notification_m2_challenger /
# tst_error_history_dock / tst_errorhub_challenger_m4 /
# tst_errorhub_challenger_m5_harness）删除——hub 级语义归 tst_errorhub，
# 呈现级语义 tst_notifications（64 栈）覆盖；裁决与覆盖迁移记录见
# .goal-loop-ledger-hub-merge.md。
add_paleo_test(tst_errorhub_storm LIBS paleo_services) # 方向54/M5 起源，方向76 重定向至全局 hub：风暴压力（环形上限/多线程风暴）
set_tests_properties(tst_errorhub_storm PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
add_paleo_test(tst_relocate LIBS paleo_ui)

# ---- goal/perf-systematize 簇2：UI 线程阻塞动态验证 ----
# F1 well_log 两段式 / F2 综合柱状图 XML 两段式 / F3 GeoJSON 流式统计：
# 页构建/受理耗时 < 0.5 × 同文件整份解析（比率门）+ 事件循环探活分片。
# 计时面挂串行（对齐 tst_perfbudget/tst_correlation_full 先例）。
add_paleo_test(tst_ui_blocking LIBS paleo_ui)
set_tests_properties(tst_ui_blocking PROPERTIES RUN_SERIAL TRUE)
target_compile_definitions(tst_relocate PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")

# ---- wave4/area-parametrization ----
# AreaRules：工程级参数 seam——层序界面名单 / 分类器目录规则 / SEG-Y 道号索引
# 约定 / ONNX 期望网格（TODOS「第二工区参数化接缝」）。默认 = 本工区内置值，
# 工程目录 project_area.json 可覆盖（docs/AREA_PARAMETERS.md）。
add_paleo_test(tst_arearules LIBS paleo_io)     # 默认等价 / 自定义生效 / 坏 JSON 拒用

# ---- data/staleness (p5b：staleness 链路接线 + 发布门 advisory) ----
# 预览侧 sha 失配产标（markDownstreamStale）、发布门 stale 计数 advisory、
# 数据页「过时」徽标。catalog 内部语义在 tst_entityview；此处测两端接线。
add_paleo_test(tst_staleness LIBS paleo_ui)
target_compile_definitions(tst_staleness PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")

# ---- wave/layer-platform (图层平台：三编图页共用底座) ----
# 图层树面板 + 属性对话框 + 页面档案服务（QgsMapThemeCollection 封装）+
# 档案工具条。QgsLayerTree/主题是唯一图层可见性状态源，Paleo 只做业务接线。
add_paleo_test(tst_layerplatform LIBS paleo_qgis)     # 档案应用/主题往返 .qgz/生命周期/层位时序
add_paleo_test(tst_labelzorder LIBS paleo_qgis)     # 标注随图层 z 序：labelsWithLayer 打标 + 像素回归
add_paleo_test(tst_dragtop_render LIBS paleo_qgis)  # 拖拽到树顶后画布渲染回归（bridge 序+标注带）
add_paleo_test(tst_layerorganizer LIBS paleo_qgis)  # 图层树布局器：置顶共享/层位组/组秩/树层双绑
add_paleo_test(tst_layertreepanel LIBS paleo_ui)    # 筛选/组级显隐/复制图层/objectName 兼容
add_paleo_test(tst_layerproperties LIBS paleo_ui)   # 矢量/栅格/缺源三类 + 业务页字段
