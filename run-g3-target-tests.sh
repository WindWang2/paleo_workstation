#!/bin/bash
# G3 目标测试直跑封装（deb 闭包 prefix 环境注入；ctest 的 ctest 二进制与
# prefix expat 冲突，故直跑 + 本脚本注入等价环境；沙箱监狱对策：TMPDIR
# 指到树内 build/paleo-tmp）。用法：./run-g3-target-tests.sh tst_a ... tst_b
cd "$(dirname "$0")"
export P="$PWD/vendor/prefix/usr"
export QGIS_PREFIX_PATH="$P"
export LD_LIBRARY_PATH="$P/lib/x86_64-linux-gnu:$P/lib"
export QT_PLUGIN_PATH="$P/lib/x86_64-linux-gnu/qt6/plugins"
export GDAL_DRIVER_PATH="$P/lib/x86_64-linux-gnu/gdalplugins"
export TMPDIR="$PWD/build/paleo-tmp"
export QT_QPA_PLATFORM=offscreen
mkdir -p "$TMPDIR"
[ -d "$P/share/proj" ] && { export PROJ_DATA="$P/share/proj"; export PROJ_LIB="$P/share/proj"; }
[ -d "$P/share/gdal" ] && export GDAL_DATA="$P/share/gdal"
pass=0; fail=0; failed=""
for t in "$@"; do
  log="$(mktemp "$TMPDIR"/paleo-g3-$t.XXXXXX.log)"
  if "./build/$t" > "$log" 2>&1; then
    pass=$((pass+1)); echo "PASS $t"
  else
    fail=$((fail+1)); failed="$failed $t"; echo "FAIL $t (log: $log)"
    grep -E 'FAIL!|Totals|Actual|Expected' "$log" | head -12
  fi
done
echo "== $pass passed, $fail failed =="
[ -n "$failed" ] && echo "FAILED:$failed"
exit $fail
