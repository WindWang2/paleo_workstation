# cmake/paleo-build-options.cmake — 编译选项档：ccache/sanitizer/LTO/unity 开关
# 由根 CMakeLists.txt 按顺序 include()（纯结构拆分，语义与拆分前逐行一致）。
# 注意：include() 不开新作用域，CMAKE_SOURCE_DIR 仍是仓库根。
# 文件名刻意不用 extra- 前缀——extra-*.cmake 受 _paleo_extra_manifest 防漏对账。

# ---- 编译缓存 launcher（devex T6）：ccache/sccache 任一在 PATH 即启用 ----
# 已显式给 launcher（-DCMAKE_CXX_COMPILER_LAUNCHER=...）或 PALEO_NO_CCACHE=1 时不抢。
if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER AND NOT PALEO_NO_CCACHE)
  find_program(PALEO_CCACHE_EXE NAMES ccache sccache)
  if(PALEO_CCACHE_EXE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${PALEO_CCACHE_EXE}")
    set(CMAKE_C_COMPILER_LAUNCHER "${PALEO_CCACHE_EXE}")
    message(STATUS "compile cache launcher: ${PALEO_CCACHE_EXE}")
  endif()
endif()

# ---- sanitizer 档（devex T7，实验性，默认 OFF；GNU/Clang 限定）----
# ASAN 档测试默认关 LSAN（QGIS/Qt 插件/单例的"泄漏"是设计内行为，见
# tools/lsan-suppressions.txt；猎泄漏时手动开 detect_leaks=1 + suppressions）。
# UBSAN 保持可恢复（只打印栈），做巡检不做门禁。
option(PALEO_ENABLE_ASAN "AddressSanitizer 实验档（测试默认关 leak 检测）" OFF)
option(PALEO_ENABLE_UBSAN "UndefinedBehaviorSanitizer 巡检档（可恢复）" OFF)
# ---- goal/perf-systematize 簇4：LTO 实验档（默认 OFF）----
# 依据（2026-10-01 本机交错三轮实测，docs/perf/BUILD_OPT.md）：LAS 冷解析
# -40.7%、SEG-Y 索引命中 -45.2%、catalog 灌库 1k -24.9%/查询 -18.2%，
# 无一致回归；代价 = 全量链接时间 ~2.3×（增量影响小）。-O3 已实验否决
# （catalog_open_10k +35% 两轮一致回归）不入档为选项。
option(PALEO_ENABLE_LTO "链接期优化档（-flto=8；热路径解析/索引收益 20-45%，默认 OFF）" OFF)
if(PALEO_ENABLE_LTO)
  if(MSVC)
    message(FATAL_ERROR "PALEO_ENABLE_LTO 仅支持 GNU/Clang（MSVC 走 /GL /LTCG 手动档）")
  endif()
  include(CheckCXXSourceCompiles)
  set(_lto_saved_cxx "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "-flto=8")
  check_cxx_source_compiles("int main(){return 0;}" PALEO_LTO_LINK_OK)
  set(CMAKE_REQUIRED_FLAGS "${_lto_saved_cxx}")
  if(NOT PALEO_LTO_LINK_OK)
    message(FATAL_ERROR "PALEO_ENABLE_LTO：工具链不支持 -flto=8（探测失败）")
  endif()
  add_compile_options(-flto=8 -ffat-lto-objects)
  add_link_options(-flto=8)
endif()
# ---- 实验性 unity build（devex T6）：逐 target opt-in，默认 OFF ----
# paleo_sbm（-fno-char8_t TU 语义）/paleo_segyio（纯 C）/paleo_saribbon（vendor）
# 显式排除；AUTOMOC 与 unity 的组合按 CMake 语义兼容，但跨 TU 静态同名符号
# 有合批冲突风险——只作本地加速实验，不进 CI 默认路径。
option(PALEO_UNITY_BUILD "实验性 unity build（vendor 目标显式排除）" OFF)
if(PALEO_ENABLE_ASAN OR PALEO_ENABLE_UBSAN)
  if(MSVC)
    message(FATAL_ERROR "sanitizer 档仅支持 GNU/Clang（MSVC 走 /fsanitize 手动档）")
  endif()
  add_compile_options(-fno-omit-frame-pointer -g)
  add_link_options(-fno-omit-frame-pointer)
  # 方向70：budget 类断言感知 sanitizer 档（墙钟预算 ×3、RSS 增长断言跳过，
  # 见 tests/perfbudget_relax.h 与 devex.md build-asan 实测口径）。非
  # sanitizer 构建不定义，测试行为零变化。
  add_compile_definitions(PALEO_SANITIZER_BUILD=1)
endif()
if(PALEO_ENABLE_ASAN)
  add_compile_options(-fsanitize=address)
  add_link_options(-fsanitize=address)
endif()
if(PALEO_ENABLE_UBSAN)
  # PW-030：UB 报告必须让测试失败（默认 recover 模式只打印、退出码 0）。
  add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=undefined -fno-sanitize-recover=undefined)
endif()
