cmake_minimum_required(VERSION 3.28)
project(paleo_workbench C CXX)

set(CMAKE_CXX_STANDARD 20) # QGIS 4.2 headers require C++20 (defaulted operator==)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON) # wave3/ux-consistency: vendored fonts qrc
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# windows.h defines max as a function-like macro; it rewrites std::max inside
# QGIS headers (C2589 on MSVC) whenever a Windows header reaches a TU first.
if(MSVC)
  add_compile_definitions(NOMINMAX)
  add_compile_options(/utf-8 /W4)
endif()

if(NOT QGIS_PREFIX AND DEFINED ENV{QGIS_PREFIX_PATH})
  set(QGIS_PREFIX "$ENV{QGIS_PREFIX_PATH}")
endif()
if(QGIS_PREFIX)
  list(PREPEND CMAKE_PREFIX_PATH "${QGIS_PREFIX}" "${QGIS_PREFIX}/apps/qt6")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test Sql Xml Concurrent Svg PrintSupport Network SerialPort Positioning UiTools OpenGLWidgets Pdf PdfWidgets)
# ---- wave3/ux-consistency D14 ----
# WebEngineWidgets is optional: without it WebViewPanel compiles its
# external-browser fallback and the build must still succeed.
find_package(Qt6 QUIET COMPONENTS WebEngineWidgets)

# QGIS: distro packages ship no cmake config — locate manually (vendor prefix later overrides these)
find_path(QGIS_INCLUDE_DIR qgsapplication.h HINTS
  "${QGIS_PREFIX}/apps/qgis/include" "${QGIS_PREFIX}/include/qgis"
  "${QGIS_PREFIX}/usr/include/qgis" PATHS /usr/include/qgis REQUIRED)
find_library(QGIS_CORE_LIB qgis_core HINTS
  "${QGIS_PREFIX}/apps/qgis/lib" "${QGIS_PREFIX}/lib" "${QGIS_PREFIX}/usr/lib" PATHS /usr/lib REQUIRED)
find_library(QGIS_GUI_LIB qgis_gui HINTS
  "${QGIS_PREFIX}/apps/qgis/lib" "${QGIS_PREFIX}/lib" "${QGIS_PREFIX}/usr/lib" PATHS /usr/lib REQUIRED)
find_library(QGIS_ANALYSIS_LIB qgis_analysis HINTS
  "${QGIS_PREFIX}/apps/qgis/lib" "${QGIS_PREFIX}/lib" "${QGIS_PREFIX}/usr/lib" PATHS /usr/lib REQUIRED)
find_path(GDAL_INCLUDE_DIR gdal.h HINTS
  "${QGIS_PREFIX}/include" "${QGIS_PREFIX}/include/gdal" "${QGIS_PREFIX}/usr/include/gdal"
  PATHS /usr/include /usr/include/gdal REQUIRED)
find_library(GDAL_LIB gdal HINTS
  "${QGIS_PREFIX}/lib" "${QGIS_PREFIX}/usr/lib" PATHS /usr/lib REQUIRED)

add_library(paleo_qgis_iface INTERFACE)
target_include_directories(paleo_qgis_iface INTERFACE ${QGIS_INCLUDE_DIR})
if(QGIS_PREFIX)
  # OSGeo4W installs SQLite, GDAL, GEOS and other public QGIS header
  # dependencies in the prefix's shared include directory.
  target_include_directories(paleo_qgis_iface INTERFACE "${QGIS_PREFIX}/include")
endif()
target_link_libraries(paleo_qgis_iface INTERFACE ${QGIS_CORE_LIB} ${QGIS_GUI_LIB} ${QGIS_ANALYSIS_LIB}
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Xml Qt6::Sql Qt6::Concurrent Qt6::Svg Qt6::PrintSupport Qt6::Network Qt6::SerialPort Qt6::Positioning Qt6::UiTools Qt6::OpenGLWidgets Qt6::Pdf Qt6::PdfWidgets)
if(TARGET Qt6::WebEngineWidgets)
  target_link_libraries(paleo_qgis_iface INTERFACE Qt6::WebEngineWidgets)
  target_compile_definitions(paleo_qgis_iface INTERFACE PALEO_HAVE_WEBENGINE=1)
  message(STATUS "Qt6 WebEngineWidgets found — embedded web shell enabled")
else()
  message(STATUS "Qt6 WebEngineWidgets NOT found — WebViewPanel degrades to the external browser")
endif()

# GPKG fixture fallback
set(FIXTURE_GPKG ${CMAKE_SOURCE_DIR}/testdata/fixture.gpkg)

# ---- ui/ribbon：SARibbon v2.9.5（vendor/saribbon，MIT，amalgamated 单文件） ----
# 静态嵌入；不带 QWindowKit（主窗走原生边框）。qrc 已内联进 SARibbon.cpp，
# 静态库资源由 PaleoMainWindow 构造时 Q_INIT_RESOURCE(SARibbonResource) 拉起。
add_library(paleo_saribbon STATIC vendor/saribbon/SARibbon.h vendor/saribbon/SARibbon.cpp)
target_include_directories(paleo_saribbon SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/vendor/saribbon)
target_compile_definitions(paleo_saribbon PUBLIC
  SA_RIBBON_BAR_NO_EXPORT SA_COLOR_WIDGETS_NO_DLL SARIBBON_USE_3RDPARTY_FRAMELESSHELPER=0)
target_link_libraries(paleo_saribbon PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_options(paleo_saribbon PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>) # 第三方源不改，静音其告警

# ---- vendor/segyio: segyio v2.0.0-alpha.1 C library ----
add_library(paleo_segyio STATIC vendor/segyio/src/segy.c)
target_include_directories(paleo_segyio SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/vendor/segyio/include)
target_compile_definitions(paleo_segyio PRIVATE
  HAVE_SYS_STAT_H HAVE_FSEEKO HAVE_FTELLO _FILE_OFFSET_BITS=64)
target_compile_options(paleo_segyio PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
set_target_properties(paleo_segyio PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)

# ---- vendor/glm: OpenGL Mathematics (header-only) ----
add_library(paleo_glm INTERFACE)
target_include_directories(paleo_glm SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/vendor/glm)

# ---- vendor/sbm: Seismic-Body-Management engine（Data/Sgy + Engine + Render/ColorMap） ----
# 上游 SeismicF3Viewer 的 headless 地震引擎，溯源与本地补丁见 vendor/sbm/{README,PATCHES}.md。
# 顶层一律经 Engine/Sdk.h 的 sdk::Dataset facade 使用；UI 侧旧引用走 domain/seismic 转发头。
add_library(paleo_sbm STATIC
    vendor/sbm/src/Data/Sgy/SgyCoordinateMapper.cpp
    vendor/sbm/src/Data/Sgy/SgyDataCache.cpp
    vendor/sbm/src/Data/Sgy/SgyFileReader.cpp
    vendor/sbm/src/Data/Sgy/SgyIndex.cpp
    vendor/sbm/src/Data/Sgy/SgyIndexBuilder.cpp
    vendor/sbm/src/Data/Sgy/SgyIndexCache.cpp
    vendor/sbm/src/Data/Sgy/SgyIndexJob.cpp
    vendor/sbm/src/Data/Sgy/SgyIndexService.cpp
    vendor/sbm/src/Data/Sgy/SgyIo.cpp
    vendor/sbm/src/Data/Sgy/SgyReadSession.cpp
    vendor/sbm/src/Data/Sgy/SgyRuleLayout.cpp
    vendor/sbm/src/Data/Sgy/SgySectionBuilder.cpp
    vendor/sbm/src/Data/Sgy/SgySequentialScan.cpp
    vendor/sbm/src/Data/Sgy/SgyTaskQueue.cpp
    vendor/sbm/src/Data/Sgy/SgyVolume.cpp
    vendor/sbm/src/Data/Sgy/SgyVolumeLookup.cpp
    vendor/sbm/src/Data/Sgy/SgyWellLocator.cpp
    vendor/sbm/src/Engine/Cache.cpp
    vendor/sbm/src/Engine/LayoutV5.cpp
    vendor/sbm/src/Engine/LodBuilder.cpp
    vendor/sbm/src/Engine/PagedPipeline.cpp
    vendor/sbm/src/Engine/PagedWorkspace.cpp
    vendor/sbm/src/Engine/PlannedReader.cpp
    vendor/sbm/src/Engine/PlannedSection.cpp
    vendor/sbm/src/Engine/QuickOpen.cpp
    vendor/sbm/src/Engine/ReadPlan.cpp
    vendor/sbm/src/Engine/RoiWorkspaceBuilder.cpp
    vendor/sbm/src/Engine/Sdk.cpp
    vendor/sbm/src/Engine/SdkC.cpp
    vendor/sbm/src/Engine/SgyVolumeSource.cpp
    vendor/sbm/src/Engine/StorageProfile.cpp
    vendor/sbm/src/Engine/TranscodeJob.cpp
    vendor/sbm/src/Engine/WorkspaceFormat.cpp
    vendor/sbm/src/Engine/WorkspaceVolumeSource.cpp
    vendor/sbm/src/Render/ColorMap.cpp)
target_include_directories(paleo_sbm SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/vendor/sbm/src)
# 上游按 C++17 写 .u8string()；char8_t 会打碎 30 处调用。头文件零 char8_t 使用，flag 即可。
target_compile_options(paleo_sbm PRIVATE
  $<IF:$<CXX_COMPILER_ID:MSVC>,/Zc:char8_t-,-fno-char8_t>
  $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>) # 第三方源不改其告警
target_link_libraries(paleo_sbm PUBLIC paleo_segyio paleo_glm PRIVATE Qt6::Core)
set_target_properties(paleo_sbm PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
# zstd 块压缩（可选；SEISMIC_HAVE_ZSTD 控制上游开关）
find_library(SBM_ZSTD_LIB zstd)
find_path(SBM_ZSTD_INCLUDE zstd.h)
if(SBM_ZSTD_LIB AND SBM_ZSTD_INCLUDE)
  target_compile_definitions(paleo_sbm PUBLIC SEISMIC_HAVE_ZSTD) # PUBLIC：消费方按此选 codec
  target_include_directories(paleo_sbm SYSTEM PRIVATE ${SBM_ZSTD_INCLUDE})
  target_link_libraries(paleo_sbm PRIVATE ${SBM_ZSTD_LIB})
endif()

add_executable(paleo_selfcheck src/selfcheck/main.cpp)
target_link_libraries(paleo_selfcheck PRIVATE paleo_qgis_iface)
target_compile_definitions(paleo_selfcheck PRIVATE FIXTURE_GPKG="${FIXTURE_GPKG}")

# ---- ONNX Runtime (vendored; spike ET4) — 先于模块库定义，paleo_ai 依赖它 ----
find_path(ORT_INCLUDE onnxruntime_cxx_api.h PATHS ${CMAKE_SOURCE_DIR}/vendor/onnxruntime/include)
find_library(ORT_LIB onnxruntime PATHS ${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib)
if(ORT_INCLUDE AND ORT_LIB)
  set(PALEO_HAVE_ORT TRUE)
  add_library(ort_vendored SHARED IMPORTED)
  set_target_properties(ort_vendored PROPERTIES IMPORTED_LOCATION ${ORT_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${ORT_INCLUDE})
  add_library(paleo_ai STATIC src/ai/onnxpredictionservice.cpp src/ai/remotepredictionservice.cpp)
  target_link_libraries(paleo_ai PUBLIC paleo_deps ort_vendored)
  target_compile_definitions(paleo_ai PUBLIC PALEO_HAVE_ORT=1)
  target_include_directories(paleo_ai PUBLIC ${CMAKE_SOURCE_DIR}/src)
  set(CMAKE_BUILD_RPATH "${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib")
else()
  set(PALEO_HAVE_ORT FALSE)
  add_library(paleo_ai STATIC src/ai/remotepredictionservice.cpp)
  target_link_libraries(paleo_ai PUBLIC Qt6::Core Qt6::Gui) # 无 ORT 时为空壳，消费方链接名不变
  message(STATUS "onnxruntime not vendored — ai/ service excluded (run ./paleo-dev bootstrap)")
endif()

# ---- 模块化分层库（按 UI_LAYER_PLAN 层契约拆分）----
# DAG（按实测 include 边排序，单向无环）：
#   domain · algorithms · ai · store(catalog+metadata) ── qgis ── io ── services
#     ── linkage · workflow ── ui ── app
# catalog↔metadata 互有 include（datacatalog→atomicfile / mapversionstore→datacatalog），
# 同层环合并为 paleo_store。paleo_core 降为 INTERFACE 伞，旧链接方零改动。
add_library(paleo_deps INTERFACE)
target_link_libraries(paleo_deps INTERFACE paleo_qgis_iface ${GDAL_LIB})
target_include_directories(paleo_deps INTERFACE ${CMAKE_SOURCE_DIR}/src ${GDAL_INCLUDE_DIR})

# ---- 数据层 ----
add_library(paleo_domain STATIC
  src/domain/types.cpp
  src/domain/wellcompositemodel.cpp
  src/domain/wellrecords.cpp                        # 井文本记录类型 + XML 嗅探（自 io 下沉）
  src/domain/projectclassifier.cpp                  # §3 path/extension classifier + xml sniff
  src/domain/arearules.cpp                          # 工程级参数 seam（docs/AREA_PARAMETERS.md）
  src/domain/seismic/nicestep.cpp
  src/domain/seismic/timedepthmodel.cpp
  src/domain/seismic/sectionwellprojector.cpp
  # sgy*.h 为指向 paleo_sbm 的转发头；实现随 vendor 库编译。
)
target_link_libraries(paleo_domain PUBLIC paleo_deps paleo_sbm paleo_segyio paleo_glm)

add_library(paleo_algorithms STATIC
  src/algorithms/paleoalgorithms.cpp
  src/algorithms/faciespolygonize.cpp
)
target_link_libraries(paleo_algorithms PUBLIC paleo_deps paleo_domain)

# catalog + metadata 同库（互依环）
add_library(paleo_store STATIC
  src/catalog/datacatalog.cpp                       # entity/link/asset/version + catalog.json
  src/catalog/roleregistry.cpp                      # RoleDef/RoleRegistry 词表 + project_area.json 覆盖
  src/catalog/entityview.cpp                        # RoleSlot/EntityView 纯查询门面
  src/metadata/paleoprojectstore.cpp
  src/metadata/paleoprojectfile.cpp
  src/metadata/layermanifest.cpp
  src/metadata/releasestore.cpp
  src/metadata/mapversionstore.cpp                  # 版本/发布状态机落表（meta sqlite）
  src/metadata/metastore.cpp                        # project.sqlite user_version 门
  src/metadata/projectlock.cpp                      # 两实例同开一工程写互斥（QLockFile）
)
target_link_libraries(paleo_store PUBLIC paleo_deps)

# ---- QGIS 封装层 ----
add_library(paleo_qgis STATIC
  src/qgis/mappingartifactwriter.cpp
  src/qgis/qgisruntime.cpp
  src/qgis/qgisprojectservice.cpp
  src/qgis/qgislayerservice.cpp
  src/qgis/qgiscanvascontroller.cpp
  src/qgis/qgiseditingservice.cpp
  src/qgis/qgisprocessingservice.cpp
  src/qgis/qgisstyleservice.cpp
  src/qgis/qgislayoutservice.cpp
  src/qgis/layoutexport.cpp                         # 层位图/设计器导出核心（QgsLayoutExporter 封装）
  src/qgis/manifestprojection.cpp
  src/qgis/factorstylewriter.cpp                    # m2(B)：单因素色带 .qml 落盘
  src/qgis/factorcontour.cpp                        # m2(B)：等值线 GDAL C API
  src/qgis/qgislayerprofile.cpp                     # 页面档案：QgsMapThemeCollection 封装 + 组档案表
)
target_link_libraries(paleo_qgis PUBLIC paleo_deps paleo_store paleo_algorithms)

add_library(paleo_io STATIC
  src/io/dataimportservice.cpp
  src/io/lasparser.cpp
  src/io/segyreader.cpp                             # wave/segy
  src/io/constraintstore.cpp                        # wave/constraint-gpkg
  src/io/wellfileparsers.cpp                        # well head / tops / TD text parsers
  src/io/horizonbinner.cpp                          # horizon scatter -> binned time raster
  src/io/timedeptool.cpp                            # TVD/MD -> TIME(ms) interpolation
  src/io/geojsonaffine.cpp                          # D11 临时配准（手工仿射）
  src/io/wellcompositexml.cpp
  src/io/ingestplan.cpp                             # C 包：纯 plan + 幂等执行器
)
target_link_libraries(paleo_io PUBLIC paleo_deps paleo_domain paleo_store paleo_qgis)

add_library(paleo_services STATIC
  src/services/toolavailability.cpp
  src/services/previewdoc.cpp                       # W1 数据页唯一数据门面
  src/services/singlefactordef.cpp                  # m2：单因素定义注册表
  src/services/algoparamschema.cpp                  # m2：算法参数 schema 注册表
  src/services/projectdata.cpp                      # 读侧门面（catalog.json + LayerManifest）
  src/services/paleotaskservice.cpp                 # D1/D2 异步任务框架
  src/services/seismicmapping.cpp                   # 测网几何↔inline/xline/CDP
  src/services/seismictaskservice.cpp               # sdk::Dataset 门面 + 任务编排
  src/services/crashreport.cpp                      # 致命信号 fd 落盘 + .running 旗标
)
target_link_libraries(paleo_services PUBLIC paleo_deps paleo_io paleo_domain paleo_store)

# ---- 功能层 ----
add_library(paleo_linkage STATIC
  src/linkage/selectioncontext.cpp
  src/linkage/wellmaplink.cpp
  src/linkage/seismicmaplink.cpp
  src/linkage/seismicsectiontool.cpp
  src/linkage/threewaylocator.cpp                  # 问题→三视图联动
)
target_link_libraries(paleo_linkage PUBLIC paleo_deps paleo_services paleo_qgis paleo_domain)

add_library(paleo_workflow STATIC
  src/workflow/mappingworkbench.cpp
  src/workflow/workflows.cpp
  src/workflow/folderimport.cpp                    # W2 文件夹/单文件导入编排
  src/workflow/projectopen.cpp                     # W2 打开/新建工程编排
  src/workflow/registration.cpp                    # W3 临时配准编排
  src/workflow/mappingworkflow.cpp                 # 厚度→约束IDW→相多边形链路
  src/workflow/mapexport.cpp                       # 层位图 PDF 导出
  src/workflow/mapversioncontroller.cpp            # 保存版本/发布编排
  src/workflow/derivedassets.cpp                   # 派生产物 + DERIVED 版本登记
)
target_link_libraries(paleo_workflow PUBLIC paleo_deps paleo_qgis paleo_store paleo_io paleo_services paleo_domain paleo_ai)

# ---- 视图层 ----
add_library(paleo_ui STATIC
  src/ui/pages/mappingworkbenchpage.cpp
  src/ui/paleomainwindow_workbench.cpp
  src/ui/paleomainwindow.cpp
  src/ui/paleomainwindow_attach.cpp                # W4 接线按页拆分
  src/ui/ribbonpanels.cpp                          # W4 ribbon 命令组构建迁出
  src/ui/pages/datalist.cpp                        # W5 数据页·列表侧
  src/ui/pages/entitypanel.cpp                     # W5 数据页·实体视图侧
  src/ui/pages/datapage.cpp                        # W5 DataPage 兼容薄壳
  src/ui/pages/predictpage.cpp                     # W5b ①智能预测
  src/ui/pages/constraintpage.cpp                  # W5b ②约束与单因素
  src/ui/pages/composepage.cpp                     # W5b ③综合编图
  src/ui/pages/validatepage.cpp                    # W5b ④验证
  src/ui/typedconstraintdrawcontroller.cpp         # m2(B)：物源线/展布线/控制点捕获
  src/ui/locator/paleolocatorfilters.cpp
  src/ui/layoutdesignershell.cpp
  src/ui/maptools/paleomaptools.cpp
  src/ui/maptools/paleoshapetools.cpp              # wave/shapetools
  src/ui/correlationpanel.cpp
  src/ui/correlation/correlationtrack.cpp          # wave/correlation
  src/ui/correlation/correlationwellcolumn.cpp
  src/ui/correlation/curvebrowser.cpp
  src/ui/correlation/depthruler.cpp
  src/ui/correlation/horizonmarkers.cpp
  src/ui/decorations/paleodecorations.cpp
  src/ui/edittools/editingtools.cpp                # wave/edit-tools
  src/ui/edittools/editingundostack.cpp
  src/ui/edittools/vertexeditortools.cpp
  src/ui/edittools/editingtoolbar.cpp
  src/ui/vertexeditorshim.cpp
  src/ui/layout/layoutitempalette.cpp              # wave/layout-designer
  src/ui/layout/layoutitempanel.cpp
  src/ui/layout/layoutexportactions.cpp
  src/ui/layout/layouttemplates.cpp
  src/ui/layout/layoutundostack.cpp
  src/ui/datapreview/datapreviewtabs.cpp           # §4 data-page preview tabs
  src/ui/dialogs/folderconfirm.cpp                 # W2 文件夹导入确认表
  src/ui/webviewpanel.cpp                          # embedded web shell (lazy, degradable)
  src/ui/wellcomposite/wellcompositetrack.cpp      # ResFormStar 多道综合柱状图
  src/ui/wellcomposite/wellcompositecanvas.cpp
  src/ui/wellcomposite/wellcompositepanel.cpp
  src/ui/wellcomposite/curveconfigdialog.cpp
  src/ui/wellcomposite/wellpositionlegendwidget.cpp
  src/ui/horizonchipbar.cpp                        # 8层位chip条（阶段E）
  src/ui/releasepanel.cpp
  src/ui/taskpanel.cpp
  src/ui/attributetablepanel.cpp
  src/ui/constraintdrawcontroller.cpp
  src/ui/layers/layertreepanel.cpp                 # 图层树面板
  src/ui/layers/layerpropertiesdialog.cpp          # 原生属性壳 + 业务页
  src/ui/layers/layerprofilebar.cpp                # 档案工具条
  src/ui/seismic3d/seismiccameracontroller.cpp     # 3D 视口相机/渲染器
  src/ui/seismic3d/seismicslicerenderer.cpp
  src/ui/seismic3d/volumeframerenderer.cpp
  src/ui/seismic3d/seismic3dviewportwidget.cpp
  src/ui/seismic3d/seismic3dviewpanel.cpp
  src/ui/seismicsection/seismicsectioncanvas.cpp   # 2D 任意线/井剖面 dock
  src/ui/seismicsection/seismicsectiondockwidget.cpp
  src/ui/paleotheme.cpp                            # DESIGN.md 字体/色 token
  src/ui/paleoicons.cpp                            # ribbon 图标出口
  src/ui/paleoribbon.cpp                           # SARibbon 壳公用件
  resources/seismic_shaders.qrc
  resources/paleo_fonts.qrc
)
target_link_libraries(paleo_ui PUBLIC paleo_deps paleo_qgis paleo_workflow paleo_services
  paleo_store paleo_linkage paleo_domain paleo_io paleo_ai paleo_saribbon paleo_sbm)

# ---- 组装根 ----
add_library(paleo_app STATIC src/app/appcontext.cpp)
target_link_libraries(paleo_app PUBLIC paleo_ui paleo_workflow paleo_linkage paleo_services
  paleo_io paleo_qgis paleo_store paleo_domain paleo_ai)

# ---- 兼容伞：旧链接方零改动，实际分派各模块 ----
add_library(paleo_core INTERFACE)
target_link_libraries(paleo_core INTERFACE
  paleo_app paleo_ui paleo_workflow paleo_linkage paleo_services paleo_io
  paleo_qgis paleo_store paleo_algorithms paleo_domain paleo_ai
  paleo_saribbon paleo_sbm paleo_segyio paleo_glm ${GDAL_LIB})
target_include_directories(paleo_core INTERFACE ${CMAKE_SOURCE_DIR}/src ${GDAL_INCLUDE_DIR})

enable_testing()

function(add_paleo_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE paleo_core Qt6::Test)
  target_compile_definitions(${name} PRIVATE FIXTURE_GPKG="${FIXTURE_GPKG}")
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

# ---- W6 层边界护栏（docs/UI_LAYER_PLAN.md）：include 归一化正向/反向扫描 +
# QtWidgets 词表禁令 + 层标记硬检查；baseline 只缩不涨。selftest 单独成项——
# 扫描器自身坏掉不能静默变绿。 ----
find_program(PALEO_PYTHON3 NAMES python3 python)
if(PALEO_PYTHON3)
  add_test(NAME layering COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_layering.py)
  add_test(NAME layering_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_layering.py --selftest)
endif()

add_paleo_test(tst_runtime)
add_paleo_test(tst_projectsvc)
add_paleo_test(tst_layerservice)
add_paleo_test(tst_canvas_tools)
add_paleo_test(tst_domain)
add_paleo_test(tst_linkage)
add_paleo_test(tst_algorithms)
add_paleo_test(tst_services2)
add_paleo_test(tst_workflows)
add_paleo_test(tst_mappingworkbench)
add_paleo_test(tst_ui)
add_paleo_test(tst_panels)
add_paleo_test(tst_mappingpages)
add_paleo_test(tst_factorworkflow) # m2(B)：单因素生成链真实栈（tst_workflows 模式）
add_paleo_test(tst_composeworkflow) # m2(C)：相属性回写 edit buffer + 壳自动编辑态（tst_ui 模式）
add_paleo_test(tst_perf)
add_paleo_test(tst_locator)
add_paleo_test(tst_layoutshell)
add_paleo_test(tst_maptools)
add_paleo_test(tst_correlation)
add_paleo_test(tst_decorations)
add_paleo_test(tst_manifestproj)
add_paleo_test(tst_import)
add_paleo_test(tst_drawctl)
add_paleo_test(tst_procdialog)
add_paleo_test(tst_rehydrate)
add_paleo_test(tst_release)
add_paleo_test(tst_releasepanel)
add_paleo_test(tst_taskpanel)
add_paleo_test(tst_attrpanel)
# ---- merged worktree waves ----
add_paleo_test(tst_segy)
add_paleo_test(tst_constraintstore)
add_paleo_test(tst_shapetools)
add_paleo_test(tst_edittools)
add_paleo_test(tst_correlation_full)
add_paleo_test(tst_layoutpalette)
add_paleo_test(tst_layoutitempanel)
add_paleo_test(tst_layoutexport)
add_paleo_test(tst_layoutundo)
add_paleo_test(tst_layoutdesigner_full)
# ---- wave/data-foundation ----
add_paleo_test(tst_catalog)
add_paleo_test(tst_projectparsers)
target_compile_definitions(tst_projectparsers PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_horizonbinner)
target_compile_definitions(tst_horizonbinner PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_segy_lines)
target_compile_definitions(tst_segy_lines PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_seismic_core)
add_paleo_test(tst_seismic_index)
add_paleo_test(tst_seismic_3d)
target_compile_definitions(tst_seismic_3d PRIVATE
  SEGY_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/testdata/project_area/mini_seismic.sgy"
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_seismic_section)
target_compile_definitions(tst_seismic_section PRIVATE
  SEGY_FIXTURE_PATH="${CMAKE_SOURCE_DIR}/testdata/project_area/mini_seismic.sgy"
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_timedeptool)
target_compile_definitions(tst_import PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_datapreview)
target_compile_definitions(tst_datapreview PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
# ---- W7 新接缝测试（docs/UI_LAYER_PLAN.md）----
add_paleo_test(tst_folderimport)
target_compile_definitions(tst_folderimport PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_previewdoc)
target_compile_definitions(tst_previewdoc PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_wellcomposite)
add_paleo_test(tst_smoke_realdata) # real-data smoke; skips unless PALEO_REAL_PROJECT_AREA is set
add_paleo_test(tst_webviewpanel)   # offscreen: engine unavailable → fallback, host survives
# ---- wave3/ux-consistency ----
# (fonts qrc + theme singleton live in paleo_core below; UI-domain tests here)
add_paleo_test(tst_uxtheme)
if(PALEO_HAVE_ORT)
  add_paleo_test(tst_onnx)
  add_paleo_test(tst_onnxworkflow)
  target_compile_definitions(tst_onnxworkflow PRIVATE
    PROJECT_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")
  set_tests_properties(tst_onnx tst_onnxworkflow PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen;LD_LIBRARY_PATH=${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib")
endif()

# ---- wave/mapping-pipeline (阶段C+E 编图链路) ----
add_paleo_test(tst_projectdata)
add_paleo_test(tst_mapping)

# ---- wave2 async-ux (D1/D2 异步任务框架 + 任务页进度条) ----
add_paleo_test(tst_taskservice)
add_paleo_test(tst_threeway)
add_paleo_test(tst_chips)
add_paleo_test(tst_versions)

# ---- wave3/derived-publish (T26/D10/D15 + §40 速度模型与 CDP 映射) ----
add_paleo_test(tst_derivedassets)
add_paleo_test(tst_seismicmapping)
target_compile_definitions(tst_seismicmapping PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")

# ---- application ----
add_executable(paleo src/app/main.cpp)
target_link_libraries(paleo PRIVATE paleo_core)

add_executable(paleo_seismic_demo tools/seismic_showcase.cpp)
target_link_libraries(paleo_seismic_demo PRIVATE paleo_core)

add_executable(tst_boot tests/tst_boot.cpp)
target_link_libraries(tst_boot PRIVATE paleo_qgis_iface Qt6::Test)
target_compile_definitions(tst_boot PRIVATE FIXTURE_GPKG="${FIXTURE_GPKG}")
add_test(NAME boot COMMAND tst_boot)
set_tests_properties(boot PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_polygonize tests/tst_polygonize.cpp spikes/polygonize/paleopolygonize.cpp)
target_link_libraries(tst_polygonize PRIVATE paleo_qgis_iface Qt6::Test ${GDAL_LIB})
target_include_directories(tst_polygonize PRIVATE ${GDAL_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/spikes/polygonize)
add_test(NAME polygonize COMMAND tst_polygonize)
set_tests_properties(polygonize PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ---- wave3/model-hardening (数据模型与测试基建加固) ----
add_paleo_test(tst_metastore)      # sqlite user_version：新库采纳/遗留升级/未来拒开
add_paleo_test(tst_segy_fixture)   # 合成最小 SEG-Y 夹具：索引/单线/角点/范围门
target_compile_definitions(tst_segy_fixture PRIVATE
  TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")
add_paleo_test(tst_algorithm_harness)  # AlgorithmTestBase harness + ≥3 算法用例
add_paleo_test(tst_perfbudget)     # 非 UI 性能预算：工程打开/folder 枚举/save 200 版本
target_compile_definitions(tst_perfbudget PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")

# ---- data/role-registry (A 包：工程作用域角色词表) ----
add_paleo_test(tst_roles)

# ---- data/commit-coord（D 包：journal + 幂等有序提交） ----
add_paleo_test(tst_commitcoord)    # commitAll 记档生命周期/幂等重入/崩溃恢复/catalog→qgz 顺序

# ---- data/entity-view (B 包：实体视图 + link ordinal + staleness-lite) ----
add_paleo_test(tst_entityview)

# ---- wave4/runtime-resilience ----
# 崩溃本地转储（§38 留白按本地优先落地）+ 外链源重定位恢复路径。
# 新增源与测试集中在此块；并行包合并冲突时保留双方行。
add_paleo_test(tst_crashreport)
add_paleo_test(tst_relocate)
target_compile_definitions(tst_relocate PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")

# ---- wave4/area-parametrization ----
# AreaRules：工程级参数 seam——层序界面名单 / 分类器目录规则 / SEG-Y 道号索引
# 约定 / ONNX 期望网格（TODOS「第二工区参数化接缝」）。默认 = 本工区内置值，
# 工程目录 project_area.json 可覆盖（docs/AREA_PARAMETERS.md）。
add_paleo_test(tst_arearules)     # 默认等价 / 自定义生效 / 坏 JSON 拒用

# ---- data/staleness (p5b：staleness 链路接线 + 发布门 advisory) ----
# 预览侧 sha 失配产标（markDownstreamStale）、发布门 stale 计数 advisory、
# 数据页「过时」徽标。catalog 内部语义在 tst_entityview；此处测两端接线。
add_paleo_test(tst_staleness)
target_compile_definitions(tst_staleness PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")

# ---- wave/layer-platform (图层平台：三编图页共用底座) ----
# 图层树面板 + 属性对话框 + 页面档案服务（QgsMapThemeCollection 封装）+
# 档案工具条。QgsLayerTree/主题是唯一图层可见性状态源，Paleo 只做业务接线。
add_paleo_test(tst_layerplatform)     # 档案应用/主题往返 .qgz/生命周期/层位时序
add_paleo_test(tst_layertreepanel)    # 筛选/组级显隐/复制图层/objectName 兼容
add_paleo_test(tst_layerproperties)   # 矢量/栅格/缺源三类 + 业务页字段
add_paleo_test(tst_layerprofilebar)   # 主题下拉/保存/管理对话框/页指示

# ---- 并行开发挂点 ----
# 各并行方向的新增源文件/测试一律放 cmake/extra-<方向>.cmake，内容为：
#   target_sources(paleo_<模块> PRIVATE src/.../foo.cpp)
#   add_paleo_test(tst_foo)
# 禁止直接改本文件的模块源列表——并行分支在此汇合零冲突。
file(GLOB _paleo_extra CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/cmake/extra-*.cmake")
foreach(_f IN LISTS _paleo_extra)
  include(${_f})
endforeach()
