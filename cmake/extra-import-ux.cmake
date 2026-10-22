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

# Optional project setup, shared with ordinary imports.
target_sources(paleo_store PRIVATE src/metadata/nativereference.cpp)
target_sources(paleo_qgis PRIVATE src/qgis/projectnativegrid.cpp)
target_sources(paleo_io PRIVATE src/io/welltable.cpp src/io/welltableimport.cpp
  src/io/projectsetupscan.cpp src/io/projectboundary.cpp src/io/horizonnativegrid.cpp src/io/welltabletext.cpp src/io/welltableprojection.cpp)
target_sources(paleo_io PRIVATE src/io/projectsetupmaps.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/projectsetup.cpp src/workflow/projectsetupmaps.cpp)
target_sources(paleo_ui PRIVATE src/ui/dialogs/projectsetupdialog.cpp src/ui/paleomainwindow_projectsetup.cpp)
target_sources(paleo_ui PRIVATE src/ui/dialogs/importsourcedialog.cpp)
target_sources(paleo_qgis PRIVATE src/qgis/offlinebasemapvalidation.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/offlinebasemapimport.cpp)
target_sources(paleo_ui PRIVATE src/ui/dialogs/offlinebasemapdialog.cpp)
add_paleo_test(tst_offlinebasemap LIBS paleo_ui)
add_paleo_test(tst_projectsetup LIBS paleo_ui paleo_io)
