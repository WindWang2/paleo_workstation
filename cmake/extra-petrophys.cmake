# wave/petrophysics-logs —— 测井岩石物理计算面（公式核/计算器/LAS 写出/
# 批处理/QC/UI 面板）。挂载点纪律见 CMakeLists.txt「并行开发挂点」段。
target_sources(paleo_algorithms PRIVATE
  src/algorithms/petrophys.cpp
  src/algorithms/curveexpr.cpp)
target_sources(paleo_io PRIVATE src/io/laswriter.cpp)
target_sources(paleo_services PRIVATE src/services/petrophyscomputeservice.cpp)

add_paleo_test(tst_petrophys LIBS paleo_algorithms)   # 公式/对齐/QC 核（解析断言）
add_paleo_test(tst_curveexpr LIBS paleo_algorithms)   # 表达式引擎（null 传播/报错路径）
add_paleo_test(tst_petrophysbatch LIBS paleo_services) # 批处理编排（进度/取消/catalog/写队列）
