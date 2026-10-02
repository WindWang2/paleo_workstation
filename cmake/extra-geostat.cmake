# goal/geostat — 地质统计学方法包（变差函数 / 克里金 / SGS / 断层绕距）。
# 源文件与测试集中在此。根 CMakeLists 的模块源列表不改；清单只加本文件。
# geostat 是既有 algorithms 模块下的子目录（同 faultsurface 口径），
# 不走 scripts/new_module.sh。

target_sources(paleo_algorithms PRIVATE
  src/algorithms/geostat/variogram.cpp
  src/algorithms/geostat/linsolve.cpp
  src/algorithms/geostat/kriging.cpp)

add_paleo_test(tst_geostat_variogram LIBS paleo_algorithms)
add_paleo_test(tst_geostat_kriging LIBS paleo_algorithms)
