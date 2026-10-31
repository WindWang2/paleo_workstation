# 恢复 lint Source gates：补层标记、撤视图层 qgs 直连、收缩传递基线

日期：2026-10-31
分支：`fix/source-gates-green`（基于 master `5c877be5`）

## 背景

最新 master 上逐条复跑 CI `Source gates`（`source build-env.local.sh` 后与 ci.yml 同序），
发现两组红项：

| 闸 | 失败项 | 来源 |
|---|---|---|
| `check_layering.py --strict` | `src/ui/subintervalselectorbar_wrap.cpp:0 [layer-marker] 头三行缺 // 层：视图` | #829 新增文件 |
| `check_layering.py --qgs-audit --strict` | `src/ui/pages/mappingreferencepanel.cpp 直连外部 qgs* 头`（137 > baseline 136） | #832 为取工程路径而 `#include <qgsproject.h>` |

另有可收缩项：`--transitive` 实测 `ui_qtwidgets_notes` 4 < baseline 5（#830/#831/#832 削边后）。

## 改动

1. `src/ui/subintervalselectorbar_wrap.cpp`：首行补 `// 层：视图`（其余注释保持不变）。
2. 新增 `src/qgis/projectscope.{h,cpp}`：`paleoProjectScopeKey(QgsProject*)` 返回工程文件
   路径 SHA1（无工程 → 空串），把唯一一处 `<qgsproject.h>` 收进 qgis 层；
   `mappingreferencepanel.cpp` 改用它，视图层不再直连 qgs 头。
   `cmake/paleo-modules-data.cmake` 登记新 TU。
3. `tools/transitive-baseline.txt`：`ui_qtwidgets_notes` 5→4（只缩不涨，附日期与归因）。

## 证据

```
check_layering --strict            => 0  检查通过：无层违规
check_layering --qgs-audit --strict=> 0  直连 136（baseline 136，新增 0）
check_layering --transitive        => 0  视图层边记录 4（= baseline 4，无 NOTE）
其余 Source gates（selftest/symbol-audit/qgis_spread/ui_invariants/ui_size/
file_budget/i18n/qgis_versions/ci_scripts/docs_drift）均 0
```

`check_ts_stale` 仍红（601 > 31），属 #755 既登记项，本批未动翻译。

## 已知环境限制

本机当前所有用到 `QTemporaryDir` 的测试（含未改动的 `tst_stratigraphy_model`）在
`--output-on-failure` 下均以 `project.isValid() == false` 失败，属宿主环境变化，
与本批改动无关；因此 `tst_mappingworkbench` 的证据面板用例未能重跑（上一版同一用例
通过）。本批判定以源码闸门为准。
