#!/usr/bin/env bash
# Paleo Workstation AppImage 打包脚本
# 用法：tools/make_appimage.sh [build 目录]
# 产出：build/appimage/PaleoWorkstation-x86_64.AppImage
#
# 策略：全量捆绑（glibc 也进包，经捆绑 ld.so 启动）——目标机无需安装
# Qt/QGIS/GDAL/PROJ 等任何依赖。唯一硬要求是内核与 fuse（或
# --appimage-extract-and-run）。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
BIN="$BUILD/paleo"
WORK="$BUILD/appimage"
APPDIR="$WORK/PaleoWorkstation.AppDir"
OUT="$WORK/PaleoWorkstation-x86_64.AppImage"
RUNTIME="$WORK/runtime-x86_64"

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
[ -x "$BIN" ] || { echo "error: $BIN 不存在，先编译" >&2; exit 1; }

# AppImage type2 runtime（纯 ELF，直接下载）
if [ ! -x "$RUNTIME" ]; then
  echo ">> 下载 AppImage runtime"
  curl -fL --retry 3 -o "$RUNTIME" \
    https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
  chmod +x "$RUNTIME"
fi

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/share"
cp "$BIN" "$APPDIR/usr/bin/paleo"

# ---- 共享库闭包 -------------------------------------------------------
# 不打包的：内核接口 + GPU 驱动桥（须与目标机驱动一致）
EXCLUDE='linux-vdso|ld-linux|ld-musl|libGLX\.so|libOpenGL\.so|libGL\.so|libEGL\.so|libGLES|libvulkan|libdrm\.so|libgbm|libglapi|libnvidia|libxcb-dri|libwayland-egl|libva\.|libva-|libXss'

collect_deps() {
  # 对 AppDir 内所有 ELF 做 ldd，凡解析到 AppDir 之外且不在排除表的，拷进 usr/lib。
  # 迭代至不动点（插件 deps → 新库的 deps……）。
  local pass=0 missing=1
  while [ "$missing" -gt 0 ] && [ $((pass+=1)) -le 12 ]; do
    missing=0
    while IFS= read -r elf; do
      while read -r lib; do
        case "$lib" in "$APPDIR"*) continue;; esac
        name="$(basename "$lib")"
        echo "$name" | grep -Eq "$EXCLUDE" && continue
        [ -f "$APPDIR/usr/lib/$name" ] && continue
        cp -L "$lib" "$APPDIR/usr/lib/$name" && missing=$((missing+1))
      done < <(LD_LIBRARY_PATH="$APPDIR/usr/lib" ldd "$elf" 2>/dev/null \
               | grep -oE '/[^ ]+\.so[^ ]*' | sort -u)
    done < <(find "$APPDIR/usr" -type f \( -name '*.so' -o -name '*.so.*' -o -name paleo -o -name '*.bin' -o -name 'python3*' \) 2>/dev/null)
  done
  echo ">> 依赖闭包完成（$pass 趟，本轮新增 $missing）"
}

# glibc 全量捆绑（ld.so 启动器方案）：让包在旧发行版上也能跑
cp -L /usr/lib/ld-linux-x86-64.so.2 /usr/lib/ld-*.so /usr/lib/libc.so.6 \
      /usr/lib/libc-*.so /usr/lib/libm.so.6 /usr/lib/libm-*.so \
      /usr/lib/libmvec.so.1 /usr/lib/libmvec-*.so /usr/lib/libdl.so.2 \
      /usr/lib/librt.so.1 /usr/lib/libpthread.so.0 /usr/lib/libutil.so.1 \
      /usr/lib/libresolv.so.2 /usr/lib/libanl.so.1 \
      "$APPDIR/usr/lib/" 2>/dev/null || true
cp -L /usr/lib/libnsl.so.* /usr/lib/libnss_*.so* "$APPDIR/usr/lib/" 2>/dev/null || true

echo ">> 收集 ELF 依赖闭包"
collect_deps

# ---- Qt / QGIS / GDAL / PROJ 运行时资源 --------------------------------
echo ">> 拷贝 Qt 插件"
QTPLUG=/usr/lib/qt6/plugins
mkdir -p "$APPDIR/usr/lib/qt6/plugins"
for d in platforms platformthemes platforminputcontexts imageformats sqldrivers \
         iconengines styles tls networkinformation xcbglintegrations \
         egldeviceintegrations generic position multimedia printsupport \
         geoservices crypto \
         wayland-decoration-client wayland-graphics-integration-client \
         wayland-shell-integration; do
  [ -d "$QTPLUG/$d" ] && cp -a "$QTPLUG/$d" "$APPDIR/usr/lib/qt6/plugins/"
done

echo ">> 拷贝 WebEngine 进程与资源"
mkdir -p "$APPDIR/usr/lib/qt6"
cp -L /usr/lib/qt6/QtWebEngineProcess "$APPDIR/usr/lib/qt6/QtWebEngineProcess.bin"
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
[ -d /usr/share/qt6/resources ] && cp -a /usr/share/qt6/resources "$APPDIR/usr/share/qt6/"
[ -d /usr/share/qt6/translations ] && cp -a /usr/share/qt6/translations "$APPDIR/usr/share/qt6/"

echo ">> 拷贝 QGIS 提供商/认证/数据"
mkdir -p "$APPDIR/usr/lib/qgis"
cp -a /usr/lib/qgis/plugins "$APPDIR/usr/lib/qgis/"
mkdir -p "$APPDIR/usr/share/qgis"
for d in resources svg images; do
  [ -d "/usr/share/qgis/$d" ] && cp -a "/usr/share/qgis/$d" "$APPDIR/usr/share/qgis/"
done
# i18n 全量 141M——只带中文与英文
mkdir -p "$APPDIR/usr/share/qgis/i18n"
find /usr/share/qgis/i18n -name 'qgis_zh*.qm' -o -name 'qgis_en*.qm' | while read -r f; do cp "$f" "$APPDIR/usr/share/qgis/i18n/"; done

echo ">> 拷贝 GDAL 插件/数据、PROJ 数据"
cp -a /usr/lib/gdalplugins "$APPDIR/usr/lib/"
cp -a /usr/share/gdal "$APPDIR/usr/share/"
cp -a /usr/share/proj "$APPDIR/usr/share/"

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
  cp -a "/usr/lib/python$PYV" "$PYEXT/base/lib/" 2>/dev/null
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
  file "$f" | grep -q 'ELF' || continue
  # patchelf 只能改 DYN/EXEC——.o 等 ET_REL 跳过；ld-linux 会被写坏也跳过
  readelf -h "$f" 2>/dev/null | grep -qE 'Type:[[:space:]]+(DYN|EXEC)' || continue
  case "$f" in */ld-linux*) continue;; esac
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
cp "$ICON" "$APPDIR/paleo.png"
cp "$ICON" "$APPDIR/usr/share/icons/hicolor/256x256/apps/paleo.png"
cp "$ICON" "$APPDIR/.DirIcon"

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

# ---- 验证 -------------------------------------------------------------
echo ">> 检查未解析依赖"
bad=0
while IFS= read -r f; do
  file "$f" | grep -q 'ELF' || continue
  nf=$(LD_LIBRARY_PATH="$APPDIR/usr/lib" ldd "$f" 2>/dev/null | grep -c 'not found' || true)
  [ "$nf" -gt 0 ] && { echo "  MISSING($nf): $f"; bad=$((bad+nf)); }
done < <(find "$APPDIR/usr" -type f \( -name '*.so*' -o -name '*.bin' -o -name paleo \))
[ "$bad" -eq 0 ] && echo "  无缺失" || echo "  共 $bad 处缺失"

echo ">> 打包 squashfs + runtime → $OUT"
"$MKSQUASHFS" "$APPDIR" "$WORK/paleo.sqfs" -root-owned -noappend -comp zstd -Xcompression-level 15 >/dev/null
cat "$RUNTIME" "$WORK/paleo.sqfs" > "$OUT"
chmod +x "$OUT"
ls -lh "$OUT"
