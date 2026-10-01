# goal/time-depth-velocity：时深转换与速度建模（algorithms 核 / workflow 执行器 / 测试）
target_sources(paleo_algorithms PRIVATE src/algorithms/velocitymodel.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/depthconversionworkflow.cpp)

add_paleo_test(tst_velocitymodel LIBS paleo_algorithms)
add_paleo_test(tst_depthconversion LIBS paleo_workflow)
target_compile_definitions(tst_depthconversion PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
