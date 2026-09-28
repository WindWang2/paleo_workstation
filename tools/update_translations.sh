#!/usr/bin/env bash
# wave/ux-polish — 翻译骨架更新脚本（docs/progress/ux.md i18n 节）。
#
#   tools/update_translations.sh [ts 文件…]
#
# 扫 src/ 全部 C++ 源（tr()/QT_TRANSLATE/translate），更新 translations/*.ts。
# lupdate 不在 PATH 时会试常见 Qt 安装位（/usr/lib/qt6/bin 等）；找不到时
# 报错退出（骨架已手工提交，翻译者可直接编辑 .ts；lrelease 同目录可用时
# 脚本顺手编译出 .qm 到 build 目录之外不动）。
set -euo pipefail
cd "$(dirname "$0")/.."

# lupdate 定位：PATH → Qt6 专有路径 → Qt5 兼容
LUPDATE="$(command -v lupdate || command -v lupdate-qt6 || true)"
if [[ -z "${LUPDATE}" ]]; then
  for cand in /usr/lib/qt6/bin/lupdate /usr/lib/qt6/libexec/lupdate \
              /usr/lib/x86_64-linux-gnu/qt6/bin/lupdate /usr/local/qt6/bin/lupdate; do
    [[ -x "${cand}" ]] && LUPDATE="${cand}" && break
  done
fi
if [[ -z "${LUPDATE}" ]]; then
  echo "error: lupdate 未找到（装 qt6-tools-dev / qttools）——translations/*.ts 骨架手工维护" >&2
  exit 1
fi
echo "using lupdate: ${LUPDATE}"

TS_FILES=("$@")
if [[ ${#TS_FILES[@]} -eq 0 ]]; then
  mapfile -t TS_FILES < <(ls translations/*.ts 2>/dev/null || true)
fi
if [[ ${#TS_FILES[@]} -eq 0 ]]; then
  echo "error: translations/ 下没有 .ts 文件（先建骨架）" >&2
  exit 1
fi

SOURCES=$(find src -name '*.cpp' -o -name '*.h' | sort)
# shellcheck disable=SC2086
"${LUPDATE}" -no-obsolete ${SOURCES} -ts "${TS_FILES[@]}"
echo "updated: ${TS_FILES[*]}"
