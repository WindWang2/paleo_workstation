# cmake/extra-deepen-c.cmake — wave/deepen-perf Track C（编图域功能深化）
# 新增源文件/测试的挂载点（见 CMakeLists.txt「并行开发挂点」段；禁止直接改
# 根 CMakeLists 的模块源列表）。

target_sources(paleo_algorithms PRIVATE src/algorithms/distancetransform.cpp) # C5 welldist 绕障距离引擎（paleo:paleo_distance_transform）
