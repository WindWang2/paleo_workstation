# Crossplot / unsupervised facies. Existing module targets preserve layering.
target_sources(paleo_algorithms PRIVATE src/algorithms/cluster/cluster.cpp)
add_paleo_test(tst_cluster LIBS paleo_algorithms)
