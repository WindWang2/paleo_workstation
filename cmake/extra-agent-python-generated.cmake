# Generated programs use the same SDK input, task/operation and publication contracts.
target_sources(paleo_services_pythonanalysis PRIVATE src/services/pythonoutputjson.cpp
  src/services/pythongenerated.cpp src/services/pythongeneratedreport.cpp)
target_sources(paleo_services_pythonanalysis PRIVATE
  src/services/pythonnumericoutput.cpp src/services/pythonnumericoutput_csv.cpp)
target_sources(paleo_services_pythonanalysis PRIVATE src/services/pythonimageoutput.cpp)
target_link_libraries(paleo_services_pythonanalysis PRIVATE Qt6::Gui)
target_sources(paleo_services_pythonruntime PRIVATE src/services/pythonsandboxprovider.cpp)
target_sources(paleo_services PRIVATE src/services/pythongeneratedspec.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/pythongeneratedoperation.cpp src/workflow/pythongeneratedworker.cpp)
add_paleo_test(tst_pythongenerated LIBS paleo_services_pythonanalysis)
add_paleo_test(tst_pythonnumericoutput LIBS paleo_services_pythonanalysis)
add_paleo_test(tst_pythonimageoutput LIBS paleo_services_pythonanalysis Qt6::Gui)
add_paleo_test(tst_pythonsandboxreadiness LIBS paleo_services_pythonruntime Qt6::Concurrent)
add_paleo_test(tst_pythongeneratedoperations LIBS paleo_workflow Qt6::Concurrent)
