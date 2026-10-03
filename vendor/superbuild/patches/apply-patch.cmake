# apply-patch.cmake — 给 ExternalProject_Add(... PATCH_COMMAND ...) 用的
# 幂等补丁脚本（仅编排不编译，见 vendor/superbuild/README.md「补丁」）。
#
# 用法：
#   cmake -DPATCH_SOURCE_DIR=<SOURCE_DIR> -DPATCH_FILE=<补丁路径>
#         -DMARKER_FILE=<相对路径> -DMARKER=<宏名>
#         -P apply-patch.cmake
#
# MARKER_FILE 里已出现 MARKER 即视为已打过补丁、直接跳过（ExternalProject
# 的 patch stamp 在重跑/换构建目录时不一定可信）；打不上则 FATAL_ERROR，
# 不留半补丁状态继续构建。

if(NOT DEFINED PATCH_SOURCE_DIR OR NOT DEFINED PATCH_FILE)
  message(FATAL_ERROR "apply-patch.cmake: PATCH_SOURCE_DIR 与 PATCH_FILE 必填")
endif()
if(NOT DEFINED MARKER_FILE OR NOT DEFINED MARKER)
  message(FATAL_ERROR "apply-patch.cmake: MARKER_FILE 与 MARKER 必填")
endif()

set(_marker_path "${PATCH_SOURCE_DIR}/${MARKER_FILE}")
if(EXISTS "${_marker_path}")
  file(READ "${_marker_path}" _marker_contents)
  if(_marker_contents MATCHES "${MARKER}")
    message(STATUS "patch already applied (${MARKER} present in ${MARKER_FILE}) — skipping")
    return()
  endif()
endif()

if(NOT EXISTS "${PATCH_FILE}")
  message(FATAL_ERROR "apply-patch.cmake: patch file not found: ${PATCH_FILE}")
endif()

find_program(_patch_exe NAMES patch)
if(NOT _patch_exe)
  message(FATAL_ERROR "apply-patch.cmake: 找不到 patch 程序，无法应用 ${PATCH_FILE}")
endif()

execute_process(
  COMMAND "${_patch_exe}" -p1 -i "${PATCH_FILE}"
  WORKING_DIRECTORY "${PATCH_SOURCE_DIR}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err
)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "apply-patch.cmake: 应用 ${PATCH_FILE} 失败 (exit ${_rc}):\n${_out}\n${_err}")
endif()
message(STATUS "applied ${PATCH_FILE}")
