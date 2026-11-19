# Source from the build shell before using the standalone SDK or native-perf.
paleo_activate_native_deps() {
  local vendor prefix expected multiarch
  vendor=$(cd -P -- "$(dirname "${BASH_SOURCE[0]}")" && pwd) || return
  prefix="$vendor/native-prefix/usr"
  expected=$(bash "$vendor/fetch-deps.sh" --native --lock-hash) || return
  if [ "$(cat "$vendor/native-prefix/.paleo-deb-closure.sha256" 2>/dev/null)" != "$expected" ]; then
    printf 'Native dependency prefix is missing or stale. Run bash %s/fetch-deps.sh --native.\n' "$vendor" >&2
    return 1
  fi
  multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo "$(uname -m)-linux-gnu")
  multiarch="$prefix/lib/$multiarch"
  export PALEO_NATIVE_PREFIX="$prefix"
  export LD_LIBRARY_PATH="$multiarch:$prefix/lib"
  export PATH="$prefix/lib/qt6/bin:$prefix/lib/qt6/libexec:$PATH"
  export CMAKE_PREFIX_PATH="$prefix"
  export PKG_CONFIG_LIBDIR="$multiarch/pkgconfig:$prefix/lib/pkgconfig:$prefix/share/pkgconfig"
  export PKG_CONFIG_SYSROOT_DIR="${prefix%/usr}"
  unset PKG_CONFIG_PATH FONTCONFIG_SYSROOT
  export QT_PLUGIN_PATH="$multiarch/qt6/plugins"
  export QT_QPA_PLATFORM_PLUGIN_PATH="$QT_PLUGIN_PATH/platforms"
  export FONTCONFIG_PATH="${prefix%/usr}/etc/fonts"
  export FONTCONFIG_FILE="$FONTCONFIG_PATH/fonts.conf"
}

paleo_activate_native_deps
