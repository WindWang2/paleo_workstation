# Prediction facies domain, bundled geological library and review panel.
target_sources(paleo_domain PRIVATE src/domain/faciescatalog.cpp src/domain/facieshierarchy.cpp src/domain/faciesconsistency.cpp src/domain/predictionevidence.cpp src/domain/interpretationevidence.cpp resources/geology.qrc)  # faciesconsistency：facies-ux 4 地震×测井相一致性
target_sources(paleo_qgis PRIVATE src/qgis/facieshierarchyrenderer.cpp src/qgis/mapcanvaslink.cpp src/qgis/draftautosave.cpp src/qgis/mappingfeatureidentity.cpp src/qgis/evidencemapadapter.cpp)  # draftautosave：facies-ux 4 工作副本属性自动保存
target_sources(paleo_workflow PRIVATE src/workflow/mappinghierarchy.cpp src/workflow/mappingevidencereview.cpp src/workflow/evidenceinteraction.cpp)
target_sources(paleo_store PRIVATE src/metadata/mapsnapshot.cpp src/metadata/mapsnapshot.h)
target_sources(paleo_workflow PRIVATE src/workflow/mapversioncontroller_async.cpp)
target_sources(paleo_ui PRIVATE src/ui/pages/wellpredictionpanel.cpp src/ui/pages/faciesconsistencydialog.cpp)  # facies-ux 4 一致性报告
target_link_libraries(paleo_qgis PRIVATE paleo_domain)
target_sources(paleo_workflow PRIVATE src/workflow/wellpredictionreview.cpp)
add_paleo_test(tst_faciesconsistency LIBS paleo_domain)  # facies-ux 4：一致性统计
add_paleo_test(tst_draftautosave LIBS paleo_qgis)  # facies-ux 4：工作副本属性自动保存
add_paleo_test(tst_predictionevidence LIBS paleo_domain)  # facies-ux 5：证据链

add_paleo_test(tst_interpretationevidence LIBS paleo_domain)

add_paleo_test(tst_featureidentity LIBS paleo_qgis)

add_paleo_test(tst_evidencegraph LIBS paleo_workflow)
add_paleo_test(tst_nativeevidenceguard LIBS paleo_workflow)
