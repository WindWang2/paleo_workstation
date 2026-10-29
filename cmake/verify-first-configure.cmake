# cmake/verify-first-configure.cmake — 新 worktree 首次 configure 行为护栏
#（cmake -P 脚本，零 Python 依赖，模式同 verify-cpp-mainline.cmake）
#
# 背景（TODOS P2「新 worktree 首 configure 漏 include」根因收口，
# goal/perf-maintenance-20261027）：历史上「首配漏 4 条 vendor include、
# 再跑同一条 configure 恢复」被写成操作手册；真正的接线缺口是显式
# QGIS_PREFIX 无效时静默回落系统栈（链接/运行资源栈分裂）。修复后语义：
#   依赖存在 → 一次 configure 生成正确目标与 include 路径；
#   依赖缺失 → configure 期 FATAL_ERROR，报文含缺失项与修复入口；
#   合法复用 → 同一命令重跑行为幂等，不留半生成状态。
#
# 用法（源码根缺省取脚本所在目录的上一级）：
#   cmake -DFIRSTCFG_SOURCE_DIR=<repo> -DFIRSTCFG_BINARY_DIR=<scratch> \
#         -P cmake/verify-first-configure.cmake
#
# 场景（每轮全新构建目录，不复制任何旧 cache/生成物）：
#   G1 依赖存在：合成 stub prefix（QGIS/GDAL/GEOS 头 + stub 库）→ 首配成功，
#      QGIS_INCLUDE_DIR 命中 stub（不静默换栈），compile_commands 含全部四条
#      vendor include（glm/saribbon/sbm/segyio——原症状的直接断言）。
#   G2 依赖缺失（显式 -D）：悬空 prefix → 非零退出 + 报文含路径与修复入口。
#   G3 幂等复用：同一目录重跑 G2 → 同样失败，且无 build.ninja 半生成残留
#      （防「第二轮魔法恢复」类状态依赖回归）。
#   G4 依赖缺失（env 路线）：QGIS_PREFIX_PATH 悬空 → 同 G2（覆盖 env→cache
#      写入路径的早失败）。
#
# 断言一律内联 if()：CMake 的 if() 不把 ${var} 展开值重新分词，把条件串
# 塞进宏参数会整串变成单个非布尔参数（NOT 恒真），故不使用条件宏。
#
# 仅注册在 UNIX 车道（见 paleo-test-infra.cmake）：stub 库文件名/路径分隔符
# 按 POSIX 写死，Windows 首配行为不在本护栏验收面内（如实记录，不假绿）。

cmake_minimum_required(VERSION 3.28)

if(NOT FIRSTCFG_SOURCE_DIR)
  get_filename_component(FIRSTCFG_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}" DIRECTORY)
endif()
if(NOT FIRSTCFG_BINARY_DIR)
  set(FIRSTCFG_BINARY_DIR "${FIRSTCFG_SOURCE_DIR}/build/first-configure-check")
endif()
get_filename_component(FIRSTCFG_SOURCE_DIR "${FIRSTCFG_SOURCE_DIR}" ABSOLUTE)
get_filename_component(FIRSTCFG_BINARY_DIR "${FIRSTCFG_BINARY_DIR}" ABSOLUTE)

set(_fc_fail "")
macro(_fc_report msg)
  string(APPEND _fc_fail "  断言失败: ${msg}\n")
endmacro()

# 每轮全新 scratch：残留的 CMakeCache 会把 G2 的失败变成「cache 命中」假象。
if(EXISTS "${FIRSTCFG_BINARY_DIR}")
  file(REMOVE_RECURSE "${FIRSTCFG_BINARY_DIR}")
endif()
file(MAKE_DIRECTORY "${FIRSTCFG_BINARY_DIR}")

set(_fc_stub "${FIRSTCFG_BINARY_DIR}/stub-prefix")
set(_fc_missing "${FIRSTCFG_BINARY_DIR}/missing-prefix")
set(_fc_missing2 "${FIRSTCFG_BINARY_DIR}/missing-prefix-2")

# ---- G1：合成 stub prefix（头 + 空 stub 库；find_* 只探存在性不验内容）----
file(WRITE "${_fc_stub}/include/qgsapplication.h" "/* stub */\n")
file(WRITE "${_fc_stub}/include/gdal.h" "/* stub */\n")
file(WRITE "${_fc_stub}/include/geos_c.h" "/* stub */\n")
foreach(_lib qgis_core qgis_gui qgis_analysis gdal geos_c)
  file(WRITE "${_fc_stub}/lib/lib${_lib}.so" "")
endforeach()

set(_fc_common_args -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo)

message(STATUS "G1: 全新目录首配（stub prefix 依赖存在）")
set(_fc_g1 "${FIRSTCFG_BINARY_DIR}/g1-build")
execute_process(
  COMMAND ${CMAKE_COMMAND} "-S${FIRSTCFG_SOURCE_DIR}" "-B${_fc_g1}"
          ${_fc_common_args} "-DQGIS_PREFIX=${_fc_stub}"
  RESULT_VARIABLE _fc_g1_rc
  OUTPUT_VARIABLE _fc_g1_out
  ERROR_VARIABLE _fc_g1_err
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT _fc_g1_rc EQUAL 0)
  _fc_report("G1 首配应成功，实际 rc=${_fc_g1_rc}：\n${_fc_g1_out}\n${_fc_g1_err}")
endif()

# QGIS_INCLUDE_DIR 必须命中 stub（显式 prefix 不静默换栈）。
file(STRINGS "${_fc_g1}/CMakeCache.txt" _fc_g1_line REGEX "^QGIS_INCLUDE_DIR:PATH=")
string(FIND "${_fc_g1_line}" "${_fc_stub}" _fc_g1_hit)
if(_fc_g1_hit EQUAL -1)
  _fc_report("G1 QGIS_INCLUDE_DIR 未命中 stub prefix（cache 行：${_fc_g1_line}）")
endif()

# 原症状直接断言：四条 vendor include 在首配生成的 compile_commands 里。
if(EXISTS "${_fc_g1}/compile_commands.json")
  file(READ "${_fc_g1}/compile_commands.json" _fc_cc)
  foreach(_pair "glm:vendor/glm:100" "sbm:vendor/sbm/src:100"
                "segyio:vendor/segyio/include:100" "saribbon:vendor/saribbon:10")
    string(REPLACE ":" ";" _pair_parts "${_pair}")
    list(GET _pair_parts 1 _fc_tok)
    list(GET _pair_parts 2 _fc_min)
    string(REGEX MATCHALL "${_fc_tok}" _fc_hits "${_fc_cc}")
    list(LENGTH _fc_hits _fc_n)
    if(_fc_n LESS _fc_min)
      _fc_report("G1 vendor include '${_fc_tok}' 仅 ${_fc_n} 条编译命令命中（阈值 ${_fc_min}）——首配漏 include 回归")
    endif()
  endforeach()
else()
  _fc_report("G1 未生成 compile_commands.json")
endif()

# ---- G2：显式悬空 prefix → configure 期报因早失败 ----
message(STATUS "G2: 悬空显式 prefix（依赖缺失）")
set(_fc_g2 "${FIRSTCFG_BINARY_DIR}/g2-build")
set(_fc_g2_cmd ${CMAKE_COMMAND} "-S${FIRSTCFG_SOURCE_DIR}" "-B${_fc_g2}"
               ${_fc_common_args} "-DQGIS_PREFIX=${_fc_missing}")
execute_process(COMMAND ${_fc_g2_cmd}
  RESULT_VARIABLE _fc_g2_rc
  OUTPUT_VARIABLE _fc_g2_out
  ERROR_VARIABLE _fc_g2_err
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(_fc_g2_rc EQUAL 0)
  _fc_report("G2 悬空显式 prefix 应早失败，实际 rc=0（静默回落/假绿）")
endif()
string(FIND "${_fc_g2_out}${_fc_g2_err}" "${_fc_missing}" _fc_g2_named)
if(_fc_g2_named EQUAL -1)
  _fc_report("G2 报文未点名缺失 prefix 路径")
endif()
string(FIND "${_fc_g2_out}${_fc_g2_err}" "BUILDING.md" _fc_g2_fix)
if(_fc_g2_fix EQUAL -1)
  _fc_report("G2 报文未给 BUILDING.md 修复入口")
endif()
string(FIND "${_fc_g2_out}${_fc_g2_err}" "bootstrap" _fc_g2_fix2)
if(_fc_g2_fix2 EQUAL -1)
  _fc_report("G2 报文未给 bootstrap 修复入口")
endif()

# ---- G3：同一命令重跑幂等，无半生成残留 ----
message(STATUS "G3: 同命令重跑（幂等复用）")
execute_process(COMMAND ${_fc_g2_cmd}
  RESULT_VARIABLE _fc_g3_rc
  OUTPUT_VARIABLE _fc_g3_out
  ERROR_VARIABLE _fc_g3_err
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(_fc_g3_rc EQUAL 0)
  _fc_report("G3 重跑应同样失败，实际 rc=0（状态依赖行为回归）")
endif()
string(FIND "${_fc_g3_out}${_fc_g3_err}" "${_fc_missing}" _fc_g3_named)
if(_fc_g3_named EQUAL -1)
  _fc_report("G3 重跑报文未点名缺失 prefix 路径")
endif()
if(EXISTS "${_fc_g2}/build.ninja")
  _fc_report("G3 悬空 prefix 的 configure 不应生成 build.ninja（旧缺陷下会整包静默生成错误栈的构建系统）")
endif()

# ---- G4：env 路线悬空 prefix（覆盖 env→cache 写入路径）----
message(STATUS "G4: 悬空 env prefix（依赖缺失，env 路线）")
set(_fc_g4 "${FIRSTCFG_BINARY_DIR}/g4-build")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "QGIS_PREFIX_PATH=${_fc_missing2}"
          ${CMAKE_COMMAND} "-S${FIRSTCFG_SOURCE_DIR}" "-B${_fc_g4}"
          ${_fc_common_args}
  RESULT_VARIABLE _fc_g4_rc
  OUTPUT_VARIABLE _fc_g4_out
  ERROR_VARIABLE _fc_g4_err
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
if(_fc_g4_rc EQUAL 0)
  _fc_report("G4 悬空 env prefix 应早失败，实际 rc=0")
endif()
string(FIND "${_fc_g4_out}${_fc_g4_err}" "${_fc_missing2}" _fc_g4_named)
if(_fc_g4_named EQUAL -1)
  _fc_report("G4 报文未点名缺失 env prefix 路径")
endif()

if(_fc_fail)
  message(FATAL_ERROR "verify-first-configure 未通过：\n${_fc_fail}")
endif()
message(STATUS "verify-first-configure: G1-G4 全绿（存在/缺失×2/幂等复用）")
