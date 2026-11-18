# Validate the Linux deb route before CMake can reuse imports from another prefix.
set(PALEO_DEB_PREFIX "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND QGIS_PREFIX)
  file(REAL_PATH "${QGIS_PREFIX}" _paleo_qgis_real)
  file(REAL_PATH "${CMAKE_SOURCE_DIR}/vendor/prefix/usr" _paleo_project_deb)
  cmake_path(GET _paleo_qgis_real PARENT_PATH _paleo_deb_root)
  set(_paleo_deb_stamp "${_paleo_deb_root}/.paleo-deb-closure.sha256")
  if(_paleo_qgis_real STREQUAL _paleo_project_deb OR EXISTS "${_paleo_deb_stamp}")
    file(READ "${CMAKE_SOURCE_DIR}/vendor/deb-closure.lock" _paleo_deb_lock)
    string(REPLACE "\r" "" _paleo_deb_lock "${_paleo_deb_lock}")
    string(SHA256 _paleo_deb_expected "${_paleo_deb_lock}")
    if(EXISTS "${_paleo_deb_stamp}")
      file(STRINGS "${_paleo_deb_stamp}" _paleo_deb_actual)
    endif()
    if(NOT _paleo_deb_actual STREQUAL _paleo_deb_expected)
      message(FATAL_ERROR "Deb prefix lock mismatch at ${_paleo_deb_root}. Run ./paleo-dev bootstrap fetch-only.")
    endif()
    set(PALEO_DEB_PREFIX "${_paleo_qgis_real}")
    set(PALEO_DEB_MULTIARCH "lib/${CMAKE_LIBRARY_ARCHITECTURE}")
  endif()
endif()

function(paleo_deb_require path component)
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Locked deb prefix is missing ${component}: ${path}")
  endif()
  file(REAL_PATH "${path}" resolved)
  cmake_path(IS_PREFIX PALEO_DEB_PREFIX "${resolved}" NORMALIZE inside)
  if(NOT inside)
    message(FATAL_ERROR "Locked deb prefix resolved ${component} outside ${PALEO_DEB_PREFIX}: ${resolved}")
  endif()
endfunction()

function(paleo_deb_check_import target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Locked deb prefix did not define imported target ${target}")
  endif()
  set(locations IMPORTED_LOCATION)
  get_target_property(configurations "${target}" IMPORTED_CONFIGURATIONS)
  if(configurations)
    foreach(configuration IN LISTS configurations)
      list(APPEND locations "IMPORTED_LOCATION_${configuration}")
    endforeach()
  endif()
  set(found FALSE)
  foreach(property IN LISTS locations)
    get_target_property(location "${target}" "${property}")
    if(location)
      paleo_deb_require("${location}" "${target}")
      set(found TRUE)
    endif()
  endforeach()
  if(NOT found)
    message(FATAL_ERROR "Missing imported location for ${target}")
  endif()
  get_target_property(headers "${target}" INTERFACE_INCLUDE_DIRECTORIES)
  foreach(directory IN LISTS headers)
    if(IS_ABSOLUTE "${directory}")
      paleo_deb_require("${directory}" "${target} headers")
    endif()
  endforeach()
endfunction()

if(PALEO_DEB_PREFIX)
  file(GLOB _paleo_qgis_desktop_plugins "${PALEO_DEB_PREFIX}/lib/qgis/plugins/libplugin_*.so")
  if(_paleo_qgis_desktop_plugins)
    message(FATAL_ERROR "Desktop QGIS plugins in the embedded provider directory. Run ./paleo-dev bootstrap fetch-only.")
  endif()
  if(NOT EXISTS "${_paleo_deb_root}/etc/fonts/fonts.conf")
    message(FATAL_ERROR "Locked deb prefix is missing Fontconfig configuration")
  endif()
  foreach(dependency IN ITEMS
      "${PALEO_DEB_MULTIARCH}/qt6/plugins/platforms/libqoffscreen.so"
      "${PALEO_DEB_MULTIARCH}/qt6/plugins/platforms/libqxcb.so"
      "${PALEO_DEB_MULTIARCH}/qt6/plugins/sqldrivers/libqsqlite.so"
      "${PALEO_DEB_MULTIARCH}/gdalplugins/drivers.ini"
      "lib/qt6/libexec/QtWebEngineProcess"
      "share/qt6/resources/qtwebengine_resources.pak"
      "share/qt6/resources/v8_context_snapshot.bin"
      "share/qt6/translations/qtwebengine_locales/en-US.pak"
      "lib/qgis/plugins/libprovider_delimitedtext.so"
      "lib/qgis/plugins/libprovider_wms.so"
      "lib/grass84/lib/libgrass_gis.8.4.so"
      "share/qgis/resources/srs.db"
      "share/gdal"
      "share/proj/proj.db")
    paleo_deb_require("${PALEO_DEB_PREFIX}/${dependency}" "${dependency}")
  endforeach()
endif()
