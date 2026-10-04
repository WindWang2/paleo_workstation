# 方向 36：版本衍生血缘图，服务取数 / 视图纯绘制。
target_sources(paleo_services PRIVATE src/services/derivationgraph.cpp)
target_sources(paleo_ui PRIVATE src/ui/pages/derivationgraph.cpp)
add_paleo_test(tst_derivationgraph LIBS paleo_ui paleo_io)
