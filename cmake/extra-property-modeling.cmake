# goal/property-modeling：地层格架、井曲线粗化、IDW 充填、属性体工作流
target_sources(paleo_algorithms PRIVATE
  src/algorithms/stratgrid/stratgrid.cpp
  src/algorithms/stratgrid/upscale.cpp
  src/algorithms/stratgrid/propfill.cpp)
target_sources(paleo_workflow PRIVATE
  src/workflow/propertymodelworkflow.cpp)

add_paleo_test(tst_stratgrid LIBS paleo_algorithms)
add_paleo_test(tst_upscale LIBS paleo_algorithms paleo_io)
target_compile_definitions(tst_upscale PRIVATE
  PROJECT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/testdata/project_area")
add_paleo_test(tst_propfill LIBS paleo_algorithms)
add_paleo_test(tst_propworkflow LIBS paleo_workflow)
