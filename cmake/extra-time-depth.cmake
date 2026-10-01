# goal/time-depth-velocity：时深转换与速度建模（algorithms 核 / workflow 执行器 / 测试）
target_sources(paleo_algorithms PRIVATE src/algorithms/velocitymodel.cpp)

add_paleo_test(tst_velocitymodel LIBS paleo_algorithms)
