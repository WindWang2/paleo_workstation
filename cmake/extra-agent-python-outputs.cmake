# Versioned three-port output sets; host alone encodes PNG. Runtime/deploy stays QtCore.
target_sources(paleo_services_pythonanalysis PRIVATE
  src/services/pythonimageoutput.cpp src/services/pythongeneratedoutputschema.cpp)
target_link_libraries(paleo_services_pythonanalysis PRIVATE Qt6::Gui)
target_sources(paleo_services PRIVATE src/services/pythongeneratedoutputspec.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/pythongeneratedoutputoperation.cpp src/workflow/pythongeneratedoutputworker.cpp)
add_paleo_test(tst_pythonimageoutput LIBS paleo_services_pythonanalysis Qt6::Gui)
add_paleo_test(tst_pythongeneratedoutputs LIBS paleo_workflow Qt6::Concurrent)
