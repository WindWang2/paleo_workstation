# vendor/superbuild — ExternalProject 回退路线骨架（Phase 0 §39/E3）
#
# 这不是主构建的一部分。默认路线是 binary vendoring（vendor/bootstrap.sh +
# vendor/fetch-deps.sh，见 docs/phase0/et0-vendor-comparison.md 的 D7 决策）；
# 仅当 deb 闭包下载验证失败、或必须支持比 binary floor（glibc ≥ 2.41）更老
# 的宿主时，才手动启用本目录（启用条件与回填步骤见 README.md）：
#
#   cmake -S vendor/superbuild -B vendor/build/superbuild -G Ninja
#   ninja -C vendor/build/superbuild            # 断点续跑：ExternalProject stamp 即进度
#
# 产物 prefix：vendor/superbuild/prefix。主构建随后以
# QGIS_PREFIX_PATH=<repo>/vendor/superbuild/prefix 指向它（BUILDING.md）。
#
# 骨架状态（刻意为之，不承诺完整构建）：
#   ✓ 依赖图与顺序（geos → proj → gdal → qgis；Qt 走发行版包，README 决策 D-QT）
#   ✓ ExternalProject stamp 语义（断点续跑 = ninja 增量，无手写 .done 标记）
#   ✓ 裁剪开关集中一处（WITH_3D/WITH_MESH/WITH_PDAL/WITH_APP，默认全 OFF）
#   ✓ URL 留空即「未启用」：configure 报警告、build 在下载步如实失败——
#     启用时按当时的 QGIS 4.2.x 发行版回填 URL+SHA256（pin 后 TTHW 不依赖
#     上游 URL 存活，plan §39）
cmake_minimum_required(VERSION 3.28)
project(paleo_superbuild) # 本层只编排不编译；带编译器仅为下面的 Qt 探测真实可信

include(ExternalProject)

set(SUPERBUILD_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/prefix" CACHE PATH "superbuild 安装 prefix")
set(SUPERBUILD_STAMP "${CMAKE_CURRENT_BINARY_DIR}/stamps")

# ---- 裁剪开关（决策依据见 README.md「裁剪决定」）----
set(WITH_3D OFF CACHE BOOL "QGIS 3D 视图（Paleo 未用：src/ 无 qgis_3d/Qgs3D 引用）")
set(WITH_MESH OFF CACHE BOOL "QGIS 网格数据（Paleo 未用：src/ 无 QgsMesh 引用）")
set(WITH_PDAL OFF CACHE BOOL "QGIS 点云/PDAL provider（Paleo 未用：产品面无点云）")
set(WITH_APP OFF CACHE BOOL "QGIS 桌面 App（Paleo 只链 qgis_core/gui/analysis）")
set(WITH_GUI ON CACHE BOOL "qgis_gui（Paleo 嵌入式 GUI 需要）")
set(WITH_ANALYSIS ON CACHE BOOL "qgis_analysis（Paleo 链接，CMakeLists.txt:37）")

# 源码钉位：骨架阶段 URL 留空 = 未启用。启用时回填（README「启用步骤」），
# 并同时补 URL_HASH SHA256=<...>——两者一起才算钉死。
set(GEOS_URL "" CACHE STRING "GEOS 源码 tarball URL")
set(PROJ_URL "" CACHE STRING "PROJ 源码 tarball URL")
set(GDAL_URL "" CACHE STRING "GDAL 源码 tarball URL")
set(QGIS_URL "" CACHE STRING "QGIS 4.2.x 源码 tarball URL")

foreach(dep_url_var GEOS_URL PROJ_URL GDAL_URL QGIS_URL)
  if(NOT "${${dep_url_var}}")
    message(WARNING "superbuild 骨架未填 ${dep_url_var}：按 README 回填后再构建（当前状态=未启用）")
  endif()
endforeach()

# ---- Qt：不进 superbuild（决策 D-QT，README）----
# Qt 由发行版/OSGeo4W 提供（LGPL 动态链接满足合规；qt-everywhere 整块编译
# 直接爆 2h 预算）。preflight 对 superbuild 路追加 flex+bison、nasm/python。
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# 未启用态（任一 URL 为空）：跳过全部 ExternalProject 目标——configure 可以
# 过（骨架可验证），ninja 无事可做；部分回填属中间编辑态，不半途构建。
set(SUPERBUILD_ENABLED ON)
foreach(dep_url_var GEOS_URL PROJ_URL GDAL_URL QGIS_URL)
  if(NOT "${${dep_url_var}}")
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
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${SUPERBUILD_PREFIX}
             -DCMAKE_BUILD_TYPE=Release
             -DBUILD_TESTING=OFF
  STAMP_DIR ${SUPERBUILD_STAMP}/geos
  BUILD_ALWAYS OFF)

ExternalProject_Add(proj
  URL "${PROJ_URL}"
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
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${SUPERBUILD_PREFIX}
             -DCMAKE_BUILD_TYPE=Release
             -DCMAKE_PREFIX_PATH=${SUPERBUILD_PREFIX}
             -DWITH_3D=${WITH_3D}
             -DWITH_MESH=${WITH_MESH}
             -DWITH_PDAL=${WITH_PDAL}
             -DWITH_APP=${WITH_APP}
             -DWITH_GUI=${WITH_GUI}
             -DWITH_ANALYSIS=${WITH_ANALYSIS}
             -DWITH_BINDINGS=OFF      # Paleo 是 C++ embed，无 Python
             -DBUILD_TESTING=OFF
  DEPENDS gdal
  STAMP_DIR ${SUPERBUILD_STAMP}/qgis
  BUILD_ALWAYS OFF)
