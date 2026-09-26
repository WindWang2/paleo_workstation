cmake_minimum_required(VERSION 3.28)
project(paleo_workbench CXX)

set(CMAKE_CXX_STANDARD 20) # QGIS 4.2 headers require C++20 (defaulted operator==)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON) # wave3/ux-consistency: vendored fonts qrc
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# windows.h defines max as a function-like macro; it rewrites std::max inside
# QGIS headers (C2589 on MSVC) whenever a Windows header reaches a TU first.
if(MSVC)
  add_compile_definitions(NOMINMAX)
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

# GPKG fixture is generated at configure time (committed binary is the fallback copy)
find_program(OGR2OGR ogr2ogr)
set(FIXTURE_GPKG ${CMAKE_SOURCE_DIR}/testdata/fixture.gpkg)

add_executable(paleo_selfcheck src/selfcheck/main.cpp)
target_link_libraries(paleo_selfcheck PRIVATE paleo_qgis_iface)
target_compile_definitions(paleo_selfcheck PRIVATE FIXTURE_GPKG="${FIXTURE_GPKG}")

# ---- P0 spine library ----
add_library(paleo_core STATIC
  src/qgis/qgisruntime.cpp
  src/qgis/qgisprojectservice.cpp
  src/qgis/qgislayerservice.cpp
  src/qgis/qgiscanvascontroller.cpp
  src/qgis/qgiseditingservice.cpp
  src/qgis/qgisprocessingservice.cpp
  src/qgis/qgisstyleservice.cpp
  src/qgis/qgislayoutservice.cpp
  src/metadata/paleoprojectstore.cpp
  src/metadata/layermanifest.cpp
  src/metadata/releasestore.cpp
  src/services/toolavailability.cpp
  src/domain/types.cpp
  src/linkage/selectioncontext.cpp
  src/linkage/wellmaplink.cpp
  src/algorithms/paleoalgorithms.cpp
  src/algorithms/faciespolygonize.cpp
  src/workflow/workflows.cpp
  src/ui/paleomainwindow.cpp
  src/ui/pages/pagepanels.cpp
  src/ui/locator/paleolocatorfilters.cpp
  src/ui/layoutdesignershell.cpp
  src/ui/maptools/paleomaptools.cpp
  src/ui/correlationpanel.cpp
  src/ui/decorations/paleodecorations.cpp
  src/ui/vertexeditorshim.cpp
  src/app/appcontext.cpp
  src/linkage/seismicmaplink.cpp
  src/qgis/manifestprojection.cpp
  src/io/dataimportservice.cpp
  src/io/lasparser.cpp
  src/ui/constraintdrawcontroller.cpp
  src/ui/releasepanel.cpp
  src/ui/taskpanel.cpp
  src/ui/attributetablepanel.cpp
  # ---- merged worktree waves ----
  src/io/segyreader.cpp                          # wave/segy
  src/io/constraintstore.cpp                     # wave/constraint-gpkg
  src/ui/maptools/paleoshapetools.cpp            # wave/shapetools
  src/ui/edittools/editingtools.cpp              # wave/edit-tools
  src/ui/edittools/editingundostack.cpp
  src/ui/edittools/vertexeditortools.cpp
  src/ui/edittools/editingtoolbar.cpp
  src/ui/correlation/correlationtrack.cpp        # wave/correlation
  src/ui/correlation/correlationwellcolumn.cpp
  src/ui/correlation/curvebrowser.cpp
  src/ui/correlation/depthruler.cpp
  src/ui/correlation/horizonmarkers.cpp
  src/ui/layout/layoutitempalette.cpp            # wave/layout-designer
  src/ui/layout/layoutitempanel.cpp
  src/ui/layout/layoutexportactions.cpp
  src/ui/layout/layouttemplates.cpp
  src/ui/layout/layoutundostack.cpp
  # ---- wave/data-foundation (keep contiguous for parallel-package merges) ----
  src/catalog/datacatalog.cpp                    # entity/link/asset/version + catalog.json
  src/io/projectclassifier.cpp                   # §3 path/extension classifier + xml sniff
  src/io/wellfileparsers.cpp                     # well head / tops / TD text parsers
  src/io/horizonbinner.cpp                       # horizon scatter -> binned time raster
  src/io/timedeptool.cpp                         # TVD/MD -> TIME(ms) interpolation
  src/io/geojsonaffine.cpp                       # D11 临时配准（手工仿射）
  src/ui/datapreview/datapreviewtabs.cpp         # §4 data-page preview tabs
  src/ui/webviewpanel.cpp                        # embedded web shell (lazy, degradable)
)
target_link_libraries(paleo_core PUBLIC paleo_qgis_iface ${GDAL_LIB})
target_include_directories(paleo_core PUBLIC ${CMAKE_SOURCE_DIR}/src ${GDAL_INCLUDE_DIR})

# ---- wave3/ux-consistency ----
# DESIGN.md typography: vendored fonts (Noto Sans SC + JetBrains Mono) travel
# inside the binary so UI text and render-regression baselines are identical
# on every platform. Registered by PaleoTheme::ensureApplicationFonts().
target_sources(paleo_core PRIVATE src/ui/paleotheme.cpp resources/paleo_fonts.qrc)

# ONNX Runtime (vendored; spike ET4). The shipped cmake config assumes lib64/ — we use lib/.
find_path(ORT_INCLUDE onnxruntime_cxx_api.h PATHS ${CMAKE_SOURCE_DIR}/vendor/onnxruntime/include)
find_library(ORT_LIB onnxruntime PATHS ${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib)
if(ORT_INCLUDE AND ORT_LIB)
  set(PALEO_HAVE_ORT TRUE)
  add_library(ort_vendored SHARED IMPORTED)
  set_target_properties(ort_vendored PROPERTIES IMPORTED_LOCATION ${ORT_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${ORT_INCLUDE})
  target_sources(paleo_core PRIVATE src/ai/onnxpredictionservice.cpp)
  target_link_libraries(paleo_core PUBLIC ort_vendored)
  target_compile_definitions(paleo_core PUBLIC PALEO_HAVE_ORT=1)
  set(CMAKE_BUILD_RPATH "${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib")
else()
  set(PALEO_HAVE_ORT FALSE)
  message(STATUS "onnxruntime not vendored — ai/ service excluded (run ./paleo-dev bootstrap)")
endif()

enable_testing()

function(add_paleo_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE paleo_core Qt6::Test)
  target_compile_definitions(${name} PRIVATE FIXTURE_GPKG="${FIXTURE_GPKG}")
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

add_paleo_test(tst_runtime)
add_paleo_test(tst_projectsvc)
add_paleo_test(tst_layerservice)
add_paleo_test(tst_canvas_tools)
add_paleo_test(tst_domain)
add_paleo_test(tst_linkage)
add_paleo_test(tst_algorithms)
add_paleo_test(tst_services2)
add_paleo_test(tst_workflows)
add_paleo_test(tst_ui)
add_paleo_test(tst_panels)
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
add_paleo_test(tst_timedeptool)
target_compile_definitions(tst_import PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_datapreview)
target_compile_definitions(tst_datapreview PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
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
target_sources(paleo_core PRIVATE
  src/services/projectdata.cpp                    # 读侧门面（catalog.json + LayerManifest）
  src/workflow/mappingworkflow.cpp                # 厚度→约束IDW→相多边形链路 + 时间残差
  src/workflow/mapexport.cpp                      # 层位图 PDF 导出（井位+相多边形）
  src/linkage/threewaylocator.cpp                 # 问题→三视图联动
  src/ui/horizonchipbar.cpp                       # 8层位chip条（阶段E）
  src/metadata/mapversionstore.cpp                # 版本/发布状态机落表（meta sqlite）
  src/workflow/mapversioncontroller.cpp           # 保存版本/发布编排（§41.7/§1223）
)
add_paleo_test(tst_projectdata)
add_paleo_test(tst_mapping)

# ---- wave2 async-ux (D1/D2 异步任务框架 + 任务页进度条) ----
target_sources(paleo_core PRIVATE
  src/services/paleotaskservice.cpp               # D1/D2 异步任务注册+池化执行+字节进度+10s ETA+协作取消
)
add_paleo_test(tst_taskservice)
add_paleo_test(tst_threeway)
add_paleo_test(tst_chips)
add_paleo_test(tst_versions)

# ---- wave3/derived-publish (T26/D10/D15 + §40 速度模型与 CDP 映射) ----
target_sources(paleo_core PRIVATE
  src/workflow/derivedassets.cpp                  # 派生产物落 artifacts/derived + DERIVED 版本登记
  src/services/seismicmapping.cpp                 # 测网几何↔inline/xline/CDP + 井 TD 速度桥
)
add_paleo_test(tst_derivedassets)
add_paleo_test(tst_seismicmapping)
target_compile_definitions(tst_seismicmapping PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")

# ---- application ----
add_executable(paleo src/app/main.cpp)
target_link_libraries(paleo PRIVATE paleo_core)

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
target_sources(paleo_core PRIVATE
  src/metadata/metastore.cpp                         # project.sqlite user_version 门
  src/metadata/projectlock.cpp                       # 两实例同开一工程的写互斥（QLockFile）
)
add_paleo_test(tst_metastore)      # sqlite user_version：新库采纳/遗留升级/未来拒开
add_paleo_test(tst_segy_fixture)   # 合成最小 SEG-Y 夹具：索引/单线/角点/范围门
target_compile_definitions(tst_segy_fixture PRIVATE
  TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")
add_paleo_test(tst_algorithm_harness)  # AlgorithmTestBase harness + ≥3 算法用例
add_paleo_test(tst_perfbudget)     # 非 UI 性能预算：工程打开/folder 枚举/save 200 版本
target_compile_definitions(tst_perfbudget PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")

# ---- wave4/runtime-resilience ----
# 崩溃本地转储（§38 留白按本地优先落地）+ 外链源重定位恢复路径。
# 新增源与测试集中在此块；并行包合并冲突时保留双方行。
target_sources(paleo_core PRIVATE
  src/services/crashreport.cpp                  # 致命信号 fd 落盘 + .running 会话旗标 + 重启提示文案
)
add_paleo_test(tst_crashreport)
add_paleo_test(tst_relocate)
target_compile_definitions(tst_relocate PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
