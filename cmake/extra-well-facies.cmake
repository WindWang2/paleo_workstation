target_sources(paleo_domain PRIVATE src/domain/welllogfacies.cpp)
target_sources(paleo_ai PRIVATE src/ai/wellfaciesservice.cpp src/ai/wellfacieskeystore.cpp)
target_include_directories(paleo_ai PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(paleo_ai PUBLIC paleo_domain Qt6::Network)
# #133：测井相 API 密钥进系统钥匙串。QtKeychain 是 QGIS 的既有依赖（deb 闭包
# 已含 libqt6keychain1；OSGeo4W 随 qgis 分发），找到 dev 包即启用；找不到时
# 回落 JSON（POSIX 0600）并在 configure 输出里如实提示。
find_package(Qt6Keychain CONFIG QUIET)
if(TARGET Qt6Keychain::Qt6Keychain)
  target_link_libraries(paleo_ai PRIVATE Qt6Keychain::Qt6Keychain)
  target_compile_definitions(paleo_ai PRIVATE PALEO_HAVE_QTKEYCHAIN)
  message(STATUS "well-facies: API key storage = system keychain (Qt6Keychain ${Qt6Keychain_VERSION})")
else()
  message(STATUS "well-facies: Qt6Keychain not found — API key falls back to ~/.config/paleo/well-facies.json (0600)")
endif()
target_sources(paleo_workflow PRIVATE src/workflow/wellfaciesworkflow.cpp)
add_paleo_test(tst_wellfacies LIBS paleo_ui paleo_io)
add_paleo_test(tst_wellfaciesconfig LIBS paleo_ai)  # #133 https 强制/loopback 例外/密钥不落 JSON
add_paleo_test(tst_wellfacies_remote LIBS paleo_ai)  # 预测专项：远端服务 mock-HTTP 合同边界
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
