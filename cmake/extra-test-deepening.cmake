# goal/test-deepening — 方向52：测试深化——零测试头清零 + dataChanged 增量通道
# 新增测试集中在此，根 CMakeLists.txt 保持最小挂点，并行分支零冲突。

# A 线：原子写与防损坏测试套件 + 零测试头清零套件
add_paleo_test(tst_metadata_atomicfile LIBS paleo_store)
add_paleo_test(tst_workflow_registration LIBS paleo_workflow)
add_paleo_test(tst_catalog_catalogindex LIBS paleo_store)
add_paleo_test(tst_domain_wellsitingplan LIBS paleo_domain)
add_paleo_test(tst_domain_faciesclassification LIBS paleo_domain)
add_paleo_test(tst_domain_importrows LIBS paleo_domain)
add_paleo_test(tst_domain_sectiontrace LIBS paleo_domain)
add_paleo_test(tst_io_laswriter LIBS paleo_io)
add_paleo_test(tst_io_inflight LIBS paleo_io)
add_paleo_test(tst_io_ziparchive_unit LIBS paleo_io)
add_paleo_test(tst_geostat_linsolve LIBS paleo_algorithms)
add_paleo_test(tst_geostat_neighborhood LIBS paleo_algorithms)
add_paleo_test(tst_inversion_volume LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_cartographicsmooth LIBS paleo_algorithms)
add_paleo_test(tst_singlefactor_structural_unit LIBS paleo_algorithms)
if(PALEO_HAVE_ORT)
  add_paleo_test(tst_ai_horizonsuggest LIBS paleo_ai)
endif()

# B 线：增量更新通道与状态保留测试套件
add_paleo_test(tst_dataops_incremental LIBS paleo_ui)
