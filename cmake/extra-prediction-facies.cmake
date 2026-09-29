# Prediction facies domain, bundled geological library and review panel.
target_sources(paleo_domain PRIVATE src/domain/faciescatalog.cpp resources/geology.qrc)
target_sources(paleo_ui PRIVATE src/ui/pages/wellpredictionpanel.cpp)
target_link_libraries(paleo_qgis PRIVATE paleo_domain)
target_sources(paleo_workflow PRIVATE src/workflow/wellpredictionreview.cpp)
