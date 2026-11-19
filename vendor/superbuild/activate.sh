# Source this file for the Linux source build and its local application tests.
paleo_activate_source_prefix() {
  local sources prefix lock wanted multiarch
  sources=$(cd -P -- "$(dirname "${BASH_SOURCE[0]}")" && pwd) || return
  prefix=$(realpath -e -- "${1:-$sources/prefix}") || return
  lock="$sources/debian13-base.lock"
  wanted=$(tr -d '\r' < "$lock" | sha256sum | cut -d' ' -f1) || return
  if [ "$(cat "$prefix/.paleo-superbuild-base.sha256" 2>/dev/null)" != "$wanted" ]; then
    echo "The source prefix is missing or stale. Configure vendor/superbuild first." >&2
    return 1
  fi
  multiarch="$prefix/lib/x86_64-linux-gnu"
  export QGIS_PREFIX_PATH="$prefix" PALEO_SOURCE_PREFIX="$prefix"
  export LD_LIBRARY_PATH="$prefix/lib:$multiarch"
  # Keep the build tools' Python on PATH. The Debian base may carry a partial Python
  # through development packages, while Agent Python is provisioned separately.
  export PATH="$prefix/lib/qt6/bin:$prefix/lib/qt6/libexec:$PATH"
  export CMAKE_PREFIX_PATH="$prefix"
  export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$multiarch/pkgconfig:$prefix/share/pkgconfig"
  unset PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR FONTCONFIG_SYSROOT
  export QT_PLUGIN_PATH="$multiarch/qt6/plugins"
  export QCA_PLUGIN_PATH="$multiarch/qca-qt6"
  export QT_QPA_PLATFORM_PLUGIN_PATH="$QT_PLUGIN_PATH/platforms"
  export FONTCONFIG_PATH="$prefix/etc/fonts" FONTCONFIG_FILE="$prefix/etc/fonts/fonts.conf"
  export QTWEBENGINEPROCESS_PATH="$prefix/lib/qt6/libexec/QtWebEngineProcess"
  export QTWEBENGINE_RESOURCES_PATH="$prefix/share/qt6/resources"
  export QTWEBENGINE_LOCALES_PATH="$prefix/share/qt6/translations/qtwebengine_locales"
  export PROJ_DATA="$prefix/share/proj" PROJ_LIB="$prefix/share/proj"
  export GDAL_DATA="$prefix/share/gdal" GDAL_DRIVER_PATH="$prefix/lib/gdalplugins"
}

paleo_activate_source_prefix "$@"
