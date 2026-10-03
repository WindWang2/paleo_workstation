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
检查器：`tools/check_layering.py`（ctest 项 `layering` + `layering_strict` +
`layering_selftest`）；词表外置在 `tools/layering_vocab.json`
（新顶层模块先用 `scripts/new_module.sh` 脚手架登记，否则全量判违规）。
残留清单 `tools/layering-baseline.txt` 已归零——`--strict` 闸门下非空即红
（防回升），可收缩条目同样红；非严格模式维持提示语义。

已知边界：各层编进 `paleo_<模块>` 分层静态库（`paleo_core` 是 INTERFACE
兼容伞，不再编代码），护栏只挡 include 层——
「不带 include 直接 new」挡不住，靠 review 纪律（见 TODOS.md 递延项）。

## 构建与测试布线
测试统一走 `add_paleo_test(name [LIBS ...])`（CMakeLists.txt）：单参 = 伞式
旧式契约（cmake/extra-*.cmake 并行方向零改动可用）；`LIBS` 给最小链接集
（瘦 relink 面，口径见 docs/progress/devex.md）。每测试自动获得独立
XDG_CONFIG_HOME/XDG_DATA_HOME 沙箱（QSettings 并行竞态根治；Windows
NativeFormat 走注册表不受 env 控制，Windows 侧保持串行）。
构建加速与实验档（ccache launcher / PALEO_ENABLE_ASAN / PALEO_ENABLE_UBSAN /
PALEO_UNITY_BUILD）见 BUILDING.md。
并行度上限：构建/测试一律 `-j8` 以内（`cmake --build -j8`、`ctest -j8`）——
用户要求限制编译资源占用，不要用 `$(nproc)`。
依赖来源：尽量不依赖系统库，尽量自编译 vendored——superbuild 首选 /
钉哈希闭包加速档 / 系统包仅兜底（政策全文 BUILDING.md「依赖来源策略」）。

## Worktree 开发
独立分支使用独立 worktree/build；基线按任务明确指定的 master/origin/master
选择，保留主 checkout 的未提交改动。gitignored vendor 依赖不随 worktree
复制，共享依赖接线和显式 QGIS_PREFIX 命令见 BUILDING.md「独立 worktree 开发」。
默认分支前缀 codex/，任务指定名字优先；仅推工作分支、提 PR，不自行合并。
