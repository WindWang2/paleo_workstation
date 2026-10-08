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
else()
  # #134：MSVC 把花括号窄化（int→double 等）当硬错误 C2397，GCC/Clang 默认
  # 只告警——Linux 绿、Windows 红。这里升为错误，让 Linux CI 提前暴露。
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror=narrowing>)
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
  # 方向70：budget 类断言感知 sanitizer 档（墙钟预算 ×3、RSS 增长断言跳过，
  # 见 tests/perfbudget_relax.h 与 devex.md build-asan 实测口径）。非
  # sanitizer 构建不定义，测试行为零变化。
  add_compile_definitions(PALEO_SANITIZER_BUILD=1)
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
  message(STATUS "onnxruntime not vendored — local ONNX inference disabled; remote services remain available (run ./paleo-dev bootstrap for ONNX)")
endif()

# ---- 模块化分层库（按 UI_LAYER_PLAN 层契约拆分）----
# DAG（按实测 include 边排序，单向无环）：
#   domain · algorithms · ai · store(catalog+metadata) ── qgis ── io ── services
#     ── linkage · workflow ── ui ── app
# catalog↔metadata 互有 include（datacatalog→atomicfile / mapversionstore→datacatalog），
# 同层环合并为 paleo_store。paleo_core 降为 INTERFACE 伞，旧链接方零改动。
add_library(paleo_deps INTERFACE)
target_link_libraries(paleo_deps INTERFACE paleo_qgis_core_deps ${GDAL_LIB} ${GEOS_C_LIB})
target_include_directories(paleo_deps INTERFACE ${CMAKE_SOURCE_DIR}/src ${GDAL_INCLUDE_DIR} ${GEOS_INCLUDE_DIR})

# ---- 数据层 ----
add_library(paleo_domain STATIC
  src/domain/singlefactorrequest.cpp
  src/domain/types.cpp
  src/domain/wellcompositemodel.cpp
  src/domain/wellrecords.cpp                        # 井文本记录类型 + XML 嗅探（自 io 下沉）
  src/domain/welltopsedit.cpp                       # 方向 32：分层校验/差异/合并/批量纯函数
  src/domain/projectclassifier.cpp                  # §3 path/extension classifier + xml sniff
  src/domain/arearules.cpp                          # 工程级参数 seam（docs/AREA_PARAMETERS.md）
  src/domain/wellsitingplan.cpp                     # 方向34：布井方案纯值类型（指标快照往返）
  src/domain/seismic/nicestep.cpp
  src/domain/seismic/timedepthmodel.cpp
  src/domain/sequenceframework.cpp                  # 方向28：层序格架模型（单元/标志层 + JSON）
  src/domain/frameworkdiagnostics.cpp               # 方向28：格架一致性诊断（悬空/空洞/重名/无归属）
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
  src/algorithms/singlefactor/corridor.cpp
  src/algorithms/singlefactor/partition.cpp
  src/algorithms/singlefactor/faultpath.cpp
  src/algorithms/singlefactor/buffertransition.cpp
  src/algorithms/singlefactor/regionalcontours.cpp
  src/algorithms/singlefactor/contouravoidance.cpp
  src/algorithms/singlefactor/wellacquisition.cpp    # 上游 _extract_current_wells 移植
  src/algorithms/singlefactor/structural.cpp         # 上游 structural_idw 面核（GEOS C API）
  src/algorithms/singlefactor/constraintparse.cpp    # 约束线 params_json 共享解析
  src/algorithms/singlefactor/structuralalgorithm.cpp
  src/algorithms/singlefactor/cartographicsmooth.cpp # 上游制图平滑折线辅助（纯数学）
  src/algorithms/singlefactor/fieldcontours.cpp      # 上游 field_contours/contour_work_field 移植
  src/algorithms/frameworksuggester.cpp              # 方向28：井间格架建议核（只出候选，不写库）
  src/algorithms/singlefactor/contourlevels.cpp      # 上游等值线级别助手（_nice_number 等）
  src/algorithms/wellsiting/wellsiting.cpp           # 方向34：井网覆盖诊断/候选/评估纯核
)
target_link_libraries(paleo_algorithms PUBLIC paleo_deps paleo_domain)

# catalog + metadata 同库（互依环）
add_library(paleo_store STATIC
  src/catalog/purgelease.cpp
  src/catalog/datacatalog.cpp                       # entity/link/asset/version + catalog.json
  src/catalog/roleregistry.cpp                      # RoleDef/RoleRegistry 词表 + project_area.json 覆盖
  src/catalog/entityview.cpp                        # RoleSlot/EntityView 纯查询门面
  src/metadata/paleoprojectstore.cpp
  src/metadata/paleoprojectfile.cpp
  src/metadata/layermanifest.cpp
  src/metadata/releasestore.cpp
  src/metadata/mapversionstore.cpp                  # 版本/发布状态机落表（meta sqlite）
  src/metadata/layouttemplatestore.cpp              # 版式模板库语义索引（方向 25；catalog 存内容）
  src/metadata/metastore.cpp                        # project.sqlite user_version 门
  src/catalog/frameworkstore.cpp                    # 方向28：格架落库（catalog 资产 + 受管版本）
  src/metadata/wellsitingstore.cpp                  # 方向34：布井方案集单行文档表
  src/metadata/projectlock.cpp                      # 两实例同开一工程写互斥（QLockFile）
)
target_link_libraries(paleo_store PUBLIC paleo_deps paleo_domain)

# ---- QGIS 封装层 ----
add_library(paleo_qgis STATIC
  src/qgis/mappingartifactwriter.cpp
  src/qgis/qgisruntime.cpp
  src/qgis/qgisprojectservice.cpp
  src/qgis/qgisprojectservice_async.cpp
  src/qgis/qgisprojectservice_map.cpp
  src/qgis/projectmapreference.cpp
  src/qgis/qgislayerservice.cpp
  src/qgis/qgiscanvascontroller.cpp
  src/qgis/qgiseditingservice.cpp
  src/qgis/qgisconstrainteditsession.cpp
  src/qgis/qgisprocessingservice.cpp
  src/qgis/qgisstyleservice.cpp
  src/qgis/geopatterns.cpp                           # 方向31：地质符号语义注册表（花纹/线型词表 + 符号构造）
  src/qgis/semanticlegend.cpp                        # 方向31：语义图例自动生成（渲染器类目→swatch+词面，增删/renderer 联动）
  src/qgis/qgislayoutservice.cpp
  src/qgis/layoutexport.cpp                         # 层位图/设计器导出核心（QgsLayoutExporter 封装）
  src/qgis/standardelements.cpp                     # 标准图件元素工厂 + 三类内置内容模板骨架（方向 25）
  src/qgis/mapbooklayout.cpp                        # 地图册：逐格/蒙太奇/索引页版面构建 + 落盘回读校验
  src/qgis/manifestprojection.cpp
  src/qgis/factorstylewriter.cpp                    # m2(B)：单因素色带 .qml 落盘
  src/qgis/factorcontour.cpp                        # m2(B)：等值线 GDAL C API
  src/qgis/qgislayerprofile.cpp                     # 页面档案：QgsMapThemeCollection 封装 + 组档案表
  src/qgis/qgislabelzorder.cpp                      # 标注随图层 z 序：给每层钉 rendering/labelsWithLayer（vendored QGIS 补丁）
  src/qgis/qgislayerorganizer.cpp                   # 图层树布局器：00_Data 置顶共享 + 层位组 + 组秩归位
  src/qgis/previewmapcanvas.cpp                     # P2 D1.1 预览画布封装（私有层容器+CRS+历史栈+渐进渲染）
  src/qgis/previewmaptools.cpp                      # P2 D1.2 预览工具注册表（量测/identify/剖面线）
  src/qgis/previewrasteranalysis.cpp                # P2 D2.2/D5.x 预览栅格分析（统计/拉伸/直方图/剖面/极值）
  src/qgis/previewidentify.cpp                      # P2 D7 identify 查询核心（空间索引缓存+栅格插值）
  src/qgis/previewrendercache.cpp                   # P2 D6.2/D6.6 渲染结果缓存（内存 LRU + QTemporaryDir PNG）
  src/qgis/seismicsectiontool.cpp
  src/qgis/wellattributestore.cpp
)
target_link_libraries(paleo_qgis PUBLIC paleo_deps paleo_ui_deps paleo_store paleo_algorithms paleo_io)

add_library(paleo_io STATIC
  src/io/dataimportservice.cpp                      # 方向57：生命周期 + 入口 wrapper
  src/io/dataimport_session.cpp                     # 方向57：会话/登记基座（plan/RAW/commit 面）
  src/io/dataimport_file.cpp                        # 方向57：单文件导入编排（importOneFile 壳+分派）
  src/io/dataimport_wells.cpp                       # 方向57：井族分支（well_head/log/tops/deviation）
  src/io/dataimport_geodata.cpp                     # 方向57：地物族分支（horizon/seismic/辅助参考）
  src/io/dataimport_folder.cpp                      # 方向57：文件夹导入/预览/plan 执行面
  src/io/dataimport_pyramids.cpp                    # 方向57：栅格金字塔/概览族
  src/io/dataimport_document.cpp                    # 方向57：文档 PDF 转换 + 外链重定位
  src/io/lasparser.cpp
  src/io/dlisparser.cpp                            # 方向44：DLIS (RP66 v1) 读口
  src/io/lisparser.cpp                             # 方向44：LIS79 读口（含 TIF）
  src/io/welllogread.cpp                           # 方向44：井曲线格式分派门面
  src/io/segyreader.cpp                             # wave/segy
  src/io/constraintstore.cpp                        # wave/constraint-gpkg
  src/io/wellfileparsers.cpp                        # well head / tops / TD text parsers
  src/io/horizonbinner.cpp                          # horizon scatter -> binned time raster
  src/io/sattrio.cpp                                # goal/attr-volume：SATR/SATV 属性容器读写
  src/io/attrgridout.cpp                            # goal/attr-volume：时间切片属性网格 → 地理参考 GeoTIFF
  src/io/timedeptool.cpp                            # TVD/MD -> TIME(ms) interpolation
  src/io/geojsonaffine.cpp                          # D11 临时配准（手工仿射）
  src/io/wellcompositexml.cpp
  src/io/ingestplan.cpp                             # C 包：纯 plan + 幂等执行器
)
target_link_libraries(paleo_io PUBLIC paleo_deps paleo_domain paleo_store)

add_library(paleo_services STATIC
  src/services/toolavailability.cpp
  src/services/previewdoc.cpp                       # W1 数据页唯一数据门面
  src/services/previewdoc_survey.cpp                # 完整测区范围后台恢复
  src/services/singlefactordef.cpp                  # m2：单因素定义注册表
  src/services/algoparamschema.cpp                  # m2：算法参数 schema 注册表
  src/services/projectdata.cpp                      # 读侧门面（catalog.json + LayerManifest）
  src/services/paleotaskservice.cpp                 # D1/D2 异步任务框架
  src/services/jobrunner.cpp                        # 方向20：统一三段式 JobRunner（作业框架）
  src/services/seismicmapping.cpp                   # 测网几何↔inline/xline/CDP
  src/services/seismichorizoncluster.cpp             # 层位窗反射特征聚类（Mock 相解释）
  src/services/seismictaskservice.cpp               # sdk::Dataset 门面 + 任务编排 (Base Core)
  src/services/seismictaskservice_transcode.cpp     # 地震任务：索引与转码
  src/services/seismictaskservice_slice.cpp         # 地震任务：切片与剖面
  src/services/seismictaskservice_attr.cpp          # 地震任务：属性计算与体生成
  src/services/seismictaskservice_interp.cpp        # 地震任务：层位追踪与解释
  src/services/seismictaskservice_viz.cpp           # 地震任务：可视化与预览
  src/services/crashreport.cpp                      # 致命信号 fd 落盘 + .running 旗标
  src/services/frameworkservice.cpp                 # 方向28：格架读侧门面 + 建议/诊断编排
  src/services/startuptrace.cpp                     # goal/perf-systematize 簇1：启动分段仪表
  src/services/storagegovernance.cpp
  src/services/cataloghealth.cpp                    # 方向30：工区资产体检（快速面 + 外链 SHA 复验）
  src/services/errorhub.cpp                         # 方向54+方向64：统一错误通道（去重/聚合/环形历史，两实现并存）
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
  src/workflow/stratigraphicwebsession.cpp
  src/workflow/mappingworkbench.cpp
  src/workflow/workflows.cpp
  src/workflow/constraintimport.cpp                 # WS-C：约束 SHP 角色化导入（方向/打断）
  src/workflow/validationworkflow.cpp                # 方向20 轮4：④验证（按类边界析出）
  src/workflow/compositionworkflow.cpp               # 方向20 轮4：③综合编图（按类边界析出）
  src/workflow/predictionworkflow.cpp              # 方向20 轮4：①智能预测（按类边界析出）
  src/workflow/constraintworkflow.cpp              # 方向20 轮4：②约束与单因素（按类边界析出）
  src/workflow/constraintwellfactors.cpp
  src/workflow/constraintfactorjobs.cpp            # 方向20 轮4：②的单因素作业面；方向57 只留统一异步面
  src/workflow/constraintfactorjobs_factor.cpp     # 方向57：基础因子生成族（idw/isopach/welldist/structural/surfer）
  src/workflow/constraintfactorjobs_localdirection.cpp # 方向57：局部方向族三段式
  src/workflow/constraintfactorjobs_geostat.cpp    # 方向57：克里金/SGS 族三段式
  src/workflow/constraintfactorjobs_contours.cpp   # 方向57：分析等值线族 + 制图工作场
  src/workflow/constraintfactorjobs_interpretive.cpp # 方向57：解释性等值线族三段式
  src/workflow/folderimport.cpp                    # W2 文件夹/单文件导入编排
  src/workflow/projectopen.cpp                     # W2 打开/新建工程编排
  src/workflow/registration.cpp                    # W3 临时配准编排
  src/workflow/mappingworkflow.cpp                 # 厚度→约束IDW→相多边形链路
  src/workflow/mapexport.cpp                       # 层位图 PDF 导出
  src/workflow/layouttemplatelibrary.cpp            # 版式模板库编排（qpt→catalog 版本 + 库表）
  src/workflow/horizonbatchexport.cpp               # 按层位组批量出图（快照换图+登记 OUTPUT）
  src/workflow/mapbook.cpp                         # 地图册：AOI 格序列 + 模板变量（纯编排）
  src/workflow/mapbookqueue.cpp                    # 地图册：批量导出队列（任务服务 + 逐版记账）
  src/workflow/mapversioncontroller.cpp            # 保存版本/发布编排
  src/workflow/aiassistworkflow.cpp
  src/workflow/derivedassets.cpp                   # 派生产物 + DERIVED 版本登记
  src/workflow/welltopseditorworkflow.cpp           # 方向 32：分层编辑版本化提交/回滚/合并编排
  src/workflow/wellsitingworkflow.cpp               # 方向34：井网辅助编排（诊断/候选/方案/planned）
  src/workflow/storagegovernancecontroller.cpp
  src/workflow/assetops.cpp                        # 方向30：版本回滚/对比 + 物理清理 + 未决归位
  src/workflow/importledger.cpp                    # 方向30：文件夹导入批次台账（sidecar 持久化）
  src/workflow/boundaryeditrules.cpp               # 方向39：相界类型编辑规则（赋类/带域/开放端）
)
target_link_libraries(paleo_workflow PUBLIC paleo_deps paleo_qgis paleo_store paleo_io paleo_services paleo_domain paleo_ai)

# ---- 视图层 ----
add_library(paleo_ui STATIC
  src/ui/notifications/notificationcenter.cpp      # 方向64：错误呈现（通知卡/severe 模态/状态栏）
  src/ui/notifications/toastcard.cpp               # 方向64：通知卡（预建池复用）
  src/ui/notifications/paleonotify.cpp             # 方向64：视图层错误/确认统一出口
  src/ui/notifications/errorhistorypanel.cpp       # 方向64：错误历史面板（过滤/复制/清空）
  src/ui/pages/stratigraphicwebpage.cpp
  src/ui/symbols/symbolpickerpanel.cpp             # 方向31：符号选择器面板（花纹/线型浏览 + 语义过滤）
  src/ui/pages/mappingworkbenchpage.cpp
  src/ui/paleomainwindow_workbench.cpp
  src/ui/paleomainwindow.cpp
  src/ui/paleomainwindow_attach.cpp                # 壳接线聚合胶水层
  src/ui/paleomainwindow_attach_data.cpp           # 数据管理页接线
  src/ui/paleomainwindow_attach_predict.cpp        # 智能预测页接线
  src/ui/paleomainwindow_attach_constraint.cpp     # 约束与单因素页接线
  src/ui/paleomainwindow_attach_compose.cpp        # 综合编图页接线
  src/ui/paleomainwindow_attach_validate.cpp       # 验证与布井辅助接线
  src/ui/paleomainwindow_attach_shell.cpp          # 壳面与全局组件接线
  src/ui/paleomainwindow_attach_mapping.cpp        # 编图流水线与版本接线
  src/ui/paleomainwindow_attach_models.cpp         # 属性建模/相图/时深转换接线
  src/ui/ribbonpanels.cpp                          # W4 ribbon 命令组构建迁出
  src/ui/pages/datalist.cpp                        # W5 数据页·列表侧（胶水层）
  src/ui/pages/datalistops.h                       # 数据页·辅助类型与持久化工具
  src/ui/pages/datalist_table.cpp                  # 数据页·表格展现与行内动作
  src/ui/pages/datalist_tree.cpp                   # 数据页·树形层级与分组
  src/ui/pages/datalist_select.cpp                 # 数据页·选中集管理与联动
  src/ui/pages/datalist_filter.cpp                 # 数据页·多维组合过滤与搜索
  src/ui/pages/datalist_batch.cpp                  # 数据页·批量操作与上下文菜单
  src/ui/pages/datalist_dnd.cpp                    # 数据页·拖拽与外部文件处理
  src/ui/pages/datalist_undo.cpp                   # 数据页·撤销重做与治理对话框
  src/ui/pages/entitypanel.cpp                     # W5 数据页·实体视图侧
  src/ui/pages/wellsitingpanel.cpp                 # 方向34：布井辅助面板（验证页页签）
  src/ui/pages/datapage.cpp                        # W5 DataPage 兼容薄壳
  src/ui/pages/predictpage.cpp                     # W5b ①智能预测
  src/ui/pages/constraintpage.cpp                  # W5b ②约束与单因素
  src/ui/pages/composepage.cpp                     # W5b ③综合编图
  src/ui/pages/validatepage.cpp                    # W5b ④验证
  src/ui/typedconstraintdrawcontroller.cpp         # m2(B)：物源线/展布线/控制点捕获
  src/ui/locator/paleolocatorfilters.cpp
  src/ui/layoutdesignershell.cpp
  src/ui/maptools/paleomaptools.cpp
  src/ui/maptools/sitingpicktool.cpp               # 方向34：地图布点拾取工具
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
  src/ui/layout/layoutitemtree.cpp                 # 方向25：元素树（原生 QgsLayoutModel 视图）
  src/ui/layout/mapbookcontroller.cpp               # #148：地图册面板 ↔ 批量导出队列接线
  src/ui/layout/mapbookpanel.cpp                    # goal/mapbook-reporting：批量参数面板（只发信号）
  src/ui/datapreview/datapreviewtabs.cpp           # §4 data-page preview tabs
  src/ui/datapreview/datapreviewtabseismic.cpp     # 方向20 轮4：seismic 预览分支析出
  src/ui/datapreview/datapreviewtabhorizon.cpp      # 方向20 轮4：horizon 预览分支析出
  src/ui/datapreview/datapreviewtabwelllog.cpp      # 方向20 轮4：well_log 预览分支析出
  src/ui/datapreview/datapreviewtabimage.cpp         # 方向20 轮4：image_reference 分支析出
  src/ui/datapreview/datapreviewtabgeojson.cpp        # 方向20 轮4：geojson 分支析出
  src/ui/datapreview/datapreviewtabmapproduct.cpp     # 成果图件（相栅格/相面矢量/单因素）分支析出
  src/ui/datapreview/previewmappage.cpp            # P2 D1.x/D3.x 预览地图组合页（工具条/状态条/侧栏/鹰眼/书签）
  src/ui/datapreview/previewtocpanel.cpp           # P2 D4.x 迷你 TOC + 符号快调 + 图例
  src/ui/datapreview/previewidentifypanel.cpp      # P2 D7.x identify 结果面板 + 全表模式
  src/ui/datapreview/previewprofilepanel.cpp       # P2 D5.1-5.4 层位剖面图面板
  src/ui/datapreview/previewhistogramwidget.cpp    # P2 D2.3/D5.8 直方图小图
  src/ui/datapreview/previewmapstates.cpp          # P2 D1.7/D2.x 状态页 + 会话记忆
  src/ui/dialogs/folderconfirm.cpp                 # W2 文件夹导入确认表
  src/ui/dialogs/projectmapsettingsdialog.cpp
  src/ui/welltops/welltopseditordialog.cpp          # 方向 32：分层表编辑器
  src/ui/welltops/welltopsmergedialog.cpp           # 方向 32：导入融合逐行取舍
  src/ui/dialogs/storagegovernancedialog.cpp
  src/ui/dialogs/cataloghealthdialog.cpp           # 方向30：工区体检对话框（分类计数 + 跳转意图）
  src/ui/webviewpanel.cpp                          # embedded web shell (lazy, degradable)
  src/ui/wellcomposite/wellcompositetrack.cpp      # ResFormStar 多道综合柱状图（D1.11 道头基类）
  src/ui/wellcomposite/trackpatterns.cpp           # 方向 66：道图案工厂（岩性/相纹理）
  src/ui/wellcomposite/trackcolumns.cpp            # 方向 66：基础六道（标尺/文本/地层/岩性/取芯/图片）
  src/ui/wellcomposite/trackcurves.cpp             # 方向 66：曲线充填与符号道
  src/ui/wellcomposite/trackcompound.cpp           # 方向 66：地层系统组/沉积相复合道
  src/ui/wellcomposite/wellcompositecanvas.cpp     # 画布主控件（视口/缩放/深度协调）
  src/ui/wellcomposite/compositeheader.cpp         # 方向 66：置顶道头（拖拽换位/分隔线/焦点环）
  src/ui/wellcomposite/compositebody.cpp           # 方向 66：道体渲染与手势
  src/ui/wellcomposite/wellcompositepanel.cpp      # 综合图总装面板（编排根）
  src/ui/wellcomposite/panelsetup.cpp              # 方向 66：面板 UI 搭建
  src/ui/wellcomposite/paneldata.cpp               # 方向 66：数据装配（XML/LAS→道布局）
  src/ui/wellcomposite/paneltracks.cpp             # 方向 66：道编排与会话同步
  src/ui/wellcomposite/paneldepth.cpp              # 方向 66：深度交互（书签/pin/TWT/读数）
  src/ui/wellcomposite/paneledit.cpp               # 方向 66：编辑会话与导出
  src/ui/wellcomposite/curveconfigdialog.cpp
  src/ui/wellcomposite/wellpositionlegendwidget.cpp
  src/ui/sequenceframeworkcolumn.cpp               # 方向28：格架柱状视图（层级色带）
  src/ui/sequenceframeworkpanel.cpp                # 方向28：格架树/标志层/诊断/建议面板
  src/ui/horizonchipbar.cpp                        # 8层位chip条（阶段E）
  src/ui/releasepanel.cpp
  src/ui/taskpanel.cpp
  src/ui/attributetablepanel.cpp
  src/ui/constraintdrawcontroller.cpp
  src/ui/layers/layertreepanel.cpp                 # 图层树面板
  src/ui/layers/layerpropertiesdialog.cpp          # 原生属性壳 + 业务页
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
  src/ui/notifications/notificationcard.cpp        # 方向54：ErrorHub 浮动通知卡片
  src/ui/notifications/notificationmanager.cpp     # 方向54：ErrorHub 呈现与模态策略管理器
  src/ui/notifications/errorhistorymodel.cpp       # 方向54：ErrorHub 错误历史模型
  src/ui/notifications/errorhistorydock.cpp        # 方向54：ErrorHub 错误历史面板
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
# 实测结论（方向70，2026-10-07，MSVC/Ninja 10 目标全编过）：早期同名匿名
# helper（setError×35 / connectionNameFor×8 / ensureOpen×7 / kNan / cluster
# stopped·report·failure / geosArea / ensureGdalRegistered / addIssue /
# sha256Hex / sfpkg 常量 / GridSpec·readGrid / seismic3d Normalize 族 /
# windows 宏撞名 slots·small 等）已按「同构→共享 internal 头（*_internal.h
# ×15）/语义不同→逐文件改名」收拢，UNITY_BUILD=ON 在 BATCH_SIZE 8 下全目标
# 编译+链接通过。已知余量（mega-TU 超集分析，当前分批未触发、源序重排可
# 复暴露，见 devex.md「Unity 实测」）：cluster valid/distance、PALEO_NODATA
# 宏、datapreview qssHeight 重载歧义、previewhistogramwidget QFont helper。
# 默认仍 OFF：未做全量 ctest 长跑对照，收益（链接期 TU 数）待量化后再定。
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
  set(_env "QT_QPA_PLATFORM=offscreen;PALEO_WELL_FACIES_NO_KEYCHAIN=1;XDG_CONFIG_HOME=${_home}/config;XDG_DATA_HOME=${_home}/data;XDG_CACHE_HOME=${_home}/cache;HOME=${_home};TMPDIR=${_home}/tmp")
  if(NOT WIN32)
    # Windows 不把 TEMP/TMP 改到构建目录：<build>/ctest-home/<test>/tmp 比系统
    # Temp 深 ~40 字符，导入类测试的托管路径（raw/<uuid>/<uuid>/文件）逼近
    # MAX_PATH=260（tst_import 只在 ctest 下红、直跑绿）；QTemporaryDir 本身
    # 已按用例隔离，Windows 上 XDG 沙箱也不生效（QSettings 走注册表）。
    string(APPEND _env ";TEMP=${_home}/tmp;TMP=${_home}/tmp")
  endif()
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "${_env}")
  if(QGIS_PREFIX)
    # vendor 构建下测试资源/provider 也走构建所链的 prefix（与二进制
    # RUNPATH 一致），不借系统 /usr 栈——裸 ctest 即全 vendored。
    set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT
      "QGIS_PREFIX_PATH=${PALEO_QGIS_RUNTIME_PREFIX}")
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

set(PALEO_QTEST_FIRST_RUN_DIR "${CMAKE_BINARY_DIR}/Testing/qtest-first-run")
file(MAKE_DIRECTORY "${PALEO_QTEST_FIRST_RUN_DIR}")

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
  add_test(NAME ${name} COMMAND ${name}
           -o "${PALEO_QTEST_FIRST_RUN_DIR}/${name}.txt,txt")
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
  add_test(NAME ui_tokens COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_tokens.py)
  add_test(NAME ui_tokens_strict COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_tokens.py --strict)
  add_test(NAME ui_tokens_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_ui_tokens.py --selftest)
  # 方向 71（#76 依赖口径统一）：三路 QGIS 版本口径一致性——superbuild 精确
  # == deb 闭包上游 == manifest qgis_family 家族，补丁名随 superbuild 版本。
  # 防 4.2.2/4.2.3/4.2.x 三口径再分裂；selftest 合成树验证各分裂形态能打红。
  add_test(NAME qgis_versions COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_qgis_versions.py)
  add_test(NAME qgis_versions_selftest COMMAND ${PALEO_PYTHON3}
           ${CMAKE_SOURCE_DIR}/tools/check_qgis_versions.py --selftest)
  set_tests_properties(layering layering_strict layering_selftest
                       ui_invariants ui_invariants_strict ui_invariants_selftest
                       i18n i18n_selftest ui_tokens ui_tokens_strict ui_tokens_selftest
                       qgis_versions qgis_versions_selftest
                       PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

# ---- 测试注册（devex T1 链接瘦身：LIBS = 各测试 include 面的最小闭包，见
# docs/progress/devex.md 对比表；单参 = 伞式旧式契约，保留 5 个作为兼容样本）----
add_paleo_test(tst_runtime LIBS paleo_qgis)                 # metadata,qgis
add_paleo_test(tst_projectsvc LIBS paleo_qgis)              # metadata,qgis
add_paleo_test(tst_layerservice LIBS paleo_qgis)            # catalog,metadata,qgis
add_paleo_test(tst_canvas_tools LIBS paleo_services paleo_qgis) # metadata,qgis,services
add_paleo_test(tst_domain LIBS paleo_domain)
add_paleo_test(tst_jobrunner LIBS paleo_services paleo_ui_deps)  # 方向20：JobRunner 三段式框架断言
add_paleo_test(tst_linkage LIBS paleo_linkage)
add_paleo_test(tst_algorithms LIBS paleo_algorithms paleo_ui_deps)
add_paleo_test(tst_seismicattr LIBS paleo_algorithms)      # 地震属性核（纯数值，无 QGIS init）
add_paleo_test(tst_sattrio LIBS paleo_io)                 # goal/attr-volume：SATR/SATV 容器 round-trip
add_paleo_test(tst_seismicattrscan LIBS paleo_services)  # goal/attr-volume：时间切片/属性体扫描编排
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
add_paleo_test(tst_singlefactor_contours LIBS paleo_qgis)
add_paleo_test(tst_singlefactor_fieldcontours LIBS paleo_workflow)
add_paleo_test(tst_singlefactor_contourbudget LIBS paleo_algorithms paleo_qgis)
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
add_paleo_test(tst_mappingworkbench)                    # 伞式（ui/pages+qgis+workflow 全栈面，兼容样本）
add_paleo_test(tst_ui)                                      # 伞式（app 面全栈，兼容样本）
add_paleo_test(tst_panels)                                  # 伞式（全栈，兼容样本）
# RUN_SERIAL 独占机器跑：dataops_d2_filterPerformance10k 的 100ms 墙钟断言
# 在 -j8 并行负载下偶发超限（108ms 实测）——同 tst_perf/tst_correlation_full
# 的计时抖动关回串行基线先例（根治归 TEST-02 类递延）。
set_tests_properties(tst_panels PROPERTIES RUN_SERIAL TRUE)
add_paleo_test(tst_mappingpages LIBS paleo_ui)
add_paleo_test(tst_wellattributes LIBS paleo_ui)
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
add_paleo_test(tst_import LIBS paleo_io paleo_services paleo_qgis) # 方向44：并集可达断言
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
add_paleo_test(tst_io_workbook_edges LIBS paleo_io)
add_paleo_test(tst_segy LIBS paleo_io)
add_paleo_test(tst_las LIBS paleo_io)
add_paleo_test(tst_dlisparser LIBS paleo_io)          # 方向44：DLIS (RP66 v1) 全集读口
add_paleo_test(tst_lisparser LIBS paleo_io)           # 方向44：LIS79 全集读口
add_paleo_test(tst_lasperf LIBS paleo_io)    # 方向21：LAS 读面比率门探针（同进程参照实现）
add_paleo_test(tst_snapshot_dedup LIBS paleo_io) # #84：快照/活对象 dedup 复核同一语义（真重哈希）
add_paleo_test(tst_constraintstore LIBS paleo_workflow)
add_paleo_test(tst_constraintediting LIBS paleo_ui)
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
add_paleo_test(tst_standardelements LIBS paleo_qgis) # 方向25：标准图件元素工厂 + 三类内容模板骨架
add_paleo_test(tst_layoutpersistence LIBS paleo_qgis) # 方向25：布局随工程 QGZ round-trip
add_paleo_test(tst_layouttemplatelibrary LIBS paleo_workflow) # 方向25：模板库（qpt→catalog 版本 + 库表）
add_paleo_test(tst_horizonbatchexport LIBS paleo_workflow) # 方向25：按层位组批量出图 + catalog 登记
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
add_test(NAME boot COMMAND tst_boot
         -o "${PALEO_QTEST_FIRST_RUN_DIR}/boot.txt,txt")
paleo_test_sandbox(boot)

add_executable(tst_polygonize tests/tst_polygonize.cpp spikes/polygonize/paleopolygonize.cpp)
target_link_libraries(tst_polygonize PRIVATE paleo_qgis_iface Qt6::Test ${GDAL_LIB})
target_include_directories(tst_polygonize PRIVATE ${GDAL_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/spikes/polygonize)
add_test(NAME polygonize COMMAND tst_polygonize
         -o "${PALEO_QTEST_FIRST_RUN_DIR}/polygonize.txt,txt")
paleo_test_sandbox(polygonize)

# ---- wave3/model-hardening (数据模型与测试基建加固) ----
add_paleo_test(tst_metastore LIBS paleo_store)      # sqlite user_version：新库采纳/遗留升级/未来拒开
add_paleo_test(tst_layermanifest LIBS paleo_store)  # P1-11 / ARCH-04 LayerManifest 线程亲和性与写队列调度门禁
add_paleo_test(tst_segy_fixture LIBS paleo_io)   # 合成最小 SEG-Y 夹具：索引/单线/角点/范围门
target_compile_definitions(tst_segy_fixture PRIVATE
  TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata")
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

# ---- wave4/runtime-resilience ----
# 崩溃本地转储（§38 留白按本地优先落地）+ 外链源重定位恢复路径。
# 新增源与测试集中在此块；并行包合并冲突时保留双方行。
add_paleo_test(tst_crashreport LIBS paleo_services)
add_paleo_test(tst_errorhub LIBS paleo_services)      # 方向64：去重窗口/聚合/环形逐出/并发
add_paleo_test(tst_errorhub_d54 LIBS paleo_services)   # 方向54：ErrorHub 核心数据服务与去重/环形缓冲单元测试
add_paleo_test(tst_notification_card LIBS paleo_ui paleo_services) # 方向54：通知卡片与管理器测试
add_paleo_test(tst_notification_m2_challenger LIBS paleo_ui paleo_services) # 方向54：M2 Challenger 经验证套件
add_paleo_test(tst_error_history_dock LIBS paleo_ui paleo_services) # 方向54/M4：错误历史模型与面板测试
add_paleo_test(tst_errorhub_challenger_m4 LIBS paleo_ui paleo_services) # 方向54/M4：M4 Challenger UI交互与边界测试套件
add_paleo_test(tst_errorhub_storm LIBS paleo_ui paleo_services) # 方向54/M5：错误风暴压力与事件循环防冻结测试
set_tests_properties(tst_errorhub_storm PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
add_paleo_test(tst_errorhub_challenger_m5_harness LIBS paleo_ui paleo_services) # 方向54/M5：M5-2 Challenger 极端压力与经验证套件
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

# ---- 并行开发挂点 ----
# 各并行方向的新增源文件/测试一律放 cmake/extra-<方向>.cmake，内容为：
#   target_sources(paleo_<模块> PRIVATE src/.../foo.cpp)
#   add_paleo_test(tst_foo)
# 禁止直接改本文件的模块源列表——并行分支在此汇合零冲突。
# 显式清单登记替代隐式 GLOB，根治构建目录漂移与测试漏注（P1-14 / TEST-01）
set(_paleo_extra_manifest
  "${CMAKE_SOURCE_DIR}/cmake/extra-ai-chat.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-batch.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-well-facies.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-catalog-sqlite.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-crossplot-facies.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-data.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-a.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-b.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-c.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-d.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-derivation-graph.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-evolution.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-facies-automapping.cmake"
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
  "${CMAKE_SOURCE_DIR}/cmake/extra-python-scripting.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-realization.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-section-split.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-sections-alignment.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-seismic.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-seismic-inversion.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-shell-lifecycle.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-sf-kriging.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-sf-completion.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-shortcuts-help.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-surfaceops.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-time-depth.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-ux.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-well-logset.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-well-trajectory.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-wellcomposite.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-welltops.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-wellsection.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-test-deepening.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-xplot-sup-supervised.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-xplot-sup-em.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-xplot-sup-som.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-xplot-sup-service.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-xplot-sup-ui.cmake"
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
  tst_wellcomposite_perf tst_cache_las tst_welllogset_perf tst_geostat_perf
  tst_lasperf tst_dataperf_index tst_coldstart)
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
