cmake_minimum_required(VERSION 3.25)
project(paleo_native_perf LANGUAGES CXX)

# Run the production numeric kernels with their original QtTests without the
# application's QGIS/bootstrap closure. No implementation is copied or stubbed.
set(PALEO_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Paleo source checkout")
get_filename_component(PALEO_SOURCE_ROOT "${PALEO_SOURCE_ROOT}" ABSOLUTE)
set(PALEO_NATIVE_PERF_TESTS "geostat" CACHE STRING "Native performance tests to build")
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
  add_executable(tst_geostat_perf
    "${PALEO_SOURCE_ROOT}/tests/tst_geostat_perf.cpp"
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/variogram.cpp"
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/linsolve.cpp"
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/kriging.cpp"
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/sgs.cpp"
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/crossvalidation.cpp"
    "${PALEO_SOURCE_ROOT}/src/algorithms/geostat/faultpath.cpp")
  target_include_directories(tst_geostat_perf PRIVATE "${PALEO_SOURCE_ROOT}/src")
  target_link_libraries(tst_geostat_perf PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
  add_test(NAME tst_geostat_perf COMMAND tst_geostat_perf)
  set_tests_properties(tst_geostat_perf PROPERTIES LABELS perf TIMEOUT 300)
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
    domain/documentsource.cpp domain/documentrelation.cpp domain/deviationsurvey.cpp)
  list(TRANSFORM _welllogset_sources PREPEND "${PALEO_SOURCE_ROOT}/src/")
  add_executable(tst_welllogset_perf "${PALEO_SOURCE_ROOT}/tests/tst_welllogset_perf.cpp"
    ${_welllogset_sources})
  target_include_directories(tst_welllogset_perf PRIVATE "${PALEO_SOURCE_ROOT}/src"
    "${PALEO_SOURCE_ROOT}/vendor/glm" "${PALEO_SOURCE_ROOT}/vendor/sbm/src")
  target_link_libraries(tst_welllogset_perf PRIVATE Paleo::paleo_data_service
    Qt6::Core Qt6::Gui Qt6::Test Qt6::Sql ZLIB::ZLIB)
  # The numeric/header paths do not call GIS transforms. Keep the exact source
  # TUs and discard their unrelated function sections rather than providing stubs.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_compile_options(tst_welllogset_perf PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(tst_welllogset_perf PRIVATE -Wl,--gc-sections)
  else()
    message(FATAL_ERROR "The focused welllogset target needs GNU/Clang section garbage collection")
  endif()
  add_test(NAME tst_welllogset_perf COMMAND tst_welllogset_perf)
  set_tests_properties(tst_welllogset_perf PROPERTIES LABELS perf TIMEOUT 300)
endif()

foreach(_paleo_native_perf_test IN LISTS PALEO_NATIVE_PERF_TESTS)
  if(NOT TARGET "tst_${_paleo_native_perf_test}_perf")
    message(FATAL_ERROR "Selected native performance target was not created: ${_paleo_native_perf_test}")
  endif()
endforeach()
