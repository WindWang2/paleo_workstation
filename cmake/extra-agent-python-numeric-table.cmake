# Explicit SDK table projection input; REPORT v1 and old generated-curve restorers remain separate.
target_sources(paleo_services_pythonanalysis PRIVATE
  src/services/pythongeneratedreportcontent.cpp
  src/services/pythonnumerictable_selection.cpp src/services/pythonnumerictable_materialize.cpp
  src/services/pythonnumerictable_metadata.cpp src/services/pythonnumerictablereport.cpp)
target_sources(paleo_services PRIVATE src/services/pythonnumerictablespec.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/pythonnumerictableoperation.cpp)
target_sources(paleo_app_agentproviders PRIVATE src/app/pythonnumerictablewiring.cpp)
add_paleo_test(tst_pythonnumerictable LIBS paleo_services_pythonanalysis)
add_paleo_test(tst_pythonnumerictablewiring LIBS paleo_app_agentproviders)
add_paleo_test(tst_pythonnumerictableoperations LIBS paleo_workflow Qt6::Concurrent)
