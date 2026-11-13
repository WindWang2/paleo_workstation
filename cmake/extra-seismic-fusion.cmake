# Optional local service client; no Python or inference runtime enters the application closure.
target_sources(paleo_services PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../src/services/seismicattributefusionclient.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/services/seismicattributefusionclient_http.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/services/seismicattributefusionclient_cli.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/services/seismicattributefusionclient_result.cpp)
target_link_libraries(paleo_services PUBLIC Qt6::Network)
add_paleo_test(tst_seismicattributefusion LIBS paleo_services)
target_compile_definitions(tst_seismicattributefusion PRIVATE
  PALEO_FUSION_TEST_PYTHON="${PALEO_PYTHON3}")
