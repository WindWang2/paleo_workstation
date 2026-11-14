# Generated programs use the same SDK input, task/operation and publication contracts.
target_sources(paleo_services_pythonanalysis PRIVATE src/services/pythonoutputjson.cpp
  src/services/pythongenerated.cpp src/services/pythongeneratedreport.cpp)
target_sources(paleo_services_pythonruntime PRIVATE src/services/pythonsandboxprovider.cpp)
target_sources(paleo_services PRIVATE src/services/pythongeneratedspec.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/pythongeneratedoperation.cpp src/workflow/pythongeneratedworker.cpp
  src/workflow/capabilityhost_native.cpp src/workflow/capabilitysourcepublication.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/capabilitychatprotocol_native.cpp)
add_paleo_test(tst_pythongenerated LIBS paleo_services_pythonanalysis)
add_paleo_test(tst_pythonsandboxreadiness LIBS paleo_services_pythonruntime Qt6::Concurrent)
