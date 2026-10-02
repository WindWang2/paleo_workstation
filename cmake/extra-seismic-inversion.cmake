# goal/seismic-inversion — 确定性叠后反演：子波库/低频模型/带限/稀疏脉冲/编排。
# 源文件与测试集中在此。根 CMakeLists 的模块源列表不改；清单只加本文件。
# inversion 与 dsp 是既有 algorithms 模块下的子目录，不走 scripts/new_module.sh。

target_sources(paleo_algorithms PRIVATE
  src/algorithms/dsp/fft.cpp
  src/algorithms/inversion/wavelet.cpp
  src/algorithms/inversion/lowfreq.cpp
  src/algorithms/inversion/bandlimit.cpp
  src/algorithms/inversion/sparse.cpp
  src/algorithms/inversion/volume.cpp)

add_paleo_test(tst_inversion_wavelet LIBS paleo_algorithms)

add_paleo_test(tst_inversion_lowfreq LIBS paleo_algorithms)

add_paleo_test(tst_inversion_bandlimit LIBS paleo_algorithms)

add_paleo_test(tst_inversion_sparse LIBS paleo_algorithms)

target_sources(paleo_workflow PRIVATE
  src/workflow/inversionworkflow.cpp)

# 三段式编排 + DERIVED 登记 + 并行确定性。串行防抖动（perf 类同款口径）。
add_paleo_test(tst_inversion_workflow LIBS paleo_workflow)
set_tests_properties(tst_inversion_workflow PROPERTIES RUN_SERIAL TRUE)

target_sources(paleo_ui PRIVATE
  src/ui/seismicsection/inversionpanel.cpp)

# 面板是薄表单（信号/回填/诚实文案），offscreen 可测。
add_paleo_test(tst_inversion_panel LIBS paleo_ui)
