# goal/sedimentary-evolution（方向35）— 沉积体系多期演化分析。
# 内核（algorithms/evolution 纯计算）+ 读取器（io）+ 编排（workflow）+
# 动览面板（ui）与测试集中在此；根 CMakeLists 模块表不改，清单只加本文件。
# evolution 是既有 algorithms 模块下的子目录（同 geostat 口径），不走
# scripts/new_module.sh。

target_sources(paleo_algorithms PRIVATE
  src/algorithms/evolution/compare.cpp)

target_sources(paleo_io PRIVATE
  src/io/faciescoveragereader.cpp)

target_sources(paleo_workflow PRIVATE
  src/workflow/evolutionworkflow.cpp)

target_sources(paleo_ui PRIVATE
  src/ui/evolution/evolutionplayerpanel.cpp)

# extra-evolution.cmake 的 include 点在 add_paleo_test 定义之后（根
# CMakeLists 模块表区），测试可直接注册。
add_paleo_test(tst_evolution_kernel LIBS paleo_algorithms)
add_paleo_test(tst_evolution_workflow LIBS paleo_workflow paleo_io)
add_paleo_test(tst_evolutionplayer LIBS paleo_ui)
