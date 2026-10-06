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

# ---- i18n 编译与资源打包（lrelease + .qm + Qt resource）----
# 目标：编译 translations/paleo_zh_CN.ts -> paleo_zh_CN.qm 并打包进 :/i18n/paleo_zh_CN.qm
find_package(Qt6 QUIET COMPONENTS LinguistTools)
if(TARGET Qt6::lrelease)
  set(PALEO_LRELEASE_EXECUTABLE Qt6::lrelease)
else()
  find_program(PALEO_LRELEASE NAMES lrelease lrelease-qt6 lrelease6
               PATHS /usr/lib/qt6/bin /usr/lib/qt6/libexec /usr/bin)
  if(PALEO_LRELEASE)
    set(PALEO_LRELEASE_EXECUTABLE "${PALEO_LRELEASE}")
  endif()
endif()

if(PALEO_LRELEASE_EXECUTABLE)
  set(PALEO_ZH_CN_TS "${CMAKE_SOURCE_DIR}/translations/paleo_zh_CN.ts")
  set(PALEO_ZH_CN_QM "${CMAKE_CURRENT_BINARY_DIR}/translations/paleo_zh_CN.qm")

  add_custom_command(
    OUTPUT "${PALEO_ZH_CN_QM}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/translations"
    COMMAND ${PALEO_LRELEASE_EXECUTABLE} "${PALEO_ZH_CN_TS}" -qm "${PALEO_ZH_CN_QM}"
    DEPENDS "${PALEO_ZH_CN_TS}"
    COMMENT "Compiling translations/paleo_zh_CN.ts -> paleo_zh_CN.qm via lrelease"
    VERBATIM
  )

  # 显式构建目标：
  #   ninja paleo_lrelease   - 编译翻译 .qm
  #   ninja translations     - 标准别名（与 QGIS/Qt 生态习惯对齐）
  # 挂 ALL 保证 cmake --build build 默认构建时自动产出 .qm 无需手动步骤
  add_custom_target(paleo_lrelease ALL DEPENDS "${PALEO_ZH_CN_QM}")
  add_custom_target(translations DEPENDS paleo_lrelease)
  add_custom_target(paleo_qm DEPENDS paleo_lrelease)

  # 打包进 Qt resource 嵌入 paleo_ui（提供 :/i18n/paleo_zh_CN.qm）
  set_source_files_properties("${PALEO_ZH_CN_QM}" PROPERTIES
    GENERATED TRUE
    QT_RESOURCE_ALIAS "paleo_zh_CN.qm"
  )
  qt6_add_resources(paleo_ui "translations"
    PREFIX "/i18n"
    FILES "${PALEO_ZH_CN_QM}"
  )
else()
  message(WARNING "lrelease 未找到（请安装 qt6-tools-dev / qttools）——无法编译 translations/*.qm")
endif()

add_paleo_test(tst_dockmanager LIBS paleo_ui)

# ---- l10n 门禁与变异自检（Milestone 5 / R5）----
if(NOT Python3_EXECUTABLE)
  if(PALEO_PYTHON3)
    set(Python3_EXECUTABLE "${PALEO_PYTHON3}")
  else()
    find_program(Python3_EXECUTABLE NAMES python3 python)
  endif()
endif()

if(Python3_EXECUTABLE)
  add_test(NAME l10n COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_l10n.py)
  add_test(NAME l10n_selftest COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_l10n.py --selftest)
  set_tests_properties(l10n l10n_selftest
                       PROPERTIES LABELS "l10n"
                                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

