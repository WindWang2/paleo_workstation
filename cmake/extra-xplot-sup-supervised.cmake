# 方向46 交会有监督分类：监督分类核（LDA/QDA/kNN + 交叉验证）。
# 归口 agent-A；并行方向惯例——只在本文件追加 target_sources/add_paleo_test。
target_sources(paleo_algorithms PRIVATE src/algorithms/cluster/supervised.cpp)
add_paleo_test(tst_supervised LIBS paleo_algorithms)
