# Optional local service client; no Python or inference runtime enters the application closure.
add_library(paleo_services_fusion STATIC
  ${CMAKE_CURRENT_LIST_DIR}/../src/services/seismicattributefusionclient.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/services/seismicattributefusionclient_http.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/services/seismicattributefusionclient_cli.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../src/services/seismicattributefusionclient_result.cpp)
target_link_libraries(paleo_services_fusion PUBLIC Qt6::Core Qt6::Network)
get_filename_component(_paleo_fusion_include_root "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)
target_include_directories(paleo_services_fusion PUBLIC "${_paleo_fusion_include_root}")
if(TARGET paleo_services)
  target_link_libraries(paleo_services PUBLIC paleo_services_fusion)
endif()
add_paleo_test(tst_seismicattributefusion LIBS paleo_services_fusion)
target_compile_definitions(tst_seismicattributefusion PRIVATE
  PALEO_FUSION_TEST_PYTHON="${PALEO_PYTHON3}")
