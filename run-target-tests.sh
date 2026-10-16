#!/bin/bash
# PR457-closure 目标测试直跑（vendored 前缀环境注入；ctest 逐项由 paleo-dev 提供，
# 这里给单测直跑同等的库/插件路径）
cd "$(dirname "$0")"
export P="$PWD/vendor/prefix/usr"
export QGIS_PREFIX_PATH="$P"
export LD_LIBRARY_PATH="$P/lib/x86_64-linux-gnu:$P/lib"
export QT_PLUGIN_PATH="$P/lib/x86_64-linux-gnu/qt6/plugins"
export GDAL_DRIVER_PATH="$P/lib/x86_64-linux-gnu/gdalplugins"
export QT_QPA_PLATFORM=offscreen
[ -d "$P/share/proj" ] && { export PROJ_DATA="$P/share/proj"; export PROJ_LIB="$P/share/proj"; }
[ -d "$P/share/gdal" ] && export GDAL_DATA="$P/share/gdal"
pass=0; fail=0; failed=""
for t in "$@"; do
  if "./build/$t" > "/tmp/$t.log" 2>&1; then
    pass=$((pass+1)); echo "PASS $t"
  else
    fail=$((fail+1)); failed="$failed $t"; echo "FAIL $t (log: /tmp/$t.log)"; grep -E 'FAIL!|Totals' "/tmp/$t.log" | tail -5
  fi
done
echo "== $pass passed, $fail failed =="
[ -n "$failed" ] && echo "FAILED:$failed"
exit $fail
