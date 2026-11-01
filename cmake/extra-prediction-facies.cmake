# Prediction facies domain, bundled geological library and review panel.
target_sources(paleo_domain PRIVATE src/domain/faciestaxonomy.cpp src/domain/faciescatalog.cpp src/domain/facieshierarchy.cpp src/domain/paleomapfacies.cpp src/domain/faciesconsistency.cpp src/domain/predictionevidence.cpp src/domain/interpretationevidence.cpp src/domain/receiptsemantics.cpp src/domain/evidenceconflict.cpp resources/geology.qrc)  # paleomapfacies：现有古地理图微相补齐三级归属；faciesconsistency：facies-ux 4；receiptsemantics：G10b 跨算法统一回执；evidenceconflict：G06 多源冲突聚合
target_sources(paleo_qgis PRIVATE src/qgis/faciesmaplegend.cpp src/qgis/facieshierarchyrenderer.cpp src/qgis/mapcanvaslink.cpp src/qgis/draftautosave.cpp src/qgis/mappingfeatureidentity.cpp src/qgis/evidencemapadapter.cpp src/qgis/composeattribution.cpp)  # draftautosave：facies-ux 4；composeattribution：图斑级编图归因重放
target_sources(paleo_workflow PRIVATE src/workflow/mappinghierarchy.cpp src/workflow/mappingevidencereview.cpp src/workflow/evidenceinteraction.cpp)
target_sources(paleo_store PRIVATE src/metadata/mapsnapshot.cpp src/metadata/mapsnapshot.h)
target_sources(paleo_workflow PRIVATE src/workflow/mapversioncontroller_async.cpp)
target_sources(paleo_ui PRIVATE src/ui/pages/wellpredictionpanel.cpp src/ui/pages/faciesconsistencydialog.cpp)  # facies-ux 4 一致性报告
target_link_libraries(paleo_qgis PRIVATE paleo_domain)
target_sources(paleo_workflow PRIVATE src/workflow/wellpredictionreview.cpp)
add_paleo_test(tst_faciesconsistency LIBS paleo_domain)  # facies-ux 4：一致性统计
add_paleo_test(tst_draftautosave LIBS paleo_qgis)  # facies-ux 4：工作副本属性自动保存
add_paleo_test(tst_predictionevidence LIBS paleo_domain)  # facies-ux 5：证据链
add_paleo_test(tst_receiptsemantics LIBS paleo_domain)  # G10b：跨算法统一回执
add_paleo_test(tst_evidenceconflict LIBS paleo_domain)  # G06：多源证据冲突聚合

add_paleo_test(tst_interpretationevidence LIBS paleo_domain)

add_paleo_test(tst_featureidentity LIBS paleo_qgis)

add_paleo_test(tst_composeattribution LIBS paleo_workflow)  # G03/G10a：图斑级冻结输入归因重放 + 融合/转面回执
add_paleo_test(tst_impactrecompute LIBS paleo_workflow)  # G05/G09：空间影响分析 + 受控重算闭环
add_paleo_test(tst_workbench_impactoutcome LIBS paleo_ui)  # #740：后台影响/对比终态文案
set_tests_properties(tst_workbench_impactoutcome PROPERTIES TIMEOUT 30)
add_paleo_test(tst_evidenceindex LIBS paleo_services)  # G08/G12：证据反向索引
add_paleo_test(tst_evidencee2e LIBS paleo_workflow)  # G14/G11：端到端场景 + 点击路径有界性与 BASELINE

add_paleo_test(tst_evidencegraph LIBS paleo_workflow)
add_paleo_test(tst_nativeevidenceguard LIBS paleo_workflow)
add_paleo_test(tst_evidenceselection LIBS paleo_ui)
add_paleo_test(tst_evidencesourcevalidation LIBS paleo_workflow)
add_paleo_test(tst_snapshotquery LIBS paleo_workflow)
