# 方向46 交会有监督分类：GMM 分块 EM（超内存预算自动分块）。
# cluster.cpp 本体已在 extra-crossplot-facies.cmake 登记，本文件只补测试。
add_paleo_test(tst_gmm_chunked LIBS paleo_algorithms)
set_tests_properties(tst_gmm_chunked PROPERTIES RUN_SERIAL TRUE)
