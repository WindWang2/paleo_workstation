target_sources(paleo_domain PRIVATE src/domain/welllogfacies.cpp src/domain/wellfaciescontract.cpp src/domain/wellref.cpp
  src/domain/wellfaciesrequest.cpp src/domain/wellfaciesrequest_schema.cpp)
add_paleo_test(tst_wellref LIBS paleo_domain)
add_paleo_test(tst_wellfaciesinputplan LIBS paleo_services Paleo::paleo_data_service)
add_paleo_test(tst_wellfaciesagent LIBS paleo_app Paleo::paleo_data_service)
target_sources(paleo_ai PRIVATE src/ai/wellfaciesservice.cpp src/ai/wellfaciesservice_frozen.cpp src/ai/wellfacieskeystore.cpp)
target_include_directories(paleo_ai PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_sources(paleo_services PRIVATE src/services/areawellmetadata.cpp src/services/wellfaciesinputplan.cpp src/services/wellfaciesinputplan_refs.cpp
  src/services/wellfaciesinputstore.cpp src/services/wellfaciesinputinspection.cpp)
target_link_libraries(paleo_ai PUBLIC paleo_domain Qt6::Network)
# The optional keychain support uses the application dependency prefix.
find_package(Qt6Keychain CONFIG QUIET NO_DEFAULT_PATH
  PATHS "${PALEO_DEPENDENCY_PREFIX}" "${PALEO_DEPENDENCY_PREFIX}/apps/qt6")
if(TARGET Qt6Keychain::Qt6Keychain)
  paleo_require_dependency_import(Qt6Keychain::Qt6Keychain)
  target_link_libraries(paleo_ai PRIVATE Qt6Keychain::Qt6Keychain)
  target_compile_definitions(paleo_ai PRIVATE PALEO_HAVE_QTKEYCHAIN)
elseif(PALEO_DEB_PREFIX)
  message(FATAL_ERROR "The locked deb closure is missing Qt6Keychain development metadata")
else()
  message(STATUS "Well facies API key storage uses the private configuration file")
endif()
target_sources(paleo_workflow PRIVATE src/workflow/wellfaciesworkflow.cpp
  src/workflow/aichattoolrunner_facies.cpp src/workflow/wellfaciesagentrun.cpp
  src/workflow/wellfaciesagentrun_predict.cpp src/workflow/wellfaciesagentrun_results.cpp src/workflow/aichattoolrunner_faciesinputs.cpp
  src/workflow/wellfaciesreplay.cpp)
add_paleo_test(tst_wellfacies LIBS paleo_ui paleo_io)
add_paleo_test(tst_wellfaciesconfig LIBS paleo_ai)  # #133 https 强制/loopback 例外/密钥不落 JSON
add_paleo_test(tst_wellfacies_remote LIBS paleo_ai)  # 预测专项：远端服务 mock-HTTP 合同边界
add_paleo_test(tst_wellfaciescontract LIBS paleo_ai)
# 预测专项（安全）：测井相服务缺省地址只允许构建期注入（打包机本地
# -DPALEO_WELL_FACIES_DEFAULT_URL=...），不入库；API 密钥从不内置。
set(PALEO_WELL_FACIES_DEFAULT_URL "" CACHE STRING "Optional build-time default well-facies service URL (no credentials)")
option(PALEO_WELL_FACIES_DEFAULT_ALLOW_HTTP "Allow cleartext http for the build-time default well-facies URL" OFF)
if(PALEO_WELL_FACIES_DEFAULT_URL)
  target_compile_definitions(paleo_ai PRIVATE PALEO_WELL_FACIES_DEFAULT_URL="${PALEO_WELL_FACIES_DEFAULT_URL}")
  if(PALEO_WELL_FACIES_DEFAULT_ALLOW_HTTP)
    target_compile_definitions(paleo_ai PRIVATE PALEO_WELL_FACIES_DEFAULT_ALLOW_HTTP)
  endif()
endif()
