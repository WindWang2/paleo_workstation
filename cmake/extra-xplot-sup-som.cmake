# 方向46 交会有监督分类：SOM 自组织图（第二无监督族）。
# 归口 agent-C；并行方向惯例——只在本文件追加 target_sources/add_paleo_test。
target_sources(paleo_algorithms PRIVATE src/algorithms/cluster/som.cpp)
add_paleo_test(tst_som LIBS paleo_algorithms)
