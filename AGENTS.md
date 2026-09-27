# paleo_workstation

## Design System
Always read DESIGN.md before making any visual or UI decisions.
All font choices, colors, spacing, and aesthetic direction are defined there.
Do not deviate without explicit user approval.
In QA mode, flag any code that doesn't match DESIGN.md.

## Architecture
The authoritative architecture plan is `docs/PALEO_QGIS_PLAN.md`
(vendored QGIS C++ embed; QGIS owns GIS plumbing, Paleo owns geological semantics).
Deferred work lives in `TODOS.md`.

## Layer Contract (docs/UI_LAYER_PLAN.md, W6 机械执行)
视图只发信号不干活，功能只编排不画像素，数据只问答不管谁来问。

| 层 | 目录 | 禁令 |
|----|------|------|
| 数据 | `src/domain` `src/catalog` `src/io` `src/metadata` `src/services` `src/algorithms` | 无 QtWidgets；无 `ui/` include |
| 功能 | `src/workflow` `src/linkage` `src/ai` | 无 QtWidgets；无 `ui/` include；不建应用对话框/页 |
| QGIS 封装 | `src/qgis` | QtWidgets 豁免（QGIS 接口所需）；无 `ui/` include |
| 视图 | `src/ui/**` | `io/*` 白名单仅 `lasdoc.h`；`metadata/*` 白名单四头（layermanifest/paleoprojectstore/mapversionstore/releasestore）；无 `algorithms/*` |
| 组装根 | `src/app` | 唯一允许 include `ui/` 的非视图目录（by design） |
| 测试壳 | `src/selfcheck` | 同 by design 豁免 |

每个 `src/` 文件头三行必须有 `// 层：<数据|功能|QGIS 封装|视图|组装根|测试壳>`。
检查器：`tools/check_layering.py`（ctest 项 `layering` + `layering_selftest`）；
残留收敛走 `tools/layering-baseline.txt`（只缩不涨）。

已知边界：各层编进同一 `paleo_core` 静态库，护栏只挡 include 层——
「不带 include 直接 new」挡不住，靠 review 纪律（见 TODOS.md 递延项）。
