# 门禁归因修正 + 地层读取/预览失败如实上报（#825-#828）

日期：2026-10-31
分支：`fix/gate-and-stratigraphy-honesty`
范围：`src/qgis`、`src/services`、`src/workflow`、`src/ui/pages`、`docs/ci`、`tests`

## 背景

上一轮整体 review 提交了 4 个 issue（#825 传递债/gate 归因、#826 模型分层静默回退、
#827 井道预览失败被派生曲线掩盖、#828 打开工程读取失败被当作空解释）。本批按
「先修边、再让失败可见」的顺序收口，不新增功能性行为。

## 改动

| Issue | 改动 | 关键文件 |
|---|---|---|
| #825 | 新增 `qgis/referenceview.h`（接口+工厂，仅前向声明 `QWidget`），`MappingReferenceCanvas` 实现该接口；`MappingReferencePanel` 只依赖接口，删掉 `→ qgis/mappingreferencecanvas.h` 的 include 边；`known-reds` 的 lint job 条目改为按当前实测归因（余红只剩 `check_ts_stale`，#755） | `src/qgis/referenceview.h`、`src/qgis/mappingreferencecanvas.{h,cpp}`、`src/ui/pages/mappingreferencepanel.{h,cpp}`、`docs/ci/known-reds.json` |
| #826 | `stratigraphymodel` 改为三态 `ModelTopsStatus{NoModel,Ready,Failed}`：打开/列表/任一修订解码失败即 `Failed`，不再跳过损坏修订去用更早版本；`topsForMapping` 增加 `error` 出参，`Failed` 时返回空且**不回退**旧修订/旧 tops；厚度与残差路径把错误显示出来 | `src/services/stratigraphymodel.{h,cpp}`、`src/services/projectdata.{h,cpp}`、`src/workflow/mappingworkflow.cpp` |
| #827 | `previewWellData` 增加 `baseLoaded` 出参；基础模型失败但仍有解释派生曲线时，写入 `dataWarnings` 并由地层页显示持久提示，不再用 `!isEmpty()` 冒充成功 | `src/workflow/stratigraphyworkflow.{h,cpp}`、`stratigraphyworkflow_preview.cpp`、`src/ui/pages/stratigraphypage_preview.cpp` |
| #828 | `setProject` 检查 `listInterpretations` / `readInterpretationRevision` 返回值，失败时清空文档、记录 `documentLoadError()` 并 `statusChanged`，不再静默当作「没有解释」 | `src/workflow/stratigraphyworkflow.cpp`、`stratigraphyworkflow_state.h` |

## 证据

- 传递闸：`python tools/check_layering.py --transitive` → 视图层 QtWidgets 边记录 5（= baseline），退出码 0。
- Source gates 逐步复跑（与 `.github/workflows/ci.yml` 同序）：
  `layering --strict/--selftest/--symbol-audit/--transitive/--qgs-audit --strict`、
  `qgis_spread --strict`、`ui_invariants --strict/--selftest`、`ui_size --strict/--selftest`、
  `file_budget --strict/--selftest`、`i18n`（含 selftest）、`qgis_versions`、`ci_scripts`、
  `docs_drift`（含 selftest）均退出码 0；`check_ts_stale` 仍红（606 > 31），属 #755 已登记项。
- 回归测试（新增，`tests/tst_stratigraphy_model.cpp`）：
  - `unreadableLatestRevisionFailsLoudly`：损坏最新修订 payload 后
    `readStratigraphyModelTops` 为 `Failed` 且 `tops` 为空；`setProject` 记录原因、发一次
    `statusChanged`、文档为空。
  - `baseModelFailureSurvivesDerivedTracks`：删除 `data.sqlite` 后 `previewWellData` 仍返回
    true（派生曲线可显示），但 `baseLoaded=false`、`error` 与 `dataWarnings` 均写明失败。
- 测试批次：`tst_stratigraphy_*`（8 套）、`tst_projectdata`、`tst_mappingpages`、
  `tst_project_wellinputs` 全绿；`tst_stratigraphy_model` 5/5。

## 已知红（本机环境，非本批引入）

- `tst_mapping`：`exportPdfWritesD61Map` / `horizonMapLayoutRestoresLiveLayerStyle` 的
  `buildHorizonMapLayout` 返回 nullptr（"无法将图层范围转换到工程坐标系"）——
  `docs/ledger-archive/goal-loop-env-debt2.md:127` 已记档。
- `tst_ui` / `tst_mappingworkbench`：QtTest 本体有失败且进程以 `0xc0000409` 退出；
  日志首行显示 `QOpenGLWidget is not supported` 与 QGIS `labelsWithLayer` 补丁缺失，
  属审查沙箱环境（此前已确认 `tsbx.dll` 注入导致退出码失真）。
- `check_ts_stale`：606 > 31，见 `known-reds` 的 `ts-stale-lupdate-backlog`（#755）。

## 边界

未对真实工区做端到端验收；`topsFor`（非编图口径）仍保留「模型读不出即回落文件」的旧语义，
本轮只收口编图/厚度/井预测使用的 `topsForMapping`。`wellpredictionservice`、
`wellsitingworkflow`、`constraintwellfactors` 等消费方暂未接 `error` 出参，失败时得到空
分层而不是过期分层。
