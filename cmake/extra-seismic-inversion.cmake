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

# #125：反演/子波井输入的逐井时深口径（header-only 助手 workflow/wellimpedancetwt.h）。
add_paleo_test(tst_inversion_welltd LIBS paleo_algorithms)

target_sources(paleo_workflow PRIVATE
  src/workflow/inversionworkflow.cpp)

# 三段式编排 + DERIVED 登记 + 并行确定性。串行防抖动（perf 类同款口径）。
add_paleo_test(tst_inversion_workflow LIBS paleo_workflow)
set_tests_properties(tst_inversion_workflow PROPERTIES RUN_SERIAL TRUE)

target_sources(paleo_ui PRIVATE
  src/ui/seismicsection/inversionpanel.cpp)

# 面板是薄表单（信号/回填/诚实文案），offscreen 可测。
add_paleo_test(tst_inversion_panel LIBS paleo_ui)

# 性能门：263k 级合成体全量反演墙钟 + 道并行加速比（RUN_SERIAL 防抖动）。
add_paleo_test(tst_inversion_perf LIBS paleo_workflow)
set_tests_properties(tst_inversion_perf PROPERTIES RUN_SERIAL TRUE)

# 真工区实测：env 门控（PALEO_REAL_PROJECT_AREA / PALEO_SEISMIC_REAL_SGY），
# 只读契约，墙钟记账不设硬门。
add_paleo_test(tst_inversion_realarea LIBS paleo_workflow)
