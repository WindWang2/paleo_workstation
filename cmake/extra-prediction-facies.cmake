# Prediction facies domain, bundled geological library and review panel.
target_sources(paleo_domain PRIVATE src/domain/faciescatalog.cpp src/domain/facieshierarchy.cpp src/domain/faciesconsistency.cpp resources/geology.qrc)  # faciesconsistency：facies-ux 4 地震×测井相一致性
target_sources(paleo_qgis PRIVATE src/qgis/facieshierarchyrenderer.cpp src/qgis/mapcanvaslink.cpp src/qgis/draftautosave.cpp)  # draftautosave：facies-ux 4 工作副本属性自动保存
target_sources(paleo_workflow PRIVATE src/workflow/mappinghierarchy.cpp)
target_sources(paleo_ui PRIVATE src/ui/pages/wellpredictionpanel.cpp src/ui/pages/faciesconsistencydialog.cpp)  # facies-ux 4 一致性报告
target_link_libraries(paleo_qgis PRIVATE paleo_domain)
target_sources(paleo_workflow PRIVATE src/workflow/wellpredictionreview.cpp)
add_paleo_test(tst_faciesconsistency LIBS paleo_domain)  # facies-ux 4：一致性统计
add_paleo_test(tst_draftautosave LIBS paleo_qgis)  # facies-ux 4：工作副本属性自动保存
