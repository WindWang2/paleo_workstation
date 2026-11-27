# The application's conversation engine is built from a pinned Pi/Node closure.
set(_paleo_pi_lock "${CMAKE_SOURCE_DIR}/vendor/pi/source.lock.json")
file(READ "${_paleo_pi_lock}" _paleo_pi_json)
file(SHA256 "${_paleo_pi_lock}" PALEO_PI_LOCK_SHA)
string(JSON PALEO_PI_UPSTREAM GET "${_paleo_pi_json}" upstream commit)
string(JSON PALEO_PI_CORE_VERSION GET "${_paleo_pi_json}" coreVersion)
string(JSON PALEO_PI_NODE_VERSION GET "${_paleo_pi_json}" nodeVersion)
string(JSON PALEO_PI_RUNTIME_VERSION GET "${_paleo_pi_json}" runtimeVersion)
string(JSON PALEO_PI_BUNDLE_SHA GET "${_paleo_pi_json}" build bundleSha256)
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _paleo_pi_cpu)
if(NOT _paleo_pi_cpu MATCHES "^(x86_64|amd64)$")
  message(FATAL_ERROR "The Pi runtime has no repository pin for ${CMAKE_SYSTEM_PROCESSOR}")
elseif(WIN32)
  set(PALEO_PI_PLATFORM "windows-x86_64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(PALEO_PI_PLATFORM "linux-x86_64")
else()
  message(FATAL_ERROR "The Pi runtime has no repository pin for ${CMAKE_SYSTEM_NAME}")
endif()
string(JSON PALEO_PI_NODE_SHA GET "${_paleo_pi_json}" platforms "${PALEO_PI_PLATFORM}" programSha256)
set(_paleo_pi_anchors "${CMAKE_SOURCE_DIR}/vendor/pi/runtime-manifests.lock.json")
file(READ "${_paleo_pi_anchors}" _paleo_pi_manifest_json)
string(JSON _paleo_pi_anchor GET "${_paleo_pi_manifest_json}" "${PALEO_PI_PLATFORM}")
file(RELATIVE_PATH _paleo_pi_from_bindir "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_PREFIX}")
add_library(paleo_services_piagent STATIC
  src/services/piprotocol.cpp src/services/piruntime.cpp src/services/piprocess.cpp
  src/services/piconversation.cpp src/services/piconversation_channel.cpp)
target_include_directories(paleo_services_piagent PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(paleo_services_piagent PUBLIC Qt6::Core)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Node's ELF has no RUNPATH; ship its two non-glibc dependencies from the
  # same managed prefix as the host, separately from the signed Pi payload.
  file(REAL_PATH "${PALEO_DEPENDENCY_PREFIX}" _pi_prefix_real)
  foreach(_pair IN ITEMS "GCC|libgcc_s.so.1" "CXX|libstdc++.so.6")
    string(REPLACE "|" ";" _fields "${_pair}")
    list(GET _fields 0 _id)
    list(GET _fields 1 _soname)
    unset(_pi_library)
    find_file(_pi_library NAMES "${_soname}" PATHS "${_pi_prefix_real}/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
      "${_pi_prefix_real}/lib" NO_DEFAULT_PATH NO_CACHE REQUIRED)
    file(REAL_PATH "${_pi_library}" PALEO_PI_${_id}_FILE)
    cmake_path(IS_PREFIX _pi_prefix_real "${PALEO_PI_${_id}_FILE}" NORMALIZE _pi_inside)
    if(NOT _pi_inside)
      message(FATAL_ERROR "Pi loader dependency ${_soname} escapes ${_pi_prefix_real}")
    endif()
    file(SHA256 "${PALEO_PI_${_id}_FILE}" PALEO_PI_${_id}_SHA)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PALEO_PI_${_id}_FILE}")
  endforeach()
  string(SHA256 PALEO_PI_LOADER_ID "${PALEO_PI_GCC_SHA}:${PALEO_PI_CXX_SHA}")
  set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/services/piruntime.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
    PALEO_PI_GCC_SHA="${PALEO_PI_GCC_SHA}" PALEO_PI_CXX_SHA="${PALEO_PI_CXX_SHA}" PALEO_PI_LOADER_ID="${PALEO_PI_LOADER_ID}")
endif()
set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/services/piruntime.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
  PALEO_AGENT_PI_SOURCE_LOCK_SHA="${PALEO_PI_LOCK_SHA}"
  PALEO_AGENT_PI_UPSTREAM="${PALEO_PI_UPSTREAM}"
  PALEO_AGENT_PI_CORE_VERSION="${PALEO_PI_CORE_VERSION}"
  PALEO_AGENT_PI_NODE_VERSION="${PALEO_PI_NODE_VERSION}"
  PALEO_AGENT_PI_RUNTIME_VERSION="${PALEO_PI_RUNTIME_VERSION}"
  PALEO_AGENT_PI_NODE_SHA="${PALEO_PI_NODE_SHA}"
  PALEO_AGENT_PI_MANIFEST_SHA="${_paleo_pi_anchor}"
  PALEO_AGENT_PI_FROM_BINDIR="${_paleo_pi_from_bindir}"
  PALEO_AGENT_PI_BUNDLE_SHA="${PALEO_PI_BUNDLE_SHA}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_paleo_pi_lock}" "${_paleo_pi_anchors}")
target_link_libraries(paleo_services PUBLIC paleo_services_piagent)
target_sources(paleo_workflow PRIVATE src/workflow/pichatbridge.cpp src/workflow/aichatcontroller_pi.cpp
  src/workflow/chatinvocationbinding.cpp)
target_link_libraries(paleo_workflow PUBLIC paleo_services_piagent)
add_paleo_test(tst_piprotocol LIBS paleo_services_piagent)
add_paleo_test(tst_piruntime LIBS paleo_services_piagent)
add_paleo_test(tst_piconversation LIBS paleo_services_piagent)
add_paleo_test(tst_chatinvocationbinding LIBS paleo_workflow)
add_paleo_test(tst_pichatbridge LIBS paleo_workflow)
add_paleo_test(tst_pigeneratedoperations LIBS paleo_workflow)
if(WIN32)
  set_property(TEST tst_pigeneratedoperations APPEND PROPERTY
    ENVIRONMENT_MODIFICATION "PALEO_TEST_REGISTRY_HIVE=set:native")
endif()
if(PALEO_PYTHON3)
  add_test(NAME pi_agent COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_pi_agent.py)
  add_test(NAME pi_agent_selftest COMMAND ${PALEO_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/check_pi_agent.py --selftest)
  set_tests_properties(pi_agent pi_agent_selftest PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT "PYTHONUTF8=1;PYTHONDONTWRITEBYTECODE=1")
endif()
option(PALEO_TEST_PI_NATIVE "Register real fixed Pi process acceptance with the bundled runtime" OFF)
set(PALEO_TEST_PI_NATIVE_ROOT "" CACHE PATH "Host-approved Pi runtime generation for native acceptance")
set(PALEO_TEST_PI_NATIVE_SHA256 "" CACHE STRING "Host-approved Pi manifest SHA-256 for native acceptance")
if(PALEO_TEST_PI_NATIVE)
  if(NOT WIN32 AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND Qt6_VERSION VERSION_GREATER_EQUAL "6.6"))
    message(FATAL_ERROR "Pi native process acceptance requires Windows or Linux with Qt >= 6.6")
  endif()
  if(NOT PALEO_TEST_PI_NATIVE_ROOT AND NOT PALEO_TEST_PI_NATIVE_SHA256)
    set(PALEO_TEST_PI_NATIVE_ROOT "${CMAKE_BINARY_DIR}/libexec/pi-agent/generations/${_paleo_pi_anchor}")
    set(PALEO_TEST_PI_NATIVE_SHA256 "${_paleo_pi_anchor}")
  elseif(NOT IS_DIRECTORY "${PALEO_TEST_PI_NATIVE_ROOT}" OR NOT PALEO_TEST_PI_NATIVE_SHA256 STREQUAL _paleo_pi_anchor)
    message(FATAL_ERROR "Explicit Pi acceptance fixtures must provide a runtime directory and the pinned ${PALEO_PI_PLATFORM} SHA-256")
  endif()
  add_paleo_test(tst_piprocess_native LIBS paleo_services_piagent)
  set_property(TEST tst_piprocess_native APPEND PROPERTY ENVIRONMENT
    "PALEO_TEST_PI_NATIVE_ROOT=${PALEO_TEST_PI_NATIVE_ROOT}"
    "PALEO_TEST_PI_NATIVE_SHA256=${PALEO_TEST_PI_NATIVE_SHA256}")
endif()
# A prebuilt generation supports offline builds. Otherwise prepare the exact
# same generation while building, using only the repository's locked producer.
set(PALEO_PI_INSTALL_ROOT "" CACHE PATH "Prepared generation for offline Pi builds (empty builds the locked runtime)")
set(PALEO_PI_INSTALL_SHA256 "" CACHE STRING "Optional assertion of the repository-pinned Pi generation identity")
set(PALEO_PI_SUPPLY_CACHE "${CMAKE_BINARY_DIR}/pi-supply-cache" CACHE PATH "Cache of locked Pi build archives")
if(PALEO_PI_INSTALL_SHA256 AND NOT PALEO_PI_INSTALL_SHA256 STREQUAL _paleo_pi_anchor)
  message(FATAL_ERROR "PALEO_PI_INSTALL_SHA256 differs from the repository pin for ${PALEO_PI_PLATFORM}")
endif()
set(_paleo_pi_source "${PALEO_PI_INSTALL_ROOT}")
if(_paleo_pi_source)
  if(_paleo_pi_source MATCHES "[;\r\n]" OR _paleo_pi_source MATCHES "\\]==\\]" OR
      NOT EXISTS "${_paleo_pi_source}/pi-manifest.json")
    message(FATAL_ERROR "PALEO_PI_INSTALL_ROOT must be a complete prepared Pi generation: ${_paleo_pi_source}")
  endif()
else()
  if(NOT PALEO_PYTHON3)
    message(FATAL_ERROR "Building Pi requires Python 3 or a prepared generation supplied via PALEO_PI_INSTALL_ROOT")
  endif()
  set(_paleo_pi_store "${CMAKE_BINARY_DIR}/pi-supply-store")
  set(_paleo_pi_source "${_paleo_pi_store}/generations/${_paleo_pi_anchor}")
  add_custom_target(paleo_pi_supply
    COMMAND "${CMAKE_COMMAND}" "-DPI_PYTHON=${PALEO_PYTHON3}" "-DPI_CACHE=${PALEO_PI_SUPPLY_CACHE}"
      "-DPI_STORE=${_paleo_pi_store}" "-DPI_SHA=${_paleo_pi_anchor}" "-DPI_PLATFORM=${PALEO_PI_PLATFORM}"
      -P "${CMAKE_SOURCE_DIR}/cmake/prepare-pi-runtime.cmake" VERBATIM)
endif()
configure_file("${CMAKE_SOURCE_DIR}/cmake/install-pi-agent.cmake.in"
  "${CMAKE_BINARY_DIR}/install-pi-agent.cmake" @ONLY)
add_custom_target(paleo_pi_runtime
  COMMAND "${CMAKE_COMMAND}" "-DPI_BUILD_PREFIX=${CMAKE_BINARY_DIR}" -P "${CMAKE_BINARY_DIR}/install-pi-agent.cmake"
  COMMENT "Verifying and staging the bundled Pi runtime" VERBATIM)
if(TARGET paleo_pi_supply)
  add_dependencies(paleo_pi_runtime paleo_pi_supply)
endif()
add_dependencies(paleo_services_piagent paleo_pi_runtime)
install(SCRIPT "${CMAKE_BINARY_DIR}/install-pi-agent.cmake" COMPONENT AgentPiRuntime)
