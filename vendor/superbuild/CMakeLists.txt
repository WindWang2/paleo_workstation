cmake_minimum_required(VERSION 3.28)
project(paleo_superbuild LANGUAGES NONE)

include(ExternalProject)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
  message(FATAL_ERROR "The source build uses the locked Debian 13 base on Linux x86_64.")
endif()

set(SUPERBUILD_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/prefix" CACHE PATH "Source install prefix")
set(SUPERBUILD_BASE "${CMAKE_CURRENT_SOURCE_DIR}/base" CACHE PATH "Verified Debian 13 base")
set(SUPERBUILD_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/../cache/tarballs" CACHE PATH "Source archives")
set(SUPERBUILD_JOBS 4 CACHE STRING "Parallel jobs per source dependency (1 to 8)")
option(SUPERBUILD_OFFLINE "Require every pinned source archive in the local cache" "$ENV{PALEO_VENDOR_OFFLINE}")
if(NOT SUPERBUILD_JOBS MATCHES "^[1-8]$")
  message(FATAL_ERROR "SUPERBUILD_JOBS must be between 1 and 8")
endif()

option(WITH_3D "Build QGIS 3D" OFF)
option(WITH_PDAL "Build the QGIS PDAL provider" OFF)
option(WITH_APP "Build the QGIS desktop application" OFF)
option(WITH_GUI "Build the QGIS GUI library" ON)
option(WITH_ANALYSIS "Build the QGIS analysis library" ON)

# These hashes identify the inputs to the build, including offline replays.
set(GEOS_URL "https://download.osgeo.org/geos/geos-3.15.0.tar.bz2" CACHE STRING "GEOS source archive")
set(PROJ_URL "https://download.osgeo.org/proj/proj-9.8.1.tar.gz" CACHE STRING "PROJ source archive")
set(GDAL_URL "https://download.osgeo.org/gdal/3.13.3/gdal-3.13.3.tar.xz" CACHE STRING "GDAL source archive")
set(QGIS_URL "https://download.qgis.org/downloads/qgis-4.2.3.tar.bz2" CACHE STRING "QGIS source archive")

function(paleo_source_archive variable url)
  cmake_path(GET url FILENAME name)
  if(EXISTS "${SUPERBUILD_CACHE}/${name}")
    set(${variable} "${SUPERBUILD_CACHE}/${name}" PARENT_SCOPE)
  elseif(SUPERBUILD_OFFLINE)
    message(FATAL_ERROR "Missing offline source archive ${SUPERBUILD_CACHE}/${name}")
  else()
    set(${variable} "${url}" PARENT_SCOPE)
  endif()
endfunction()

find_program(BASH_EXECUTABLE bash REQUIRED)
find_program(MAKE_EXECUTABLE NAMES gmake make REQUIRED)
find_program(SOURCE_PYTHON NAMES python3 REQUIRED)
set(receipt_tool "${CMAKE_CURRENT_SOURCE_DIR}/source_receipt.py")
file(GLOB patch_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  debian13-base.lock debian-sources.lock closed-dependencies.cmake prepare-prefix.sh
  prepare-debian-source.cmake "${receipt_tool}" ${patch_inputs})
execute_process(COMMAND "${SOURCE_PYTHON}" "${receipt_tool}" digest
  OUTPUT_VARIABLE recipe OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
string(SHA256 profile "${WITH_3D};${WITH_PDAL};${WITH_APP};${WITH_GUI};${WITH_ANALYSIS}")
execute_process(COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare-prefix.sh"
  "${SUPERBUILD_PREFIX}" "${SUPERBUILD_BASE}" "${CMAKE_CURRENT_SOURCE_DIR}/debian13-base.lock"
  "${recipe}" "${CMAKE_BINARY_DIR}" "${profile}"
  COMMAND_ERROR_IS_FATAL ANY)

set(source_cache DOWNLOAD_DIR "${SUPERBUILD_CACHE}" TLS_VERIFY ON)
set(source_build BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel ${SUPERBUILD_JOBS})
set(source_arguments
  -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
  -DCMAKE_INSTALL_LIBDIR=lib
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/closed-dependencies.cmake
  -DSUPERBUILD_PREFIX=<INSTALL_DIR>
  "-DCMAKE_INSTALL_RPATH=$ORIGIN|$ORIGIN/../lib|$ORIGIN/../..")

function(paleo_debian_source_command variable package)
  set(${variable}
    "${CMAKE_COMMAND}" -DSOURCE_PACKAGE=${package}
    -DSOURCE_CACHE=${SUPERBUILD_CACHE} -DSOURCE_TREE=<SOURCE_DIR>
    -DOFFLINE=${SUPERBUILD_OFFLINE}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare-debian-source.cmake"
    PARENT_SCOPE)
endfunction()

paleo_debian_source_command(zlib_download zlib)
ExternalProject_Add(zlib
  INSTALL_DIR "${SUPERBUILD_PREFIX}"
  DOWNLOAD_COMMAND ${zlib_download}
  CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --libdir=<INSTALL_DIR>/lib --shared
  BUILD_COMMAND "${MAKE_EXECUTABLE}" -j${SUPERBUILD_JOBS}
  INSTALL_COMMAND "${MAKE_EXECUTABLE}" install)

paleo_source_archive(zstd_archive "https://github.com/meta/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz")
ExternalProject_Add(zstd
  DEPENDS zlib
  INSTALL_DIR "${SUPERBUILD_PREFIX}"
  URL "${zstd_archive}"
  URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
  SOURCE_SUBDIR build/cmake
  ${source_cache}
  LIST_SEPARATOR |
  CMAKE_ARGS ${source_arguments}
             -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
  ${source_build})

paleo_debian_source_command(sqlite_download sqlite3)
# Keep the SQLite capabilities used by the Debian Qt SQL driver and GIS tools.
set(sqlite_cflags -O2 -fno-strict-aliasing
  -DSQLITE_SECURE_DELETE -DSQLITE_ENABLE_COLUMN_METADATA
  -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_FTS3_PARENTHESIS -DSQLITE_ENABLE_RTREE=1 -DSQLITE_SOUNDEX=1
  -DSQLITE_ENABLE_UNLOCK_NOTIFY -DSQLITE_ENABLE_DBSTAT_VTAB -DSQLITE_ENABLE_DBPAGE_VTAB
  -DSQLITE_ALLOW_ROWID_IN_VIEW -DSQLITE_ENABLE_UPDATE_DELETE_LIMIT=1 -DSQLITE_ENABLE_LOAD_EXTENSION
  -DSQLITE_ENABLE_JSON1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS3_TOKENIZER=1
  -DSQLITE_USE_URI=1 -DSQLITE_MAX_SCHEMA_RETRY=25 -DSQLITE_ENABLE_PREUPDATE_HOOK -DSQLITE_ENABLE_SESSION
  -DSQLITE_ENABLE_STMTVTAB -DSQLITE_STRICT_SUBTYPE=1 -DSQLITE_MAX_VARIABLE_NUMBER=250000)
list(JOIN sqlite_cflags " " sqlite_cflags)
ExternalProject_Add(sqlite
  DEPENDS zstd
  INSTALL_DIR "${SUPERBUILD_PREFIX}"
  DOWNLOAD_COMMAND ${sqlite_download}
  CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
    "CFLAGS=${sqlite_cflags}"
    <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --libdir=<INSTALL_DIR>/lib
    --disable-tcl --disable-readline --disable-static --enable-fts4 --enable-fts5
    --enable-threadsafe --enable-load-extension --enable-json
  BUILD_COMMAND "${MAKE_EXECUTABLE}" -j${SUPERBUILD_JOBS}
  INSTALL_COMMAND "${MAKE_EXECUTABLE}" install)

paleo_source_archive(geos_archive "${GEOS_URL}")
ExternalProject_Add(geos
  DEPENDS sqlite
  INSTALL_DIR "${SUPERBUILD_PREFIX}"
  URL "${geos_archive}"
  URL_HASH SHA256=d5e5192a686d065eaed082de14dd26244c5c8e02bff16b2c6cce3265f648e00e
  ${source_cache}
  LIST_SEPARATOR |
  CMAKE_ARGS ${source_arguments} -DBUILD_TESTING=OFF
  ${source_build})

paleo_source_archive(proj_archive "${PROJ_URL}")
ExternalProject_Add(proj
  DEPENDS geos
  INSTALL_DIR "${SUPERBUILD_PREFIX}"
  URL "${proj_archive}"
  URL_HASH SHA256=af5b731c145c1d13c4e3b4eeb7d167e94e845e440f71e3496b4ed8dae0291960
  ${source_cache}
  LIST_SEPARATOR |
  CMAKE_ARGS ${source_arguments} -DENABLE_CURL=ON -DBUILD_TESTING=OFF
             -DEXE_SQLITE3=<INSTALL_DIR>/bin/sqlite3
             -DPROJ_DATA_PATH=<INSTALL_DIR>/share/proj
  ${source_build})

paleo_source_archive(gdal_archive "${GDAL_URL}")
ExternalProject_Add(gdal
  DEPENDS proj
  INSTALL_DIR "${SUPERBUILD_PREFIX}"
  URL "${gdal_archive}"
  URL_HASH SHA256=a3348c2102dd91962290007d7c293b467bb1b0cd89f086f24bb97b9b653a9804
  ${source_cache}
  LIST_SEPARATOR |
  CMAKE_ARGS ${source_arguments} -DBUILD_TESTING=OFF
             -DBUILD_PYTHON_BINDINGS=OFF -DBUILD_JAVA_BINDINGS=OFF -DBUILD_CSHARP_BINDINGS=OFF
  ${source_build})

paleo_source_archive(qgis_archive "${QGIS_URL}")
ExternalProject_Add(qgis
  DEPENDS gdal
  INSTALL_DIR "${SUPERBUILD_PREFIX}"
  URL "${qgis_archive}"
  URL_HASH SHA256=b044889d895c07d9355512cba76973822830153f6f15feb0d5e00077d3d76e59
  ${source_cache}
  PATCH_COMMAND "${CMAKE_COMMAND}"
    -DPATCH_SOURCE_DIR=<SOURCE_DIR>
    -DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/patches/qgis-4.2.3-labels-with-layer.patch
    -DMARKER_FILE=src/core/maprenderer/qgsmaprendererjob.h
    -DMARKER=QGIS_PALEO_LABELS_WITH_LAYER
    -P "${CMAKE_CURRENT_SOURCE_DIR}/patches/apply-patch.cmake"
  LIST_SEPARATOR |
  CMAKE_ARGS ${source_arguments}
             -DWITH_3D=${WITH_3D} -DWITH_PDAL=${WITH_PDAL} -DWITH_DESKTOP=${WITH_APP}
             -DWITH_GUI=${WITH_GUI} -DWITH_ANALYSIS=${WITH_ANALYSIS}
             -DWITH_INTERNAL_SPATIALINDEX=ON -DWITH_PYTHON=OFF -DWITH_BINDINGS=OFF
             -DWITH_QGIS_PROCESS=OFF -DENABLE_TESTS=OFF
  ${source_build})

add_custom_target(source_receipt ALL
  COMMAND "${SOURCE_PYTHON}" "${receipt_tool}" finish "${SUPERBUILD_PREFIX}" --build "${CMAKE_BINARY_DIR}"
  DEPENDS qgis VERBATIM)
