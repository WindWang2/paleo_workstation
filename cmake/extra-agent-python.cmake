# Optional, out-of-process Agent Python. No Python application code is installed.
file(SHA256 "${CMAKE_SOURCE_DIR}/vendor/agent-python/runtime.lock.json" _paleo_python_lock_sha)
file(SHA256 "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/summary.py" _paleo_python_summary_sha)
add_library(paleo_services_pythonruntime STATIC
  src/services/pythonruntime.cpp
  src/services/pythonruntime_probe.cpp)
target_sources(paleo_services_pythonruntime PRIVATE src/services/pythoninputbundle.cpp src/services/pythonprocess.cpp)
target_sources(paleo_services PRIVATE src/services/pythoncapabilityspec.cpp)
if(WIN32)
  target_sources(paleo_services_pythonruntime PRIVATE src/services/pythonprocess_windows.cpp
    src/services/pythonprocess_windows_job.cpp src/services/pythonprocess_windows_launch.cpp)
  target_link_libraries(paleo_services_pythonruntime PRIVATE advapi32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND Qt6_VERSION VERSION_GREATER_EQUAL "6.7")
  target_sources(paleo_services_pythonruntime PRIVATE src/services/pythonprocess_linux.cpp src/services/pythonprocess_linux_helpers.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(paleo_services_pythonruntime PRIVATE src/services/pythonprocess_linux_unsupported.cpp)
endif()
target_include_directories(paleo_services_pythonruntime PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(paleo_services_pythonruntime PUBLIC Qt6::Core)
target_compile_definitions(paleo_services_pythonruntime PRIVATE
  PALEO_AGENT_PYTHON_SOURCE_LOCK_SHA="${_paleo_python_lock_sha}")
target_compile_definitions(paleo_services_pythonruntime PRIVATE PALEO_AGENT_PYTHON_SUMMARY_SHA="${_paleo_python_summary_sha}")
target_compile_definitions(paleo_services PRIVATE PALEO_AGENT_PYTHON_SUMMARY_SHA="${_paleo_python_summary_sha}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/vendor/agent-python/runtime.lock.json"
  "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/summary.py")
target_link_libraries(paleo_services PUBLIC paleo_services_pythonruntime)
target_sources(paleo_workflow PRIVATE src/workflow/aichattoolrunner_python.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/capabilityoperationeffects.cpp
  src/workflow/pythonscriptoperation.cpp src/workflow/pythonscriptworker.cpp)
add_paleo_test(tst_pythonruntime LIBS paleo_services_pythonruntime)
add_paleo_test(tst_pythonenvironment LIBS paleo_workflow)
add_paleo_test(tst_pythonprocess LIBS paleo_services_pythonruntime)
add_paleo_test(tst_pythoninputbundle LIBS paleo_services_pythonruntime)
add_paleo_test(tst_pythonoperations LIBS paleo_workflow)
target_compile_definitions(tst_pythonoperations PRIVATE
  PALEO_AGENT_PYTHON_TEMPLATE_ROOT="${CMAKE_SOURCE_DIR}/vendor/agent-python/templates")
target_compile_definitions(tst_pythonprocess PRIVATE PALEO_PROCESS_FIXTURE_PYTHON="${PALEO_PYTHON3}")
if(PALEO_PYTHON3)
  add_test(NAME agent_python_supply COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_agent_python_runtime.py)
  add_test(NAME agent_python_supply_selftest COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_agent_python_runtime.py --selftest)
  set_tests_properties(agent_python_supply agent_python_supply_selftest PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT "PYTHONUTF8=1;PYTHONDONTWRITEBYTECODE=1")
endif()
