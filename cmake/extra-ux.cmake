# wave/ux-polish — UX 方向新源文件/测试/翻译挂点（CMakeLists 模块表禁改，见
# CMakeLists.txt「并行开发挂点」注释）。
target_sources(paleo_ui PRIVATE
  src/ui/paleodockmanager.cpp
  src/ui/paleoemptystate.cpp # 共享空态卡片（T31 收敛，layertreepanel 迁移见 docs/progress/ux.md seam 表）
)

# ---- i18n（可选，缺组件不断构建）----
# lupdate 骨架：translations/paleo_zh_CN.ts；更新走 tools/update_translations.sh
# （脚本自带 lupdate 探测——PATH/Qt 专有路径都找）。这里只挂一个手动 target：
#   ninja paleo_translations
# 不进默认构建（ translations 是翻译者动作，不是编译依赖）。
find_program(PALEO_LUPDATE NAMES lupdate lupdate-qt6
             PATHS /usr/lib/qt6/bin /usr/lib/qt6/libexec)
add_custom_target(paleo_translations
  COMMAND ${CMAKE_COMMAND} -E env bash
          ${CMAKE_SOURCE_DIR}/tools/update_translations.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "lupdate → translations/*.ts（骨架更新）"
  VERBATIM)
if(NOT PALEO_LUPDATE)
  message(STATUS "lupdate 不在常见路径——paleo_translations target 仍可用（脚本自行探测），构建不受影响")
endif()

add_paleo_test(tst_dockmanager LIBS paleo_ui)
