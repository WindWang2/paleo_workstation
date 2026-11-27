#!/usr/bin/env bash
# Paleo Workstation AppImage 打包脚本
# 用法：
#   tools/make_appimage.sh [build 目录]
#   tools/make_appimage.sh [--build-dir DIR] [--out FILE]
#                          [--prefix DIR] [--qt-prefix DIR] [--qgis-prefix DIR]
#                          [--allow-system-fallback] [--runtime FILE] [--stage-only]
#   tools/make_appimage.sh --verify-appdir DIR
#
# 策略：全量捆绑（glibc 也进包，经捆绑 ld.so 启动）——目标机无需安装
# Qt/QGIS/GDAL/PROJ 等任何依赖。唯一硬要求是内核与 fuse（或
# --appimage-extract-and-run）。
#
# #956 纪律：
#   1. Qt/QGIS/GDAL/PROJ 优先使用项目 vendored 前缀（vendor/superbuild/prefix
#      或 vendor/prefix/usr），系统 /usr 布局仅在显式 --allow-system-fallback 时允许；
#   2. 对关键 Qt 插件、QGIS provider、GDAL/PROJ 数据执行存在性硬探针，缺任一即失败；
#   3. 所有 ELF 的未解析依赖（bad > 0）立即非零退出，绝不产出带缺依赖的 AppImage。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MULTIARCH="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || echo "$(uname -m)-linux-gnu")"

usage() {
  cat <<'EOF'
Usage:
  tools/make_appimage.sh [BUILD_DIR]
  tools/make_appimage.sh [--build-dir DIR] [--out FILE]
                         [--prefix DIR] [--qt-prefix DIR] [--qgis-prefix DIR]
                         [--allow-system-fallback] [--runtime FILE] [--stage-only]
  tools/make_appimage.sh --verify-appdir DIR

Options:
  --build-dir DIR           Build directory containing 'paleo' (default: <repo>/build)
  --out FILE                Output AppImage path
  --prefix DIR              Unified vendored dependency prefix (overrides auto-detect)
  --qt-prefix DIR           Qt6 prefix override (defaults to --prefix)
  --qgis-prefix DIR         QGIS/GDAL/PROJ prefix override (defaults to --prefix)
  --allow-system-fallback   Explicitly opt in to falling back to /usr when no vendored prefix exists
  --runtime FILE            Local AppImage type2 runtime binary (still sha256-verified)
  --stage-only              Stage and verify AppDir without downloading runtime or running mksquashfs
  --verify-appdir DIR       Verify an existing AppDir (required assets + ELF ldd closure) and exit
  -h, --help                Show this help
EOF
}

is_dynamic_elf() {
  local f="$1"
  [ -f "$f" ] || return 1
  LC_ALL=C readelf -h "$f" 2>/dev/null | grep -qE 'Type:[[:space:]]+(DYN|EXEC)'
}

find_first_dir() {
  local d
  for d in "$@"; do
    if [ -n "$d" ] && [ -d "$d" ]; then
      printf '%s\n' "$d"
      return 0
    fi
  done
  return 1
}

find_first_file() {
  local f
  for f in "$@"; do
    if [ -n "$f" ] && [ -f "$f" ]; then
      printf '%s\n' "$f"
      return 0
    fi
  done
  return 1
}

verify_appdir() {
  local appdir="$1"
  local probe_bad=0
  local bad=0

  echo ">> 校验关键运行时资产探针（$appdir）"
  if [ ! -x "$appdir/usr/bin/paleo" ]; then
    echo "  PROBE MISSING: usr/bin/paleo（主程序缺失或不可执行）" >&2
    probe_bad=$((probe_bad + 1))
  fi

  # 关键 Qt 插件：平台插件（xcb 或 offscreen）+ SQLite 驱动 + WebEngine 进程
  if [ ! -f "$appdir/usr/lib/qt6/plugins/platforms/libqxcb.so" ] && \
     [ ! -f "$appdir/usr/lib/qt6/plugins/platforms/libqoffscreen.so" ]; then
    echo "  PROBE MISSING: usr/lib/qt6/plugins/platforms/libqxcb.so 或 libqoffscreen.so（缺少 Qt 平台插件）" >&2
    probe_bad=$((probe_bad + 1))
  fi
  if [ ! -f "$appdir/usr/lib/qt6/plugins/sqldrivers/libqsqlite.so" ]; then
    echo "  PROBE MISSING: usr/lib/qt6/plugins/sqldrivers/libqsqlite.so（缺少 Qt SQLite 驱动）" >&2
    probe_bad=$((probe_bad + 1))
  fi
  if [ ! -x "$appdir/usr/lib/qt6/QtWebEngineProcess" ] || \
     [ ! -f "$appdir/usr/lib/qt6/QtWebEngineProcess.bin" ]; then
    echo "  PROBE MISSING: usr/lib/qt6/QtWebEngineProcess(.bin)（缺少 QtWebEngineProcess）" >&2
    probe_bad=$((probe_bad + 1))
  fi

  # 关键 QGIS provider 与数据（QGIS 4.2 中 ogr/gdal/memory 为内置，动态 provider 须含 spatialite 与 delimitedtext）
  if [ ! -d "$appdir/usr/lib/qgis/plugins" ]; then
    echo "  PROBE MISSING: usr/lib/qgis/plugins（缺少 QGIS provider 目录）" >&2
    probe_bad=$((probe_bad + 1))
  else
    for prov in libprovider_spatialite.so libprovider_delimitedtext.so; do
      if [ ! -f "$appdir/usr/lib/qgis/plugins/$prov" ]; then
        echo "  PROBE MISSING: usr/lib/qgis/plugins/$prov（缺少关键 QGIS provider）" >&2
        probe_bad=$((probe_bad + 1))
      fi
    done
  fi
  if [ ! -f "$appdir/usr/share/qgis/resources/srs.db" ]; then
    echo "  PROBE MISSING: usr/share/qgis/resources/srs.db（缺少 QGIS 坐标系库）" >&2
    probe_bad=$((probe_bad + 1))
  fi

  # GDAL / PROJ 关键数据
  if [ ! -d "$appdir/usr/share/gdal" ] || [ -z "$(find "$appdir/usr/share/gdal" -maxdepth 2 -type f -print -quit 2>/dev/null)" ]; then
    echo "  PROBE MISSING: usr/share/gdal（缺少 GDAL 数据目录或目录为空）" >&2
    probe_bad=$((probe_bad + 1))
  fi
  if [ ! -f "$appdir/usr/share/proj/proj.db" ]; then
    echo "  PROBE MISSING: usr/share/proj/proj.db（缺少 PROJ 数据库）" >&2
    probe_bad=$((probe_bad + 1))
  fi

  if [ "$probe_bad" -gt 0 ]; then
    echo "error: 关键运行时资产探针失败（共 $probe_bad 项缺失）" >&2
  else
    echo "  关键资产探针全部通过"
  fi

  echo ">> 检查未解析依赖"
  while IFS= read -r f; do
    is_dynamic_elf "$f" || continue
    case "$f" in */ld-linux*) continue ;; esac
    local ldd_out nf ldd_rc
    set +e
    ldd_out="$(LC_ALL=C LD_LIBRARY_PATH="$appdir/usr/lib" ldd "$f" 2>&1)"
    ldd_rc=$?
    set -e
    nf="$(printf '%s\n' "$ldd_out" | grep -c 'not found' || true)"
    if [ "$nf" -gt 0 ]; then
      echo "  MISSING($nf): $f" >&2
      printf '%s\n' "$ldd_out" | grep 'not found' | sed 's/^/    /' >&2
      bad=$((bad + nf))
    elif [ "$ldd_rc" -ne 0 ]; then
      # 含 DT_NEEDED 却 ldd 失败视为异常依赖
      if LC_ALL=C readelf -d "$f" 2>/dev/null | grep -q '(NEEDED)'; then
        echo "  LDD_ERROR($ldd_rc): $f" >&2
        bad=$((bad + 1))
      fi
    fi
  done < <(find "$appdir/usr" -type f)

  if [ "$bad" -eq 0 ]; then
    echo "  无缺失"
  else
    echo "error: 发现 $bad 处未解析 ELF 依赖，中止打包" >&2
  fi

  if [ "$probe_bad" -gt 0 ] || [ "$bad" -gt 0 ]; then
    return 1
  fi
  return 0
}

BUILD=""
OUT=""
PREFIX="${PALEO_APPIMAGE_PREFIX:-}"
QT_PREFIX="${PALEO_QT_PREFIX:-}"
QGIS_PREFIX_ARG="${PALEO_QGIS_PREFIX:-}"
ALLOW_SYSTEM="${PALEO_APPIMAGE_ALLOW_SYSTEM:-0}"
RUNTIME_ARG="${PALEO_APPIMAGE_RUNTIME:-}"
STAGE_ONLY=0
VERIFY_APPDIR=""

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir)
      [ $# -ge 2 ] || { echo "error: --build-dir requires an argument" >&2; exit 2; }
      BUILD="$2"; shift 2 ;;
    --out)
      [ $# -ge 2 ] || { echo "error: --out requires an argument" >&2; exit 2; }
      OUT="$2"; shift 2 ;;
    --prefix)
      [ $# -ge 2 ] || { echo "error: --prefix requires an argument" >&2; exit 2; }
      PREFIX="$2"; shift 2 ;;
    --qt-prefix)
      [ $# -ge 2 ] || { echo "error: --qt-prefix requires an argument" >&2; exit 2; }
      QT_PREFIX="$2"; shift 2 ;;
    --qgis-prefix)
      [ $# -ge 2 ] || { echo "error: --qgis-prefix requires an argument" >&2; exit 2; }
      QGIS_PREFIX_ARG="$2"; shift 2 ;;
    --allow-system-fallback)
      ALLOW_SYSTEM=1; shift ;;
    --runtime)
      [ $# -ge 2 ] || { echo "error: --runtime requires an argument" >&2; exit 2; }
      RUNTIME_ARG="$2"; shift 2 ;;
    --stage-only)
      STAGE_ONLY=1; shift ;;
    --verify-appdir)
      [ $# -ge 2 ] || { echo "error: --verify-appdir requires an argument" >&2; exit 2; }
      VERIFY_APPDIR="$2"; shift 2 ;;
    -h|--help)
      usage; exit 0 ;;
    -*)
      echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)
      if [ -z "$BUILD" ]; then
        BUILD="$1"; shift
      else
        echo "error: unexpected positional argument: $1" >&2; usage >&2; exit 2
      fi ;;
  esac
done

if [ -n "$VERIFY_APPDIR" ]; then
  [ -d "$VERIFY_APPDIR" ] || { echo "error: AppDir 不存在：$VERIFY_APPDIR" >&2; exit 1; }
  verify_appdir "$VERIFY_APPDIR"
  exit $?
fi

BUILD="${BUILD:-$ROOT/build}"
BIN="$BUILD/paleo"
WORK="$BUILD/appimage"
APPDIR="$WORK/PaleoWorkstation.AppDir"
OUT="${OUT:-$WORK/PaleoWorkstation-x86_64.AppImage}"
RUNTIME="${RUNTIME_ARG:-$WORK/runtime-x86_64}"

# ---- 解析 Qt / QGIS 前缀（优先 vendored，系统 /usr 仅显式兜底）----------
if [ -z "$PREFIX" ]; then
  if [ -n "${QGIS_PREFIX_PATH:-}" ] && [ -d "${QGIS_PREFIX_PATH}" ]; then
    PREFIX="$QGIS_PREFIX_PATH"
  elif [ -d "$ROOT/vendor/superbuild/prefix" ]; then
    PREFIX="$ROOT/vendor/superbuild/prefix"
  elif [ -d "$ROOT/vendor/prefix/usr" ]; then
    PREFIX="$ROOT/vendor/prefix/usr"
  elif [ "$ALLOW_SYSTEM" = "1" ]; then
    PREFIX="/usr"
  else
    echo "error: 未找到项目 vendored 依赖前缀（vendor/superbuild/prefix 或 vendor/prefix/usr）。" >&2
    echo "       请先运行 ./paleo-dev bootstrap fetch-only，或显式传 --prefix/--qt-prefix/--qgis-prefix，" >&2
    echo "       或仅在明确知情时传入 --allow-system-fallback 使用系统 /usr 布局。" >&2
    exit 1
  fi
fi

QT_PREFIX="${QT_PREFIX:-$PREFIX}"
QGIS_PREFIX_DIR="${QGIS_PREFIX_ARG:-$PREFIX}"

[ -d "$QT_PREFIX" ] || { echo "error: Qt 前缀不存在：$QT_PREFIX" >&2; exit 1; }
[ -d "$QGIS_PREFIX_DIR" ] || { echo "error: QGIS 前缀不存在：$QGIS_PREFIX_DIR" >&2; exit 1; }
echo ">> 依赖前缀：Qt=$QT_PREFIX  QGIS=$QGIS_PREFIX_DIR"

[ -x "$BIN" ] || { echo "error: $BIN 不存在，先编译" >&2; exit 1; }

if [ "$STAGE_ONLY" -eq 0 ]; then
  # mksquashfs：系统优先，其次 Ubuntu deb 解出的本地副本
  MKSQUASHFS="${MKSQUASHFS:-}"
  if [ -z "$MKSQUASHFS" ]; then
    if command -v mksquashfs >/dev/null; then
      MKSQUASHFS="$(command -v mksquashfs)"
    elif [ -x /tmp/appimage-tools/sqt/usr/bin/mksquashfs ]; then
      MKSQUASHFS=/tmp/appimage-tools/sqt/usr/bin/mksquashfs
    else
      echo "error: mksquashfs not found" >&2; exit 1
    fi
  fi

  # AppImage type2 runtime（纯 ELF，直接下载）。
  # FE-003：钉具体 release 资产 + sha256 校验——runtime 是 AppImage 里第一段
  # 被执行的代码，可变 tag（continuous）+ 无校验的下载与本仓「actions 钉
  # SHA、vendored 依赖钉哈希」的供应链纪律相悖。升级时同步换 URL 与摘要。
  RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
  RUNTIME_SHA256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"
  # #540-9：sha256sum -c 提到「文件不存在或不可执行」分支之外无条件执行——
  # 复用路径（build/appimage/runtime-x86_64 已存在且带 x 位）此前永不与新 pin
  # 对账，改 pin 值后旧 runtime 会被静默打进 AppImage。缺文件先下载，随后
  # 无论下载还是复用都过同一道校验。
  mkdir -p "$(dirname "$RUNTIME")"
  if [ ! -x "$RUNTIME" ]; then
    echo ">> 下载 AppImage runtime（type2-runtime 20251108，sha256 钉版）"
    curl -fL --retry 3 -o "$RUNTIME" "$RUNTIME_URL"
    chmod +x "$RUNTIME"
  fi
  echo "$RUNTIME_SHA256  $RUNTIME" | sha256sum -c
fi

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share"
cp "$BIN" "$APPDIR/usr/bin/paleo"

# ---- 共享库闭包 -------------------------------------------------------
# 优先从 AppDir 与 vendored 前缀搜索共享库
SEARCH_LIB_DIRS=("$APPDIR/usr/lib")
for cand in \
  "$QT_PREFIX/lib/$MULTIARCH" "$QT_PREFIX/lib" \
  "$QGIS_PREFIX_DIR/lib/$MULTIARCH" "$QGIS_PREFIX_DIR/lib" \
  "$ROOT/vendor/onnxruntime/lib"; do
  [ -d "$cand" ] && SEARCH_LIB_DIRS+=("$cand")
done
for grass_lib in "$QGIS_PREFIX_DIR"/lib/grass*/lib; do
  [ -d "$grass_lib" ] && SEARCH_LIB_DIRS+=("$grass_lib")
done
SEARCH_LD_PATH="$(IFS=:; echo "${SEARCH_LIB_DIRS[*]}")"

# 不打包的：内核接口 + GPU 驱动桥（须与目标机驱动一致）
EXCLUDE='linux-vdso|ld-linux|ld-musl|libGLX\.so|libOpenGL\.so|libGL\.so|libEGL\.so|libGLES|libvulkan|libdrm\.so|libgbm|libglapi|libnvidia|libxcb-dri|libwayland-egl|libva\.|libva-|libXss'

collect_deps() {
  # 对 AppDir 内所有 ELF 做 ldd，凡解析到 AppDir 之外且不在排除表的，拷进 usr/lib。
  # 迭代至不动点（插件 deps → 新库的 deps……）。
  local pass=0 missing=1
  while [ "$missing" -gt 0 ] && [ $((pass+=1)) -le 12 ]; do
    missing=0
    while IFS= read -r elf; do
      is_dynamic_elf "$elf" || continue
      case "$elf" in */ld-linux*) continue ;; esac
      while read -r lib; do
        case "$lib" in "$APPDIR"*) continue ;; esac
        [ -f "$lib" ] || continue
        name="$(basename "$lib")"
        echo "$name" | grep -Eq "$EXCLUDE" && continue
        [ -f "$APPDIR/usr/lib/$name" ] && continue
        cp -L "$lib" "$APPDIR/usr/lib/$name" && missing=$((missing+1))
      done < <(LC_ALL=C LD_LIBRARY_PATH="$SEARCH_LD_PATH" ldd "$elf" 2>/dev/null \
               | grep -oE '/[^ ]+\.so[^ ]*' | sort -u)
    done < <(find "$APPDIR/usr" -type f 2>/dev/null)
  done
  echo ">> 依赖闭包完成（$pass 趟，本轮新增 $missing）"
}

# glibc 全量捆绑（ld.so 启动器方案）：优先按前缀/multiarch/标准目录定位，
# 不再硬编码 /usr/lib（Debian/Ubuntu multiarch 在 /lib/x86_64-linux-gnu）。
GLIBC_DIR=""
for cand in "$PREFIX/lib/$MULTIARCH" "/lib/$MULTIARCH" "/usr/lib/$MULTIARCH" "/usr/lib"; do
  if [ -f "$cand/libc.so.6" ]; then
    GLIBC_DIR="$cand"
    break
  fi
done
if [ -n "$GLIBC_DIR" ]; then
  shopt -s nullglob
  glibc_files=(
    "$GLIBC_DIR"/ld-linux-x86-64.so.2 "$GLIBC_DIR"/ld-*.so
    "$GLIBC_DIR"/libc.so.6 "$GLIBC_DIR"/libc-*.so
    "$GLIBC_DIR"/libm.so.6 "$GLIBC_DIR"/libm-*.so
    "$GLIBC_DIR"/libmvec.so.1 "$GLIBC_DIR"/libmvec-*.so
    "$GLIBC_DIR"/libdl.so.2 "$GLIBC_DIR"/librt.so.1
    "$GLIBC_DIR"/libpthread.so.0 "$GLIBC_DIR"/libutil.so.1
    "$GLIBC_DIR"/libresolv.so.2 "$GLIBC_DIR"/libanl.so.1
    "$GLIBC_DIR"/libnsl.so.* "$GLIBC_DIR"/libnss_*.so*
  )
  shopt -u nullglob
  if [ ${#glibc_files[@]} -gt 0 ]; then
    cp -L "${glibc_files[@]}" "$APPDIR/usr/lib/" 2>/dev/null || true
  fi
fi

echo ">> 收集 ELF 依赖闭包"
collect_deps

# ---- Qt / QGIS / GDAL / PROJ 运行时资源 --------------------------------
echo ">> 拷贝 Qt 插件"
QTPLUG="$(find_first_dir \
  "$QT_PREFIX/lib/$MULTIARCH/qt6/plugins" \
  "$QT_PREFIX/lib/qt6/plugins" \
  "$QT_PREFIX/plugins" || true)"
if [ -z "$QTPLUG" ]; then
  echo "error: 在 Qt 前缀 $QT_PREFIX 下找不到 qt6/plugins" >&2
  exit 1
fi
mkdir -p "$APPDIR/usr/lib/qt6/plugins"
for d in platforms platformthemes platforminputcontexts imageformats sqldrivers \
         iconengines styles tls networkinformation xcbglintegrations \
         egldeviceintegrations generic position multimedia printsupport \
         geoservices crypto \
         wayland-decoration-client wayland-graphics-integration-client \
         wayland-shell-integration; do
  [ -d "$QTPLUG/$d" ] && cp -a "$QTPLUG/$d" "$APPDIR/usr/lib/qt6/plugins/"
done
# Debian multiarch 下 QCA 插件位于 lib/<multiarch>/qca-qt6/crypto
QCA_CRYPTO="$(find_first_dir \
  "$QT_PREFIX/lib/$MULTIARCH/qca-qt6/crypto" \
  "$QT_PREFIX/lib/qca-qt6/crypto" || true)"
if [ -n "$QCA_CRYPTO" ] && [ ! -d "$APPDIR/usr/lib/qt6/plugins/crypto" ]; then
  cp -a "$QCA_CRYPTO" "$APPDIR/usr/lib/qt6/plugins/crypto"
fi

echo ">> 拷贝 WebEngine 进程与资源"
QTWEBENGINE_BIN="$(find_first_file \
  "$QT_PREFIX/lib/qt6/libexec/QtWebEngineProcess" \
  "$QT_PREFIX/lib/$MULTIARCH/qt6/libexec/QtWebEngineProcess" \
  "$QT_PREFIX/lib/qt6/QtWebEngineProcess" \
  "$QT_PREFIX/libexec/QtWebEngineProcess" || true)"
if [ -z "$QTWEBENGINE_BIN" ]; then
  echo "error: 在 Qt 前缀 $QT_PREFIX 下找不到 QtWebEngineProcess" >&2
  exit 1
fi
mkdir -p "$APPDIR/usr/lib/qt6"
cp -L "$QTWEBENGINE_BIN" "$APPDIR/usr/lib/qt6/QtWebEngineProcess.bin"
cat > "$APPDIR/usr/lib/qt6/QtWebEngineProcess" <<'WEOF'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "$0")")"
ROOT="$(readlink -f "$HERE/../..")"
exec "$ROOT/lib/ld-linux-x86-64.so.2" \
  --library-path "$ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$HERE/QtWebEngineProcess.bin" "$@"
WEOF
chmod +x "$APPDIR/usr/lib/qt6/QtWebEngineProcess"
mkdir -p "$APPDIR/usr/share/qt6"
[ -d "$QT_PREFIX/share/qt6/resources" ] && cp -a "$QT_PREFIX/share/qt6/resources" "$APPDIR/usr/share/qt6/"
[ -d "$QT_PREFIX/share/qt6/translations" ] && cp -a "$QT_PREFIX/share/qt6/translations" "$APPDIR/usr/share/qt6/"

echo ">> 拷贝 QGIS 提供商/认证/数据"
QGIS_PLUGINS="$(find_first_dir \
  "$QGIS_PREFIX_DIR/lib/qgis/plugins" \
  "$QGIS_PREFIX_DIR/lib/$MULTIARCH/qgis/plugins" || true)"
if [ -z "$QGIS_PLUGINS" ]; then
  echo "error: 在 QGIS 前缀 $QGIS_PREFIX_DIR 下找不到 lib/qgis/plugins" >&2
  exit 1
fi
mkdir -p "$APPDIR/usr/lib/qgis"
cp -a "$QGIS_PLUGINS" "$APPDIR/usr/lib/qgis/plugins"

QGIS_SHARE="$QGIS_PREFIX_DIR/share/qgis"
if [ ! -d "$QGIS_SHARE/resources" ]; then
  echo "error: 在 QGIS 前缀 $QGIS_PREFIX_DIR 下找不到 share/qgis/resources" >&2
  exit 1
fi
mkdir -p "$APPDIR/usr/share/qgis"
for d in resources svg images; do
  [ -d "$QGIS_SHARE/$d" ] && cp -a "$QGIS_SHARE/$d" "$APPDIR/usr/share/qgis/"
done
# i18n 全量 141M——只带中文与英文
mkdir -p "$APPDIR/usr/share/qgis/i18n"
if [ -d "$QGIS_SHARE/i18n" ]; then
  find "$QGIS_SHARE/i18n" -maxdepth 2 \( -name 'qgis_zh*.qm' -o -name 'qgis_en*.qm' \) -exec cp -a {} "$APPDIR/usr/share/qgis/i18n/" \;
fi

echo ">> 拷贝 GDAL 插件/数据、PROJ 数据"
GDAL_PLUGINS="$(find_first_dir \
  "$QGIS_PREFIX_DIR/lib/$MULTIARCH/gdalplugins" \
  "$QGIS_PREFIX_DIR/lib/gdalplugins" || true)"
if [ -n "$GDAL_PLUGINS" ]; then
  cp -a "$GDAL_PLUGINS" "$APPDIR/usr/lib/gdalplugins"
else
  mkdir -p "$APPDIR/usr/lib/gdalplugins"
fi

GDAL_SHARE="$QGIS_PREFIX_DIR/share/gdal"
if [ ! -d "$GDAL_SHARE" ]; then
  echo "error: 在前缀 $QGIS_PREFIX_DIR 下找不到 share/gdal" >&2
  exit 1
fi
cp -a "$GDAL_SHARE" "$APPDIR/usr/share/gdal"

PROJ_SHARE="$QGIS_PREFIX_DIR/share/proj"
if [ ! -d "$PROJ_SHARE" ]; then
  echo "error: 在前缀 $QGIS_PREFIX_DIR 下找不到 share/proj" >&2
  exit 1
fi
cp -a "$PROJ_SHARE" "$APPDIR/usr/share/proj"

# ---- Python 环境（MAMCL venv）------------------------------------------
# 应用侧约定（src/services/pythonenv.cpp）：envroot=<AppDataLocation>/
# external/mamcl，base/<bin/python3> 优先于 PALEO_PYTHON/PATH；venvReady 校验
# pyvenv.cfg 的 home == base/bin canonical。AppDir 内放只读源，首启由
# AppRun 落到真实 envroot（base/pkg/bin/cfg 实体拷贝 + lib 软链）。
PYEXT="$APPDIR/usr/share/paleo-external/mamcl"
# 预制树（PALEO_PYEXT_SRC=<dir>）：目录须含 base/（完整 python 前缀：
# bin/python3 + lib/python3.x + libpython*.so）+ venv/mamcl + pkg/。
# 未提供时退回本机已落地的 envroot + 系统 python。
PYEXT_SRC="${PALEO_PYEXT_SRC:-}"
ENVROOT_SRC="${PALEO_MAMCL_ROOT:-$HOME/.local/share/paleo/paleo_workbench/external/mamcl}"
if [ -n "$PYEXT_SRC" ] && [ -x "$PYEXT_SRC/base/bin/python3" ]; then
  echo ">> 打包 Python 环境（预制树 $PYEXT_SRC）"
  mkdir -p "$PYEXT"
  for d in base venv pkg; do
    [ -e "$PYEXT_SRC/$d" ] && cp -a "$PYEXT_SRC/$d" "$PYEXT/"
  done
elif [ -x "$ENVROOT_SRC/venv/mamcl/bin/python" ]; then
  echo ">> 打包 Python 环境（base + venv + pkg）"
  mkdir -p "$PYEXT/base/bin" "$PYEXT/base/lib" "$PYEXT/base/include"
  cp -L "$(readlink -f "$ENVROOT_SRC/venv/mamcl/bin/python3")" "$PYEXT/base/bin/python3"
  PYV="$(readlink -f "$ENVROOT_SRC/venv/mamcl/bin/python3" | xargs basename | sed 's/python//')"
  cp -a "/usr/lib/python$PYV" "$PYEXT/base/lib/" 2>/dev/null || true
  # 发行版 site-packages 是系统包的地盘，venv 用不到（include-system-site-packages=false）
  rm -rf "$PYEXT/base/lib/python$PYV/site-packages"
  find "$PYEXT/base/lib/python$PYV" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true
  cp -a "/usr/include/python$PYV" "$PYEXT/base/include/" 2>/dev/null || true
  mkdir -p "$PYEXT/venv"
  cp -a "$ENVROOT_SRC/venv/mamcl" "$PYEXT/venv/"
  [ -d "$ENVROOT_SRC/pkg" ] && cp -a "$ENVROOT_SRC/pkg" "$PYEXT/pkg"
else
  echo ">> 警告：$ENVROOT_SRC 下没有可用 venv，跳过 Python 环境打包"
fi

# onnxruntime 等 vendor 库已由依赖闭包带进；这里兜底一次
echo ">> 第二趟依赖闭包（插件/provider 的 deps）"
collect_deps

# ---- RPATH -------------------------------------------------------------
echo ">> 修 RPATH 为 \$ORIGIN 相对"
# cp -a 保留源权限，venv/pip 产物常是只读——patchelf 前统一加 u+w
find "$APPDIR/usr" -type f ! -perm -u+w -exec chmod u+w {} +
rpath_to_lib() {  # 计算文件所在目录到 usr/lib 的相对路径 → $ORIGIN 前缀
  local rel
  rel="$(realpath --relative-to="$(dirname "$1")" "$APPDIR/usr/lib")"
  [ "$rel" = "." ] && echo '$ORIGIN' || echo "\$ORIGIN/$rel"
}
find "$APPDIR/usr" -type f | while IFS= read -r f; do
  is_dynamic_elf "$f" || continue
  case "$f" in */ld-linux*) continue ;; esac
  case "$f" in */usr/bin/paleo)
    patchelf --set-rpath '$ORIGIN/../lib' "$f" ;;
  */share/paleo-external/*)
    # venv/site-packages 自带 $ORIGIN 相对 rpath（torch/nvidia 轮子）——追加不替换
    patchelf --add-rpath "$(rpath_to_lib "$f")" "$f" ;;
  *)
    patchelf --set-rpath "$(rpath_to_lib "$f")" "$f" ;;
  esac
done

# ---- qt.conf / desktop / AppRun ----------------------------------------
cat > "$APPDIR/usr/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Plugins = lib/qt6/plugins
LibraryExecutables = lib/qt6
Translations = share/qt6/translations
Data = share/qt6
EOF

mkdir -p "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cat > "$APPDIR/paleo.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Paleo Workstation
Comment=Geological workstation — well logs, facies prediction, QGIS mapping
Exec=paleo
Icon=paleo
Categories=Science;Geology;
Terminal=false
EOF
cp "$APPDIR/paleo.desktop" "$APPDIR/usr/share/applications/"
ICON="$ROOT/tools/appimage-icon.png"
if [ -f "$ICON" ]; then
  cp "$ICON" "$APPDIR/paleo.png"
  cp "$ICON" "$APPDIR/usr/share/icons/hicolor/256x256/apps/paleo.png"
  cp "$ICON" "$APPDIR/.DirIcon"
fi

cat > "$APPDIR/AppRun" <<'EOF'
#!/usr/bin/env bash
HERE="$(dirname "$(readlink -f "$0")")"
export PALEO_APPDIR="$HERE"

# ---- Python 环境落位 ----------------------------------------------------
# 源树在包内 usr/share/paleo-external/mamcl；真实 envroot 用 base/pkg/bin/
# cfg 实体拷贝（路径须确定以满足 venvReady 的 home==canonical(base/bin) 校验）。
# venv/mamcl/lib 等大体量目录软链回包内——AppImage 挂载点每次运行都变，
# 因此软链每次启动都重新指向当前 $HERE（幂等、便宜）。
PYEXT="$HERE/usr/share/paleo-external/mamcl"
if [ -x "$PYEXT/base/bin/python3" ]; then
  ENVROOT="${XDG_DATA_HOME:-$HOME/.local/share}/paleo/paleo_workbench/external/mamcl"
  if [ ! -x "$ENVROOT/venv/mamcl/bin/python" ]; then
    mkdir -p "$ENVROOT/venv/mamcl/bin"
    (
      set -e
      [ -d "$ENVROOT/base/bin" ] || cp -a "$PYEXT/base" "$ENVROOT/base"
      if [ -d "$PYEXT/pkg" ] && [ ! -d "$ENVROOT/pkg" ]; then
        cp -a "$PYEXT/pkg" "$ENVROOT/pkg"
      fi
      # base/bin/python3 可能是软链（standalone 树里 python3→python3.12）：
      # 真实 ELF 改 .bin 后缀，原位置放 ld.so 包装器——版本名自适应
      real="$(basename "$(readlink -f "$ENVROOT/base/bin/python3")")"
      [ -f "$ENVROOT/base/bin/$real.bin" ] || \
        mv "$ENVROOT/base/bin/$real" "$ENVROOT/base/bin/$real.bin"
      cat > "$ENVROOT/base/bin/$real" <<'PYW'
#!/usr/bin/env bash
BIN="$(basename "$(readlink -f "$0")")"
SELF="$(dirname "$(readlink -f "$0")")"
exec "$PALEO_APPDIR/usr/lib/ld-linux-x86-64.so.2" \
  --argv0 "$0" \
  --library-path "$PALEO_APPDIR/usr/lib:$SELF/../lib" \
  "$SELF/$BIN.bin" "$@"
PYW
      chmod +x "$ENVROOT/base/bin/$real"
      ln -sf "$real" "$ENVROOT/base/bin/python3"
      ln -sf "$real" "$ENVROOT/base/bin/python"
      # venv 入口脚本：实体拷贝 + shebang 改指本 envroot 的 venv python
      for s in "$PYEXT/venv/mamcl/bin"/*; do
        n="$(basename "$s")"
        case "$n" in python|python3|python3.*) continue;; esac
        if head -c2 "$s" 2>/dev/null | grep -q '#!'; then
          sed "1s|^#!.*|#!$ENVROOT/venv/mamcl/bin/python|" "$s" \
            > "$ENVROOT/venv/mamcl/bin/$n"
          chmod +x "$ENVROOT/venv/mamcl/bin/$n"
        else
          cp -a "$s" "$ENVROOT/venv/mamcl/bin/$n"
        fi
      done
      ln -sf ../../../base/bin/python3 "$ENVROOT/venv/mamcl/bin/python"
      ln -sf ../../../base/bin/python3 "$ENVROOT/venv/mamcl/bin/python3"
      PYVER="$(awk -F= '/^version/{gsub(/ /,"",$2); print $2}' \
                 "$PYEXT/venv/mamcl/pyvenv.cfg" 2>/dev/null)"
      [ -n "$PYVER" ] && \
        ln -sf ../../../base/bin/python3 \
          "$ENVROOT/venv/mamcl/bin/python${PYVER%.*}"
      cat > "$ENVROOT/venv/mamcl/pyvenv.cfg" <<CFG
home = $ENVROOT/base/bin
include-system-site-packages = false
version = ${PYVER:-3.12}
executable = $ENVROOT/base/bin/python3
CFG
    ) || echo "paleo: python env seeding failed (non-fatal)" >&2
  fi
  # 每次启动刷新指向包内的大件软链（挂载点随运行变化）；不碰真实目录
  if [ -d "$ENVROOT/venv/mamcl" ]; then
    for e in "$PYEXT/venv/mamcl"/* "$PYEXT/venv/mamcl"/.[!.]*; do
      [ -e "$e" ] || continue
      n="$(basename "$e")"
      case "$n" in bin|pyvenv.cfg|.gitignore) continue;; esac
      d="$ENVROOT/venv/mamcl/$n"
      { [ -L "$d" ] || [ ! -e "$d" ]; } && ln -sfn "$e" "$d"
    done
  fi
  [ -x "$ENVROOT/base/bin/python3" ] && export PALEO_PYTHON="$ENVROOT/base/bin/python3"
fi

export QGIS_PREFIX_PATH="$HERE/usr"
export GDAL_DATA="$HERE/usr/share/gdal"
export PROJ_DATA="$HERE/usr/share/proj"
export PROJ_LIB="$HERE/usr/share/proj"
export GDAL_DRIVER_PATH="$HERE/usr/lib/gdalplugins"
export QCA_PLUGIN_PATH="$HERE/usr/lib/qt6/plugins/crypto"
export QT_PLUGIN_PATH="$HERE/usr/lib/qt6/plugins"
export QTWEBENGINEPROCESS_PATH="$HERE/usr/lib/qt6/QtWebEngineProcess"
export QTWEBENGINE_RESOURCES_PATH="$HERE/usr/share/qt6/resources"
export QTWEBENGINE_LOCALES_PATH="$HERE/usr/share/qt6/translations/qtwebengine_locales"
LIBS="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ -x "$HERE/usr/lib/ld-linux-x86-64.so.2" ]; then
  exec "$HERE/usr/lib/ld-linux-x86-64.so.2" --argv0 "$HERE/usr/bin/paleo" \
       --library-path "$LIBS" "$HERE/usr/bin/paleo" "$@"
fi
exec "$HERE/usr/bin/paleo" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# 后端依赖系统不存在的死重插件（在源机/目标机同样无法加载）——剔除
for f in usr/lib/gdalplugins/ogr_MySQL.so usr/lib/gdalplugins/gdal_HEIF.so \
         usr/lib/gdalplugins/gdal_PDF.so usr/lib/qt6/plugins/sqldrivers/libqsqlmysql.so \
         usr/lib/qt6/plugins/sqldrivers/libqsqlibase.so usr/lib/qt6/plugins/crypto/libqca-botan.so \
         usr/lib/qt6/plugins/imageformats/kimg_heif.so usr/lib/qt6/plugins/imageformats/kimg_jxr.so; do
  rm -f "$APPDIR/$f"
done

# ---- 验证（任一资产探针缺失或未解析依赖 > 0 必须非零退出）--------------
verify_appdir "$APPDIR"

if [ "$STAGE_ONLY" -eq 1 ]; then
  echo ">> --stage-only 完成：$APPDIR"
  exit 0
fi

echo ">> 打包 squashfs + runtime → $OUT"
"$MKSQUASHFS" "$APPDIR" "$WORK/paleo.sqfs" -root-owned -noappend -comp zstd -Xcompression-level 15 >/dev/null
cat "$RUNTIME" "$WORK/paleo.sqfs" > "$OUT"
chmod +x "$OUT"
ls -lh "$OUT"
