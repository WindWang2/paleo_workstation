# wave/io-perf-cache (P4) — IO/服务层性能与缓存体系。
# 根 CMakeLists 的模块源列表不动（并行开发纪律）：新源/测试全部挂这里。
#
# 布线：
#   paleo_io        + cachecore/cachebudget/lrucache/lascache/lasalias/
#                    inflight/encodingdetect/partialread/pathcanon/streaming/
#                    shacache/perffixtures/benchreport/segyindexstore/rasterpyramid
#   paleo_store     + catalog/catalogindex
#   paleo_selfcheck + perfgroup（perf 组，QCoreApplication 即可）
#   zstd 可用则 paleo_io 链 zstd（D2.4/D1 压缩；缺失降级未压缩路径）

find_library(PALEO_IOPERF_ZSTD_LIB NAMES zstd)
find_path(PALEO_IOPERF_ZSTD_INCLUDE NAMES zstd.h)
if(PALEO_IOPERF_ZSTD_LIB AND PALEO_IOPERF_ZSTD_INCLUDE)
  message(STATUS "io-perf: zstd found — cache payloads compressed  (${PALEO_IOPERF_ZSTD_LIB})")
  target_compile_definitions(paleo_io PUBLIC PALEO_HAVE_ZSTD)
  target_include_directories(paleo_io PRIVATE ${PALEO_IOPERF_ZSTD_INCLUDE})
  target_link_libraries(paleo_io PRIVATE ${PALEO_IOPERF_ZSTD_LIB})
else()
  message(STATUS "io-perf: zstd not found — caches fall back to uncompressed payload")
endif()

target_sources(paleo_io PRIVATE
  src/io/cachecore.h
  src/io/cachecore.cpp
  src/io/cachebudget.h
  src/io/cachebudget.cpp
  src/io/lrucache.h
  src/io/inflight.h
  src/io/lascache.h
  src/io/lascache.cpp
  src/io/lasalias.h
  src/io/lasalias.cpp
  src/io/encodingdetect.h
  src/io/encodingdetect.cpp
  src/io/pathcanon.h
  src/io/partialread.h
  src/io/partialread.cpp
  src/io/streaming.h
  src/io/streaming.cpp
  src/io/shacache.h
  src/io/shacache.cpp
  src/io/segyindexstore.h
  src/io/segyindexstore.cpp
  src/io/rasterpyramid.h
  src/io/rasterpyramid.cpp
  src/io/perffixtures.h
  src/io/perffixtures.cpp
  src/io/benchreport.h
  src/io/benchreport.cpp
)

target_sources(paleo_store PRIVATE
  src/catalog/catalogindex.h
  src/catalog/catalogindex.cpp
)

target_sources(paleo_selfcheck PRIVATE
  src/selfcheck/perfgroup.h
  src/selfcheck/perfgroup.cpp
)
target_link_libraries(paleo_selfcheck PRIVATE paleo_io paleo_store)

# ---- 测试（命名匹配 ctest -R 'perf|cache|catalog|import|index|layering'）----
add_paleo_test(tst_cache_core LIBS paleo_io)
add_paleo_test(tst_cache_las LIBS paleo_io)
add_paleo_test(tst_perf_las LIBS paleo_io)
add_paleo_test(tst_cache_segyindex LIBS paleo_io)
add_paleo_test(tst_perf_segyindex LIBS paleo_io)
add_paleo_test(tst_cache_pyramid LIBS paleo_io)
add_paleo_test(tst_perf_catalog LIBS paleo_io)
add_paleo_test(tst_cache_async LIBS paleo_services)
add_paleo_test(tst_cache_io LIBS paleo_io)
add_paleo_test(tst_perf_regress LIBS paleo_io paleo_store)
