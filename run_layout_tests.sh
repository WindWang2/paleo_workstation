#!/bin/bash
# ---------------------------------------------------------------------------
# run_layout_tests.sh — layout-designer test gate (subtask E).
#
# Builds all six layout test binaries off-tree (/tmp/pw-layout-gate, rebuilt
# from scratch on every run) against the distro Qt6/QGIS 4.2 headers and runs
# them offscreen. Any failure prints that binary's output and exits non-zero;
# all green prints one ALL_TESTS_PASSED line with the per-binary detail.
#
#   tst_layoutshell          — designer shell / interface contract (pre-existing)
#   tst_layoutpalette        — subtask A: PaleoLayoutItemPalette
#   tst_layoutitempanel      — subtask B: PaleoLayoutItemPanel
#   tst_layoutexport         — subtask C: PaleoLayoutExportActions/Templates
#   tst_layoutundo           — subtask D: PaleoLayoutUndoStack
#   tst_layoutdesigner_full  — subtask E: full shell integration
#
# Usage: bash run_layout_tests.sh   (or ./run_layout_tests.sh)
# ---------------------------------------------------------------------------
set -u

WT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # worktree root
cd "$WT" || exit 1

B=/tmp/pw-layout-gate
rm -rf "$B"
mkdir -p "$B" || exit 1
cd "$B" || exit 1

QT="Qt6Widgets Qt6Test Qt6Xml Qt6PrintSupport Qt6Svg Qt6Network Qt6Concurrent"
FLAGS="$(pkg-config --cflags $QT) -I/usr/include/qgis -I$WT/src -I$WT/src/qgis -I$WT/src/ui/layout -I."
LIBS="$(pkg-config --libs $QT) -lqgis_core -lqgis_gui"
MOC=/usr/lib/qt6/moc
CXX="g++ -std=c++20 -fPIC"

COMPONENT_HEADERS=(
  "$WT/src/ui/layoutdesignershell.h"
  "$WT/src/ui/layout/layoutitempalette.h"
  "$WT/src/ui/layout/layoutitempanel.h"
  "$WT/src/ui/layout/layoutexportactions.h"
  "$WT/src/ui/layout/layouttemplates.h"
  "$WT/src/ui/layout/layoutundostack.h"
)
TESTS=( tst_layoutshell tst_layoutpalette tst_layoutitempanel
        tst_layoutexport tst_layoutundo tst_layoutdesigner_full )

echo "== moc =="
for h in "${COMPONENT_HEADERS[@]}"; do
  base="$(basename "${h%.h}")"
  "$MOC" "$h" -o "moc_${base}.cpp" || { echo "moc failed: $h"; exit 1; }
done
for t in "${TESTS[@]}"; do
  "$MOC" "$WT/tests/$t.cpp" -o "$t.moc" || { echo "moc failed: $t.cpp"; exit 1; }
done

# --- compile every unique TU once, in parallel ------------------------------
echo "== compile (parallel) =="
SOURCES=(
  "$WT/src/ui/layoutdesignershell.cpp"
  "$WT/src/ui/layout/layoutitempalette.cpp"
  "$WT/src/ui/layout/layoutitempanel.cpp"
  "$WT/src/ui/layout/layoutexportactions.cpp"
  "$WT/src/ui/layout/layouttemplates.cpp"
  "$WT/src/ui/layout/layoutundostack.cpp"
  "$WT/src/qgis/qgisruntime.cpp"
  moc_layoutdesignershell.cpp
  moc_layoutitempalette.cpp
  moc_layoutitempanel.cpp
  moc_layoutexportactions.cpp
  moc_layouttemplates.cpp
  moc_layoutundostack.cpp
)
for t in "${TESTS[@]}"; do
  SOURCES+=( "$WT/tests/$t.cpp" )
done

for src in "${SOURCES[@]}"; do
  obj="$(basename "${src%.cpp}").o"
  ( $CXX $FLAGS -c "$src" -o "$obj" ) > "${obj%.o}.compile.log" 2>&1 &
done
wait

fail=0
for src in "${SOURCES[@]}"; do
  obj="$(basename "${src%.cpp}").o"
  if [ ! -f "$obj" ]; then
    echo "COMPILE FAILED: $src"
    cat "${obj%.o}.compile.log"
    fail=1
  fi
done
[ $fail -eq 0 ] || exit 1

# --- link (each binary gets exactly the objects it needs) --------------------
echo "== link =="
# The designer shell composes the A-D components, so its binaries link them all.
ALL_COMPONENT_OBJS=(
  layoutitempalette.o layoutitempanel.o layoutexportactions.o
  layouttemplates.o layoutundostack.o
  moc_layoutitempalette.o moc_layoutitempanel.o moc_layoutexportactions.o
  moc_layouttemplates.o moc_layoutundostack.o
)
link() {
  local out=$1; shift
  g++ "$@" -o "$out" $LIBS || exit 1
}
link tst_layoutshell \
  tst_layoutshell.o layoutdesignershell.o moc_layoutdesignershell.o \
  qgisruntime.o "${ALL_COMPONENT_OBJS[@]}"
link tst_layoutpalette \
  tst_layoutpalette.o layoutitempalette.o moc_layoutitempalette.o qgisruntime.o
link tst_layoutitempanel \
  tst_layoutitempanel.o layoutitempanel.o moc_layoutitempanel.o qgisruntime.o
link tst_layoutexport \
  tst_layoutexport.o layoutexportactions.o layouttemplates.o \
  moc_layoutexportactions.o moc_layouttemplates.o qgisruntime.o
link tst_layoutundo \
  tst_layoutundo.o layoutundostack.o moc_layoutundostack.o qgisruntime.o
link tst_layoutdesigner_full \
  tst_layoutdesigner_full.o \
  layoutdesignershell.o layoutitempalette.o layoutitempanel.o \
  layoutexportactions.o layouttemplates.o layoutundostack.o \
  moc_layoutdesignershell.o moc_layoutitempalette.o moc_layoutitempanel.o \
  moc_layoutexportactions.o moc_layouttemplates.o moc_layoutundostack.o \
  qgisruntime.o

# --- run offscreen ------------------------------------------------------------
echo "== run =="
overall=0
for t in "${TESTS[@]}"; do
  if QT_QPA_PLATFORM=offscreen ./"$t" > "$t.out" 2>&1; then
    printf 'PASS  %-26s %s\n' "$t" "$(grep -E '^Totals:' "$t.out" | head -1)"
  else
    printf 'FAIL  %-26s (exit %s)\n' "$t" "$?"
    echo "-------- $t output --------"
    cat "$t.out"
    echo "----------------------------"
    overall=1
  fi
done

if [ $overall -eq 0 ]; then
  echo "ALL_TESTS_PASSED (${#TESTS[@]} binaries: ${TESTS[*]})"
fi
exit $overall
