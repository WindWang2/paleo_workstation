# goal/wellsection — 连井剖面（well correlation）：domain 数据核 + workflow
# 取数编排 + ui 图件面板/壳层装配。
target_sources(paleo_domain PRIVATE src/domain/wellsection.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/wellsectionworkflow.cpp)
target_sources(paleo_store PRIVATE src/metadata/wellsectionstore.cpp)
target_sources(paleo_qgis PRIVATE src/qgis/wellsectionmapband.cpp)
target_link_libraries(paleo_store PUBLIC paleo_domain)
target_sources(paleo_ui PRIVATE
  src/ui/wellsection/wellsectionstyle.cpp
  src/ui/wellsection/wellsectionscene.cpp
  src/ui/wellsection/wellsectionpanel.cpp
  src/ui/wellsection/wellsectiondialogs.cpp
  src/ui/wellsection/fencewidget.cpp
  src/ui/paleomainwindow_wellsection.cpp)

add_paleo_test(tst_wellsection LIBS paleo_domain)
add_paleo_test(tst_wellsection_workflow LIBS paleo_workflow paleo_store)
add_paleo_test(tst_wellsection_ui LIBS paleo_ui)
