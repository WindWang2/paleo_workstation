#!/usr/bin/env bash
# vendor/fetch-libreoffice.sh — pinned official LibreOffice tarball -> vendor/libreoffice.
#
# 仅用于显式调用的旧转换 API；Office 预览已经改用 Calligra。
# 不走 deb 闭包：官方 tarball 全自含（${ORIGIN} 相对寻址、glibc 基线老、不依赖
# 发行版库集），且不依赖 apt 宿主重解闭包。pin 在 vendor/manifest.json 的
# deps.libreoffice（url + archive.sha256/size_bytes；MPL-2.0 允许二进制再分发，
# 解出的 LICENSE/NOTICE 一并保留在 vendor/libreoffice/ 顶层）。
#
# Usage:
#   ./vendor/fetch-libreoffice.sh [--print-only] [--force]
#   --print-only  打印 pin 与将解包的成员清单，不下载
#   --force       vendor/libreoffice 已存在也重取（升级 pin 后强制重建）
set -euo pipefail
cd "$(dirname "$0")/.."

LOG_DIR=vendor/logs; mkdir -p "$LOG_DIR"
CACHE=vendor/cache/lo
PREFIX=vendor/libreoffice
PRINT_ONLY=0
FORCE=0
for arg in "$@"; do
  case "$arg" in
    --print-only) PRINT_ONLY=1 ;;
    --force) FORCE=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

fail() { echo "FAIL fetch-libreoffice: $1" >&2; echo "       fix: $2" >&2; exit 1; }

command -v curl >/dev/null || fail "curl absent" "install curl"
command -v ar   >/dev/null || fail "ar absent (binutils)" "install binutils — deb 解包走 ar+tar，无 dpkg-deb 依赖"

# --- pin from manifest.json ---------------------------------------------------
# deps.libreoffice 块（4 空格缩进的同级对象），块内取 url/sha256/size。
LO_BLOCK=$(awk '/"libreoffice"[[:space:]]*:[[:space:]]*\{/{f=1} f{print; if (/^    \}/) exit}' vendor/manifest.json)
[ -n "$LO_BLOCK" ] || fail "no deps.libreoffice pin in vendor/manifest.json" "add the pin block and re-run"
LO_URL=$(printf '%s\n' "$LO_BLOCK" | grep -oE '"url": *"[^"]+"' | head -1 | sed 's/.*"\(http[^"]*\)".*/\1/')
LO_SHA=$(printf '%s\n' "$LO_BLOCK" | grep -oE '"sha256": *"[a-f0-9]{64}"' | head -1 | grep -oE '[a-f0-9]{64}')
LO_SIZE=$(printf '%s\n' "$LO_BLOCK" | grep -oE '"size_bytes": *[0-9]+' | head -1 | grep -oE '[0-9]+')
LO_VER=$(printf '%s\n' "$LO_BLOCK" | grep -oE '"version": *"[0-9.]+"' | head -1 | grep -oE '[0-9.]+')
[ -n "$LO_URL" ] && [ -n "$LO_SHA" ] && [ -n "$LO_SIZE" ] || \
  fail "manifest deps.libreoffice needs url/archive.sha256/archive.size_bytes" "complete the pin"
echo "== fetch-libreoffice: $LO_VER =="

# 成员白名单：headless --convert-to pdf 的最小面——core(框架+soffice)+ure(UNO 运行时)
# +品牌根包(bootstraprc)+writer/impress/draw/calc(导入过滤器)+images(资源)
# +en-us(本地化基础)+ooofonts(与 Calibri/Cambria 等宽的 Carlito/Caladea——
# docx 版式保真的关键)+graphicfilter(文档内嵌 wmf/emf 图)。
# 不取：base/math/pyuno/script-provider/kde/gnome 集成/扩展/dict-*/menus。
MEMBER_RE='/DEBS/(libobasis[0-9.]*-(core|writer|impress|draw|calc|images|en-us|ooofonts|graphicfilter)|libreoffice[0-9.]*(-ure|-en-us)?)_[0-9][^/]*\.deb$'

TARBALL="$CACHE/${LO_URL##*/}"

if [ "$PRINT_ONLY" = "1" ]; then
  echo "  url:    $LO_URL"
  echo "  sha256: $LO_SHA"
  echo "  size:   $LO_SIZE bytes"
  if [ -f "$TARBALL" ]; then
    echo "  members (from cache):"
    tar tzf "$TARBALL" | grep -E "$MEMBER_RE" | sed 's|.*/DEBS/|    |'
  else
    echo "  member regex: $MEMBER_RE"
  fi
  exit 0
fi

if [ -x "$PREFIX/program/soffice" ] && [ "$FORCE" != "1" ]; then
  echo "  .. vendored libreoffice present ($PREFIX) — skip (use --force to re-fetch)"
  exit 0
fi

# --- download + verify ---------------------------------------------------------
mkdir -p "$CACHE"
if [ ! -f "$TARBALL" ]; then
  echo "  .. fetch $LO_URL"
  curl -fSL "$LO_URL" -o "$TARBALL.part" || { rm -f "$TARBALL.part"; fail "download failed" "check network / re-pin vendor/manifest.json"; }
  mv "$TARBALL.part" "$TARBALL"
fi
[ "$(stat -c %s "$TARBALL")" = "$LO_SIZE" ] || fail "size mismatch for $TARBALL" "delete the cached archive and retry"
[ "$(sha256sum "$TARBALL" | cut -d' ' -f1)" = "$LO_SHA" ] || fail "sha256 mismatch for $TARBALL" "re-pin vendor/manifest.json or delete the cached archive"
echo "  .. verified $(basename "$TARBALL") ($(stat -c %s "$TARBALL") bytes)"

# --- member whitelist extract ---------------------------------------------------
STAGING="$PWD/${PREFIX}.new" # 绝对路径：deb 解包子 shell 内会 cd
rm -rf -- "$STAGING"
mkdir -p "$STAGING/debs" "$STAGING/root"
tar tzf "$TARBALL" | grep -E "$MEMBER_RE" > "$STAGING/members.txt" || \
  fail "no member debs matched whitelist" "tarball layout changed? update MEMBER_RE"
COUNT=$(wc -l < "$STAGING/members.txt")
echo "  .. extracting $COUNT member debs"
tar xzf "$TARBALL" -C "$STAGING/debs" -T "$STAGING/members.txt"

# deb 解包不需要 dpkg-deb：ar 抽 data.tar.*，再 tar 进 root（压缩格式不限）。
find "$STAGING/debs" -name '*.deb' -print0 | while IFS= read -r -d '' deb; do
  deb=$(realpath "$deb") # 子 shell 内 cd，先钉绝对路径
  ( tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    cd "$tmp"
    ar x "$deb"
    for f in data.tar.*; do
      [ -f "$f" ] && tar xf "$f" -C "$STAGING/root"
    done )
done

# 拍平 opt/libreoffice*/ -> vendor/libreoffice/（树内 ${ORIGIN} 相对，可整体移动）。
LODIR=$(find "$STAGING/root/opt" -maxdepth 1 -mindepth 1 -type d -name 'libreoffice*' | head -1)
[ -n "$LODIR" ] || fail "no opt/libreoffice* dir after extract" "member whitelist drifted?"
rm -rf -- "$PREFIX"
mkdir -p "$PREFIX"
cp -a "$LODIR"/. "$PREFIX"/
rm -rf -- "$STAGING"

# CJK 兜底：仓库自带 NotoSansSC 进 LO 自带字体目录（vcl 注册 share/fonts），
# 无 CJK 系统字体的宿主/容器也能渲染中文文档。
if ls resources/fonts/NotoSansSC-*.otf >/dev/null 2>&1; then
  cp resources/fonts/NotoSansSC-*.otf "$PREFIX/share/fonts/truetype/" || true
fi

[ -x "$PREFIX/program/soffice" ] || fail "$PREFIX/program/soffice missing after extract" "inspect $LOG_DIR and the member whitelist"
echo "  OK vendored LibreOffice $LO_VER -> $PREFIX (license: $PREFIX/LICENSE, MPL-2.0)"
