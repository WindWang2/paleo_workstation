cmake_minimum_required(VERSION 3.28)
project(paleo_workbench C CXX)

set(CMAKE_CXX_STANDARD 20) # QGIS 4.2 headers require C++20 (defaulted operator==)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 本仓无 C++20 named modules；CMake≥3.28+Ninja 的默认扫描会往每条编译命令
# 塞 -fdeps-format/-fmodule-mapper（GCC 专属 flag，clang-tidy 不认，见
# tools/check_tidy.py）还拖慢编译——显式关掉。
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON) # wave3/ux-consistency: vendored fonts qrc
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# windows.h defines max as a function-like macro; it rewrites std::max inside
# QGIS headers (C2589 on MSVC) whenever a Windows header reaches a TU first.
if(MSVC)
  add_compile_definitions(NOMINMAX)
  add_compile_options(/utf-8 /W4)
else()
  # #134：MSVC 把花括号窄化（int→double 等）当硬错误 C2397，GCC/Clang 默认
  # 只告警——Linux 绿、Windows 红。这里升为错误，让 Linux CI 提前暴露。
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror=narrowing>)
endif()

include("${CMAKE_SOURCE_DIR}/cmake/paleo-build-options.cmake")

include("${CMAKE_SOURCE_DIR}/cmake/paleo-dependencies.cmake")

include("${CMAKE_SOURCE_DIR}/cmake/paleo-modules-data.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/paleo-modules-ui.cmake")

enable_testing()

include("${CMAKE_SOURCE_DIR}/cmake/paleo-test-infra.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/paleo-tests.cmake")

# ---- 并行开发挂点 ----
# 各并行方向的新增源文件/测试一律放 cmake/extra-<方向>.cmake，内容为：
#   target_sources(paleo_<模块> PRIVATE src/.../foo.cpp)
#   add_paleo_test(tst_foo)
# 禁止直接改本文件的模块源列表——并行分支在此汇合零冲突。
# 显式清单登记替代隐式 GLOB，根治构建目录漂移与测试漏注（P1-14 / TEST-01）
set(_paleo_extra_manifest
  "${CMAKE_SOURCE_DIR}/cmake/extra-ai-chat.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-batch.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-well-facies.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-catalog-sqlite.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-crossplot-facies.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-data.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-data-asset-deepening.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-a.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-b.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-c.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-deepen-d.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-derivation-graph.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-evolution.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-facies-automapping.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-faultinterp.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-fault-surface.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-geostat.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-install.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-io-perf.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-mamcl.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-mapping.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-petrophys.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-property-modeling.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-prediction-facies.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-python-scripting.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-qgis-ui-deepening.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-realization.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-section-split.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-sections-alignment.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-seismic.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-seismic-inversion.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-shell-lifecycle.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-sf-kriging.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-sf-completion.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-sf-deep.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-shortcuts-help.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-surfaceops.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-time-depth.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-ux.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-well-logset.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-well-trajectory.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-wellcomposite.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-welltops.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-wellsection.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-test-deepening.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-xplot-sup-supervised.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-xplot-sup-em.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-xplot-sup-som.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-xplot-sup-service.cmake"
  "${CMAKE_SOURCE_DIR}/cmake/extra-xplot-sup-ui.cmake"
)
foreach(_f IN LISTS _paleo_extra_manifest)
  if(EXISTS "${_f}")
    include("${_f}")
  endif()
endforeach()
# 防漏登记对账：磁盘上的 extra-*.cmake 必须全部在清单内（显式清单防的是
# 构建目录漂移，不是删文件——漏登记的模块会静默丢源文件/测试，曾致
# mincurvature.cpp 未编译 → vtable 链接失败）。
file(GLOB _paleo_extra_ondisk CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/cmake/extra-*.cmake")
foreach(_f IN LISTS _paleo_extra_ondisk)
  if(NOT _f IN_LIST _paleo_extra_manifest)
    # #420：$(notdir ...) 是 Make 语法，在 message 里会原样输出——
    # 用 get_filename_component 取文件名。
    get_filename_component(_fname "${_f}" NAME)
    message(FATAL_ERROR "cmake/${_fname} 未登记进 _paleo_extra_manifest——"
      "请补进上面的显式清单（防漂移约定不变：include 仍只走清单）")
  endif()
endforeach()

include("${CMAKE_SOURCE_DIR}/cmake/paleo-test-labels.cmake")
