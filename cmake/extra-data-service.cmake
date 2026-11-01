# Data_service is the shared structured-data and artifact-catalog backend.
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/vendor/Data_service/CMakeLists.txt")
  message(FATAL_ERROR "Data_service is missing. Run: git submodule update --init --recursive -- vendor/Data_service")
endif()
set(PDS_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(PDS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(PDS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PDS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PDS_INSTALL OFF CACHE BOOL "" FORCE)
set(PDS_SEISMIC_ENGINE_TARGET paleo_sbm CACHE STRING "" FORCE)
set(PDS_SEISMIC_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/vendor/sbm/src" CACHE PATH "" FORCE)
add_subdirectory(vendor/Data_service EXCLUDE_FROM_ALL)

# Keep the BIFF compatibility fixes from the pinned Data_service source in the
# runtime too. Falling through to an unrelated PATH DLL loses those fixes.
if(WIN32)
  set(_paleo_freexl_source "${CMAKE_SOURCE_DIR}/vendor/Data_service/vendor/freexl")
  # A private name avoids replacing GDAL's own FreeXL dependency/ABI.
  set(_paleo_freexl_runtime "${CMAKE_BINARY_DIR}/paleo_freexl.dll")
  find_file(PALEO_ICONV_RUNTIME NAMES iconv-2.dll libiconv-2.dll iconv.dll
    HINTS ${CMAKE_PREFIX_PATH} "${CMAKE_SOURCE_DIR}/vendor/osgeo4w" PATH_SUFFIXES bin REQUIRED)
  get_filename_component(_paleo_iconv_name "${PALEO_ICONV_RUNTIME}" NAME)
  get_filename_component(_paleo_iconv_dir "${PALEO_ICONV_RUNTIME}" DIRECTORY)
  set(_paleo_charset_args)
  if(_paleo_iconv_name STREQUAL "iconv.dll")
    find_file(PALEO_CHARSET_RUNTIME NAMES charset.dll HINTS "${_paleo_iconv_dir}" NO_DEFAULT_PATH REQUIRED)
    list(APPEND _paleo_charset_args -CharsetDll charset.dll)
  endif()
  add_custom_command(OUTPUT "${_paleo_freexl_runtime}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${_paleo_freexl_source}/build_windows.ps1"
      -OutputDirectory "${CMAKE_BINARY_DIR}/freexl-runtime"
      -IconvDll "${_paleo_iconv_name}" ${_paleo_charset_args}
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${CMAKE_BINARY_DIR}/freexl-runtime/freexl.dll" "${_paleo_freexl_runtime}"
    DEPENDS "${_paleo_freexl_source}/build_windows.ps1"
      "${_paleo_freexl_source}/freexl-2.0.0/src/freexl.c"
      "${_paleo_freexl_source}/freexl-2.0.0/headers/freexl.h"
      "${_paleo_freexl_source}/freexl-2.0.0/headers/freexl_internals.h"
      "${_paleo_freexl_source}/freexl-2.0.0/config-msvc.h"
      "${_paleo_freexl_source}/windows/iconv.h" "${_paleo_freexl_source}/windows/iconv.def"
    VERBATIM)
  add_custom_target(paleo_freexl_runtime DEPENDS "${_paleo_freexl_runtime}")
  add_dependencies(paleo_data_service paleo_freexl_runtime)
endif()
target_link_libraries(paleo_store PRIVATE Paleo::paleo_data_service)
target_sources(paleo_store PRIVATE src/catalog/areawellprojection.cpp src/catalog/areasourceprojection.cpp
  src/catalog/externalsourceverification.cpp src/catalog/wellavailability.cpp)
add_paleo_test(tst_dataservice LIBS paleo_store paleo_io Paleo::paleo_data_service)
add_paleo_test(tst_dataservice_formats LIBS Paleo::paleo_data_service Qt6::Sql)
add_paleo_test(tst_dataservice_wellplot LIBS Paleo::paleo_data_service Qt6::Sql)
add_paleo_test(tst_dataservice_hd LIBS paleo_domain Paleo::paleo_data_service)
add_paleo_test(tst_dataservice_batch LIBS paleo_domain Paleo::paleo_data_service)
add_paleo_test(tst_nativeimport LIBS paleo_io paleo_services Paleo::paleo_data_service)

target_sources(paleo_io PRIVATE src/io/nativemodel_prepare.cpp src/io/nativemodel_read.cpp src/io/nativemodel_tables.cpp src/io/wellfileread.cpp)
target_link_libraries(paleo_io PRIVATE Paleo::paleo_data_service)

target_sources(paleo_io PRIVATE src/io/areamodel.cpp src/io/areamodel_log.cpp)
target_sources(paleo_services PRIVATE src/services/previewdoc_area.cpp src/services/areawellchart.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/projectsetup_area.cpp)
target_sources(paleo_ui PRIVATE src/ui/dialogs/dataserviceimportwizard.cpp)
target_link_libraries(paleo_ui PRIVATE Paleo::paleo_data_service)
add_paleo_test(tst_dataserviceimportwizard LIBS paleo_ui paleo_workflow paleo_io Paleo::paleo_data_service)
target_link_libraries(paleo_services PRIVATE Paleo::paleo_data_service)
target_link_libraries(paleo_workflow PRIVATE Paleo::paleo_data_service)
add_paleo_test(tst_areamodel LIBS paleo_io paleo_services paleo_workflow Paleo::paleo_data_service)
add_paleo_test(tst_realprojectacceptance LIBS paleo_io paleo_services Paleo::paleo_data_service)
add_paleo_test(tst_modelwellselection LIBS paleo_ui)
target_link_libraries(tst_wellcomposite PRIVATE Paleo::paleo_data_service)

target_sources(paleo_workflow PRIVATE src/workflow/stratigraphyworkflow.cpp src/workflow/stratigraphyworkflow.h)
target_sources(paleo_workflow PRIVATE src/workflow/stratigraphyworkflow_edit.cpp src/workflow/stratigraphyworkflow_versions.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/stratigraphygeometry.cpp src/workflow/stratigraphyworkflow_settings.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/stratigraphyworkflow_templates.cpp)
target_link_libraries(paleo_workflow PRIVATE Paleo::paleo_stratigraphy_service)
target_sources(paleo_ui PRIVATE src/ui/pages/stratigraphypage.cpp src/ui/pages/stratigraphypage.h)
target_sources(paleo_ui PRIVATE src/ui/pages/stratigraphypage_options.cpp)
target_sources(paleo_ui PRIVATE src/ui/pages/stratigraphypage_controls.cpp src/ui/pages/stratigraphypage_editors.cpp)
target_link_libraries(paleo_ui PRIVATE Paleo::paleo_data_service)
target_sources(paleo_ui PRIVATE src/ui/pages/stratigraphypage_settings.cpp)
target_sources(paleo_ui PRIVATE src/ui/pages/stratigraphypage_results.cpp src/ui/pages/stratigraphyresultplot.cpp)
target_sources(paleo_ui PRIVATE src/ui/pages/stratigraphypage_templates.cpp)
target_sources(paleo_ui PRIVATE src/ui/pages/stratigraphypage_view.cpp)

target_sources(paleo_services PRIVATE src/services/stratigraphymodel.cpp)
target_sources(paleo_services PRIVATE src/services/stratigraphyediting.cpp)
target_sources(paleo_services PRIVATE src/services/stratigraphyexport.cpp)
target_link_libraries(paleo_services PRIVATE Paleo::paleo_stratigraphy_service)
add_paleo_test(tst_stratigraphy_editing LIBS paleo_ui paleo_workflow paleo_services Paleo::paleo_stratigraphy_service)

add_paleo_test(tst_stratigraphy_core LIBS Paleo::paleo_stratigraphy_service Qt6::Core)

add_paleo_test(tst_stratigraphy_workflow LIBS Paleo::paleo_stratigraphy_service Qt6::Core)

add_paleo_test(tst_stratigraphy_quick_workflow LIBS Paleo::paleo_stratigraphy_service Qt6::Core)

add_paleo_test(tst_stratigraphy_endpoint_extrapolation LIBS Paleo::paleo_stratigraphy_service Qt6::Core)

add_paleo_test(tst_stratigraphy_template_guidance LIBS Paleo::paleo_stratigraphy_service Qt6::Core)

add_paleo_test(tst_stratigraphy_time_depth_selection LIBS Paleo::paleo_stratigraphy_service Qt6::Core)

add_paleo_test(tst_stratigraphy_storage LIBS Paleo::paleo_stratigraphy_service Qt6::Core)

target_include_directories(tst_stratigraphy_storage PRIVATE vendor/Data_service/src)
target_link_libraries(tst_stratigraphy_storage PRIVATE Qt6::Sql)
add_paleo_test(tst_stratigraphy_model LIBS paleo_workflow paleo_services Paleo::paleo_data_service)

target_sources(paleo_ui PRIVATE src/ui/pages/stratigraphypage_preview.cpp src/ui/wellcomposite/multiwellview_range.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/stratigraphyworkflow_preview.cpp)

target_sources(paleo_services PRIVATE src/services/seismicdataaccess.cpp src/services/seismicdataprojection.cpp)
target_sources(paleo_services PRIVATE src/services/seismictaskservice_data.cpp)
target_link_libraries(tst_seismic_index PRIVATE Paleo::paleo_data_service)
target_sources(paleo_services PRIVATE src/services/projectdata_timedepth.cpp src/services/wellreadiness.cpp)

# 四子层段体系（docs/STRATIGRAPHIC_SUBINTERVALS.md）：选择/分类值类型 + Data Service 适配
target_sources(paleo_domain PRIVATE src/domain/stratigraphicunit.cpp)
target_sources(paleo_services PRIVATE src/services/stratigraphicsubintervals.cpp)
add_paleo_test(tst_stratigraphic_subintervals LIBS paleo_services paleo_linkage Paleo::paleo_data_service)

# 子层段砂地比 / 井相 / 地震相成果与编图证据（docs/STRATIGRAPHIC_SUBINTERVALS.md §井相地震相）
target_sources(paleo_domain PRIVATE src/domain/subintervalfacies.cpp)
target_sources(paleo_services PRIVATE src/services/subintervalfactors.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/analysisresultsync.cpp src/workflow/wellfaciesworkflow_unit.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/analysisframework.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/seismicresultpublisher.cpp)
add_paleo_test(tst_analysisresultsync LIBS paleo_workflow Paleo::paleo_data_service)
add_paleo_test(tst_dataservice_framework LIBS Paleo::paleo_data_service Qt6::Core)
target_link_libraries(tst_wellfacies PRIVATE Paleo::paleo_data_service)
target_link_libraries(tst_factorworkflow PRIVATE Paleo::paleo_data_service)
target_link_libraries(tst_mappingworkbench PRIVATE Paleo::paleo_data_service)
target_link_libraries(paleo PRIVATE Paleo::paleo_data_service)
