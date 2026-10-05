# goal/property-modeling：地层格架、井曲线粗化、IDW 充填、属性体工作流
# goal/prop-model-v2 增量：断块错位（faultoffset）+ 序贯高斯充填（sgsfill，
# 消费 geostat::sgs3——paleo_algorithms 内部跨子目录链接）。
target_sources(paleo_algorithms PRIVATE
  src/algorithms/stratgrid/stratgrid.cpp
  src/algorithms/stratgrid/upscale.cpp
  src/algorithms/stratgrid/propfill.cpp
  src/algorithms/stratgrid/faultoffset.cpp
  src/algorithms/stratgrid/sgsfill.cpp
  src/algorithms/stratgrid/objectmodel.cpp)
target_sources(paleo_workflow PRIVATE
  src/workflow/propertymodelworkflow.cpp)
target_sources(paleo_ui PRIVATE
  src/ui/propertymodel/propertymodelpanel.cpp)

add_paleo_test(tst_stratgrid LIBS paleo_algorithms)
add_paleo_test(tst_upscale LIBS paleo_algorithms paleo_io)
target_compile_definitions(tst_upscale PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_propfill LIBS paleo_algorithms)
add_paleo_test(tst_faultoffset LIBS paleo_algorithms)
add_paleo_test(tst_sgsfill LIBS paleo_algorithms)
add_paleo_test(tst_objectmodel LIBS paleo_algorithms)
add_paleo_test(tst_propworkflow LIBS paleo_workflow)
add_paleo_test(tst_propmodelpanel LIBS paleo_ui)
add_paleo_test(tst_propmodelperf LIBS paleo_workflow)
target_compile_definitions(tst_propmodelperf PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
