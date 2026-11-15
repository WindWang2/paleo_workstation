# Optional, out-of-process Agent Python. No Python application code is installed.
file(SHA256 "${CMAKE_SOURCE_DIR}/vendor/agent-python/runtime.lock.json" _paleo_python_lock_sha)
file(SHA256 "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/summary.py" _paleo_python_summary_sha)
file(SHA256 "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/well_quality.py" _paleo_delivery_quality_sha)
file(SHA256 "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/attribute_correlation.py" _paleo_delivery_correlation_sha)
add_library(paleo_services_pythonruntime STATIC
  src/services/pythonruntime.cpp
  src/services/pythonruntime_probe.cpp)
target_sources(paleo_services_pythonruntime PRIVATE
  src/services/pythondelivery.cpp src/services/pythondelivery_manifest.cpp
  src/services/pythondelivery_files.cpp src/services/pythondelivery_store.cpp)
set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/services/pythondelivery_manifest.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
  PALEO_DELIVERY_SUMMARY_SHA="${_paleo_python_summary_sha}"
  PALEO_DELIVERY_QUALITY_SHA="${_paleo_delivery_quality_sha}"
  PALEO_DELIVERY_CORRELATION_SHA="${_paleo_delivery_correlation_sha}")
# Deployment-only native installer; neither a default install nor an Agent tool.
add_executable(paleo_python_environment EXCLUDE_FROM_ALL src/app/pythonenvironment_main.cpp)
target_link_libraries(paleo_python_environment PRIVATE paleo_services_pythonruntime)
install(TARGETS paleo_python_environment RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT AgentPythonDeployment EXCLUDE_FROM_ALL)
target_sources(paleo_services_pythonruntime PRIVATE src/services/pythoninputbundle.cpp src/services/pythonprocess.cpp
  src/services/pythonsandbox_windows.cpp)
target_sources(paleo_services PRIVATE src/services/pythoncapabilityspec.cpp)
if(WIN32)
  target_sources(paleo_services_pythonruntime PRIVATE src/services/pythonprocess_windows.cpp
    src/services/pythonprocess_windows_job.cpp src/services/pythonprocess_windows_launch.cpp
    src/services/pythonsandbox_windows_security.cpp src/services/pythonsandbox_windows_journal.cpp
    src/services/pythonsandbox_windows_snapshot.cpp)
  target_link_libraries(paleo_services_pythonruntime PRIVATE advapi32 ole32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND Qt6_VERSION VERSION_GREATER_EQUAL "6.7")
  target_sources(paleo_services_pythonruntime PRIVATE src/services/pythonprocess_linux.cpp src/services/pythonprocess_linux_helpers.cpp
    src/services/pythonsandbox_linux.cpp src/services/pythonsandbox_linux_image.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(paleo_services_pythonruntime PRIVATE src/services/pythonprocess_linux_unsupported.cpp)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR Qt6_VERSION VERSION_LESS "6.7")
  target_sources(paleo_services_pythonruntime PRIVATE src/services/pythonsandbox_linux_unsupported.cpp)
endif()
target_include_directories(paleo_services_pythonruntime PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(paleo_services_pythonruntime PUBLIC Qt6::Core)
set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/services/pythonruntime.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
  PALEO_AGENT_PYTHON_SOURCE_LOCK_SHA="${_paleo_python_lock_sha}")
set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/services/pythoncapabilityspec.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
  PALEO_AGENT_PYTHON_SUMMARY_SHA="${_paleo_python_summary_sha}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/vendor/agent-python/runtime.lock.json"
  "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/summary.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/well_quality.py"
  "${CMAKE_SOURCE_DIR}/vendor/agent-python/templates/attribute_correlation.py")
target_link_libraries(paleo_services PUBLIC paleo_services_pythonruntime)
target_sources(paleo_workflow PRIVATE src/workflow/aichattoolrunner_python.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/capabilityoperationeffects.cpp
  src/workflow/pythonscriptoperation.cpp src/workflow/pythonscriptworker.cpp)
add_paleo_test(tst_pythonruntime LIBS paleo_services_pythonruntime)
add_paleo_test(tst_pythondelivery LIBS paleo_services_pythonruntime)
target_compile_definitions(tst_pythondelivery PRIVATE PALEO_DELIVERY_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
add_paleo_test(tst_pythonenvironment LIBS paleo_workflow)
add_paleo_test(tst_pythonprocess LIBS paleo_services_pythonruntime)
add_paleo_test(tst_pythoninputbundle LIBS paleo_services_pythonruntime)
if(WIN32)
  add_paleo_test(tst_python_windows_sandbox LIBS paleo_services_pythonruntime)
  target_link_libraries(tst_python_windows_sandbox PRIVATE ws2_32)
else()
  add_paleo_test(tst_python_windows_unavailable LIBS paleo_services_pythonruntime)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND Qt6_VERSION VERSION_GREATER_EQUAL "6.7")
  add_paleo_test(tst_pythonsandbox_linux LIBS paleo_services_pythonruntime)
else()
  add_paleo_test(tst_python_linux_unavailable LIBS paleo_services_pythonruntime)
endif()
option(PALEO_TEST_PYTHON_LINUX_NATIVE "Register native Linux sandbox acceptance with an explicit locked fixture" OFF)
set(PALEO_TEST_PYTHON_LINUX_NATIVE_ROOT "" CACHE PATH "Native acceptance runtime generation root")
set(PALEO_TEST_PYTHON_LINUX_NATIVE_SHA256 "" CACHE STRING "Host-approved native acceptance manifest SHA-256")
if(PALEO_TEST_PYTHON_LINUX_NATIVE)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR Qt6_VERSION VERSION_LESS "6.7")
    message(FATAL_ERROR "Native Linux sandbox acceptance requires Linux and Qt >= 6.7")
  endif()
  string(LENGTH "${PALEO_TEST_PYTHON_LINUX_NATIVE_SHA256}" _paleo_native_sha_length)
  if(NOT IS_DIRECTORY "${PALEO_TEST_PYTHON_LINUX_NATIVE_ROOT}" OR
      NOT _paleo_native_sha_length EQUAL 64 OR NOT PALEO_TEST_PYTHON_LINUX_NATIVE_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Native Linux sandbox acceptance requires a runtime directory and host SHA-256")
  endif()
  add_paleo_test(tst_pythonsandbox_linux_native LIBS paleo_services_pythonruntime)
  set_property(TEST tst_pythonsandbox_linux_native APPEND PROPERTY ENVIRONMENT
    "PALEO_TEST_PYTHON_LINUX_NATIVE_ROOT=${PALEO_TEST_PYTHON_LINUX_NATIVE_ROOT}"
    "PALEO_TEST_PYTHON_LINUX_NATIVE_SHA256=${PALEO_TEST_PYTHON_LINUX_NATIVE_SHA256}")
endif()
add_paleo_test(tst_pythonoperations LIBS paleo_workflow)
target_compile_definitions(tst_pythonoperations PRIVATE
  PALEO_AGENT_PYTHON_TEMPLATE_ROOT="${CMAKE_SOURCE_DIR}/vendor/agent-python/templates")
target_compile_definitions(tst_pythonprocess PRIVATE PALEO_PROCESS_FIXTURE_PYTHON="${PALEO_PYTHON3}")
if(PALEO_PYTHON3)
  add_test(NAME agent_python_supply COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_agent_python_runtime.py)
  add_test(NAME agent_python_supply_selftest COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_agent_python_runtime.py --selftest)
  add_test(NAME agent_python_delivery COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_agent_python_delivery.py)
  add_test(NAME agent_python_delivery_selftest COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_agent_python_delivery.py --selftest)
  set_tests_properties(agent_python_supply agent_python_supply_selftest agent_python_delivery agent_python_delivery_selftest PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT "PYTHONUTF8=1;PYTHONDONTWRITEBYTECODE=1")
endif()
