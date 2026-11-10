# ---- 安装/重定位（#86，最小可用面）----
# 只装主程序与 selfcheck；QGIS/GDAL 等运行时闭包不在此复制（体积与许可面
# 需单独决策），重定位后的 prefix 需自行放置 <prefix>/share/qgis 与库，
# 或用 QGIS_PREFIX_PATH 指向——QgisRuntime::defaultPrefixPath() 会优先识别
# <exe>/../share/qgis/resources/srs.db 布局。
include(GNUInstallDirs)
# ---- Office 编辑组件布线（构建树即跑即用）----
# 运行时仍由 OfficePreviewSession 按既有候选序查找。CI 的依赖准备和
# 夹具测试可在组件缺失时配置，安装树在写入任何应用文件前检查真实组件。
set(PALEO_OFFICE_EDITOR "" CACHE PATH "ranuts/document editor directory (empty = env or vendor)")
set(_paleo_office_src "${PALEO_OFFICE_EDITOR}")
if(NOT _paleo_office_src)
  set(_paleo_office_src "$ENV{PALEO_OFFICE_EDITOR}")
endif()
set(_paleo_office_override "${_paleo_office_src}")
if(NOT _paleo_office_src)
  set(_paleo_office_src "${CMAKE_SOURCE_DIR}/vendor/ranuts-document")
endif()
get_filename_component(_paleo_office_src "${_paleo_office_src}" ABSOLUTE
                       BASE_DIR "${CMAKE_SOURCE_DIR}")
set(_paleo_office_dst "${CMAKE_BINARY_DIR}/office-editor")
file(STRINGS "${CMAKE_SOURCE_DIR}/vendor/office-editor-required.txt" _paleo_office_assets)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_SOURCE_DIR}/vendor/office-editor-required.txt")

function(_paleo_office_ready root out)
  set(${out} FALSE PARENT_SCOPE)
  foreach(asset IN LISTS _paleo_office_assets)
    set(path "${root}/${asset}")
    if(NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
      return()
    endif()
    file(SIZE "${path}" size)
    if(size EQUAL 0)
      return()
    endif()
  endforeach()
  set(${out} TRUE PARENT_SCOPE)
endfunction()

_paleo_office_ready("${_paleo_office_src}" _paleo_office_src_ready)
_paleo_office_ready("${_paleo_office_dst}" _paleo_office_dst_ready)
if(NOT _paleo_office_dst_ready AND _paleo_office_src_ready)
  if(NOT EXISTS "${_paleo_office_dst}")
    if(NOT WIN32)
      execute_process(
        COMMAND "${CMAKE_COMMAND}" -E create_symlink "${_paleo_office_src}" "${_paleo_office_dst}"
        RESULT_VARIABLE _paleo_office_link)
    else()
      # 固定批处理命令，路径由环境变量一次展开。空格与 cmd 元字符保留原样。
      set(_paleo_office_batch "${CMAKE_BINARY_DIR}/.paleo-office-mklink.cmd")
      file(WRITE "${_paleo_office_batch}"
        "@echo off\r\nmklink /J \"%PALEO_OFFICE_LINK%\" \"%PALEO_OFFICE_TARGET%\"\r\n")
      execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
          "PALEO_OFFICE_LINK=${_paleo_office_dst}" "PALEO_OFFICE_TARGET=${_paleo_office_src}"
          cmd /d /v:off /c .paleo-office-mklink.cmd
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        RESULT_VARIABLE _paleo_office_link OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(NOT _paleo_office_link EQUAL 0)
      message(WARNING "office-editor: 自动接线失败——请手动执行 "
        "mklink /J \"${_paleo_office_dst}\" \"${_paleo_office_src}\"，"
        "或设 PALEO_OFFICE_EDITOR 指向组件目录")
    endif()
  endif()
  _paleo_office_ready("${_paleo_office_dst}" _paleo_office_dst_ready)
endif()
if(_paleo_office_dst_ready)
  message(STATUS "office-editor: <build>/office-editor 已就绪")
elseif(NOT _paleo_office_src_ready)
  message(WARNING
    "office-editor 组件缺失或不完整。请运行 bash vendor/fetch-ranuts-document.sh，"
    "或设置 PALEO_OFFICE_EDITOR 指向完整的编辑器目录。")
else()
  message(WARNING "office-editor: <build>/office-editor 未就绪，请接入完整组件目录。")
endif()

set(_paleo_office_install_src "${_paleo_office_src}")
if(NOT _paleo_office_src_ready AND NOT _paleo_office_override AND _paleo_office_dst_ready)
  set(_paleo_office_install_src "${_paleo_office_dst}")
endif()
string(CONFIGURE [=[
  foreach(asset IN ITEMS @_paleo_office_assets@)
    set(path "@_paleo_office_install_src@/${asset}")
    if(NOT EXISTS "${path}" OR IS_DIRECTORY "${path}")
      message(FATAL_ERROR "office-editor: missing ${path}. Prepare the editor before installing.")
    endif()
    file(SIZE "${path}" size)
    if(size EQUAL 0)
      message(FATAL_ERROR "office-editor: empty ${path}. Prepare the editor before installing.")
    endif()
  endforeach()
]=] _paleo_office_install_guard @ONLY)
install(CODE "${_paleo_office_install_guard}")
install(DIRECTORY "${_paleo_office_install_src}/" DESTINATION libexec/office-editor)

# 字体已嵌入程序；安装树同时保留可独立阅读的许可与来源清单。
install(FILES resources/fonts/OFL-IBMPlex.txt resources/fonts/OFL.txt
              resources/fonts/README.md resources/fonts/ibm-plex.lock.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/paleo/licenses/fonts)

if(NOT WIN32)
  # 安装树只用 $ORIGIN 相对 RUNPATH：不把源码树/构建树绝对路径带进安装产物。
  # 构建树的 BUILD_RPATH（vendor 绝对路径）不受影响。
  set(_paleo_install_rpath
    "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    "$ORIGIN/../lib")
  list(REMOVE_DUPLICATES _paleo_install_rpath)
  set_target_properties(paleo paleo_selfcheck paleo_data_snapshot_worker PROPERTIES
    INSTALL_RPATH "${_paleo_install_rpath}"
    INSTALL_RPATH_USE_LINK_PATH FALSE)
endif()

install(TARGETS paleo paleo_selfcheck
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS paleo_data_snapshot_worker RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32 AND TARGET paleo_freexl_runtime)
  install(FILES "${CMAKE_BINARY_DIR}/paleo_freexl.dll" DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES vendor/Data_service/vendor/freexl/NOTICE.txt
    vendor/Data_service/vendor/freexl/LGPL-2.1.txt
    vendor/Data_service/vendor/freexl/spreadsheet_compat.patch
    vendor/Data_service/vendor/freexl/freexl-2.0.0/COPYING
    DESTINATION ${CMAKE_INSTALL_DATADIR}/paleo/licenses/freexl)
endif()

if(PALEO_HAVE_ORT AND NOT WIN32)
  # onnxruntime 是本仓 vendored 的唯一非 QGIS 共享库，随安装树走 lib/。
  file(GLOB _paleo_ort_libs "${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib/libonnxruntime.so*")
  install(FILES ${_paleo_ort_libs} DESTINATION ${CMAKE_INSTALL_LIBDIR})
elseif(PALEO_HAVE_ORT AND WIN32)
  install(FILES "${ORT_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
