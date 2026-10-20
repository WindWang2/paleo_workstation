target_sources(paleo_io PRIVATE src/io/dataimport_log.cpp src/io/importrecovery.cpp
                               src/io/importclassification.cpp)
add_paleo_test(tst_import_ux LIBS paleo_services paleo_io)
add_paleo_test(tst_import_preview LIBS paleo_ui)
add_paleo_test(tst_import_performance LIBS paleo_workflow paleo_services paleo_io)
# UI helpers registered here to avoid editing the shared module source table.
if(EXISTS "${CMAKE_SOURCE_DIR}/src/ui/dialogs/folderconfirm_preview.cpp")
  target_sources(paleo_ui PRIVATE src/ui/dialogs/folderconfirm_preview.cpp
                                  src/ui/dialogs/folderconfirm_results.cpp
                                  src/ui/pages/datalist_logsummary.cpp)
endif()
