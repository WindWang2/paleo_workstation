#!/usr/bin/env bash
set -euo pipefail
root=$(cd -P -- "$(dirname -- "$0")" && pwd)
unset LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT FONTCONFIG_SYSROOT
export QGIS_PREFIX_PATH="$root"
export GDAL_DATA="$root/share/gdal" GDAL_DRIVER_PATH="$root/lib/gdalplugins"
export PROJ_DATA="$root/share/proj" PROJ_LIB="$root/share/proj"
export QT_PLUGIN_PATH="$root/lib/qt6/plugins"
export QML_IMPORT_PATH="$root/lib/qt6/qml" QML2_IMPORT_PATH="$root/lib/qt6/qml"
export QT_QPA_PLATFORM_PLUGIN_PATH="$QT_PLUGIN_PATH/platforms"
export QTWEBENGINEPROCESS_PATH="$root/lib/qt6/libexec/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="$root/share/qt6/resources"
export QTWEBENGINE_LOCALES_PATH="$root/share/qt6/translations/qtwebengine_locales"
export FONTCONFIG_PATH="$root/etc/fonts" FONTCONFIG_FILE="$root/etc/fonts/fonts.conf"
export SSL_CERT_FILE="$root/share/certs/ca-bundle.pem" CURL_CA_BUNDLE="$root/share/certs/ca-bundle.pem"
export LIBGL_DRIVERS_PATH="$root/lib/dri"
if [ -f "$root/share/glvnd/egl_vendor.d/50_mesa.json" ]; then
  export __EGL_VENDOR_LIBRARY_FILENAMES="$root/share/glvnd/egl_vendor.d/50_mesa.json"
fi
for base in "$root"/lib/grass[0-9]*; do
  if [ -d "$base" ]; then export GISBASE="$base"; break; fi
done
if [ "${1:-}" = selfcheck ]; then
  shift
  exec "$root/bin/paleo_selfcheck" "$@"
fi
exec "$root/bin/paleo" "$@"
