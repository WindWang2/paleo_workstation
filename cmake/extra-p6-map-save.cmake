# P6 地图版本保存保护与空间编辑增量历史。
target_sources(paleo_workflow PRIVATE
  src/workflow/mapediteventhistory.cpp)
target_sources(paleo_ui PRIVATE
  src/ui/pages/mappingworkbenchpage_spatialhistory.cpp)
add_paleo_test(tst_p6_mapsave LIBS paleo_workflow paleo_ui)
