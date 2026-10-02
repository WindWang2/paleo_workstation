# ---- 安装/重定位（#86，最小可用面）----
# 只装主程序与 selfcheck；QGIS/GDAL 等运行时闭包不在此复制（体积与许可面
# 需单独决策），重定位后的 prefix 需自行放置 <prefix>/share/qgis 与库，
# 或用 QGIS_PREFIX_PATH 指向——QgisRuntime::defaultPrefixPath() 会优先识别
# <exe>/../share/qgis/resources/srs.db 布局。
include(GNUInstallDirs)

if(NOT WIN32)
  # 安装树只用 $ORIGIN 相对 RUNPATH：不把源码树/构建树绝对路径带进安装产物。
  # 构建树的 BUILD_RPATH（vendor 绝对路径）不受影响。
  set(_paleo_install_rpath
    "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    "$ORIGIN/../lib")
  list(REMOVE_DUPLICATES _paleo_install_rpath)
  set_target_properties(paleo paleo_selfcheck PROPERTIES
    INSTALL_RPATH "${_paleo_install_rpath}"
    INSTALL_RPATH_USE_LINK_PATH FALSE)
endif()

install(TARGETS paleo paleo_selfcheck
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(PALEO_HAVE_ORT AND NOT WIN32)
  # onnxruntime 是本仓 vendored 的唯一非 QGIS 共享库，随安装树走 lib/。
  file(GLOB _paleo_ort_libs "${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib/libonnxruntime.so*")
  install(FILES ${_paleo_ort_libs} DESTINATION ${CMAKE_INSTALL_LIBDIR})
elseif(PALEO_HAVE_ORT AND WIN32)
  install(FILES "${ORT_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
