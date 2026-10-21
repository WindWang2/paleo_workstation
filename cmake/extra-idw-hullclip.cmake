# perf/idw-hull — IDW 凸包裁剪 prepared GEOS 基准（paleo::algorithms::PreparedHull，实现在 paleoalgorithms.cpp）。
# 等价性功能断言在 tst_algorithms（preparedHullMatchesUnpreparedContains /
# constraintIdwHullClipPreparedIdentical）；这里只放墙钟对比，打 perf 标签
#（见 cmake/paleo-test-labels.cmake 的 _paleo_perf_tests）。
add_paleo_test(tst_idw_hullclip_perf LIBS paleo_algorithms paleo_ui_deps)
