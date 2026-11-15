# Optional trusted Pi orchestration: the default C++ build never discovers or starts Node.
set(_paleo_pi_lock "${CMAKE_SOURCE_DIR}/vendor/pi/source.lock.json")
file(READ "${_paleo_pi_lock}" _paleo_pi_json)
file(SHA256 "${_paleo_pi_lock}" PALEO_PI_LOCK_SHA)
string(JSON PALEO_PI_UPSTREAM GET "${_paleo_pi_json}" upstream commit)
string(JSON PALEO_PI_CORE_VERSION GET "${_paleo_pi_json}" coreVersion)
string(JSON PALEO_PI_NODE_VERSION GET "${_paleo_pi_json}" nodeVersion)
string(JSON PALEO_PI_RUNTIME_VERSION GET "${_paleo_pi_json}" runtimeVersion)
string(JSON PALEO_PI_BUNDLE_SHA GET "${_paleo_pi_json}" build bundleSha256)
if(WIN32)
  set(PALEO_PI_PLATFORM "windows-x86_64")
else()
  set(PALEO_PI_PLATFORM "linux-x86_64")
endif()
string(JSON PALEO_PI_NODE_SHA GET "${_paleo_pi_json}" platforms "${PALEO_PI_PLATFORM}" programSha256)
add_library(paleo_services_piagent STATIC
  src/services/piprotocol.cpp src/services/piruntime.cpp src/services/piprocess.cpp)
target_include_directories(paleo_services_piagent PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(paleo_services_piagent PUBLIC Qt6::Core)
set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/services/piruntime.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
  PALEO_AGENT_PI_SOURCE_LOCK_SHA="${PALEO_PI_LOCK_SHA}"
  PALEO_AGENT_PI_UPSTREAM="${PALEO_PI_UPSTREAM}"
  PALEO_AGENT_PI_CORE_VERSION="${PALEO_PI_CORE_VERSION}"
  PALEO_AGENT_PI_NODE_VERSION="${PALEO_PI_NODE_VERSION}"
  PALEO_AGENT_PI_RUNTIME_VERSION="${PALEO_PI_RUNTIME_VERSION}"
  PALEO_AGENT_PI_NODE_SHA="${PALEO_PI_NODE_SHA}"
  PALEO_AGENT_PI_BUNDLE_SHA="${PALEO_PI_BUNDLE_SHA}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_paleo_pi_lock}")
target_link_libraries(paleo_services PUBLIC paleo_services_piagent)
add_paleo_test(tst_piprotocol LIBS paleo_services_piagent)
add_paleo_test(tst_piruntime LIBS paleo_services_piagent)
if(PALEO_PYTHON3)
  add_test(NAME pi_agent COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_pi_agent.py)
  add_test(NAME pi_agent_selftest COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_pi_agent.py --selftest)
  set_tests_properties(pi_agent pi_agent_selftest PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT "PYTHONUTF8=1;PYTHONDONTWRITEBYTECODE=1")
endif()
option(PALEO_TEST_PI_NATIVE "Register real fixed Pi process acceptance with an explicit locked runtime" OFF)
set(PALEO_TEST_PI_NATIVE_ROOT "" CACHE PATH "Host-approved Pi runtime generation for native acceptance")
set(PALEO_TEST_PI_NATIVE_SHA256 "" CACHE STRING "Host-approved Pi manifest SHA-256 for native acceptance")
if(PALEO_TEST_PI_NATIVE)
  if(NOT WIN32 AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND Qt6_VERSION VERSION_GREATER_EQUAL "6.6"))
    message(FATAL_ERROR "Pi native process acceptance requires Windows or Linux with Qt >= 6.6")
  endif()
  string(LENGTH "${PALEO_TEST_PI_NATIVE_SHA256}" _paleo_pi_test_sha_size)
  if(NOT IS_DIRECTORY "${PALEO_TEST_PI_NATIVE_ROOT}" OR NOT _paleo_pi_test_sha_size EQUAL 64 OR
      NOT PALEO_TEST_PI_NATIVE_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Pi native acceptance requires a runtime directory and host manifest SHA-256")
  endif()
  add_paleo_test(tst_piprocess_native LIBS paleo_services_piagent)
  set_property(TEST tst_piprocess_native APPEND PROPERTY ENVIRONMENT
    "PALEO_TEST_PI_NATIVE_ROOT=${PALEO_TEST_PI_NATIVE_ROOT}"
    "PALEO_TEST_PI_NATIVE_SHA256=${PALEO_TEST_PI_NATIVE_SHA256}")
endif()
# Explicit optional deployment of a preverified generation, never npm or a startup installer.
set(PALEO_PI_INSTALL_ROOT "" CACHE PATH "Prepared Pi generation for explicit AgentPiRuntime component installation")
set(PALEO_PI_INSTALL_SHA256 "" CACHE STRING "Host-approved Pi manifest SHA-256 for explicit component installation")
foreach(_paleo_pi_value IN ITEMS "${PALEO_PI_INSTALL_ROOT}" "${PALEO_PI_INSTALL_SHA256}")
  if(_paleo_pi_value MATCHES "[;\r\n]" OR _paleo_pi_value MATCHES "\\]==\\]")
    message(FATAL_ERROR "Pi install configuration contains unsupported delimiter characters")
  endif()
endforeach()
configure_file("${CMAKE_SOURCE_DIR}/cmake/install-pi-agent.cmake.in"
  "${CMAKE_BINARY_DIR}/install-pi-agent.cmake" @ONLY)
install(SCRIPT "${CMAKE_BINARY_DIR}/install-pi-agent.cmake" COMPONENT AgentPiRuntime EXCLUDE_FROM_ALL)
