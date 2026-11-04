# 删除已被生产路径覆盖的 spike 验收测试

日期：2026-11-02
分支：`chore/prune-spike-tests`

## 删了什么

| 删除项 | 原用途 | 为什么算多余 |
|---|---|---|
| `tests/tst_boot.cpp` | Spike-1 验收：QgsApplication 初始化、providers>0、srs.db、GPKG 渲染非均匀像素 | 同一批断言已由 `tst_runtime`（init 幂等、`providerCount()>0`、srs.db/prefix 语义）与 `tst_basemap_project` / 预览渲染用例承担；该文件是 vendor 打通阶段的一次性冒烟 |
| `tests/tst_polygonize.cpp` | Spike-2 验收：`PaleoPolygonizeAlgorithm` 经 `QgsProcessingRegistry` 注册并栅格转面 | 生产侧已有 `src/algorithms/faciespolygonize` 与 `src/qgis/qgisprocessingservice` 的正式路径及对应用例；spike 实现不在任何生产路径上 |
| `spikes/polygonize/paleopolygonize.{h,cpp}` | spike 2 的算法实现 | 全仓唯一消费者是 `tst_polygonize`；测试删除后成死代码 |

`spikes/onnx/` 保留：ONNX 已是生产特性（`src/ai/onnx*`），夹具模型仍被现有测试引用。

## 注册与注释同步

- `cmake/paleo-tests.cmake`：删除 `tst_boot`（target / add_test / sandbox）与
  `tst_polygonize`（target / include / add_test / sandbox）两段。
- `cmake/paleo-dependencies.cmake`：把「只链 paleo_qgis_iface 的目标」注释里的
  `tst_boot / tst_polygonize` 改为 `paleo_selfcheck`。

## 验证

- `ctest -N | grep -E 'boot|polygonize'` → 空（两个测试项已从清单移除）。
- CMake 重配置成功，`tst_runtime` 目标增量构建通过（无悬空引用）。
- Source gates 复跑：`layering --strict/--transitive/--qgs-audit --strict`、
  `qgis_spread`、`ui_invariants`、`ui_size`、`file_budget`、`docs_drift`、
  `ci_scripts` 均退出码 0；`git diff --check` 干净。

## 保留的历史记录

`docs/PALEO_QGIS_PLAN.md` 里 spike 1/2 PASS 的验收记录不动——那是当时的打通证据，
不是当前测试清单；本 PR 只清掉仍在编译与计数的测试项，不改写历史。
