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

# .sfpkg/.xlsx 的 ZIP deflate 解压复用 SDK 选择的 Zlib。
target_link_libraries(paleo_io PRIVATE ZLIB::ZLIB)
target_compile_definitions(paleo_io PRIVATE PALEO_HAVE_ZLIB=1)

add_paleo_test(tst_singlefactor_kriging LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_variogram_barrier LIBS paleo_algorithms)
add_paleo_test(tst_io_sfpkg LIBS paleo_io)
add_paleo_test(tst_io_outsource LIBS paleo_io)

target_sources(paleo_domain PRIVATE
  src/domain/singlefactorstrategy.cpp)

add_paleo_test(tst_singlefactor_strategy LIBS paleo_domain)
