cmake_minimum_required(VERSION 3.25)
project(paleo_native_perf LANGUAGES CXX)

# Run the production numeric kernels with their original QtTests without the
# application's QGIS/bootstrap closure. No implementation is copied or stubbed.
set(PALEO_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Paleo source checkout")
get_filename_component(PALEO_SOURCE_ROOT "${PALEO_SOURCE_ROOT}" ABSOLUTE)
set(PALEO_NATIVE_PERF_TESTS "geostat" CACHE STRING "Native performance tests to build")
option(PALEO_NATIVE_GEOSTAT_PROFILE "Build the development-only geostat component timing probe" OFF)
if(NOT PALEO_NATIVE_PERF_TESTS)
  message(FATAL_ERROR "PALEO_NATIVE_PERF_TESTS must select at least one native performance test")
endif()
set(_paleo_native_perf_supported geostat welllogset)
foreach(_paleo_native_perf_test IN LISTS PALEO_NATIVE_PERF_TESTS)
  if(NOT _paleo_native_perf_test IN_LIST _paleo_native_perf_supported)
    message(FATAL_ERROR "Unknown native performance test: ${_paleo_native_perf_test}")
  endif()
endforeach()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)
enable_testing()

if("geostat" IN_LIST PALEO_NATIVE_PERF_TESTS)
  add_library(paleo_native_geostat STATIC
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/variogram.cpp"
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/linsolve.cpp"
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/kriging.cpp"
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/sgs.cpp"
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/crossvalidation.cpp"
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/faultpath.cpp")
  target_include_directories(paleo_native_geostat PUBLIC "${PALEO_SOURCE_ROOT}/src")
  target_link_libraries(paleo_native_geostat PUBLIC Qt6::Core)
  foreach(_geostat_test perf neighborhood kriging sgs variogram linsolve faultpath)
    add_executable(tst_geostat_${_geostat_test}
      "${PALEO_SOURCE_ROOT}/tests/tst_geostat_${_geostat_test}.cpp")
    target_link_libraries(tst_geostat_${_geostat_test} PRIVATE paleo_native_geostat
      Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    add_test(NAME tst_geostat_${_geostat_test} COMMAND tst_geostat_${_geostat_test} -o -,txt)
    set_tests_properties(tst_geostat_${_geostat_test} PROPERTIES TIMEOUT 300)
  endforeach()
  set_tests_properties(tst_geostat_perf PROPERTIES LABELS perf)
  if(PALEO_NATIVE_GEOSTAT_PROFILE)
    # kriging.cpp is included by the probe to expose private components. It is
    # deliberately not linked a second time through paleo_native_geostat.
    add_executable(geostat_phase_profile geostat_phase_profile.cpp
      "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/variogram.cpp"
      "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/linsolve.cpp"
      "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/crossvalidation.cpp"
      "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/faultpath.cpp")
    target_include_directories(geostat_phase_profile PRIVATE "${PALEO_SOURCE_ROOT}/src")
    target_link_libraries(geostat_phase_profile PRIVATE Qt6::Core)
  endif()
endif()

if("welllogset" IN_LIST PALEO_NATIVE_PERF_TESTS)
  find_package(Qt6 REQUIRED COMPONENTS Sql)
  find_package(ZLIB REQUIRED)
  set(PDS_BUILD_DEMO OFF CACHE BOOL "" FORCE)
  set(PDS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(PDS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(PDS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(PDS_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory("${PALEO_SOURCE_ROOT}/vendor/Data_service" data-service EXCLUDE_FROM_ALL)
  set_target_properties(paleo_data_snapshot_worker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set(_welllogset_sources
    services/welllogset.cpp
    io/welllogread.cpp io/lasparser.cpp io/dlisparser.cpp io/lisparser.cpp
    io/welllogtext.cpp io/welllogtext_convert.cpp io/welllogtext_xml.cpp
    io/encodingdetect.cpp io/nativemodel_read.cpp io/dataimport_log.cpp
    io/areamodel.cpp io/areamodel_log.cpp io/areamodel_import.cpp io/areamodel_conventions.cpp
    io/timedeptool.cpp io/cachebudget.cpp io/outsourceworkbook.cpp io/ziparchive.cpp
    catalog/datacatalog.cpp catalog/datacatalog_entities.cpp catalog/datacatalog_assets.cpp
    catalog/datacatalog_links.cpp catalog/datacatalog_versions.cpp catalog/datacatalog_derivation.cpp
    catalog/datacatalog_projection.cpp catalog/datacatalog_export.cpp
    catalog/catalogindex.cpp catalog/roleregistry.cpp catalog/purgelease.cpp
    catalog/catalogstore.cpp catalog/catalogstore_schema.cpp catalog/catalogstore_crud.cpp
    catalog/catalogstore_json.cpp catalog/areawellprojection.cpp catalog/wellavailability.cpp
    catalog/externalsourceverification.cpp metadata/managedpath.cpp metadata/metastore.cpp
    domain/documentsource.cpp domain/documentrelation.cpp domain/deviationsurvey.cpp
    io/wellcompositexml.cpp domain/wellcompositemodel.cpp)
  list(TRANSFORM _welllogset_sources PREPEND "${PALEO_SOURCE_ROOT}/src/")
  add_library(paleo_native_welllog STATIC ${_welllogset_sources})
  target_include_directories(paleo_native_welllog PUBLIC "${PALEO_SOURCE_ROOT}/src"
    "${PALEO_SOURCE_ROOT}/vendor/glm" "${PALEO_SOURCE_ROOT}/vendor/sbm/src")
  target_link_libraries(paleo_native_welllog PUBLIC Paleo::paleo_data_service
    Qt6::Core Qt6::Gui Qt6::Sql ZLIB::ZLIB)
  target_compile_definitions(paleo_native_welllog PRIVATE PALEO_HAVE_ZLIB=1)
  # The numeric/header paths do not call GIS transforms. Keep the exact source
  # TUs and discard their unrelated function sections rather than providing stubs.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_compile_options(paleo_native_welllog PRIVATE -ffunction-sections -fdata-sections)
  else()
    message(FATAL_ERROR "The focused welllogset target needs GNU/Clang section garbage collection")
  endif()
  foreach(_test IN ITEMS tst_welllogset_perf tst_welllogset tst_welllogtext tst_dlisparser tst_lisparser tst_las)
    add_executable(${_test} "${PALEO_SOURCE_ROOT}/tests/${_test}.cpp")
    target_link_libraries(${_test} PRIVATE paleo_native_welllog Qt6::Test)
    target_link_options(${_test} PRIVATE -Wl,--gc-sections)
    add_test(NAME ${_test} COMMAND ${_test} -o -,txt)
    set_tests_properties(${_test} PROPERTIES TIMEOUT 300)
  endforeach()
  foreach(_test IN ITEMS prepared_import_tests import_performance_tests spreadsheet_xml_tests model_change_tests curve_window_tests)
    add_executable(native_${_test} "${PALEO_SOURCE_ROOT}/vendor/Data_service/tests/${_test}.cpp")
    target_include_directories(native_${_test} PRIVATE "${PALEO_SOURCE_ROOT}/vendor/Data_service/src")
    target_link_libraries(native_${_test} PRIVATE Paleo::paleo_data_service Qt6::Core Qt6::Sql Qt6::Test)
    add_test(NAME native_${_test} COMMAND native_${_test})
    set_tests_properties(native_${_test} PROPERTIES TIMEOUT 300)
  endforeach()
  set_tests_properties(tst_welllogset_perf PROPERTIES LABELS perf)
endif()

foreach(_paleo_native_perf_test IN LISTS PALEO_NATIVE_PERF_TESTS)
  if(NOT TARGET "tst_${_paleo_native_perf_test}_perf")
    message(FATAL_ERROR "Selected native performance target was not created: ${_paleo_native_perf_test}")
  endif()
endforeach()

get_property(_native_tests DIRECTORY PROPERTY TESTS)
foreach(_native_test IN LISTS _native_tests)
  set(_native_sandbox "${CMAKE_CURRENT_BINARY_DIR}/test-sandboxes/${_native_test}")
  file(MAKE_DIRECTORY "${_native_sandbox}/config" "${_native_sandbox}/data")
  set_tests_properties(${_native_test} PROPERTIES ENVIRONMENT
    "XDG_CONFIG_HOME=${_native_sandbox}/config;XDG_DATA_HOME=${_native_sandbox}/data")
endforeach()
