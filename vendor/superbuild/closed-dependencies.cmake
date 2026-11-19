# For the native Linux superbuild. Compilers and generators use the host;
# application headers, libraries and CMake packages use the prepared prefix.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SUPERBUILD_PREFIX)
set(CMAKE_FIND_ROOT_PATH "${SUPERBUILD_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY FALSE)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY FALSE)
list(PREPEND CMAKE_PREFIX_PATH "${SUPERBUILD_PREFIX}")
list(PREPEND CMAKE_PROGRAM_PATH "${SUPERBUILD_PREFIX}/bin"
  "${SUPERBUILD_PREFIX}/lib/qt6/bin" "${SUPERBUILD_PREFIX}/lib/qt6/libexec")

# Debian's architecture-independent headers include generated multiarch
# headers such as jconfig.h. Keep both under the same prepared prefix.
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES "${SUPERBUILD_PREFIX}/include/x86_64-linux-gnu")
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "${SUPERBUILD_PREFIX}/include/x86_64-linux-gnu")

# FindOpenMP otherwise extracts the compiler's implicit, absolute libgomp path.
set(OpenMP_gomp_LIBRARY "${SUPERBUILD_PREFIX}/lib/x86_64-linux-gnu/libgomp.so.1"
  CACHE FILEPATH "Locked GNU OpenMP runtime" FORCE)

file(GLOB _paleo_multiarch_pkgconfig "${SUPERBUILD_PREFIX}/lib/*-linux-gnu/pkgconfig")
set(_paleo_pc "${SUPERBUILD_PREFIX}/lib/pkgconfig" ${_paleo_multiarch_pkgconfig}
  "${SUPERBUILD_PREFIX}/share/pkgconfig")
list(JOIN _paleo_pc ":" _paleo_pc)
set(ENV{PKG_CONFIG_LIBDIR} "${_paleo_pc}")
unset(ENV{PKG_CONFIG_PATH})
unset(ENV{PKG_CONFIG_SYSROOT_DIR})
