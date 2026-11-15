# Native Host, publication and chat adapters belong to the shared capability path.
target_sources(paleo_workflow PRIVATE
  src/workflow/capabilityhost_native.cpp
  src/workflow/capabilitysourcepublication.cpp
  src/workflow/capabilitychatprotocol_native.cpp)
add_paleo_test(tst_evidenceruntime LIBS paleo_workflow)
