# Goal Loop ledger — fault-surface（断棒成面与断距）

分支 `goal/fault-surface-20261002`，基线 `eaaf46d`，worktree `.worktrees/fault-surface`。
语义全文在 `docs/progress/fault-surface.md`。

构建：`cmake -S .worktrees/fault-surface -B .worktrees/fault-surface/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix -DQt6_DIR=C:/deps/Qt/6.8.0/msvc2022_64/lib/cmake/Qt6 -DCMAKE_PREFIX_PATH=C:/deps/Qt/6.8.0/msvc2022_64;C:/deps/qca-install;C:/deps/kc-install;C:/deps/qscintilla-install;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library`。
CMake 3.29.3（`C:/Qt/Tools/CMake_64`）。编 `paleo_ui` 时 MSVC 还要
`INCLUDE` 含 `C:/deps/qscintilla-install/include`（QGIS 头引用 `Qsci/qsciapis.h`）。
跑测试时 Qt 6.11.2 的 bin 在 PATH 最前，`QT_PLUGIN_PATH=paleo-qgis-deps/Library/lib/qt6/plugins`。

## 轮 0 — 口径

- 棒没有地图 XY。IL/XL→XY 是调用方的 `SurveyFrame`，不进 FaultSet，也不查投影服务。
- 纵向域是棒上的枚举。混用 TWT 与深度则 `MixedDomain`。
- 单剖面拒绝，不外推。
- `FaultHorizonCut` 的存储字段不动。断距走 `measureSlip`。
- 网格：`Fault::surface` + `fault_set.surface` 列 + 工作流 DERIVED 文件。
  `MetaStore::kUserVersion` 1 → 2。
- `faultsurface` 不是新顶层模块，不跑 `scripts/new_module.sh`。

签名见 `docs/progress/fault-surface.md`。

## 轮 1–2 — 成面与断距

`tst_faultsurface` 10/10，43 ms。性能行：

`fault-surface perf: 24 ms for 100 x 200 (budget 3000 ms, ratio 0.008000)`

倾角 30° ±0.5°，单棒拒绝，分叉/混域/走向突变显式失败，断距绝对误差 0.05，
未拾取剖面交点 traceFrac 0.5。部分深度的三角网不把整柱隔死；贯通墙隔开
`cellBlock`。

## 轮 3 — 存储

`tst_faultsetstore` 9/9，244 ms。网格字段相等。旧库无 `surface` 列，迁移后
棒和切割还在，断面为空，`user_version` 为 2。

`tst_metastore`：版本矩阵改为当前版本可开、`kUserVersion+1` 拒开。
`staleLockAutoRecovered` 在本机失败（见进度文档），不是断面列引起的。

## 轮 4 — 编排

`tst_faultsurfaceworkflow` 5/5，116 ms。失败不 `stage`。成功版本的父 id 是
`ver-parent`，阶段 DERIVED。

## 轮 5 — 视图与回归

`tst_faultsurfaceview` 5/5，54 ms。场景 Y = −地质 Z。远裁剪 > 1000，包围盒在视锥内。
offscreen 无 GL，`QOpenGLWidget is not supported on this platform`。
`tst_faultsectionui` 9/9，986 ms。

`python tools/check_layering.py --strict` → `检查通过：无层违规。`
`python tools/check_i18n.py` → `i18n 检查通过`。
`python tools/check_ui_invariants.py` → `ui invariants: clean`。

## 轮 6 — 真工区

`PALEO_REAL_PROJECT_AREA` 空。Linux 路径与本机候选路径都不存在。不记实测耗时。

## 轮 6 续 — 本机全量

ninja 全量（`-k 0`）退出码 1。编译错误只有三处既有测试，断面目标已链接：

- `tests/tst_mem_budget.cpp(58)` `malloc_trim`（C3861）
- `tests/tst_gridsolver.cpp(66)` `std::numbers::pi`（C3083/C2039，缺 `<numbers>`）
- `tests/tst_velocitymodel.cpp(297)` `qQNaN()` 收窄进 `QVector<float>`（C2398）

这三项没有 exe，ctest 用 `-E "tst_mem_budget|tst_gridsolver|tst_velocitymodel"` 排除。

```
ctest -j1 --output-on-failure -E "tst_mem_budget|tst_gridsolver|tst_velocitymodel"
```

结果：`84% tests passed, 27 tests failed out of 174`。真实时间 724.39 s，退出码 1。
147 通过、27 失败。断面相关与分层门禁通过：

| 测试 | 结果 |
|---|---|
| layering / layering_strict / layering_selftest | Passed 0.47 / 0.33 / 0.13 s |
| ui_invariants / ui_invariants_strict | Passed 0.15 / 0.15 s |
| i18n | Passed 1.01 s |
| tst_faultset / tst_faultsetstore / tst_faultinterp | Passed 0.07 / 0.28 / 1.20 s |
| tst_faultsectionui / tst_faultsurface | Passed 1.13 / 0.10 s |
| tst_faultsurfaceworkflow / tst_faultsurfaceview | Passed 0.18 / 0.16 s |
| tst_stratgrid / tst_propfill / tst_propworkflow | Passed 0.13 / 0.04 / 1.45 s |

27 个失败不在断面源码：

- 断言：`tst_runtime`、`boot` 缺 `paleo-qgis-prefix/resources/srs.db`；`tst_metastore` 仅 `staleLockAutoRecovered`（直跑 `-o`：13 通过、1 失败、867 ms）；`tst_import::manyConcurrentImportsDoNotDeadlock` 比较值不同（48 通过、1 失败、2 跳过、31494 ms）。
- `tst_cache_las` 在套件里 Failed（0.37 s），立刻重跑通过。
- 段错误：`tst_panels` `validatePage_populatesAndLocates` / `QTableWidgetItem::text`（0xC0000005）。
- 退出码 0xC0000139（22）：tst_layerservice、tst_mappingworkbench、tst_ui、tst_procdialog、tst_rehydrate、tst_seismic_index、tst_seismic_3d、tst_previewmap_canvas、tst_previewmap_page、tst_datapreview、tst_taskservice、tst_versions、tst_layertreepanel、tst_faciesclassify、tst_crossplot_controller、tst_wellcomposite_shell、tst_sectionlifecycle、tst_sections_alignment、tst_seismic_3dviz、tst_seismic_welltie、tst_dockmanager。PATH 已把 Qt 6.11.2 放在 Qt 6.8 之前。
