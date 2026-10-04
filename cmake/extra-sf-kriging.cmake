# goal/sf-kriging — 方向41：克里金接入 singlefactor 本地方向插值面
# + 外委格式（SFPKG / XML / XLSX）批量读取 + 制图策略参数包词表。
# 源文件与测试集中在此（根 CMakeLists 的模块源列表不改，并行分支零冲突）。
# 数值核复用 algorithms/geostat（方向18 的变差函数与加边 LU 克里金求解器）；
# 这里只加「同一插值面出真克里金」的接入层、io 读取面与 domain 词表。

target_sources(paleo_algorithms PRIVATE
  src/algorithms/singlefactor/krigingsurface.cpp)

target_sources(paleo_io PRIVATE
  src/io/ziparchive.cpp
  src/io/sfpkgreader.cpp
  src/io/outsourceworkbook.cpp)

# zlib：.sfpkg/.xlsx 的 ZIP deflate 解压（stored 条目不需要它）。QGIS/GDAL 已经
# 依赖 zlib，这里显式找一次；找不到时 ziparchive.cpp 直接 #error —— 硬依赖，
# 不静默降级成「只能读 stored 包」。
find_path(PALEO_ZLIB_INCLUDE zlib.h
  HINTS ${QGIS_PREFIX} ENV OSGEO4W_ROOT PATH_SUFFIXES include)
find_library(PALEO_ZLIB_LIBRARY NAMES z zlib zdll
  HINTS ${QGIS_PREFIX} ENV OSGEO4W_ROOT PATH_SUFFIXES lib)
if(PALEO_ZLIB_INCLUDE AND PALEO_ZLIB_LIBRARY)
  target_include_directories(paleo_io SYSTEM PRIVATE ${PALEO_ZLIB_INCLUDE})
  target_link_libraries(paleo_io PRIVATE ${PALEO_ZLIB_LIBRARY})
  target_compile_definitions(paleo_io PRIVATE PALEO_HAVE_ZLIB=1)
else()
  message(WARNING "zlib 未找到：paleo_io 的 ZIP 读面（.sfpkg/.xlsx）会编译失败（PALEO_HAVE_ZLIB 未定义）")
endif()

add_paleo_test(tst_singlefactor_kriging LIBS paleo_algorithms)
add_paleo_test(tst_io_sfpkg LIBS paleo_io)
add_paleo_test(tst_io_outsource LIBS paleo_io)

target_sources(paleo_domain PRIVATE
  src/domain/singlefactorstrategy.cpp)

add_paleo_test(tst_singlefactor_strategy LIBS paleo_domain)
