# cmake/paleo-dependencies.cmake — 外部依赖探测：QGIS/Qt/GDAL/GEOS/QScintilla、4 层依赖伞、vendored 库（SARibbon/segyio/glm/sbm/ORT）
# 由根 CMakeLists.txt 按顺序 include()（纯结构拆分，语义与拆分前逐行一致）。
# 注意：include() 不开新作用域，CMAKE_SOURCE_DIR 仍是仓库根。
# 文件名刻意不用 extra- 前缀——extra-*.cmake 受 _paleo_extra_manifest 防漏对账。

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
# QGIS 运行时 prefix（QgsApplication::setPrefixPath 用，决定 srs.db/provider 位置）。
# OSGeo4W 的 QGIS_PREFIX 是 osgeo4w 根，QGIS 真正装在 <根>/apps/qgis——直接拿根
# 当运行时 prefix，ctest 下 srs.db 找不到（windows tst_runtime/boot 红的根因；
# paleo-dev.ps1 直跑时设的 QGIS_PREFIX_PATH 本来就是 apps\qgis）。
set(PALEO_QGIS_RUNTIME_PREFIX "${QGIS_PREFIX}")
if(QGIS_PREFIX AND EXISTS "${QGIS_PREFIX}/apps/qgis/resources")
  set(PALEO_QGIS_RUNTIME_PREFIX "${QGIS_PREFIX}/apps/qgis")
endif()

# deb 闭包链接期 NEEDED 解析：Debian 库相互 NEEDED 依赖靠系统默认路径 +
# update-alternatives 顶层链接，解包式 vendored 前缀两者皆无——ld 传递依赖
# 只认 -rpath-link 目录（实测 -L/LIBRARY_PATH 均不参与 NEEDED 解析）。
# 把前缀下所有含 .so 的目录统一注入（含 blas/lapack/pulseaudio 这类
# alternatives 子目录），armadillo→blas、libpulse→pulsecommon 等链才接得上。
if(QGIS_PREFIX AND UNIX AND NOT APPLE AND EXISTS "${QGIS_PREFIX}/lib")
  file(GLOB_RECURSE _pal_sos "${QGIS_PREFIX}/lib/*.so*")
  set(_pal_linkdirs "")
  foreach(_so IN LISTS _pal_sos)
    get_filename_component(_d "${_so}" DIRECTORY)
    list(APPEND _pal_linkdirs "${_d}")
  endforeach()
  list(REMOVE_DUPLICATES _pal_linkdirs)
  foreach(_d IN LISTS _pal_linkdirs)
    add_link_options("-Wl,-rpath-link,${_d}")
  endforeach()
  list(LENGTH _pal_linkdirs _pal_n)
  message(STATUS "deb 闭包 rpath-link 目录数: ${_pal_n}")
  unset(_pal_sos)
  unset(_pal_linkdirs)
  unset(_pal_n)
endif()
# QScintilla：QGIS 的代码编辑器头（qgscodeeditor.h）传递包含它。它有自己的
# 独立前缀（与 QGIS 分开安装），故也走 cache/env 解析；未显式给出时从既有的
# CMAKE_PREFIX_PATH 逐个前缀自动发现——与上面 QGIS 的 find_* 同一套 HINTS 风格，
# 不要求调用方额外记着传一个前缀。
set(QSCINTILLA_PREFIX "" CACHE PATH "QScintilla install prefix（QGIS 代码编辑器头依赖；空=从 CMAKE_PREFIX_PATH 自动发现）")
if(NOT QSCINTILLA_PREFIX AND DEFINED ENV{QSCINTILLA_PREFIX_PATH})
  set(QSCINTILLA_PREFIX "$ENV{QSCINTILLA_PREFIX_PATH}" CACHE PATH "QScintilla install prefix" FORCE)
endif()
if(QSCINTILLA_PREFIX)
  list(PREPEND CMAKE_PREFIX_PATH "${QSCINTILLA_PREFIX}")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test Sql Xml Concurrent Svg PrintSupport Network SerialPort Positioning UiTools OpenGLWidgets Pdf PdfWidgets Core5Compat)
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
find_path(GEOS_INCLUDE_DIR geos_c.h HINTS
  "${QGIS_PREFIX}/include" "${QGIS_PREFIX}/usr/include" PATHS /usr/include REQUIRED)
find_library(GEOS_C_LIB geos_c HINTS
  "${QGIS_PREFIX}/lib" "${QGIS_PREFIX}/usr/lib" PATHS /usr/lib REQUIRED)
# QGIS 的 qgscodeeditor.h 传递包含 <Qsci/qsciapis.h>。该包含是「QGIS 头 →
# QScintilla 头」的形式，不在任何 *_INCLUDE_DIR 探测的范围内，故 QGIS 的
# include 目录里并没有 Qsci/——头只存在于 QScintilla 自己的前缀里。缺这行时，
# 任何（哪怕完全不用代码编辑器的）间接包含 qgscodeeditor.h 的 TU 都会报
# C1083: 无法打开包括文件 "Qsci/qsciapis.h"。
# 注意只需**头**，不需链库：QgsCodeEditor 继承 QsciScintilla，而 INDIC_MAX 是
# 编译期常量；只有真正调用其成员函数的 TU 才需要 qscintilla2_qt6.lib。
set(_qsci_hints "")
foreach(_pfx IN LISTS CMAKE_PREFIX_PATH)
  list(APPEND _qsci_hints "${_pfx}/include" "${_pfx}")
endforeach()
find_path(QSCINTILLA_INCLUDE_DIR Qsci/qsciapis.h HINTS
  ${_qsci_hints} "${QGIS_PREFIX}/include"
  "${QGIS_PREFIX}/apps/Qt6/include" "${QGIS_PREFIX}/apps/qt6/include"
  PATHS /usr/include /usr/local/include)
unset(_qsci_hints)
if(QSCINTILLA_INCLUDE_DIR)
  message(STATUS "QScintilla headers: ${QSCINTILLA_INCLUDE_DIR}")
else()
  message(WARNING "QScintilla headers not found — QGIS code editor headers "
                  "(qgscodeeditor.h) will fail to compile if included transitively")
endif()

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
# QScintilla 的头得跟着 QGIS 的头一起传播，见上面 find_path 处的说明。
if(QSCINTILLA_INCLUDE_DIR)
  target_include_directories(paleo_qgis_core_deps INTERFACE "${QSCINTILLA_INCLUDE_DIR}")
endif()
if(QGIS_PREFIX)
  # OSGeo4W installs SQLite, GDAL, GEOS and other public QGIS header
  # dependencies in the prefix's shared include directory.
  target_include_directories(paleo_qgis_core_deps INTERFACE "${QGIS_PREFIX}/include")
  # 非 OSGeo4W 布局（本机 vcpkg 依赖根与 QGIS prefix 分离）下这些头在
  # GDAL/GEOS 的 include 根里：qgsstyle.h 直接 #include <sqlite3.h>，任何
  # 只链 paleo_qgis_iface 的目标（tst_boot / tst_polygonize / selfcheck）
  # 都要跟着拿到，否则编译期报 C1083。
  if(GDAL_INCLUDE_DIR)
    target_include_directories(paleo_qgis_core_deps INTERFACE "${GDAL_INCLUDE_DIR}")
  endif()
  if(GEOS_INCLUDE_DIR)
    target_include_directories(paleo_qgis_core_deps INTERFACE "${GEOS_INCLUDE_DIR}")
  endif()
  # 默认运行时 prefix 随构建钉住：裸跑（无 QGIS_PREFIX_PATH env）时
  # provider/srs.db 仍解析到二进制实际链接的 vendor prefix，不静默回落
  # 系统栈（链接与运行资源栈分裂）。env/CLI 覆盖语义不变。
  target_compile_definitions(paleo_qgis_core_deps INTERFACE
    PALEO_QGIS_PREFIX_DEFAULT="${PALEO_QGIS_RUNTIME_PREFIX}")
endif()
# QGIS 公有头直接 include <sqlite3.h>（qgsstyle.h:19）与 GDAL/GEOS 头，但
# 「OSGeo4W 把它们放在 prefix include」这一假设在本机不成立——依赖闭包是
# vcpkg 装的，根在 paleo-qgis-deps/Library/include。只链 paleo_qgis_iface 的
# 目标（tst_boot / tst_polygonize / paleo_selfcheck）因此拿不到该头。
# 传播已找到的依赖根，与 paleo_deps 的口径对齐。
if(GDAL_INCLUDE_DIR)
  target_include_directories(paleo_qgis_core_deps INTERFACE "${GDAL_INCLUDE_DIR}")
endif()
if(GEOS_INCLUDE_DIR)
  target_include_directories(paleo_qgis_core_deps INTERFACE "${GEOS_INCLUDE_DIR}")
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

# demo 工程无头构建工具（组装根）：xlsx 转规范井文本 + 生产导入路径 +
# georeference 落 project.paleo + 重开自校验。用法见 src/app/mkproject.cpp 头注。
# 方向 78：--manifest 清单外置（夹具工厂化）；无 manifest 时行为零变化。
add_executable(paleo_mkproject src/app/mkproject.cpp)
target_link_libraries(paleo_mkproject PRIVATE paleo_services paleo_qgis)

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
  # 方向 92：domaintools 角色清单单源消费 catalogRoles()（catalogroles.cpp
  # 在 paleo_store）——ai(功能) → store(数据) 属允许的向下依赖（层 DAG 同层
  # 邻接，无环：store 不含 ai）。
  target_link_libraries(paleo_ai PUBLIC paleo_deps paleo_store ort_vendored)
  target_compile_definitions(paleo_ai PUBLIC PALEO_HAVE_ORT=1)
  # 审计 01 M5：vendorRuntimeDir() 从可执行文件往上找 vendor/onnxruntime，构建目录在
  # 源码树外（-B ../b-asan）时找不到——编译期注入 ORT 根作为向上查找后的回落。
  get_filename_component(_paleo_ort_root "${ORT_INCLUDE}" DIRECTORY)
  target_compile_definitions(paleo_ai PRIVATE PALEO_ORT_ROOT="${_paleo_ort_root}")
  target_include_directories(paleo_ai PUBLIC ${CMAKE_SOURCE_DIR}/src)
  # 刻意保持目录级变量（非逐 target BUILD_RPATH）：它对此后定义的全部 target 生效、
  # 对此前的 paleo_mkproject 等不生效——改成逐 target 会改变 RPATH 集合，非纯重构。
  set(CMAKE_BUILD_RPATH "${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib")
else()
  set(PALEO_HAVE_ORT FALSE)
  add_library(paleo_ai STATIC src/ai/remotepredictionservice.cpp)
  target_link_libraries(paleo_ai PUBLIC Qt6::Core Qt6::Gui paleo_store) # 无 ORT 时为空壳，消费方链接名不变
  message(STATUS "onnxruntime not vendored — local ONNX inference disabled; remote services remain available (run ./paleo-dev bootstrap for ONNX)")
endif()
