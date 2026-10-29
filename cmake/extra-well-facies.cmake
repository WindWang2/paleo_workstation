target_sources(paleo_domain PRIVATE src/domain/welllogfacies.cpp src/domain/wellfaciescontract.cpp)
target_sources(paleo_ai PRIVATE src/ai/wellfaciesservice.cpp src/ai/wellfacieskeystore.cpp)
target_include_directories(paleo_ai PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(paleo_ai PUBLIC paleo_domain Qt6::Network)
# #133：测井相 API 密钥进系统钥匙串。QtKeychain 是 QGIS 的既有依赖（deb 闭包
# 已含 libqt6keychain1；OSGeo4W 随 qgis 分发），找到 dev 包即启用；找不到时
# 回落 JSON（POSIX 0600）并在 configure 输出里如实提示。
# #766：单一份口径——Qt6Keychain 必须与构建所链的 Qt 同源。有 vendored 前缀
# （QGIS_PREFIX 非空）时只认前缀内/QGIS 前缀那一份：apt 的
# qtkeychain-qt6-dev（0.17，按 Qt 6.11 编）与 vendored Qt 6.10 混链会在链接期
# 报 qt_version_tag@Qt_6.11 未定义（实测配方见
# docs/ledger-archive/goal-loop-initial.md 环境坑④），运行时还会出现
# 「系统与 vendor 各一份 libqt6keychain」的库遮蔽。故用 NO_DEFAULT_PATH 把
# /usr 这类系统前缀排除出搜索面（CMAKE_PREFIX_PATH 与 PATHS 仍参与，见
# find_package 搜索序），前缀内没有即回落 JSON，绝不静默吃系统那份；
# 无 vendored 前缀的系统兜底构建下 Qt 本身就是系统的，保持原全量搜索。
if(QGIS_PREFIX)
  find_package(Qt6Keychain CONFIG QUIET NO_DEFAULT_PATH
               PATHS "${QGIS_PREFIX}" "${QGIS_PREFIX}/apps/qt6" "${QGIS_PREFIX}/usr")
else()
  find_package(Qt6Keychain CONFIG QUIET)
endif()
if(TARGET Qt6Keychain::Qt6Keychain)
  target_link_libraries(paleo_ai PRIVATE Qt6Keychain::Qt6Keychain)
  target_compile_definitions(paleo_ai PRIVATE PALEO_HAVE_QTKEYCHAIN)
  if(QGIS_PREFIX)
    message(STATUS "well-facies: API key storage = vendored keychain (Qt6Keychain ${Qt6Keychain_VERSION}, prefix ${QGIS_PREFIX})")
  else()
    message(STATUS "well-facies: API key storage = system keychain (Qt6Keychain ${Qt6Keychain_VERSION})")
  endif()
else()
  if(QGIS_PREFIX)
    message(STATUS "well-facies: Qt6Keychain not found under vendored prefix ${QGIS_PREFIX} — API key falls back to ~/.config/paleo/well-facies.json (0600)（#766：不吃系统那份，防与 vendored Qt 混链）")
  else()
    message(STATUS "well-facies: Qt6Keychain not found — API key falls back to ~/.config/paleo/well-facies.json (0600)")
  endif()
endif()
target_sources(paleo_workflow PRIVATE src/workflow/wellfaciesworkflow.cpp)
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
