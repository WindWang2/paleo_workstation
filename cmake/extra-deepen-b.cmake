# cmake/extra-deepen-b.cmake — wave/deepen-perf Track B（IO/缓存/目录性能）
# 新增源文件/测试的挂载点（见 CMakeLists.txt「并行开发挂点」段；禁止直接改
# 根 CMakeLists 的模块源列表）。

# ---- B1：correlation 大 LAS 异步化（quiet 任务 + 世代号回填）----
add_paleo_test(tst_correlation_async LIBS paleo_ui paleo_services paleo_io)

# ---- B2：导入队列真进度条（整体进度/ETA/取消钩子 + 生产 runner 适配器）----
add_paleo_test(tst_importqueue_progress LIBS paleo_ui paleo_services)

# ---- B3：栅格金字塔消费侧（Lazy 批量接线 + GDAL .ovr + 大图读块对照）----
add_paleo_test(tst_pyramid_consume LIBS paleo_ui)

# ---- B4：catalog.sqlite 触发条件实测（PALEO_CATALOG_SCALE 按需；默认 SKIP）----
add_paleo_test(tst_catalog_scale LIBS paleo_io paleo_store)
