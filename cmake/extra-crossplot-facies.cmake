# Crossplot / unsupervised facies. Existing module targets preserve layering.
target_sources(paleo_algorithms PRIVATE src/algorithms/cluster/cluster.cpp)
add_paleo_test(tst_cluster LIBS paleo_algorithms)
target_sources(paleo_services PRIVATE src/services/crossplotsamples.cpp)
add_paleo_test(tst_crossplot_samples LIBS paleo_services)
target_sources(paleo_ui PRIVATE src/ui/crossplot/crossplotpanel.cpp)
add_paleo_test(tst_crossplotpanel LIBS paleo_ui)
