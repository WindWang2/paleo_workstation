# goal/geostat — 地质统计学方法包（变差函数 / 克里金 / SGS / 断层绕距）。
# 源文件与测试集中在此。根 CMakeLists 的模块源列表不改；清单只加本文件。
# geostat 是既有 algorithms 模块下的子目录（同 faultsurface 口径），
# 不走 scripts/new_module.sh。

target_sources(paleo_algorithms PRIVATE
  src/algorithms/geostat/variogram.cpp
  src/algorithms/geostat/linsolve.cpp
  src/algorithms/geostat/kriging.cpp
  src/algorithms/geostat/sgs.cpp
  src/algorithms/geostat/sgs3.cpp
  src/algorithms/geostat/faultpath.cpp)

add_paleo_test(tst_geostat_variogram LIBS paleo_algorithms)
add_paleo_test(tst_geostat_kriging LIBS paleo_algorithms)
add_paleo_test(tst_geostat_sgs LIBS paleo_algorithms)
# goal/prop-model-v2：三维点集入口（stratgrid IJK 格架消费）。2D 入口的
# 共享机件抽在 sgs_internal.h（detail），tst_geostat_sgs 同时守 2D 恒等面。
add_paleo_test(tst_geostat_sgs3 LIBS paleo_algorithms)
add_paleo_test(tst_geostat_faultpath LIBS paleo_algorithms)
# 编排面：workflows.cpp 属根 CMakeLists 既有源（本方向只改内容不挂新源）；
# paleo_workflow→paleo_algorithms 链接由 extra-fault-surface.cmake 先例已建立。
add_paleo_test(tst_geostat_workflow LIBS paleo_workflow)
# 性能：规模线性度比率门（TEST-02 禁绝对墙钟）；绝对门在 tst_geostat_realarea
#（PALEO_REAL_PROJECT_AREA env 门控真机口径）。
add_paleo_test(tst_geostat_perf LIBS paleo_algorithms)
add_paleo_test(tst_geostat_realarea LIBS paleo_algorithms paleo_io) # skips unless PALEO_REAL_PROJECT_AREA
