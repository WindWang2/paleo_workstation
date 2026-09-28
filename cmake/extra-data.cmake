# wave/data-foundation —— 本方向新增源文件/测试的挂载点（见 CMakeLists.txt
# 「并行开发挂点」段；禁止直接改根 CMakeLists 的模块源列表）。
target_sources(paleo_algorithms PRIVATE src/algorithms/welldist.cpp) # T11 welldist 单因素核

# T10 ingestplan 幂等压力（重复执行/中断续跑/部分失败）+ 快照等价性
add_paleo_test(tst_ingestplan)
target_compile_definitions(tst_ingestplan PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
