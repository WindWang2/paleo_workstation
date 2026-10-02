# goal/seismic-inversion — 确定性叠后反演：子波库/低频模型/带限/稀疏脉冲/编排。
# 源文件与测试集中在此。根 CMakeLists 的模块源列表不改；清单只加本文件。
# inversion 与 dsp 是既有 algorithms 模块下的子目录，不走 scripts/new_module.sh。

target_sources(paleo_algorithms PRIVATE
  src/algorithms/dsp/fft.cpp
  src/algorithms/inversion/wavelet.cpp
  src/algorithms/inversion/lowfreq.cpp
  src/algorithms/inversion/bandlimit.cpp)

add_paleo_test(tst_inversion_wavelet LIBS paleo_algorithms)

add_paleo_test(tst_inversion_lowfreq LIBS paleo_algorithms)

add_paleo_test(tst_inversion_bandlimit LIBS paleo_algorithms)
