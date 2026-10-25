# cmake/verify-cpp-mainline.cmake — C++ 主线边界护栏（cmake -P 脚本，零 Python 依赖）
#
# 目标：C++ 是主程序与原生业务模块的唯一权威实现。本脚本机械化六条边界
# （详见 docs/CPP_PRIMARY_BOUNDARY.md），违规即非零退出：
#
#   src-python           R1 src/ 出现 Python 应用源码（*.py / *.pyw）。
#                        按扩展名判「应用源码进主线」，不按文件名含 python
#                        判违规——src/services/pythonenv.* 等合法 C++ 模块
#                        与本条无关；开发/测试期 Python 工具放 tools/、
#                        tests/、scripts/，不受本条约束。
#   archive-ref          R2 主线或构建面引用历史 Python 归档目录
#                        （archive/legacy-python）。src/ 源文件、根
#                        CMakeLists.txt、cmake/*.cmake 与安装/打包入口
#                        （paleo-dev、paleo-dev.ps1、tools/package_portable.py、
#                        vendor/bootstrap.sh、vendor/fetch-deps.sh）均不得
#                        出现该目录名——归档原件只允许被归档任务与文档提及。
#                        归档目录由并行任务负责，可能尚不存在：本检查是纯
#                        文本扫描，目录缺席时自然通过，不报错。
#   install-py           R3 构建配置把 .py 文件写进默认安装/打包清单
#                        （install(...) 行含 .py）。可选脚本功能经运行时
#                        解包（MAMCL zip）或用户自备解释器提供，不走安装面。
#   module-targets       R4 必需的 C++ 模块库/主程序 target 声明从 CMake
#                        里消失（误删注册而非有意重构）。
#   python-ext           R5 现有合法 Python 扩展（pythonenv / scriptrunner /
#                        pythonrepl / 控制台面板）的源文件、extra 挂点登记
#                        或测试注册失踪——防以「删模块」方式假纯化；源码
#                        闸门解释器发现（find_program(PALEO_PYTHON3)）同理。
#   superbuild-bindings  R6 vendor/superbuild 的 QGIS 构建失去
#                        WITH_BINDINGS=OFF——PyQGIS 运行时不得经 superbuild
#                        回流主程序依赖闭包。
#
# R4 的 target 针串按「add_library(<名> <空格>…」单行形态匹配、R5 的受保护
# 清单是现状锚点——有意重构（改声明排版/迁移受保护文件）时须同步更新下方
# 两个清单变量，闸门红是提醒而非障碍。
#
# 用法（源码根缺省取脚本所在目录的上一级，CI/ctest 显式传参双保险；
# 相对形态「.」按进程 CWD 解析，须在仓库根执行——CI Source gates 即如此）：
#   cmake -DCPP_MAINLINE_SOURCE_DIR=<repo> -P cmake/verify-cpp-mainline.cmake
#   cmake -DCPP_MAINLINE_SOURCE_DIR=<repo> -DCPP_MAINLINE_SELFTEST=ON \
#         -DCPP_MAINLINE_SELFTEST_DIR=<tmp> -P cmake/verify-cpp-mainline.cmake
#
# selftest 在合成夹具树上逐条打红/放行（含「归档目录不存在」「归档目录存在
# 但未被引用」两种容忍态），与仓库内 Python 闸门的 --selftest 同约定：
# 检查器自身坏掉不能静默变绿。

# 归档目录名运行期拼接：本文件自身在 cmake/ 扫描面内，落字面量会自匹配。
string(CONCAT _cpp_ml_archive_token "legacy" "-python")

set(_cpp_ml_required_targets
    paleo_domain paleo_algorithms paleo_store paleo_qgis paleo_io
    paleo_services paleo_linkage paleo_workflow paleo_ui paleo_app)

# R5 受保护注册清单：<文件相对路径> 必含的 <特征串>（文件缺席同样判红）。
set(_cpp_ml_protected_files
    "src/services/pythonenv.cpp"
    "src/services/pythonrepl.cpp"
    "src/services/scriptrunner.cpp"
    "src/services/scriptprotocol.cpp"
    "src/workflow/pythonconsolecontroller.cpp"
    "src/ui/python/pythonconsolepanel.cpp"
    "src/ui/python/pythonreplpanel.cpp")
set(_cpp_ml_protected_markers
    "CMakeLists.txt:extra-python-scripting.cmake"
    "cmake/extra-python-scripting.cmake:src/services/scriptrunner.cpp"
    "cmake/extra-python-scripting.cmake:src/services/pythonrepl.cpp"
    "cmake/extra-python-scripting.cmake:add_paleo_test(tst_scriptrunner"
    "cmake/extra-python-scripting.cmake:add_paleo_test(tst_pythonrepl"
    "cmake/extra-python-scripting.cmake:add_paleo_test(tst_pythonconsole"
    "cmake/extra-mamcl.cmake:src/services/pythonenv.cpp"
    "cmake/extra-mamcl.cmake:add_paleo_test(tst_pythonenv"
    "cmake/paleo-test-infra.cmake:find_program(PALEO_PYTHON3")

# 读取 <root>/<rel> 到 out_var；<rel> 为绝对路径时按原样读。
# 文件缺失返回空串（扫描面按存在与否容忍）。
function(_cpp_ml_read root rel out_var)
  if(IS_ABSOLUTE "${rel}")
    set(_p "${rel}")
  else()
    set(_p "${root}/${rel}")
  endif()
  if(EXISTS "${_p}")
    file(READ "${_p}" _content)
  else()
    set(_content "")
  endif()
  set(${out_var} "${_content}" PARENT_SCOPE)
endfunction()

# ---- R1 src/ 零 Python 应用源码 ----
function(_cpp_ml_check_src_python root)
  file(GLOB_RECURSE _py RELATIVE "${root}"
       "${root}/src/*.py" "${root}/src/*.pyw")
  foreach(_f IN LISTS _py)
    list(APPEND _v "src-python: ${_f}——Python 应用源码不得进入 C++ 主线（工具/夹具放 tools/ tests/ scripts/）")
  endforeach()
  set(_violations "${_v}" PARENT_SCOPE)
endfunction()

# ---- R2 主线/构建面不引用归档目录 ----
function(_cpp_ml_check_archive_refs root)
  set(_v "")
  file(GLOB_RECURSE _src_texts RELATIVE "${root}"
       "${root}/src/*.cpp" "${root}/src/*.h" "${root}/src/*.hpp"
       "${root}/src/*.ui" "${root}/src/*.qml" "${root}/src/*.qrc")
  foreach(_f IN LISTS _src_texts)
    _cpp_ml_read("${root}" "${_f}" _content)
    string(FIND "${_content}" "${_cpp_ml_archive_token}" _pos)
    if(NOT _pos LESS 0)
      list(APPEND _v "archive-ref: ${_f} 引用了历史 Python 归档目录（归档说明放 docs/，主线代码零指针）")
    endif()
  endforeach()
  # 构建面：CMake + 安装/打包入口（跳过缺失文件与本脚本自身）。
  file(GLOB _cmake_files "${root}/CMakeLists.txt" "${root}/cmake/*.cmake")
  foreach(_f IN LISTS _cmake_files
                   ITEMS
                   "${root}/paleo-dev" "${root}/paleo-dev.ps1"
                   "${root}/tools/package_portable.py"
                   "${root}/vendor/bootstrap.sh" "${root}/vendor/fetch-deps.sh")
    if(NOT EXISTS "${_f}")
      continue()
    endif()
    if(_f STREQUAL "${_cpp_ml_self}")
      continue()
    endif()
    file(READ "${_f}" _content)
    string(FIND "${_content}" "${_cpp_ml_archive_token}" _pos)
    if(NOT _pos LESS 0)
      file(RELATIVE_PATH _rel "${root}" "${_f}")
      list(APPEND _v "archive-ref: ${_rel} 引用了历史 Python 归档目录——构建/安装/打包面不得指向归档原件")
    endif()
  endforeach()
  set(_violations "${_v}" PARENT_SCOPE)
endfunction()

# ---- R3 install(...) 不得装 .py（按行匹配 + 折叠换行兜底跨行参数）----
function(_cpp_ml_check_install_py root)
  set(_v "")
  file(GLOB _cmake_files "${root}/CMakeLists.txt" "${root}/cmake/*.cmake")
  foreach(_f IN LISTS _cmake_files)
    if(_f STREQUAL "${_cpp_ml_self}")
      # 本脚本头注/用例里的规则示例文本会自匹配——跳过。
      continue()
    endif()
    _cpp_ml_read_path("${_f}" "${root}" _content _rel)
    # 先剥 # 注释行：注释里的 install(…示例文本不算安装清单（防未来误报）。
    string(REGEX REPLACE "(^|\n)[ \t]*#[^\n]*" "\n" _stripped "${_content}")
    set(_hit "")
    string(REGEX MATCH "install\\([^\n]*\\.py[\") ]" _hit "${_stripped}")
    if(_hit STREQUAL "")
      # install() 参数跨行时单行匹配会漏——折叠换行后放宽到非括号段再查。
      string(REPLACE "\n" " " _folded "${_stripped}")
      string(REGEX MATCH "install\\([^)]*\\.py[\") ]" _hit "${_folded}")
    endif()
    if(NOT _hit STREQUAL "")
      list(APPEND _v "install-py: ${_rel} 的安装清单包含 .py（${_hit}）——可选脚本功能不经默认安装面分发")
    endif()
  endforeach()
  set(_violations "${_v}" PARENT_SCOPE)
endfunction()

# ---- R4 必需 C++ 模块 target 仍在构建注册 ----
function(_cpp_ml_check_module_targets root)
  set(_v "")
  set(_cmake_blob "")
  file(GLOB _cmake_files "${root}/CMakeLists.txt" "${root}/cmake/*.cmake")
  foreach(_f IN LISTS _cmake_files)
    _cpp_ml_read("${root}" "${_f}" _content)
    string(APPEND _cmake_blob "\n${_content}")
  endforeach()
  foreach(_t IN LISTS _cpp_ml_required_targets)
    string(FIND "${_cmake_blob}" "add_library(${_t} " _pos)
    if(_pos LESS 0)
      list(APPEND _v "module-targets: add_library(${_t} ...) 声明失踪——C++ 主线模块被误移出构建")
    endif()
  endforeach()
  string(FIND "${_cmake_blob}" "add_executable(paleo " _pos)
  if(_pos LESS 0)
    list(APPEND _v "module-targets: add_executable(paleo ...) 主程序声明失踪")
  endif()
  set(_violations "${_v}" PARENT_SCOPE)
endfunction()

# ---- R5 合法 Python 扩展与闸门工具链保持注册 ----
function(_cpp_ml_check_python_ext root)
  set(_v "")
  foreach(_f IN LISTS _cpp_ml_protected_files)
    if(NOT EXISTS "${root}/${_f}")
      list(APPEND _v "python-ext: ${_f} 失踪——可选 Python 脚本功能（C++ 实现）被删除")
    endif()
  endforeach()
  foreach(_m IN LISTS _cpp_ml_protected_markers)
    string(REPLACE ":" ";" _pair "${_m}")
    list(GET _pair 0 _file)
    list(GET _pair 1 _marker)
    _cpp_ml_read("${root}" "${_file}" _content)
    string(FIND "${_content}" "${_marker}" _pos)
    if(_pos LESS 0)
      list(APPEND _v "python-ext: ${_file} 不再包含「${_marker}」——合法 Python 扩展/闸门的构建登记被拆除")
    endif()
  endforeach()
  set(_violations "${_v}" PARENT_SCOPE)
endfunction()

# ---- R6 superbuild 不带 Python 绑定 ----
function(_cpp_ml_check_superbuild root)
  set(_v "")
  _cpp_ml_read("${root}" "vendor/superbuild/CMakeLists.txt" _content)
  string(FIND "${_content}" "WITH_BINDINGS=OFF" _pos)
  if(_pos LESS 0)
    list(APPEND _v "superbuild-bindings: vendor/superbuild/CMakeLists.txt 失去 WITH_BINDINGS=OFF——PyQGIS 运行时不得回流依赖闭包")
  endif()
  set(_violations "${_v}" PARENT_SCOPE)
endfunction()

function(_cpp_ml_run_all root out_var)
  set(_all "")
  foreach(_fn IN ITEMS
           _cpp_ml_check_src_python _cpp_ml_check_archive_refs
           _cpp_ml_check_install_py _cpp_ml_check_module_targets
           _cpp_ml_check_python_ext _cpp_ml_check_superbuild)
    cmake_language(CALL ${_fn} "${root}")
    list(APPEND _all ${_violations})
  endforeach()
  set(${out_var} "${_all}" PARENT_SCOPE)
endfunction()

# 供 install 扫描的带路径读取（abs 路径入、相对路径出）。
function(_cpp_ml_read_path abs root content_var rel_var)
  file(RELATIVE_PATH _rel "${root}" "${abs}")
  _cpp_ml_read("${root}" "${_rel}" _content)
  set(${content_var} "${_content}" PARENT_SCOPE)
  set(${rel_var} "${_rel}" PARENT_SCOPE)
endfunction()

# ============================ 入口 ============================

if(NOT DEFINED CPP_MAINLINE_SOURCE_DIR)
  cmake_path(SET CPP_MAINLINE_SOURCE_DIR NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
# 入口统一规范化为绝对真实路径：相对根（CI 的 "." 形态，按进程 CWD 解析）
# 或带尾斜杠的根会让 file(GLOB) 产物内嵌 "./" 或 "//" 分量，与脚本路径的
# 字面比较失配——本脚本头注含规则示例文本，自跳（R2/R3 跳过自身）必须对
# 任何调用形态可靠。
file(REAL_PATH "${CPP_MAINLINE_SOURCE_DIR}" CPP_MAINLINE_SOURCE_DIR)
file(REAL_PATH "${CMAKE_CURRENT_LIST_FILE}" _cpp_ml_self)

if(CPP_MAINLINE_SELFTEST)
  if(NOT DEFINED CPP_MAINLINE_SELFTEST_DIR)
    set(CPP_MAINLINE_SELFTEST_DIR "${CPP_MAINLINE_SOURCE_DIR}/build/cpp-mainline-selftest")
  endif()

  # 写一棵「干净主线」夹具树（再按用例做单点突变）。
  function(_cpp_ml_fixture root)
    # 注意不建 archive/ 目录——用例①要在「归档目录不存在」的树上验证容忍。
    file(MAKE_DIRECTORY "${root}/src/app" "${root}/src/services"
         "${root}/src/workflow" "${root}/src/ui/python"
         "${root}/cmake" "${root}/vendor/superbuild")
    file(WRITE "${root}/src/app/main.cpp" "int main() { return 0; }\n")
    foreach(_f pythonenv pythonrepl scriptrunner scriptprotocol)
      file(WRITE "${root}/src/services/${_f}.cpp" "// optional python extension (C++)\n")
    endforeach()
    file(WRITE "${root}/src/workflow/pythonconsolecontroller.cpp" "// controller\n")
    file(WRITE "${root}/src/ui/python/pythonconsolepanel.cpp" "// panel\n")
    file(WRITE "${root}/src/ui/python/pythonreplpanel.cpp" "// panel\n")
    file(WRITE "${root}/CMakeLists.txt"
"# fixture root\ninclude(cmake/paleo-modules-data.cmake)\n\
include(cmake/paleo-modules-ui.cmake)\n\
set(_paleo_extra_manifest cmake/extra-python-scripting.cmake)\n")
    file(WRITE "${root}/cmake/paleo-modules-data.cmake"
"add_library(paleo_domain STATIC src/d.cpp)\n\
add_library(paleo_algorithms STATIC src/a.cpp)\n\
add_library(paleo_store STATIC src/s.cpp)\n\
add_library(paleo_qgis STATIC src/q.cpp)\n\
add_library(paleo_io STATIC src/i.cpp)\n\
add_library(paleo_services STATIC src/v.cpp)\n")
    file(WRITE "${root}/cmake/paleo-modules-ui.cmake"
"add_library(paleo_linkage STATIC src/l.cpp)\n\
add_library(paleo_workflow STATIC src/w.cpp)\n\
add_library(paleo_ui STATIC src/u.cpp)\n\
add_library(paleo_app STATIC src/app/main.cpp)\n")
    file(WRITE "${root}/cmake/paleo-tests.cmake"
"add_executable(paleo src/app/main.cpp)\n")
    file(WRITE "${root}/cmake/paleo-test-infra.cmake"
"find_program(PALEO_PYTHON3 NAMES python3 python)\n")
    file(WRITE "${root}/cmake/extra-python-scripting.cmake"
"target_sources(paleo_services PRIVATE src/services/scriptrunner.cpp src/services/pythonrepl.cpp)\n\
add_paleo_test(tst_scriptrunner)\n\
add_paleo_test(tst_scriptprotocol)\n\
add_paleo_test(tst_pythonrepl)\n\
add_paleo_test(tst_pythonconsole)\n")
    file(WRITE "${root}/cmake/extra-mamcl.cmake"
"target_sources(paleo_services PRIVATE src/services/pythonenv.cpp)\n\
add_paleo_test(tst_pythonenv)\n")
    file(WRITE "${root}/cmake/extra-install.cmake"
"install(TARGETS paleo RUNTIME DESTINATION bin)\n")
    file(WRITE "${root}/vendor/superbuild/CMakeLists.txt"
"ExternalProject_Add(qgis ... -DWITH_BINDINGS=OFF)\n")
  endfunction()

  # 单用例：expected_rule 为空 = 应绿；否则违规里必须含该 rule 前缀。
  function(_cpp_ml_case name dir expected_rule)
    _cpp_ml_run_all("${dir}" _v)
    set(_ok FALSE)
    if("${expected_rule}" STREQUAL "")
      if("${_v}" STREQUAL "")
        set(_ok TRUE)
      endif()
    else()
      foreach(_x IN LISTS _v)
        string(FIND "${_x}" "${expected_rule}:" _p)
        if(NOT _p LESS 0)
          set(_ok TRUE)
        endif()
      endforeach()
    endif()
    if(_ok)
      message(STATUS "cpp-mainline selftest [pass] ${name}")
    else()
      if("${expected_rule}" STREQUAL "")
        set(_want "绿（零违规）")
      else()
        set(_want "违规 ${expected_rule}")
      endif()
      message(STATUS "cpp-mainline selftest [FAIL] ${name}：期望 ${_want}，实际违规：${_v}")
      set_property(GLOBAL APPEND PROPERTY _cpp_ml_st_fail "${name}")
    endif()
  endfunction()

  set(_d "${CPP_MAINLINE_SELFTEST_DIR}")
  file(REMOVE_RECURSE "${_d}")
  # 先初始化为空：未设 property 经 get_property 读回 <var>-NOTFOUND 而非空串。
  set_property(GLOBAL PROPERTY _cpp_ml_st_fail "")

  # ① 干净树（归档目录不存在）应绿。
  _cpp_ml_fixture("${_d}/clean")
  _cpp_ml_case("clean-no-archive" "${_d}/clean" "")

  # ② 归档目录存在但未被引用，应绿（容忍并行任务的归档落地）。
  _cpp_ml_fixture("${_d}/archive-ok")
  file(WRITE "${_d}/archive-ok/archive/legacy-python/old_gui.py" "# archived original\n")
  _cpp_ml_case("archive-present-unreferenced" "${_d}/archive-ok" "")

  # ③ src/ 出现 .py → src-python 红。
  _cpp_ml_fixture("${_d}/src-py")
  file(WRITE "${_d}/src-py/src/ui/legacy_gui.py" "import qgis\n")
  _cpp_ml_case("src-python-file" "${_d}/src-py" "src-python")

  # ④ src 源码引用归档目录 → archive-ref 红。
  _cpp_ml_fixture("${_d}/src-ref")
  file(APPEND "${_d}/src-ref/src/app/main.cpp" "// see archive/legacy-python/foo.py\n")
  _cpp_ml_case("src-references-archive" "${_d}/src-ref" "archive-ref")

  # ⑤ 构建面引用归档目录 → archive-ref 红。
  _cpp_ml_fixture("${_d}/cmake-ref")
  file(APPEND "${_d}/cmake-ref/cmake/extra-install.cmake"
       "install(FILES archive/${_cpp_ml_archive_token}/x.py DESTINATION share)\n")
  _cpp_ml_case("cmake-references-archive" "${_d}/cmake-ref" "archive-ref")

  # ⑥ install 清单装 .py → install-py 红（与归档无关的 .py 同样拦）。
  _cpp_ml_fixture("${_d}/install-py")
  file(APPEND "${_d}/install-py/cmake/extra-install.cmake"
       "install(FILES tools/dev_helper.py DESTINATION bin)\n")
  _cpp_ml_case("install-includes-py" "${_d}/install-py" "install-py")

  # ⑥b install 参数跨行的 .py → 折叠换行兜底同样要红。
  _cpp_ml_fixture("${_d}/install-py-ml")
  file(APPEND "${_d}/install-py-ml/cmake/extra-install.cmake"
       "install(FILES\n  tools/dev_helper.py\n  DESTINATION bin)\n")
  _cpp_ml_case("install-includes-py-multiline" "${_d}/install-py-ml" "install-py")

  # ⑦ 必需模块声明被删 → module-targets 红。
  _cpp_ml_fixture("${_d}/no-services")
  file(READ "${_d}/no-services/cmake/paleo-modules-data.cmake" _c)
  string(REPLACE "add_library(paleo_services STATIC src/v.cpp)" "" _c "${_c}")
  file(WRITE "${_d}/no-services/cmake/paleo-modules-data.cmake" "${_c}")
  _cpp_ml_case("module-target-removed" "${_d}/no-services" "module-targets")

  # ⑧ 合法 Python 扩展测试注册被删 → python-ext 红。
  _cpp_ml_fixture("${_d}/no-ext")
  file(READ "${_d}/no-ext/cmake/extra-python-scripting.cmake" _c)
  string(REPLACE "add_paleo_test(tst_pythonrepl)" "" _c "${_c}")
  file(WRITE "${_d}/no-ext/cmake/extra-python-scripting.cmake" "${_c}")
  _cpp_ml_case("python-ext-unregistered" "${_d}/no-ext" "python-ext")

  # ⑨ 闸门解释器发现被删 → python-ext 红。
  _cpp_ml_fixture("${_d}/no-py3")
  file(WRITE "${_d}/no-py3/cmake/paleo-test-infra.cmake" "# gates gone\n")
  _cpp_ml_case("gate-interpreter-gone" "${_d}/no-py3" "python-ext")

  # ⑩ superbuild 绑定打开 → superbuild-bindings 红。
  _cpp_ml_fixture("${_d}/bindings")
  file(WRITE "${_d}/bindings/vendor/superbuild/CMakeLists.txt"
       "ExternalProject_Add(qgis ... -DWITH_BINDINGS=ON)\n")
  _cpp_ml_case("superbuild-bindings-on" "${_d}/bindings" "superbuild-bindings")

  get_property(_fails GLOBAL PROPERTY _cpp_ml_st_fail)
  if(_fails STREQUAL "")
    message(STATUS "cpp-mainline selftest: 11/11 通过")
    return()
  endif()
  message(FATAL_ERROR "cpp-mainline selftest 失败用例：${_fails}")
endif()

_cpp_ml_run_all("${CPP_MAINLINE_SOURCE_DIR}" _violations)
if("${_violations}" STREQUAL "")
  message(STATUS "cpp-mainline: OK（src/ 零 Python 应用源码；构建/安装面零归档引用；模块/扩展注册齐全）")
  return()
endif()
list(LENGTH _violations _n)
message(STATUS "cpp-mainline: ${_n} 项违规：")
foreach(_x IN LISTS _violations)
  message(STATUS "  - ${_x}")
endforeach()
message(FATAL_ERROR "C++ 主线边界检查未通过（规则见 cmake/verify-cpp-mainline.cmake 头注与 docs/CPP_PRIMARY_BOUNDARY.md）")
