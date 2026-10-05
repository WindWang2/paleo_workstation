# goal/well-logset — 多文件井曲线并集读面。
target_sources(paleo_services PRIVATE src/services/welllogset.cpp)

add_paleo_test(tst_welllogset LIBS paleo_services)
add_paleo_test(tst_welllogset_perf LIBS paleo_services)
set_tests_properties(tst_welllogset_perf PROPERTIES RUN_SERIAL TRUE)
add_paleo_test(tst_welllog_consumers LIBS paleo_workflow)
add_paleo_test(tst_welllog_ui LIBS paleo_ui)
# 方向44：混格式并集 + 深度基准对齐（Oracle 2/5）
add_paleo_test(tst_welllog_multiformat LIBS paleo_workflow)
