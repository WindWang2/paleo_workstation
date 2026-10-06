# goal/sf-completion — 方向67：单因素域补完（协克里金 / 隔断感知变差函数 /
# SFPKG 写出 / 外委统计与因素候选 / 策略包 UI 接线）。
# 源文件与测试集中在此（根 CMakeLists 的模块源列表不改，并行分支零冲突）。

target_sources(paleo_algorithms PRIVATE
  src/algorithms/geostat/cokriging.cpp)

add_paleo_test(tst_geostat_cokriging LIBS paleo_algorithms)
