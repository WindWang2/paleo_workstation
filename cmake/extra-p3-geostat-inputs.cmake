# P3/geostat-inputs — SGS 硬屏障真实消费与协克里金协变量跨 CRS 采样（2026-11-08）。
# 源与测试集中在此；公共清单只登记本文件。
#  - singlefactor/sgsbarrier：BarrierGrid → SgsBarrierInput 纯数据适配
#   （稳定重编号 + 引擎 consumedIds 透传），workflow 物化与算法测试共用。
#  - qgis/geostatcovariatesampler：协变量跨 CRS 局部采样（井位/格点同一实现）。
#  - 测试：隔断行为回归（tst_geostat_barrier_partition）、协变量采样跨 CRS
#   一致性（tst_geostat_covariate_sampling）。

target_sources(paleo_algorithms PRIVATE
  src/algorithms/singlefactor/sgsbarrier.cpp)

target_sources(paleo_qgis PRIVATE
  src/qgis/geostatcovariatesampler.cpp)

add_paleo_test(tst_geostat_barrier_partition LIBS paleo_algorithms)
add_paleo_test(tst_geostat_covariate_sampling LIBS paleo_qgis)
