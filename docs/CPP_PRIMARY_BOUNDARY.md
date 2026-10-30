# C++ 主线边界（CPP primary boundary）

一句话口径：**主程序与原生业务模块以 C++20 为唯一权威实现**——Qt6 Widgets
是界面基础，vendored QGIS 4.2.x C++ API 是 GIS 底座，地质业务逻辑由
`src/` 各层 C++ 模块实现。Python 只出现在开发/测试/质量闸门与用户显式
启用的可选脚本功能里，永不作为主程序的运行基础。

本文是这条边界的速查与约束清单；分层契约（谁可以 include 谁）以
[AGENTS.md](../AGENTS.md) 的层表与 [docs/UI_LAYER_PLAN.md](UI_LAYER_PLAN.md)
为准，本文不重复。

## 现状锚点（2026-10-25）

- `src/` **零 `.py` 文件**；全仓非 vendor 的 `.py` 不含业务/运行时的
  qgis/PyQt 导入（开发工具的可选导入除外，如 `tools/verify_l10n_offscreen.py`
  的 PyQt6 翻译校验；PyQt6 旧原型在仓库外冻结检出，
  见 `tools/reference/singlefactor/README.md`）。
- 主程序：`add_executable(paleo src/app/main.cpp)`
  （`cmake/paleo-tests.cmake`），链 `paleo_core` 分层静态库伞。
- 可选 Python 能力全部是 **C++ 实现 + QProcess 子进程**：
  `src/services/scriptrunner.*`（用户脚本，方向68）、
  `src/services/pythonrepl.*`（REPL 会话桥）、
  `src/services/pythonenv.*`（MAMCL venv 底座）、
  `src/ui/python/`（控制台/REPL 面板）。无内嵌解释器、无 Python C API。
- superbuild 构建的 QGIS 保持 `WITH_BINDINGS=OFF`
  （`vendor/superbuild/CMakeLists.txt`）——PyQGIS 运行时不进依赖闭包。

## 构建与启动入口

```bash
./paleo-dev bootstrap          # 或按 BUILDING.md 手动 vendor
./paleo-dev build && ./paleo-dev test
build/paleo                    # 主程序（GUI）
build/paleo_selfcheck          # QGIS 运行时自检
```

安装面是「最小可用面」（`cmake/extra-install.cmake`）：只装主程序与
selfcheck；QGIS/GDAL 运行时闭包不随默认安装复制，MAMCL 外部工具包经
运行时解包（`cmake/extra-mamcl.cmake`，SHA-256 编译期核对）。

## Python 的三类合法用途

| 类别 | 内容 | 位置 |
|------|------|------|
| Development/Validation | 源码闸门（layering/i18n/ui_size/…）、CI 脚本、打包与 vendor 钉版 | `tools/`、`vendor/*.py`、`.github/workflows/` |
| 测试/数据 | 夹具与黄金对拍生成器、进程级测试夹具 | `tools/reference/`、`tests/fixtures/scripts/`、`spikes/` |
| Optional Scripting（运行时，用户显式启用） | 用户脚本控制台、REPL、MAMCL venv | C++ 模块 + `tools/reference/scripts/` 示例 |

口径推论：**「应用无内嵌 Python」成立，「整个仓库无 Python」不成立**——
质量闸门与部分测试依赖 python3（`cmake/paleo-test-infra.cmake` 的
`find_program(PALEO_PYTHON3)` 找不到时 configure 直接 FATAL_ERROR）。
运行期 Python 依赖只在用户启用对应功能时出现（PATH 发现或
`PALEO_PYTHON`/`PALEO_CORRELATION_PYTHON` 显式指定）。

## 历史 Python 原件归档

- 历史遗留 Python 源件的**原件归档**位于 `archive/legacy-python/`
  （由独立归档任务维护；该目录可能尚未落地，边界检查容忍其缺席）。
- 归档约定：**不进构建、不进默认安装/打包清单、主线代码零引用**；
  需要说明历史时放 `docs/` 并注明「历史/归档」。
- 归档件只读保留，不得从中把 Python GUI/业务实现「复活」回主程序
  （见下节闸门）。

## 新模块/新工具开发规则

- 新 C++ 模块：`scripts/new_module.sh` 脚手架登记词表，遵守分层契约；
  新源文件/测试一律挂 `cmake/extra-<方向>.cmake` 并登记进根
  CMakeLists.txt 的 `_paleo_extra_manifest`。
- 新 Python **工程工具**：放 `tools/`（层：测试壳），需要进闸门的按
  `paleo-test-infra.cmake` 现有模式注册（无标签 add_test +
  `WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}`，配 `--selftest`）；**不得放
  `src/`**。
- 业务功能需要脚本能力：走 `ScriptRunnerService`/`PythonEnvService`
  既有契约（子进程、JSON 行协议，示例见 `tools/reference/scripts/`），
  不内嵌解释器、不引入 PyQGIS。
- 供货链辅助脚本（vendor 钉版、打包）：保持 `vendor/*.py` 现状模式，
  修改需过 `check_ci_scripts.py` 引用存在性门禁。

## 防回退闸门（机械执行）

`cmake/verify-cpp-mainline.cmake`（cmake -P，零 Python 依赖）六条规则，
ctest 项 `cpp_mainline` / `cpp_mainline_selftest`（无标签，随
`ctest -LE 'core|perf'` 跑），CI 侧权威闸口在 lint 车道 Source gates 步：

| 规则 | 拦截 |
|------|------|
| `src-python` | `src/` 出现 `.py`/`.pyw` 应用源码（按扩展名判，不按文件名含 python——`pythonenv.*` 等合法 C++ 模块不受影响） |
| `archive-ref` | `src/` 源码、CMake、安装/打包入口引用 `archive/legacy-python` |
| `install-py` | 默认安装/打包清单装 `.py` 文件 |
| `module-targets` | 必需 C++ 模块库/主程序 target 声明从 CMake 失踪 |
| `python-ext` | 合法 Python 扩展的源文件/extra 挂点/测试登记、闸门解释器发现被拆除（防「删模块式假纯化」） |
| `superbuild-bindings` | superbuild 失去 `WITH_BINDINGS=OFF` |

selftest 在合成夹具树上对六条规则做正反向验证（含归档目录存在/不存在
两种容忍态），检查器自身坏掉不会静默变绿。

## 质量验证流程（与主仓一致）

本地：`cmake --build build -j8` + `ctest --test-dir build -j8`；CI：
lint（Source gates 权威闸口）/ linux / windows / linux-perf /
linux-asan 车道。文档口径漂移由 `docs_drift` 闸门看守，CI 红项分诊见
`docs/ci/known-reds.json`。
