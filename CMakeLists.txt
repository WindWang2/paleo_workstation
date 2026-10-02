cmake_minimum_required(VERSION 3.28)
project(paleo_workbench C CXX)

set(CMAKE_CXX_STANDARD 20) # QGIS 4.2 headers require C++20 (defaulted operator==)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 本仓无 C++20 named modules；CMake≥3.28+Ninja 的默认扫描会往每条编译命令
# 塞 -fdeps-format/-fmodule-mapper（GCC 专属 flag，clang-tidy 不认，见
# tools/check_tidy.py）还拖慢编译——显式关掉。
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON) # wave3/ux-consistency: vendored fonts qrc
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# windows.h defines max as a function-like macro; it rewrites std::max inside
# QGIS headers (C2589 on MSVC) whenever a Windows header reaches a TU first.
if(MSVC)
  add_compile_definitions(NOMINMAX)
  add_compile_options(/utf-8 /W4)
endif()

# ---- 编译缓存 launcher（devex T6）：ccache/sccache 任一在 PATH 即启用 ----
# 已显式给 launcher（-DCMAKE_CXX_COMPILER_LAUNCHER=...）或 PALEO_NO_CCACHE=1 时不抢。
if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER AND NOT PALEO_NO_CCACHE)
  find_program(PALEO_CCACHE_EXE NAMES ccache sccache)
  if(PALEO_CCACHE_EXE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${PALEO_CCACHE_EXE}")
    set(CMAKE_C_COMPILER_LAUNCHER "${PALEO_CCACHE_EXE}")
    message(STATUS "compile cache launcher: ${PALEO_CCACHE_EXE}")
  endif()
endif()

# ---- sanitizer 档（devex T7，实验性，默认 OFF；GNU/Clang 限定）----
# ASAN 档测试默认关 LSAN（QGIS/Qt 插件/单例的"泄漏"是设计内行为，见
# tools/lsan-suppressions.txt；猎泄漏时手动开 detect_leaks=1 + suppressions）。
# UBSAN 保持可恢复（只打印栈），做巡检不做门禁。
option(PALEO_ENABLE_ASAN "AddressSanitizer 实验档（测试默认关 leak 检测）" OFF)
option(PALEO_ENABLE_UBSAN "UndefinedBehaviorSanitizer 巡检档（可恢复）" OFF)
# ---- goal/perf-systematize 簇4：LTO 实验档（默认 OFF）----
# 依据（2026-10-01 本机交错三轮实测，docs/perf/BUILD_OPT.md）：LAS 冷解析
# -40.7%、SEG-Y 索引命中 -45.2%、catalog 灌库 1k -24.9%/查询 -18.2%，
# 无一致回归；代价 = 全量链接时间 ~2.3×（增量影响小）。-O3 已实验否决
# （catalog_open_10k +35% 两轮一致回归）不入档为选项。
option(PALEO_ENABLE_LTO "链接期优化档（-flto=8；热路径解析/索引收益 20-45%，默认 OFF）" OFF)
if(PALEO_ENABLE_LTO)
  if(MSVC)
    message(FATAL_ERROR "PALEO_ENABLE_LTO 仅支持 GNU/Clang（MSVC 走 /GL /LTCG 手动档）")
  endif()
  include(CheckCXXSourceCompiles)
  set(_lto_saved_cxx "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "-flto=8")
  check_cxx_source_compiles("int main(){return 0;}" PALEO_LTO_LINK_OK)
  set(CMAKE_REQUIRED_FLAGS "${_lto_saved_cxx}")
  if(NOT PALEO_LTO_LINK_OK)
    message(FATAL_ERROR "PALEO_ENABLE_LTO：工具链不支持 -flto=8（探测失败）")
  endif()
  add_compile_options(-flto=8 -ffat-lto-objects)
  add_link_options(-flto=8)
endif()
# ---- 实验性 unity build（devex T6）：逐 target opt-in，默认 OFF ----
# paleo_sbm（-fno-char8_t TU 语义）/paleo_segyio（纯 C）/paleo_saribbon（vendor）
# 显式排除；AUTOMOC 与 unity 的组合按 CMake 语义兼容，但跨 TU 静态同名符号
# 有合批冲突风险——只作本地加速实验，不进 CI 默认路径。
option(PALEO_UNITY_BUILD "实验性 unity build（vendor 目标显式排除）" OFF)
if(PALEO_ENABLE_ASAN OR PALEO_ENABLE_UBSAN)
  if(MSVC)
    message(FATAL_ERROR "sanitizer 档仅支持 GNU/Clang（MSVC 走 /fsanitize 手动档）")
  endif()
  add_compile_options(-fno-omit-frame-pointer -g)
  add_link_options(-fno-omit-frame-pointer)
endif()
if(PALEO_ENABLE_ASAN)
  add_compile_options(-fsanitize=address)
  add_link_options(-fsanitize=address)
endif()
if(PALEO_ENABLE_UBSAN)
  add_compile_options(-fsanitize=undefined)
  add_link_options(-fsanitize=undefined)
endif()

# QGIS_PREFIX 必须进 cache：CMakeLists 变更时 make 自动重配，若依赖 env
# 则任何无 QGIS_PREFIX_PATH 的 shell（含并发 agent 会话）重配会静默丢
# vendor 定位——PALEO_QGIS_PREFIX_DEFAULT/测试 env 注入全失效，回落系统栈。
set(QGIS_PREFIX "" CACHE PATH "QGIS install prefix（vendor superbuild/deb 闭包；空=系统兜底）")
if(NOT QGIS_PREFIX AND DEFINED ENV{QGIS_PREFIX_PATH})
  set(QGIS_PREFIX "$ENV{QGIS_PREFIX_PATH}" CACHE PATH "QGIS install prefix" FORCE)
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

# ============================================================================
# 4-Tier Dependency Architecture (ARCH-03 / P1-05)
# Tier 1: paleo_qt_core        (Headless Qt Core/Gui/Xml/Sql/Concurrent/Network)
# Tier 2: paleo_qgis_core_deps (Headless QGIS Core/Analysis + Tier 1)
# Tier 3: paleo_deps           (Data Umbrella: Tier 2 + GDAL + src include)
# Tier 4: paleo_ui_deps        (UI/Presentation: QGIS GUI + QtWidgets + UI tools)
# Compatibility: paleo_qgis_iface (Combines Tier 2 + Tier 4)
# ============================================================================

add_library(paleo_qt_core INTERFACE)
target_link_libraries(paleo_qt_core INTERFACE
  Qt6::Core Qt6::Gui Qt6::Xml Qt6::Sql Qt6::Concurrent Qt6::Network Qt6::SerialPort Qt6::Positioning)

add_library(paleo_qgis_core_deps INTERFACE)
target_include_directories(paleo_qgis_core_deps INTERFACE ${QGIS_INCLUDE_DIR})
if(QGIS_PREFIX)
  # OSGeo4W installs SQLite, GDAL, GEOS and other public QGIS header
  # dependencies in the prefix's shared include directory.
  target_include_directories(paleo_qgis_core_deps INTERFACE "${QGIS_PREFIX}/include")
  # 默认运行时 prefix 随构建钉住：裸跑（无 QGIS_PREFIX_PATH env）时
  # provider/srs.db 仍解析到二进制实际链接的 vendor prefix，不静默回落
  # 系统栈（链接与运行资源栈分裂）。env/CLI 覆盖语义不变。
  target_compile_definitions(paleo_qgis_core_deps INTERFACE
    PALEO_QGIS_PREFIX_DEFAULT="${QGIS_PREFIX}")
endif()
target_link_libraries(paleo_qgis_core_deps INTERFACE
  ${QGIS_CORE_LIB} ${QGIS_ANALYSIS_LIB} paleo_qt_core)

add_library(paleo_ui_deps INTERFACE)
target_link_libraries(paleo_ui_deps INTERFACE
  ${QGIS_GUI_LIB} Qt6::Widgets Qt6::OpenGLWidgets Qt6::Svg Qt6::PrintSupport Qt6::UiTools Qt6::Pdf Qt6::PdfWidgets)
if(TARGET Qt6::WebEngineWidgets)
  target_link_libraries(paleo_ui_deps INTERFACE Qt6::WebEngineWidgets)
  target_compile_definitions(paleo_ui_deps INTERFACE PALEO_HAVE_WEBENGINE=1)
  message(STATUS "Qt6 WebEngineWidgets found — embedded web shell enabled")
else()
  message(STATUS "Qt6 WebEngineWidgets NOT found — WebViewPanel degrades to the external browser")
endif()

add_library(paleo_qgis_iface INTERFACE)
target_link_libraries(paleo_qgis_iface INTERFACE paleo_qgis_core_deps paleo_ui_deps)

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

# ---- vendor/segyio: segyio v2.0.0-alpha.1 C library（本地补丁见 vendor/segyio/PATCHES.md）----
add_library(paleo_segyio STATIC vendor/segyio/src/segy.c)
target_include_directories(paleo_segyio SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/vendor/segyio/include)
# 64 位 seek 宏按平台：MSVC 无 fseeko/ftello（LLP64 long=32bit 走
# _fseeki64/_ftelli64 分支），POSIX 走 fseeko/ftello。
if(MSVC)
  target_compile_definitions(paleo_segyio PRIVATE HAVE_SYS_STAT_H HAVE_FSEEKI64 HAVE_FTELLI64)
else()
  target_compile_definitions(paleo_segyio PRIVATE
    HAVE_SYS_STAT_H HAVE_FSEEKO HAVE_FTELLO _FILE_OFFSET_BITS=64)
endif()
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
if(WIN32)
  # Windows 上 find_library 命中的是导入库 .lib；SHARED IMPORTED 要求
  # IMPORTED_LOCATION=DLL、IMPORTED_IMPLIB=.lib，否则 configure 期报
  # "IMPORTED_IMPLIB not set"。dll 与 .lib 同居 lib/（manifest layout.bin）。
  find_file(ORT_DLL onnxruntime.dll PATHS ${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib)
endif()
if(ORT_INCLUDE AND ORT_LIB AND (NOT WIN32 OR ORT_DLL))
  set(PALEO_HAVE_ORT TRUE)
  add_library(ort_vendored SHARED IMPORTED)
  if(WIN32)
    set_target_properties(ort_vendored PROPERTIES IMPORTED_LOCATION ${ORT_DLL}
      IMPORTED_IMPLIB ${ORT_LIB} INTERFACE_INCLUDE_DIRECTORIES ${ORT_INCLUDE})
  else()
    set_target_properties(ort_vendored PROPERTIES IMPORTED_LOCATION ${ORT_LIB}
      INTERFACE_INCLUDE_DIRECTORIES ${ORT_INCLUDE})
  endif()
  add_library(paleo_ai STATIC src/ai/onnxpredictionservice.cpp src/ai/onnxfixture.cpp
    src/ai/tileinference.cpp src/ai/horizonsuggest.cpp src/ai/modelregistry.cpp
    src/ai/remotepredictionservice.cpp src/ai/remotepredictrouter.cpp)
  target_link_libraries(paleo_ai PUBLIC paleo_deps ort_vendored)
  target_compile_definitions(paleo_ai PUBLIC PALEO_HAVE_ORT=1)
  # 审计 01 M5：vendorRuntimeDir() 从可执行文件往上找 vendor/onnxruntime，构建目录在
  # 源码树外（-B ../b-asan）时找不到——编译期注入 ORT 根作为向上查找后的回落。
  get_filename_component(_paleo_ort_root "${ORT_INCLUDE}" DIRECTORY)
  target_compile_definitions(paleo_ai PRIVATE PALEO_ORT_ROOT="${_paleo_ort_root}")
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
target_link_libraries(paleo_deps INTERFACE paleo_qgis_core_deps ${GDAL_LIB})
target_include_directories(paleo_deps INTERFACE ${CMAKE_SOURCE_DIR}/src ${GDAL_INCLUDE_DIR})

# ---- 数据层 ----
add_library(paleo_domain STATIC
  src/domain/singlefactorrequest.cpp
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
  src/algorithms/seismicattr.cpp
  src/algorithms/singlefactor/curvekernel.cpp
  src/algorithms/singlefactor/support.cpp
  src/algorithms/singlefactor/localidw.cpp
  src/algorithms/singlefactor/samples.cpp
  src/algorithms/singlefactor/localdirectionalgorithm.cpp
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
  src/qgis/mapbooklayout.cpp                        # 地图册：逐格/蒙太奇/索引页版面构建 + 落盘回读校验
  src/qgis/manifestprojection.cpp
  src/qgis/factorstylewriter.cpp                    # m2(B)：单因素色带 .qml 落盘
  src/qgis/factorcontour.cpp                        # m2(B)：等值线 GDAL C API
  src/qgis/qgislayerprofile.cpp                     # 页面档案：QgsMapThemeCollection 封装 + 组档案表
  src/qgis/previewmapcanvas.cpp                     # P2 D1.1 预览画布封装（私有层容器+CRS+历史栈+渐进渲染）
  src/qgis/previewmaptools.cpp                      # P2 D1.2 预览工具注册表（量测/identify/剖面线）
  src/qgis/previewrasteranalysis.cpp                # P2 D2.2/D5.x 预览栅格分析（统计/拉伸/直方图/剖面/极值）
  src/qgis/previewidentify.cpp                      # P2 D7 identify 查询核心（空间索引缓存+栅格插值）
  src/qgis/previewrendercache.cpp                   # P2 D6.2/D6.6 渲染结果缓存（内存 LRU + QTemporaryDir PNG）
  src/qgis/seismicsectiontool.cpp
)
target_link_libraries(paleo_qgis PUBLIC paleo_deps paleo_ui_deps paleo_store paleo_algorithms)

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
target_link_libraries(paleo_io PUBLIC paleo_deps paleo_domain paleo_store)

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
  src/services/startuptrace.cpp                     # goal/perf-systematize 簇1：启动分段仪表
)
target_link_libraries(paleo_services PUBLIC paleo_deps paleo_io paleo_domain paleo_store paleo_algorithms)

# ---- 功能层 ----
add_library(paleo_linkage STATIC
  src/linkage/selectioncontext.cpp
  src/linkage/wellmaplink.cpp
  src/linkage/seismicmaplink.cpp
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
  src/workflow/mapbook.cpp                         # 地图册：AOI 格序列 + 模板变量（纯编排）
  src/workflow/mapbookqueue.cpp                    # 地图册：批量导出队列（任务服务 + 逐版记账）
  src/workflow/mapversioncontroller.cpp            # 保存版本/发布编排
  src/workflow/aiassistworkflow.cpp
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
  src/ui/layout/mapbookpanel.cpp                    # goal/mapbook-reporting：批量参数面板（只发信号）
  src/ui/datapreview/datapreviewtabs.cpp           # §4 data-page preview tabs
  src/ui/datapreview/previewmappage.cpp            # P2 D1.x/D3.x 预览地图组合页（工具条/状态条/侧栏/鹰眼/书签）
  src/ui/datapreview/previewtocpanel.cpp           # P2 D4.x 迷你 TOC + 符号快调 + 图例
  src/ui/datapreview/previewidentifypanel.cpp      # P2 D7.x identify 结果面板 + 全表模式
  src/ui/datapreview/previewprofilepanel.cpp       # P2 D5.1-5.4 层位剖面图面板
  src/ui/datapreview/previewhistogramwidget.cpp    # P2 D2.3/D5.8 直方图小图
  src/ui/datapreview/previewmapstates.cpp          # P2 D1.7/D2.x 状态页 + 会话记忆
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
target_link_libraries(paleo_ui PUBLIC paleo_deps paleo_ui_deps paleo_qgis paleo_workflow paleo_services
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
  paleo_saribbon paleo_sbm paleo_segyio paleo_glm ${GDAL_LIB} paleo_ui_deps)
target_include_directories(paleo_core INTERFACE ${CMAKE_SOURCE_DIR}/src ${GDAL_INCLUDE_DIR})

# ---- 实验性 unity build（devex T6）：PALEO_UNITY_BUILD=ON 时仅逐产品 target ----
# vendor 三家（sbm/segyio/saribbon）显式排除——见顶部 option 注释。
# 实测结论（build-unity 轮）：ON 当前**编译不过**——全仓 .cpp 惯用同名
# 匿名 helper（setError/connectionNameFor…），metadata/catalog/qgis 等目标
# 合批即 redefinition。开关按约定保留（默认 OFF，不影响任何现有路径），
# 真正可用需先做 helper 去重/改名（src/ 改动归各模块方向，递延 devex.md）；
# 逐源 SKIP_UNITY_BUILD_INCLUSION 可作局部逃生门但维护成本高于收益。
if(PALEO_UNITY_BUILD)
  foreach(_t paleo_domain paleo_algorithms paleo_store paleo_qgis paleo_io
             paleo_services paleo_linkage paleo_workflow paleo_ui paleo_app)
    set_target_properties(${_t} PROPERTIES UNITY_BUILD TRUE UNITY_BUILD_BATCH_SIZE 8)
  endforeach()
endif()

enable_testing()

# ---- 测试环境沙箱（devex T2 竞态根治，单点定义）----
# 每测试独立 XDG/HOME 目录：QSettings NativeFormat 在 Linux 走 XDG_CONFIG_HOME，
# tst_panels/tst_webviewpanel 等不再互踩真实 ~/.config/paleo（tst_ui/tst_procdialog
# 曾靠 .cpp 里 setPath 自救，现收归测试布线，零 .cpp 改动）。
# 已知边界：Windows NativeFormat 走注册表不受 env 控制——Windows 侧仍靠
# 串行 ctest（ci.yml windows leg 未开并行）；文档见 BUILDING.md。
# 注意 set_tests_properties 的 ENVIRONMENT 是整体覆盖非追加：外部对本函数
# 注入环境做扩展，非路径变量用 set_property(TEST ... APPEND PROPERTY ...)；
# LD_LIBRARY_PATH 等路径变量必须走 ENVIRONMENT_MODIFICATION 的
# path_list_prepend（APPEND 一条整体赋值仍会替换外部注入值，见下方
# tst_onnx 的修复注记）。
function(paleo_test_sandbox name)
  set(_home ${CMAKE_BINARY_DIR}/ctest-home/${name})
  file(MAKE_DIRECTORY "${_home}/config" "${_home}/data" "${_home}/cache" "${_home}/tmp")
  set_tests_properties(${name} PROPERTIES ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${_home}/config;XDG_DATA_HOME=${_home}/data;XDG_CACHE_HOME=${_home}/cache;HOME=${_home};TMPDIR=${_home}/tmp;TEMP=${_home}/tmp;TMP=${_home}/tmp")
  if(QGIS_PREFIX)
    # vendor 构建下测试资源/provider 也走构建所链的 prefix（与二进制
    # RUNPATH 一致），不借系统 /usr 栈——裸 ctest 即全 vendored。
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
      "QGIS_PREFIX_PATH=${QGIS_PREFIX}")
  endif()
  if(PALEO_ENABLE_ASAN)
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
      "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:print_summary=1")
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
      "LSAN_OPTIONS=detect_leaks=0")
  endif()
  if(PALEO_ENABLE_UBSAN)
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
      "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=0")
  endif()
endfunction()

# ---- 跨方向契约：add_paleo_test(name [LIBS ...]) ----
# 单参旧式调用零改动可用（回退 paleo_core 伞）——cmake/extra-*.cmake 里
# 四个并行方向都是旧式调用，勿破坏。新式 LIBS 给最小链接集（链接瘦身 T1：
# touch 单模块不再牵连全部测试 relink）；LIBS 按依赖序书写，首模块决定
# ctest LABEL（CI -L 分步用，不拆 job）。
# 注册行保持单行；并行合并冲突时保留双方行。
function(add_paleo_test name)
  cmake_parse_arguments(PARSE_ARGV 1 apt "" "" "LIBS")
  set(_libs ${apt_LIBS})
  if(NOT _libs)
    set(_libs paleo_core)
  endif()
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE ${_libs} Qt6::Test)
  target_compile_definitions(${name} PRIVATE FIXTURE_GPKG="${FIXTURE_GPKG}")
  add_test(NAME ${name} COMMAND ${name})
  paleo_test_sandbox(${name})
  set(_label core)
  foreach(_l IN LISTS _libs)
    if(_l MATCHES "^paleo_[a-z]+$" AND NOT _l STREQUAL "paleo_core")
      string(REGEX REPLACE "^paleo_" "" _label "${_l}")
      break()
    endif()
  endforeach()
  set_tests_properties(${name} PROPERTIES LABELS "${_label}")
endfunction()

# ---- W6 层边界护栏（docs/UI_LAYER_PLAN.md）：include 归一化正向/反向扫描 +
# QtWidgets 词表禁令 + 层标记硬检查；词表外置 tools/layering_vocab.json
# （scripts/new_module.sh 同步维护）。baseline 已归零，--strict 是防回升闸门
# （非空即红）；selftest 单独成项——扫描器自身坏掉不能静默变绿。 ----
find_program(PALEO_PYTHON3 NAMES python3 python)
if(PALEO_PYTHON3)
  add_test(NAME layering COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_layering.py)
  add_test(NAME layering_strict COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_layering.py --strict)
  add_test(NAME layering_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_layering.py --selftest)
  # goal/ui-experience-polish：UI 不变量闸门——DESIGN.md Motion 禁编排动画
  #（src/ui 零 QPropertyAnimation/QEasingCurve/setAnimated(true)）+ QSS 裸
  # hex 零容忍（token 单一真源 = paleotheme；baseline 只缩不涨，strict 防回升）。
  add_test(NAME ui_invariants COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_invariants.py)
  add_test(NAME ui_invariants_strict COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_invariants.py --strict)
  add_test(NAME ui_invariants_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_invariants.py --selftest)
  # #77：i18n 裸字面量门禁（#44「无 CI 门禁」收口）。脚本按自身位置定位
  # src/，WORKING_DIRECTORY 显式钉源码根作双保险。
  add_test(NAME i18n COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_i18n.py)
  add_test(NAME i18n_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_i18n.py --selftest)
  set_tests_properties(layering layering_strict layering_selftest
                       ui_invariants ui_invariants_strict ui_invariants_selftest
                       i18n i18n_selftest
                       PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- 测试注册（devex T1 链接瘦身：LIBS = 各测试 include 面的最小闭包，见
# docs/progress/devex.md 对比表；单参 = 伞式旧式契约，保留 5 个作为兼容样本）----
add_paleo_test(tst_runtime LIBS paleo_qgis)                 # metadata,qgis
add_paleo_test(tst_projectsvc LIBS paleo_qgis)              # metadata,qgis
add_paleo_test(tst_layerservice LIBS paleo_qgis)            # catalog,metadata,qgis
add_paleo_test(tst_canvas_tools LIBS paleo_services paleo_qgis) # metadata,qgis,services
add_paleo_test(tst_domain LIBS paleo_domain)
add_paleo_test(tst_linkage LIBS paleo_linkage)
add_paleo_test(tst_algorithms LIBS paleo_algorithms paleo_ui_deps)
add_paleo_test(tst_seismicattr LIBS paleo_algorithms)      # 地震属性核（纯数值，无 QGIS init）
add_paleo_test(tst_singlefactor_kernel LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_perf LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_contours LIBS paleo_qgis)
add_paleo_test(tst_singlefactor_contourbudget LIBS paleo_algorithms paleo_qgis)
add_paleo_test(tst_singlefactor_guilatency LIBS paleo_algorithms paleo_services)
add_paleo_test(tst_singlefactor_faults LIBS paleo_workflow)
add_paleo_test(tst_singlefactor_asynccontour LIBS paleo_workflow)
add_paleo_test(tst_singlefactor_parity LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_contract LIBS paleo_domain)
add_paleo_test(tst_singlefactor_samples LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_store LIBS paleo_io)
add_paleo_test(tst_services2 LIBS paleo_qgis)               # algorithms,metadata,qgis
add_paleo_test(tst_workflows LIBS paleo_workflow)
add_paleo_test(tst_mappingworkbench)                    # 伞式（ui/pages+qgis+workflow 全栈面，兼容样本）
add_paleo_test(tst_ui)                                      # 伞式（app 面全栈，兼容样本）
add_paleo_test(tst_panels)                                  # 伞式（全栈，兼容样本）
# RUN_SERIAL 独占机器跑：dataops_d2_filterPerformance10k 的 100ms 墙钟断言
# 在 -j8 并行负载下偶发超限（108ms 实测）——同 tst_perf/tst_correlation_full
# 的计时抖动关回串行基线先例（根治归 TEST-02 类递延）。
set_tests_properties(tst_panels PROPERTIES RUN_SERIAL TRUE)
add_paleo_test(tst_mappingpages LIBS paleo_ui)
add_paleo_test(tst_factorworkflow LIBS paleo_workflow) # m2(B)：单因素生成链真实栈（tst_workflows 模式）
add_paleo_test(tst_composeworkflow) # m2(C)：相属性回写 edit buffer + 壳自动编辑态（tst_ui 模式；伞式兼容样本）
add_paleo_test(tst_perf LIBS paleo_qgis)
set_tests_properties(tst_perf PROPERTIES RUN_SERIAL TRUE)
add_paleo_test(tst_locator LIBS paleo_ui)
add_paleo_test(tst_layoutshell LIBS paleo_ui)
add_paleo_test(tst_maptools LIBS paleo_ui)
add_paleo_test(tst_correlation LIBS paleo_ui)
add_paleo_test(tst_decorations LIBS paleo_ui)
add_paleo_test(tst_manifestproj LIBS paleo_qgis)
add_paleo_test(tst_import LIBS paleo_io paleo_qgis)
add_paleo_test(tst_drawctl LIBS paleo_ui)
add_paleo_test(tst_procdialog LIBS paleo_qgis)
add_paleo_test(tst_rehydrate)                               # 伞式（app 面全栈，兼容样本）
add_paleo_test(tst_release LIBS paleo_store)
add_paleo_test(tst_releasepanel LIBS paleo_ui)
add_paleo_test(tst_taskpanel LIBS paleo_ui)
add_paleo_test(tst_attrpanel LIBS paleo_ui)
# ---- merged worktree waves ----
add_paleo_test(tst_segy LIBS paleo_io)
add_paleo_test(tst_las LIBS paleo_io)
add_paleo_test(tst_snapshot_dedup LIBS paleo_io) # #84：快照/活对象 dedup 复核同一语义（真重哈希）
add_paleo_test(tst_constraintstore LIBS paleo_workflow)
add_paleo_test(tst_shapetools LIBS paleo_ui)
add_paleo_test(tst_edittools LIBS paleo_ui)
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
# ---- wave/data-foundation ----
add_paleo_test(tst_catalog LIBS paleo_store)
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
add_paleo_test(tst_seismic_index LIBS paleo_services)
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
add_paleo_test(tst_previewdoc LIBS paleo_services paleo_qgis)
target_compile_definitions(tst_previewdoc PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_wellcomposite LIBS paleo_ui)
add_paleo_test(tst_smoke_realdata LIBS paleo_workflow) # real-data smoke; skips unless PALEO_REAL_PROJECT_AREA is set
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
  # ORT 运行库目录必须出现在 LD_LIBRARY_PATH：用 ENVIRONMENT_MODIFICATION
  # 的 path_list_prepend 前置，而不是 APPEND 一条整体 LD_LIBRARY_PATH 赋值
  # ——后者仍会替换外部注入的值（deb 闭包路 paleo-dev 注入的 vendored
  # QGIS/Qt 目录会被吞掉，间接依赖落到系统 Qt 上产生版本冲突——本机
  # Arch Qt6.11 + vendored Qt6.10 即此症状）。
  set_tests_properties(tst_onnx tst_onnxworkflow tst_onnxsessions tst_tileinference tst_modelregistry
    tst_aiassistworkflow tst_remotepredictrouter tst_aiassist_perf PROPERTIES ENVIRONMENT_MODIFICATION
    "LD_LIBRARY_PATH=path_list_prepend:${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib")
endif()

# ---- wave/mapping-pipeline (阶段C+E 编图链路) ----
add_paleo_test(tst_projectdata LIBS paleo_services paleo_qgis)
add_paleo_test(tst_mapping LIBS paleo_workflow)

# ---- wave2 async-ux (D1/D2 异步任务框架 + 任务页进度条) ----
add_paleo_test(tst_taskservice LIBS paleo_ui)
add_paleo_test(tst_threeway)          # 伞式（全栈，兼容样本）
add_paleo_test(tst_chips LIBS paleo_ui)
add_paleo_test(tst_versions LIBS paleo_workflow)

# ---- wave3/derived-publish (T26/D10/D15 + §40 速度模型与 CDP 映射) ----
add_paleo_test(tst_derivedassets LIBS paleo_workflow)
add_paleo_test(tst_seismicmapping LIBS paleo_ui)
target_compile_definitions(tst_seismicmapping PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")

# ---- application ----
add_executable(paleo src/app/main.cpp)
target_link_libraries(paleo PRIVATE paleo_core)

# ---- goal/perf-systematize 簇1：启动分段仪表 + 比率门 ----
# 真实 paleo 二进制 offscreen 起停（首帧即退），PALEO_STARTUP_TRACE 落盘
# JSON → 段完整性 + 机器无关比率门（startup_ratios.json ×1.5）；另起一次
# 注入 1500ms 劣化断言门必红（判别力）。计时面挂串行，对齐 tst_perfbudget。
add_paleo_test(tst_startup_trace LIBS paleo_services)
target_compile_definitions(tst_startup_trace PRIVATE
  PALEO_APP_BINARY="$<TARGET_FILE:paleo>"
  PALEO_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # 审计 01 M5：基线不靠 CWD 相对路径
set_tests_properties(tst_startup_trace PROPERTIES RUN_SERIAL TRUE)

add_executable(paleo_seismic_demo tools/seismic_showcase.cpp)
target_link_libraries(paleo_seismic_demo PRIVATE paleo_core)

add_executable(tst_boot tests/tst_boot.cpp)
target_link_libraries(tst_boot PRIVATE paleo_qgis_iface Qt6::Test)
target_compile_definitions(tst_boot PRIVATE FIXTURE_GPKG="${FIXTURE_GPKG}")
add_test(NAME boot COMMAND tst_boot)
paleo_test_sandbox(boot)

add_executable(tst_polygonize tests/tst_polygonize.cpp spikes/polygonize/paleopolygonize.cpp)
target_link_libraries(tst_polygonize PRIVATE paleo_qgis_iface Qt6::Test ${GDAL_LIB})
target_include_directories(tst_polygonize PRIVATE ${GDAL_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/spikes/polygonize)
add_test(NAME polygonize COMMAND tst_polygonize)
paleo_test_sandbox(polygonize)

# ---- wave3/model-hardening (数据模型与测试基建加固) ----
add_paleo_test(tst_metastore LIBS paleo_store)      # sqlite user_version：新库采纳/遗留升级/未来拒开
add_paleo_test(tst_layermanifest LIBS paleo_store)  # P1-11 / ARCH-04 LayerManifest 线程亲和性与写队列调度门禁
add_paleo_test(tst_segy_fixture LIBS paleo_io)   # 合成最小 SEG-Y 夹具：索引/单线/角点/范围门
target_compile_definitions(tst_segy_fixture PRIVATE
  TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")
add_paleo_test(tst_algorithm_harness LIBS paleo_algorithms paleo_ui_deps)  # AlgorithmTestBase harness + ≥3 算法用例
add_paleo_test(tst_perfbudget LIBS paleo_services paleo_qgis)
# 墙钟预算门禁在 CI 并行负载下必抖（实测 525ms>500ms，预热救不了 CPU
# 争抢）——计时测试串行跑，对齐 tst_correlation_full 先例。
set_tests_properties(tst_perfbudget PROPERTIES RUN_SERIAL TRUE)     # 非 UI 性能预算：工程打开/folder 枚举/save 200 版本
target_compile_definitions(tst_perfbudget PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")

# ---- data/role-registry (A 包：工程作用域角色词表) ----
add_paleo_test(tst_roles LIBS paleo_store)

# ---- data/commit-coord（D 包：journal + 幂等有序提交） ----
add_paleo_test(tst_commitcoord LIBS paleo_store)    # commitAll 记档生命周期/幂等重入/崩溃恢复/catalog→qgz 顺序

# ---- data/entity-view (B 包：实体视图 + link ordinal + staleness-lite) ----
add_paleo_test(tst_entityview LIBS paleo_store)

# ---- wave4/runtime-resilience ----
# 崩溃本地转储（§38 留白按本地优先落地）+ 外链源重定位恢复路径。
# 新增源与测试集中在此块；并行包合并冲突时保留双方行。
add_paleo_test(tst_crashreport LIBS paleo_services)
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
add_paleo_test(tst_layertreepanel LIBS paleo_ui)    # 筛选/组级显隐/复制图层/objectName 兼容
add_paleo_test(tst_layerproperties LIBS paleo_ui)   # 矢量/栅格/缺源三类 + 业务页字段
add_paleo_test(tst_layerprofilebar LIBS paleo_ui)   # 主题下拉/保存/管理对话框/页指示

# ---- 并行开发挂点 ----
# 各并行方向的新增源文件/测试一律放 cmake/extra-<方向>.cmake，内容为：
#   target_sources(paleo_<模块> PRIVATE src/.../foo.cpp)
#   add_paleo_test(tst_foo)
# 禁止直接改本文件的模块源列表——并行分支在此汇合零冲突。
# 显式清单登记替代隐式 GLOB，根治构建目录漂移与测试漏注（P1-14 / TEST-01）
set(_paleo_extra_manifest
  "${CMAKE_SOURCE_DIR}/cmake/extra-catalog-sqlite.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-crossplot-facies.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-data.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-a.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-b.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-c.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-d.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-faultinterp.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-fault-surface.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-geostat.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-install.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-io-perf.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-mamcl.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-mapping.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-petrophys.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-property-modeling.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-prediction-facies.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-sections-alignment.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-seismic.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-surfaceops.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-time-depth.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-ux.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-well-logset.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-wellcomposite.cmake"
)
foreach(_f IN LISTS _paleo_extra_manifest)
  if(EXISTS "${_f}")
    include("${_f}")
  endif()
endforeach()
# 防漏登记对账：磁盘上的 extra-*.cmake 必须全部在清单内（显式清单防的是
# 构建目录漂移，不是删文件——漏登记的模块会静默丢源文件/测试，曾致
# mincurvature.cpp 未编译 → vtable 链接失败）。
file(GLOB _paleo_extra_ondisk CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/cmake/extra-*.cmake")
foreach(_f IN LISTS _paleo_extra_ondisk)
  if(NOT _f IN_LIST _paleo_extra_manifest)
    message(FATAL_ERROR "cmake/$(notdir ${_f}) 未登记进 _paleo_extra_manifest——"
      "请补进上面的显式清单（防漂移约定不变：include 仍只走清单）")
  endif()
endforeach()

# ---- 审计 01 M6：计时/黄金图类测试打标签（放在所有 extra-*.cmake 之后统一登记）----
# perf   = 墙钟预算/比率门/基准类：本机负载敏感。`ctest -LE perf` 可整体排除，
#          `ctest -L perf` 单独串行重跑。阈值一律不动；统一 RUN_SERIAL 防并行抖动。
# golden = 渲染像素比对类（字体/优化级别敏感）。
# add_paleo_test 已按首模块设了 LABELS——这里 APPEND，不覆盖（CI 的 -L 分步不受影响）。
# tst_panels / tst_correlation_full 只有个别计时断言、主体是功能覆盖：保持既有
# RUN_SERIAL，不打 perf（-LE perf 不应丢掉这些功能测试）。
set(_paleo_perf_tests
  tst_perf tst_previewmap_perf tst_aiassist_perf tst_startup_trace tst_perfbudget
  tst_singlefactor_perf tst_singlefactor_contourbudget tst_singlefactor_guilatency
  tst_ui_blocking tst_mem_budget tst_perf_las tst_perf_segyindex tst_perf_catalog
  tst_perf_regress tst_petrophysperf tst_seismic_perf tst_seismic_baseline
  tst_seismic_budgets tst_seismicattrperf tst_horizontrackperf tst_timedepthperf
  tst_wellcomposite_perf tst_cache_las tst_welllogset_perf)
foreach(_t IN LISTS _paleo_perf_tests)
  if(TEST ${_t})
    set_property(TEST ${_t} APPEND PROPERTY LABELS perf)
    set_tests_properties(${_t} PROPERTIES RUN_SERIAL TRUE)
  endif()
endforeach()
if(TEST tst_singlefactor_perf)
  set_tests_properties(tst_singlefactor_perf PROPERTIES TIMEOUT 1200)
endif()
if(TEST tst_singlefactor_contourbudget)
  set_tests_properties(tst_singlefactor_contourbudget PROPERTIES TIMEOUT 600)
endif()
if(TEST tst_singlefactor_guilatency)
  set_tests_properties(tst_singlefactor_guilatency PROPERTIES TIMEOUT 180)
endif()
foreach(_t IN ITEMS tst_wellcomposite_visual)
  if(TEST ${_t})
    set_property(TEST ${_t} APPEND PROPERTY LABELS golden)
  endif()
endforeach()
