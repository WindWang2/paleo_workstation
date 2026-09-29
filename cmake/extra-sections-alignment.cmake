# Well sections reuse the engine and keep model / workflow / view dependencies separate.
target_sources(paleo_domain PRIVATE src/domain/seismic/sectiongeometry.cpp)
target_sources(paleo_workflow PRIVATE src/workflow/sectionworkbench.cpp)
target_sources(paleo_ui PRIVATE src/ui/seismicsection/sectionsetupdialog.cpp src/ui/paleomainwindow_sections.cpp)
add_paleo_test(tst_sections_alignment)
