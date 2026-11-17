# Explicit SDK table projection input; REPORT v1 and old generated-curve restorers remain separate.
target_sources(paleo_services_pythonanalysis PRIVATE
  src/services/pythongeneratedreportcontent.cpp
  src/services/pythonnumerictable_selection.cpp src/services/pythonnumerictable_materialize.cpp
  src/services/pythonnumerictable_metadata.cpp src/services/pythonnumerictablereport.cpp
  src/services/pythonnumerictablequery.cpp)
target_sources(paleo_services_query PRIVATE src/services/projectquery_numeric_table.cpp)
target_link_libraries(paleo_services_query PUBLIC paleo_services_pythonanalysis)
target_sources(paleo_services PRIVATE src/services/pythonnumerictablespec.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/pythonnumerictableoperation.cpp src/workflow/pythonnumerictableenvironment.cpp)
target_sources(paleo_app_agentproviders PRIVATE src/app/pythonnumerictablewiring.cpp)
add_paleo_test(tst_pythonnumerictable LIBS paleo_services_pythonanalysis)
add_paleo_test(tst_pythonnumerictablewiring LIBS paleo_app_agentproviders)
add_paleo_test(tst_pythonnumerictableoperations LIBS paleo_workflow Qt6::Concurrent)
option(PALEO_TEST_PYTHON_NUMERIC_TABLE_NATIVE "Register real managed numeric table and legacy-v1 business acceptance" OFF)
if(PALEO_TEST_PYTHON_NUMERIC_TABLE_NATIVE)
  add_paleo_test(tst_pythonnumerictable_native LIBS paleo_app_agentproviders Qt6::Concurrent)
endif()
