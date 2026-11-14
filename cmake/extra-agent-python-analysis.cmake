# Bounded numerical input materialization stays outside the QtCore process/runtime library.
file(SHA256 "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/well_quality.py" _paleo_python_quality_sha)
file(SHA256 "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/attribute_correlation.py" _paleo_python_correlation_sha)
add_library(paleo_services_pythonanalysis STATIC
  src/services/pythonanalysis.cpp src/services/pythonanalysis_materialize.cpp src/services/pythonanalysis_metadata.cpp
  src/services/pythonanalysisreport.cpp src/services/pythonanalysisselection.cpp)
target_include_directories(paleo_services_pythonanalysis PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(paleo_services_pythonanalysis PUBLIC paleo_services_pythonruntime paleo_domain Paleo::paleo_data_service Qt6::Core)
target_compile_definitions(paleo_services_pythonanalysis PRIVATE
  PALEO_AGENT_PYTHON_WELL_QUALITY_SHA="${_paleo_python_quality_sha}"
  PALEO_AGENT_PYTHON_ATTRIBUTE_CORRELATION_SHA="${_paleo_python_correlation_sha}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/well_quality.py"
  "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/attribute_correlation.py")
target_sources(paleo_services PRIVATE src/services/pythonanalysiscapabilityspec.cpp)
target_link_libraries(paleo_services PUBLIC paleo_services_pythonanalysis)
target_link_libraries(paleo_ai PUBLIC paleo_services_pythonanalysis)
target_sources(paleo_ai PRIVATE src/ai/chat/pythonanalysistools.cpp)
target_sources(paleo_services_query PRIVATE src/services/projectquery_curves.cpp)
target_sources(paleo_app_agentproviders PRIVATE src/app/pythonanalysiswiring.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/capabilityhost_async.cpp
  src/workflow/pythonanalysisoperation.cpp src/workflow/pythonanalysisworker.cpp src/workflow/pythonoperationdiagnostics.cpp)
add_paleo_test(tst_pythonanalysis LIBS paleo_services_pythonanalysis)
target_compile_definitions(tst_pythonanalysis PRIVATE
  PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES="${CMAKE_SOURCE_DIR}/vendor/agent-python/templates")
add_paleo_test(tst_pythonanalysisreport LIBS paleo_services_pythonanalysis)
add_paleo_test(tst_pythonanalysisselection LIBS paleo_services_pythonanalysis paleo_ai)
add_paleo_test(tst_pythonanalysisquery LIBS paleo_services_query)
add_paleo_test(tst_pythonanalysiswiring LIBS paleo_app_agentproviders)
target_compile_definitions(tst_pythonanalysiswiring PRIVATE
  PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES="${CMAKE_SOURCE_DIR}/vendor/agent-python/templates")
add_paleo_test(tst_pythonanalysisoperations LIBS paleo_workflow)
add_paleo_test(tst_pythonpreparation LIBS paleo_workflow)
target_compile_definitions(tst_pythonanalysisoperations PRIVATE
  PALEO_TEST_PYTHON_ANALYSIS_TEMPLATES="${CMAKE_SOURCE_DIR}/vendor/agent-python/templates")

if(PALEO_PYTHON3)
  add_test(NAME agent_python_scripts COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_agent_python_scripts.py)
  add_test(NAME agent_python_scripts_selftest COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_agent_python_scripts.py --selftest)
  set_tests_properties(agent_python_scripts agent_python_scripts_selftest PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT "PYTHONUTF8=1;PYTHONDONTWRITEBYTECODE=1")
endif()
