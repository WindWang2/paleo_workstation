# vendor/superbuild — ExternalProject 自编译路线（策略首选，已钉位）
#
# 这不是主构建的一部分。2026-10-01 起「尽量不依赖系统库、尽量自编译
# vendored」是仓库级依赖策略（BUILDING.md「依赖来源策略」）：本目录即策略
# 首选路线，binary vendoring（vendor/fetch-deps.sh）与 OSGeo4W 闭包降为
# 加速档，发行版系统包仅兜底。历史对比与旧决策（D7：批准 binary
# vendoring）见 docs/phase0/et0-vendor-comparison.md（保留作底账，优先级
# 以新政策为准）。启用步骤见 README.md：
#
#   cmake -S vendor/superbuild -B vendor/build/superbuild -G Ninja
#   ninja -C vendor/build/superbuild            # 断点续跑：ExternalProject stamp 即进度
#
# 产物 prefix：vendor/superbuild/prefix。主构建随后以
# QGIS_PREFIX_PATH=<repo>/vendor/superbuild/prefix 指向它（BUILDING.md）。
#
# 已钉位状态（URL + SHA256，pin 后 TTHW 不依赖上游 URL 存活，plan §39）：
#   GEOS 3.15.0 / PROJ 9.8.1 / GDAL 3.13.3 / QGIS 4.2.2（与已验证的 Arch
#   闭包同版）；tarball 缓存于 vendor/cache/tarballs/，哈希与各
#   ExternalProject 的 URL_HASH 一一对应。
#   ✓ 依赖图与顺序（geos → proj → gdal → qgis；Qt 走发行版包，README 决策 D-QT）
#   ✓ ExternalProject stamp 语义（断点续跑 = ninja 增量，无手写 .done 标记）
#   ✓ 裁剪面仍集中一处（WITH_3D/WITH_PDAL 默认 OFF；WITH_APP→QGIS 真名
#     WITH_DESKTOP=FALSE；WITH_GUI/WITH_ANALYSIS ON。QGIS 4.2.2 无
#     WITH_MESH option——mesh 已并入 core，无裁剪面）
cmake_minimum_required(VERSION 3.28)
project(paleo_superbuild) # 本层只编排不编译；带编译器仅为下面的 Qt 探测真实可信

include(ExternalProject)

set(SUPERBUILD_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/prefix" CACHE PATH "superbuild 安装 prefix")
set(SUPERBUILD_STAMP "${CMAKE_CURRENT_BINARY_DIR}/stamps")

# ---- 裁剪开关（决策依据见 README.md「裁剪决定」；与 QGIS 4.2.2 真实 option 名单对齐）----
set(WITH_3D OFF CACHE BOOL "QGIS 3D 视图（Paleo 未用：src/ 无 qgis_3d/Qgs3D 引用）")
# QGIS 4.2.2 无 WITH_MESH option（tarball CMakeLists 实证：无该 cache 变量；
# mesh 已并入 core，无裁剪面）——原「Paleo 未用 QgsMesh」的裁剪意图无从落地，
# 保留此说明备查，不再设开关。
set(WITH_PDAL OFF CACHE BOOL "QGIS 点云/PDAL provider（Paleo 未用：产品面无点云）")
set(WITH_APP OFF CACHE BOOL "QGIS 桌面 App（Paleo 只链 qgis_core/gui/analysis）；下方映射到 QGIS 真名 WITH_DESKTOP")
set(WITH_GUI ON CACHE BOOL "qgis_gui（Paleo 嵌入式 GUI 需要）")
set(WITH_ANALYSIS ON CACHE BOOL "qgis_analysis（Paleo 链接，CMakeLists.txt:37）")

# 源码钉位（版本与已验证的 Arch 闭包同版）：tarball 缓存于
# vendor/cache/tarballs/；URL 与 URL_HASH SHA256 成对才算钉死——换版本时
# 两处同步改，缓存 tarball 与哈希一并更新。
set(GEOS_URL "https://download.osgeo.org/geos/geos-3.15.0.tar.bz2" CACHE STRING "GEOS 源码 tarball URL（GEOS 3.15.0）")
set(PROJ_URL "https://download.osgeo.org/proj/proj-9.8.1.tar.gz" CACHE STRING "PROJ 源码 tarball URL（PROJ 9.8.1）")
set(GDAL_URL "https://download.osgeo.org/gdal/3.13.3/gdal-3.13.3.tar.xz" CACHE STRING "GDAL 源码 tarball URL（GDAL 3.13.3）")
set(QGIS_URL "https://download.qgis.org/downloads/qgis-4.2.2.tar.bz2" CACHE STRING "QGIS 源码 tarball URL（QGIS 4.2.2）")

foreach(dep_url_var GEOS_URL PROJ_URL GDAL_URL QGIS_URL)
  if("${${dep_url_var}}" STREQUAL "")
    message(WARNING "superbuild ${dep_url_var} 未钉位：URL 与 URL_HASH SHA256 需成对回填后再构建")
  endif()
endforeach()

# ---- Qt：不进 superbuild（决策 D-QT，README）----
# Qt 由发行版/OSGeo4W 提供（LGPL 动态链接满足合规；qt-everywhere 整块编译
# 直接爆 2h 预算）。preflight 对 superbuild 路追加 flex+bison、nasm/python。
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# 兜底（任一 URL 为空）：跳过全部 ExternalProject 目标——configure 仍可过
# （Qt 探测可单独验证），ninja 无事可做；部分钉位属中间编辑态，不半途构建。
set(SUPERBUILD_ENABLED ON)
foreach(dep_url_var GEOS_URL PROJ_URL GDAL_URL QGIS_URL)
  if("${${dep_url_var}}" STREQUAL "")
    set(SUPERBUILD_ENABLED OFF)
  endif()
endforeach()
if(NOT SUPERBUILD_ENABLED)
  message(STATUS "superbuild 未启用（源码 URL 未钉死）——Qt 探测已过，不建 ExternalProject 目标。见 vendor/superbuild/README.md")
  return()
endif()

# ---- 底座库（源码构建进同一 prefix，后者经 CMAKE_PREFIX_PATH 看见前者）----
ExternalProject_Add(geos
  URL "${GEOS_URL}"
  URL_HASH SHA256=d5e5192a686d065eaed082de14dd26244c5c8e02bff16b2c6cce3265f648e00e
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${SUPERBUILD_PREFIX}
             -DCMAKE_BUILD_TYPE=Release
             -DBUILD_TESTING=OFF
  STAMP_DIR ${SUPERBUILD_STAMP}/geos
  BUILD_ALWAYS OFF)

ExternalProject_Add(proj
  URL "${PROJ_URL}"
  URL_HASH SHA256=af5b731c145c1d13c4e3b4eeb7d167e94e845e440f71e3496b4ed8dae0291960
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${SUPERBUILD_PREFIX}
             -DCMAKE_BUILD_TYPE=Release
             -DENABLE_CURL=ON
             -DBUILD_TESTING=OFF
             # proj.db 随包安装——QGIS 运行时要能解析到（plan §39 运行时数据文件）
             -DPROJ_DATA_PATH=${SUPERBUILD_PREFIX}/share/proj
  STAMP_DIR ${SUPERBUILD_STAMP}/proj
  BUILD_ALWAYS OFF)

ExternalProject_Add(gdal
  URL "${GDAL_URL}"
  URL_HASH SHA256=a3348c2102dd91962290007d7c293b467bb1b0cd89f086f24bb97b9b653a9804
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${SUPERBUILD_PREFIX}
             -DCMAKE_BUILD_TYPE=Release
             -DCMAKE_PREFIX_PATH=${SUPERBUILD_PREFIX}   # 找到已装的 geos/proj
             -DBUILD_TESTING=OFF
             -DBUILD_PYTHON_BINDINGS=OFF
  DEPENDS geos proj
  STAMP_DIR ${SUPERBUILD_STAMP}/gdal
  BUILD_ALWAYS OFF)

# ---- QGIS（裁剪开关直通；analysis/gui 是 Paleo 的链接面）----
ExternalProject_Add(qgis
  URL "${QGIS_URL}"
  URL_HASH SHA256=45c3a85b359ec1acd11725a9af8c66b90ee7ca0e85f14df18ed9792f1b9becf0
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${SUPERBUILD_PREFIX}
             -DCMAKE_BUILD_TYPE=Release
             -DCMAKE_PREFIX_PATH=${SUPERBUILD_PREFIX}
             -DWITH_3D=${WITH_3D}
             -DWITH_PDAL=${WITH_PDAL}
             -DWITH_DESKTOP=${WITH_APP}   # QGIS 4.2.2 的真名是 WITH_DESKTOP（无 WITH_APP option；亦无 WITH_MESH option，mesh 在 core 无裁剪面）
             -DWITH_GUI=${WITH_GUI}
             -DWITH_ANALYSIS=${WITH_ANALYSIS}
             # 系统 libspatialindex 2.1 触发 QGIS 版本门禁（要求 <2.1），改走
             # tarball 内置副本（2.0.0）——与 Arch 官方 qgis 包同策（不链系统版）
             -DWITH_INTERNAL_SPATIALINDEX=ON
             -DWITH_BINDINGS=OFF      # Paleo 是 C++ embed，无 Python
             -DBUILD_TESTING=OFF
  DEPENDS gdal
  STAMP_DIR ${SUPERBUILD_STAMP}/qgis
  BUILD_ALWAYS OFF)
