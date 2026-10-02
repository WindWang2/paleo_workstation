# goal/well-logset — 多文件井曲线并集读面。
target_sources(paleo_services PRIVATE src/services/welllogset.cpp)

add_paleo_test(tst_welllogset LIBS paleo_services)
add_paleo_test(tst_welllog_consumers LIBS paleo_workflow)
add_paleo_test(tst_welllog_ui LIBS paleo_ui)
