#!/usr/bin/env bash
# vendor/fetch-libreoffice.sh — pinned official LibreOffice archive -> vendor/libreoffice.
#
# 仅用于显式调用的旧转换 API；Office 预览已经改用 Calligra（见 manifest
# deps.libreoffice notes）。不走 deb 闭包也不走安装器：官方自含构建
# （${ORIGIN} 相对寻址、可整体移动），pin 在 vendor/manifest.json 的
# deps.libreoffice（Linux）/ deps.libreoffice_win（Windows），解出的
# LICENSE/NOTICE 一并保留在 vendor/libreoffice/ 顶层。
#
# 平台两分支（方向 94）：
#   Linux   官方 deb tarball → ar+tar 解 12 个 headless 子集 deb；
#   Windows 官方 MSI → msiexec /a administrative extract（无需提权，
#           见 R0 实测）→ 剪 headless 子集（dict-*/registry res/python/
#           help/shlxthdl）→ icacls 完整性标签归一（Low 标签工作区会让
#           soffice user-installation 引导失败 exit 77，见 ledger）。
#
# Usage:
#   ./vendor/fetch-libreoffice.sh [--print-only] [--force]
#   --print-only  打印 pin 与将解包/剪裁的成员清单，不下载
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

case "${OSTYPE:-}" in
  msys*|cygwin*|win32*) WINDOWS=1 ;;
  *) WINDOWS=0 ;;
esac

command -v curl >/dev/null || fail "curl absent" "install curl"
if [ "$WINDOWS" = "0" ]; then
  command -v ar >/dev/null || fail "ar absent (binutils)" "install binutils — deb 解包走 ar+tar，无 dpkg-deb 依赖"
else
  command -v msiexec >/dev/null || fail "msiexec absent" "run under Git Bash on Windows (or install msiexec)"
  command -v icacls >/dev/null || fail "icacls absent" "run under Git Bash on Windows"
fi

# --- pin from manifest.json ---------------------------------------------------
# 平台对应的 deps.<key> 块（4 空格缩进的同级对象），块内取 url/sha256/size。
PIN_KEY=libreoffice
[ "$WINDOWS" = "1" ] && PIN_KEY=libreoffice_win
LO_BLOCK=$(awk -v key="\"$PIN_KEY\"" 'index($0, key " ") || index($0, key ":") {f=1} f{print; if (/^    \}/) exit}' vendor/manifest.json)
[ -n "$LO_BLOCK" ] || fail "no deps.$PIN_KEY pin in vendor/manifest.json" "add the pin block and re-run"
LO_URL=$(printf '%s\n' "$LO_BLOCK" | grep -oE '"url": *"[^"]+"' | head -1 | sed 's/.*"\(http[^"]*\)".*/\1/')
LO_SHA=$(printf '%s\n' "$LO_BLOCK" | grep -oE '"sha256": *"[a-f0-9]{64}"' | head -1 | grep -oE '[a-f0-9]{64}')
LO_SIZE=$(printf '%s\n' "$LO_BLOCK" | grep -oE '"size_bytes": *[0-9]+' | head -1 | grep -oE '[0-9]+')
LO_VER=$(printf '%s\n' "$LO_BLOCK" | grep -oE '"version": *"[0-9.]+"' | head -1 | grep -oE '[0-9.]+')
[ -n "$LO_URL" ] && [ -n "$LO_SHA" ] && [ -n "$LO_SIZE" ] || \
  fail "manifest deps.$PIN_KEY needs url/archive.sha256/archive.size_bytes" "complete the pin"
echo "== fetch-libreoffice: $LO_VER ($PIN_KEY) =="

# 成员白名单：headless --convert-to 的最小面——core(框架+soffice)+ure(UNO 运行时)
# +品牌根包(bootstraprc)+writer/impress/draw/calc(导入过滤器)+images(资源)
# +en-us(本地化基础)+ooofonts(与 Calibri/Cambria 等宽的 Carlito/Caladea——
# docx 版式保真的关键)+graphicfilter(文档内嵌 wmf/emf 图)。
# 不取：base/math/pyuno/script-provider/kde/gnome 集成/扩展/dict-*/menus。
MEMBER_RE='/DEBS/(libobasis[0-9.]*-(core|writer|impress|draw|calc|images|en-us|ooofonts|graphicfilter)|libreoffice[0-9.]*(-ure|-en-us)?)_[0-9][^/]*\.deb$'

# Windows 剪裁清单：MSI admin extract 是全量树，这里砍掉 Linux 白名单本来就
# 不取的面（词典/语言 res/python 脚本面/帮助/资源管理器 shell 扩展）+ 树内
# MSI 副本；剩 program+share+Fonts+presets+System[64]+许可文件 ≈ 914MB。
WIN_PRUNE='help readmes share/extensions share/registry/res program/python* program/shlxthdl*'

TARBALL="$CACHE/${LO_URL##*/}"

if [ "$PRINT_ONLY" = "1" ]; then
  echo "  url:    $LO_URL"
  echo "  sha256: $LO_SHA"
  echo "  size:   $LO_SIZE bytes"
  if [ "$WINDOWS" = "0" ] && [ -f "$TARBALL" ]; then
    echo "  members (from cache):"
    tar tzf "$TARBALL" | grep -E "$MEMBER_RE" | sed 's|.*/DEBS/|    |'
  elif [ "$WINDOWS" = "1" ]; then
    echo "  extract: msiexec /a (administrative, no elevation)"
    echo "  prune:   $WIN_PRUNE"
  else
    echo "  member regex: $MEMBER_RE"
  fi
  exit 0
fi

SOFFICE_CHECK="$PREFIX/program/soffice"
[ "$WINDOWS" = "1" ] && SOFFICE_CHECK="$PREFIX/program/soffice.exe"
if [ -e "$SOFFICE_CHECK" ] && [ "$FORCE" != "1" ]; then
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

# --- extract -------------------------------------------------------------------
STAGING="$PWD/${PREFIX}.new" # 绝对路径：解包子进程/子 shell 内会 cd
rm -rf -- "$STAGING"

if [ "$WINDOWS" = "0" ]; then
  # deb tarball：白名单成员解到 debs/，ar 抽 data.tar.* 再 tar 进 root。
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
else
  # Windows MSI：msiexec /a administrative extract——无提权、出全量平铺树
  # （program/ share/ 顶层，正好是 resolver 期望的布局）。//a 是 MSYS 的
  # /a 转义；TARGETDIR 要 Windows 路径。
  mkdir -p "$STAGING/root"
  MSI_WIN=$(cygpath -w "$TARBALL")
  TARGET_WIN=$(cygpath -w "$STAGING/root")
  echo "  .. msiexec /a extract (2-3 min typical)"
  msiexec //a "$MSI_WIN" "TARGETDIR=$TARGET_WIN" //qn || \
    fail "msiexec administrative extract failed (exit $?)" "group policy may block admin installs — see ledger R0 case (b)"
  [ -e "$STAGING/root/program/soffice.exe" ] || \
    fail "no program/soffice.exe after extract" "MSI layout changed? inspect $STAGING/root"

  echo "  .. pruning headless subset"
  ( cd "$STAGING/root" && rm -rf -- $WIN_PRUNE ${TARBALL##*/} )

  rm -rf -- "$PREFIX"
  mkdir -p "$PREFIX"
  cp -a "$STAGING/root"/. "$PREFIX"/

  # 完整性标签归一：工作区若带 Low mandatory label（沙箱防护），继承它的
  # vendored 树会让 soffice user-installation 引导失败（exit 77，R0 实测：
  # 同树同路径 Low 必挂 / Medium 即活）。CI runner 无此标签，操作幂等无害。
  icacls "$(cygpath -w "$PREFIX")" //setintegritylevel "(OI)(CI)medium" >/dev/null || \
    echo "  .. WARN icacls setintegritylevel failed — vendored tree may fail to boot in labeled workspaces"
fi

# CJK 兜底：仓库自带 NotoSansSC 进 LO 自带字体目录（vcl 注册品牌字体），
# 无 CJK 系统字体的宿主/容器也能渲染中文文档（Windows 侧品牌字体在 Fonts/）。
if ls resources/fonts/NotoSansSC-*.otf >/dev/null 2>&1; then
  if [ "$WINDOWS" = "1" ]; then
    mkdir -p "$PREFIX/Fonts"
    cp resources/fonts/NotoSansSC-*.otf "$PREFIX/Fonts/" || true
  else
    cp resources/fonts/NotoSansSC-*.otf "$PREFIX/share/fonts/truetype/" || true
  fi
fi

rm -rf -- "$STAGING"
[ -e "$SOFFICE_CHECK" ] || fail "$SOFFICE_CHECK missing after extract" "inspect $LOG_DIR and the member whitelist/prune list"
echo "  OK vendored LibreOffice $LO_VER -> $PREFIX (license: $PREFIX, MPL-2.0)"
