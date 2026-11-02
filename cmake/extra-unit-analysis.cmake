target_sources(paleo_services PRIVATE src/services/analysisunitcontext.cpp src/services/seismicunitinput.cpp src/services/wellpredictioninput.cpp)
target_sources(paleo_qgis PRIVATE src/qgis/unitraster.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/mappingworkbench_seismicunit.cpp)
add_paleo_test(tst_analysisunitcontext LIBS paleo_services Paleo::paleo_data_service)
