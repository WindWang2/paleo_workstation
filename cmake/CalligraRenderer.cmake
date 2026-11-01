# Included by the pinned Calligra build, after its libraries and parts exist.
# INTENTIONALLY not referenced from this tree's CMakeLists: the only consumer is
# vendor/fetch-calligra.sh, which appends include(".../cmake/CalligraRenderer.cmake")
# to the extracted Calligra CMakeLists (#874). No CI lane builds it — after editing this
# file or src/app/officepreviewrenderer.cpp, run vendor/fetch-calligra.sh by hand.
# See cmake/README.md「树外消费的片段」. Do not delete as "dead" / do not move to root.
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
