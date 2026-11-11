# Focused native regression tests

This diagnostic build compiles the existing QtTests and their production source
files without configuring the application's QGIS dependency closure. It
supplements the full application build; it does not replace its source gates,
QGIS integration tests, packaging, or Office preview checks.

Use Qt 6 Core/Gui/Widgets/Sql/Test development files, ZLIB, and the initialized
Data_service submodule. On Linux, install the same Qt build dependencies used by
the native-perf workflow. Select `geostat`, `welllogset`, or both:

```sh
cmake -S tools/native-perf -B build-native-perf -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O2 -DNDEBUG' \
  -DPALEO_NATIVE_PERF_TESTS='geostat;welllogset'
cmake --build build-native-perf --parallel 2
QT_QPA_PLATFORM=offscreen ctest --test-dir build-native-perf \
  -j1 --no-tests=error --output-on-failure --output-junit results.xml
```

`PALEO_SOURCE_ROOT` may point at a separate checkout for comparison. Empty and
unknown selections fail configuration. The welllogset diagnostic uses GNU/Clang
function-section garbage collection to discard unused GIS transformations from
the original translation units; there are no substitute GIS implementations.
Its functional tests and five Data_service contracts also run against the
current source. The snapshot worker is placed beside those executables.

Tests run serially, retain their first-run assertions on stdout and in CTest's
logs, and receive individual XDG configuration/data directories. Performance
limits and numerical assertions are the same as in the main test suite.
