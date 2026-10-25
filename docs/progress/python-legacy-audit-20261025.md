# Python 遗留原件全面审计报告（2026-10-25）

- 分支：`goal/archive-legacy-python-20261025`
- 审计基线：`39515ad9`（origin/master @ 2026-10-25，PR #716 合并后）
- 审计对象：全仓被跟踪 Python 文件（`git ls-files '*.py'`）共 **66** 个
- 方法：`git grep` 全仓引用矩阵（文件名+去扩展名双口径）、
  CMake/ctest 注册面核对（`cmake/*.cmake`、`CMakeLists.txt`）、
  CI workflow 调用核对（`.github/workflows/ci.yml`）、C++ 源运行时
  调用核对（`QProcess`/路径字面量）、`git log --diff-filter=D` 历史
  删除扫描、逐文件头/内容阅读、git 入库历史与提交说明核对。
- 归档落点：`archive/legacy-python/`（README/MANIFEST/SHA256SUMS/original）

## 核心结论

1. **本仓库从未存在过 Python 版业务应用实现。** 首批 commit
   （2026-09-25，`b6550264` 起）即为 C++20/Qt6/QGIS 工程文档与源码；
   `git log --all --diff-filter=D -- '*.py'` 为空——历史上没有任何
   Python 文件被删除过，全部 66 个文件均为独立加入的工具/夹具。
   因此**不存在「旧版业务模块/GUI/算法/工作流 Python 原件」意义上的
   A 类**；本审计按任务书 §5「已经明确验证可移出的历史文件」条款，
   归档 2 个确证退役的一次性评审产物生成器（见 §A）。
2. 其余 **64 个文件全部活跃**（构建闸门、CI、ctest、测试夹具、用户
   脚本功能面、供应链），逐文件证据见 §B–§E。
3. Python 在本仓库中的四个正当角色：源码闸门与工程工具（B）、测试
   夹具与数据/参考生成器（C）、用户 Python 脚本功能面的契约示例
   （D，C++ 侧 `pythonenv`/`pythonrepl`/`scriptrunner`/`pythonconsolepanel`
   完整在册）、供应链/底图工具（E）。**清理不触碰这四类。**

## A 类：已退役历史文件（归档 2 个）

| 原路径 | 退役证据 | 归档新路径 |
|--------|---------|-----------|
| `artifacts/gen_batch_status.py`（120 行） | 2026-10-16 issue #497–#504 批次交付状态 docx 一次性生成器；全仓零引用；输出路径硬编码特定 Windows 机器（`C:\Users\wangj.KEVIN\...`）；产物 docx 已随同一 commit（#598）入库——自包含交付物在档，生成器再无运行需求；#598 提交说明自述「收尾提交——工区评审产物（docx/批量状态脚本）」 | `archive/legacy-python/original/artifacts/gen_batch_status.py` |
| `artifacts/gen_review_doc.py`（160 行） | 2026-10-16 整体性评价 docx 一次性生成器；全仓零引用、硬编码他人机器路径、产物 docx 已随 #598 入库、#598 收尾提交定性为评审产物遗留 | `archive/legacy-python/original/artifacts/gen_review_doc.py` |

两者均以 `git mv` 迁移，内容零改动；SHA256/大小/还原方式见
`archive/legacy-python/MANIFEST.md`。迁移零悬空引用（对账结论同文档）。

## B 类：活跃工程工具（保留 37 个）

**CI/ctest 注册闸门（22）**——注册于 `.github/workflows/ci.yml` Source
gates 步和/或 `cmake/paleo-test-infra.cmake`、`cmake/extra-*.cmake`：

`check_layering.py`（layering 族 6 项 + AGENTS.md 契约）、
`check_qgis_spread.py`（qgis_spread 族 3 项）、`check_ui_invariants.py`、
`check_ui_size.py`、`check_file_budget.py`、`check_i18n.py`、
`check_ui_tokens.py`、`check_qgis_versions.py`、`check_docs_drift.py`、
`check_ci_scripts.py`、`check_tidy.py`（ci.yml + CMakeLists）、
`check_ts_stale.py`（ci.yml）、`check_l10n.py`（extra-ux）、
`check_mamcl_lock.py`（extra-mamcl）、`check_env_redset.py`、
`check_flaky_ledger.py`、`check_lo_pin.py`、`check_role_tables.py`、
`ensure_qgis_resources.py`、`pin_osgeo4w.py`、`ci_triage_reds.py`
（ci.yml triage 步 + ctest）、`test_geology_preparation.py`
（extra-import-ux 注册 `geology_preparation`）。

**构建/夹具工具链（4）**：
- `make_segy_fixture.py`——`cmake/extra-seismic.cmake` 以
  `PALEO_SEGY_FIXTURE_TOOL` 注入四个 seismic 测试的夹具生成；
- `simplify_historical_maps.py`——数据预处理产物（简化边界记录 JSON）
  生成器，`src/io/projectsetupmaps.cpp` 与 `tst_projectsetup.cpp` 在册
  记载其产物格式；
- `package_portable.py`——Windows 便携包打包器（#598 引入、#714 仍在
  维护性修改，与 `package-portable.ps1` 并存，维护者活跃触碰，不判退役）；
- `scripts/relink-surface.py`——`docs/progress/devex.md` 在册的 relink
  面测量口径工具。

**l10n 管线（2，translations/ zh_CN 持续演进）**：`batch_translate.py`
（M2 批量翻译引擎，`.goal-loop-ledger-zh-cn-l10n.md` 与
docs/progress/full-audit-20261017/findings/agent-e.md 在册）、
`verify_l10n_offscreen.py`（M4 运行时 locale 动态加载验证）。

**竞赛/微相数据工具链（3，2026-10-22~24 活跃演进）**：
`audit_well_facies.py`、`audit_well_facies_inputs.py`（ctest
`well_facies_input_audit` 的被测对象）、`prepare_comp_data.py`
（配套单测 `tests/test_prepare_comp_data.py`）。

**视觉实证工具组（3）**：`capture_ui_visual_polish.py` /
`compare_ui_visual_polish.py` / `audit_ui_visual_polish.py`——方向 29
视觉实证档案 `docs/visual-polish/`（在册证据目录）的再生成工具组；
无 CI/ctest 引用但产物目录活跃在册，保留以维持证据可再生成。

**Windows 调试辅助（3，保守保留）**：`run_cdb.py`、`probe_tests.py`、
`compare_mods.py`——硬编码开发机路径的一次性排查脚本，随 #598 收尾
提交入库。`artifacts/issue-drafts/issue-H.md` 裁决「对有长期价值者显式
入库」并点名 `probe_tests.py`、`compare_mods.py` 两件（未点名
`run_cdb.py`——后者为 64 个保留件中活性最弱者：全仓零文字引用、
单 commit 入库后零触碰，**此处显式记档：按「不确定不迁移」原则保守
保留，如后续确认不再使用可列入下一批归档候选**）；入库仅两周，
无法确证退役，一律保留。

## C 类：测试夹具与数据/参考生成资源（保留 24 个）

- `tests/fixtures/scripts/`（9）：`tst_scriptrunner` / `tst_pythonconsole`
  的脚本执行夹具（echo/fail/flood/ignore_term/protocol/sleep/stream/
  unknown_result/utf8_echo）；
- `tests/test_audit_well_facies_inputs.py`：ctest 注册
  （unittest discover 模式）；
- `tests/test_prepare_comp_data.py`：`prepare_comp_data.py` 的单测
  （手动运行，docs/progress/full-audit-20261023 在册）；
- `testdata/onnx/gen_grid_models.py`：D15 端到端推理冒烟夹具
  `grid.onnx` 的再生工具（`tst_onnxworkflow` 使用，README 在册）；
- `spikes/onnx/gen_grid_models.py`、`gen_toy_model.py`：ONNX 可行性
  原型目录**同时是活跃测试夹具源**——`tst_onnxworkflow.cpp:72/78/183`
  直接定位 `spikes/onnx/toy.onnx`，移动即破坏测试，禁移；
- `tools/reference/`（12）：mkproject 夹具生成（`tst_mkprojectfixture`/
  `tst_io_workbook_edges` 在册引用）、singlefactor 参考生成与对拍
  （manifest/requirements.lock/`tst_singlefactor_structural` 在册）、
  io robustness 夹具（AUDIT_ISSUES.md 在册）。

## D 类：用户 Python 脚本功能面（保留 1 个 + C++ 侧在册）

- `tools/reference/scripts/example_progress.py`：方向 68 用户脚本
  契约示例，`tools/reference/scripts/README.md` 在册，且
  `src/ui/python/pythonconsolepanel.cpp` 运行时向用户指路该 README。
- C++ 侧功能面完整在册：`src/services/pythonenv.*`、`pythonrepl.*`、
  `scriptrunner.*`、`src/workflow/pythonconsolecontroller.*`、
  `src/ui/python/*`、`cmake/extra-python-scripting.cmake`——
  脚本控制台/REPL/脚本执行与测试（tst_scriptrunner/tst_pythonconsole）
  全部保留且为本审计验证对象。

## E 类：供应链/第三方工具（保留 2 个）

- `vendor/basemap/fetch-tiles.py`：离线底图瓦片获取（vendor/basemap/
  README.md、docs/progress/offline-basemap.md 在册）；
- `vendor/lock-debs.py`：deb 闭包锁生成，`vendor/fetch-deps.sh`（活跃
  构建路径）在册调用。

## 统计

| 分类 | 数量 | 处置 |
|------|-----|------|
| A 退役历史文件 | 2 | git mv → `archive/legacy-python/original/` |
| B 活跃工程工具 | 37 | 保留原位 |
| C 测试/数据生成资源 | 24 | 保留原位 |
| D 用户脚本功能面 | 1 | 保留原位 |
| E 供应链工具 | 2 | 保留原位 |
| 未定 | 0 | — |
| 合计 | 66 | — |

## 验证

见本文件同 PR 的构建/ctest 记录（PR 描述）与
`archive/legacy-python/SHA256SUMS`（`sha256sum -c` 可复验）。
