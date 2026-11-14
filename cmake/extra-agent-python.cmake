# Optional, out-of-process Agent Python. No Python application code is installed.
file(SHA256 "${CMAKE_SOURCE_DIR}/vendor/agent-python/runtime.lock.json" _paleo_python_lock_sha)
add_library(paleo_services_pythonruntime STATIC
  src/services/pythonruntime.cpp
  src/services/pythonruntime_probe.cpp)
target_include_directories(paleo_services_pythonruntime PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(paleo_services_pythonruntime PUBLIC Qt6::Core)
target_compile_definitions(paleo_services_pythonruntime PRIVATE
  PALEO_AGENT_PYTHON_SOURCE_LOCK_SHA="${_paleo_python_lock_sha}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/vendor/agent-python/runtime.lock.json")
target_link_libraries(paleo_services PUBLIC paleo_services_pythonruntime)
target_sources(paleo_workflow PRIVATE src/workflow/aichattoolrunner_python.cpp)
add_paleo_test(tst_pythonruntime LIBS paleo_services_pythonruntime)
add_paleo_test(tst_pythonenvironment LIBS paleo_workflow)
if(PALEO_PYTHON3)
  add_test(NAME agent_python_supply COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_agent_python_runtime.py)
  add_test(NAME agent_python_supply_selftest COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_agent_python_runtime.py --selftest)
  set_tests_properties(agent_python_supply agent_python_supply_selftest PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT "PYTHONUTF8=1;PYTHONDONTWRITEBYTECODE=1")
endif()
