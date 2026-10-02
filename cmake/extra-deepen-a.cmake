# cmake/extra-deepen-a.cmake — wave/deepen-perf worker A（地震链路性能深化）
# 新增测试挂载点（CMakeLists.txt 模块表禁改惯例；根 CMakeLists GLOB 自动
# include 本文件）。交付：A1 SBM 引擎剩余入口消费接线（ReadVoxelWindow 体窗
# → 3D 堆叠层合并通道）、A2 三维拖动链路取数合并、A3 瓦片取代语义与空态、
# A4 966MiB 真工区只读复测。

# A4：真工区只读复测（未设 PALEO_SEISMIC_REAL_SGY / PALEO_REAL_PROJECT_AREA
# 时整套 QSKIP——CI 无本地数据不红）。GL 帧率段与 budgets 同款离屏守卫。
add_paleo_test(tst_seismic_realarea LIBS paleo_ui)

# goal/perf-systematize 簇5：真工区内存预算门（RSS 上限 + 开/放循环泄漏
# 嗅探；同款 env 门控未设即 QSKIP）。RSS 计量面挂串行（malloc_trim 采样
# 对并发堆扰动敏感）。
add_paleo_test(tst_mem_budget LIBS paleo_ui)
set_tests_properties(tst_mem_budget PROPERTIES RUN_SERIAL TRUE)
