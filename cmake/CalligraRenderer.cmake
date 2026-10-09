# Included by the pinned Calligra build, after its libraries and parts exist.
add_executable(paleo_office_renderer "${PALEO_WORKSTATION_SOURCE}/src/app/officepreviewrenderer.cpp")
target_include_directories(paleo_office_renderer PRIVATE
  ${KOMAIN_INCLUDES} ${KOPAGEAPP_INCLUDES}
  ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/words/part ${CMAKE_BINARY_DIR}/words/part
  ${CMAKE_BINARY_DIR}/sheets/engine ${CMAKE_BINARY_DIR}/sheets/core
  ${CMAKE_BINARY_DIR}/sheets/ui ${CMAKE_BINARY_DIR}/sheets/part)
target_link_libraries(paleo_office_renderer PRIVATE
  komain kopageapp koplugin calligrasheetspartlib wordsprivate)
set_target_properties(paleo_office_renderer PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
install(TARGETS paleo_office_renderer RUNTIME DESTINATION bin)
