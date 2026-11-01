# ---- 安装/重定位（#86，最小可用面）----
# 只装主程序与 selfcheck；QGIS/GDAL 等运行时闭包不在此复制（体积与许可面
# 需单独决策），重定位后的 prefix 需自行放置 <prefix>/share/qgis 与库，
# 或用 QGIS_PREFIX_PATH 指向——QgisRuntime::defaultPrefixPath() 会优先识别
# <exe>/../share/qgis/resources/srs.db 布局。
include(GNUInstallDirs)
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
  set_target_properties(paleo paleo_selfcheck PROPERTIES
    INSTALL_RPATH "${_paleo_install_rpath}"
    INSTALL_RPATH_USE_LINK_PATH FALSE)
endif()

install(TARGETS paleo paleo_selfcheck
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

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

# ---- Office 编辑组件布线（构建树即跑即用）----
# OfficePreviewSession::editorRoot() 的候选序：PALEO_OFFICE_EDITOR 环境变量 →
# <exe>/office-editor → <exe>/../vendor/ranuts-document → <exe>/../libexec/office-editor。
# 组件是 gitignored 的 vendored 静态编辑器（ranuts/document v0.0.6，OnlyOffice
# WASM，AGPL-3.0——vendor/fetch-ranuts-document.sh 钉版构建，勿移 ONLYOFFICE
# 标识）。构建树直跑 build/paleo.exe 原先只剩相对回退 <exe>/../vendor/
# ranuts-document 可命中——worktree/独立构建目录没有它，应用报「Office 编辑
# 组件未安装」。configure 期把组件接进 <build>/office-editor（环境变量之下
# 的首位候选）：Unix 符号链接；Windows 免特权用目录 junction（cmd 整命令
# 单参传递，规避 CMake 参数转义把引号变 \" 的坑；本树 459MB/深路径，不做
# 整树复制兜底——复制在 MAX_PATH 上必翻车）。组件缺失不阻断构建（可选件），
# 但警告给出抓取指引；tst_officepreview 用 PALEO_OFFICE_EDITOR 自带夹具，
# 不依赖本接线。
set(_paleo_office_src "${CMAKE_SOURCE_DIR}/vendor/ranuts-document")
set(_paleo_office_dst "${CMAKE_BINARY_DIR}/office-editor")
if(EXISTS "${_paleo_office_dst}/index.html")
  message(STATUS "office-editor: <build>/office-editor 已就绪")
elseif(NOT EXISTS "${_paleo_office_src}/index.html")
  message(WARNING
    "office-editor 组件缺失：应用内 Office 预览将报「未安装」。抓取："
    "bash vendor/fetch-ranuts-document.sh（钉版 ranuts/document v0.0.6，AGPL-3.0）；"
    "或设 PALEO_OFFICE_EDITOR 指向已就绪的组件目录。worktree 开发请 symlink "
    "主仓 vendor/ranuts-document（见 BUILDING.md「独立 worktree 开发」）。")
else()
  if(NOT EXISTS "${_paleo_office_dst}")
    set(_paleo_office_link 1)
    if(NOT WIN32)
      execute_process(
        COMMAND "${CMAKE_COMMAND}" -E create_symlink "${_paleo_office_src}" "${_paleo_office_dst}"
        RESULT_VARIABLE _paleo_office_link)
    else()
      # cmd /c 直传内嵌引号会被 CMake 转义成 \"（cmd 报「卷标语法不正确」），
      # 借中转批处理把引号握在自己手里——路径含空格也稳。
      set(_paleo_office_batch "${CMAKE_BINARY_DIR}/.paleo-office-mklink.cmd")
      file(WRITE "${_paleo_office_batch}"
        "@echo off\r\nmklink /J \"${_paleo_office_dst}\" \"${_paleo_office_src}\"\r\n")
      execute_process(
        COMMAND cmd /d /c "${_paleo_office_batch}"
        RESULT_VARIABLE _paleo_office_link OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(NOT _paleo_office_link EQUAL 0)
      message(WARNING "office-editor: 自动接线失败——请手动执行 "
        "mklink /J \"${_paleo_office_dst}\" \"${_paleo_office_src}\"，"
        "或设 PALEO_OFFICE_EDITOR 指向组件目录")
    endif()
  endif()
  if(EXISTS "${_paleo_office_dst}/index.html")
    message(STATUS "office-editor: 已接入 <build>/office-editor（来源 ${_paleo_office_src}）")
  else()
    message(WARNING "office-editor: <build>/office-editor 未就绪——Office 预览将报「未安装」，"
      "请按上一条警告接线")
  endif()
  # 安装树走 <exe>/../libexec/office-editor 候选（同组件，许可文件随树携带）。
  install(DIRECTORY "${_paleo_office_src}/" DESTINATION libexec/office-editor)
endif()
