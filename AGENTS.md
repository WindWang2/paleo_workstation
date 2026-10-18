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
| 视图 | `src/ui/**` | `io/*` 白名单仅 `lasdoc.h`；`metadata/*` 白名单六头（layermanifest/paleoprojectstore/mapversionstore/releasestore/wellsectionstore/faultsetstore）；无 `algorithms/*` |
| 组装根 | `src/app` | 唯一允许 include `ui/` 的非视图目录（by design） |
| 测试壳 | `src/selfcheck` | 同 by design 豁免 |

每个 `src/` 文件头三行必须有 `// 层：<数据|功能|QGIS 封装|视图|组装根|测试壳>`。
检查器：`tools/check_layering.py`（ctest 项 `layering` + `layering_strict` +
`layering_selftest` + `layering_symbol_audit` + `layering_transitive` +
`layering_qgs_audit`）；词表外置在 `tools/layering_vocab.json`
（新顶层模块先用 `scripts/new_module.sh` 脚手架登记，否则全量判违规）。
残留清单 `tools/layering-baseline.txt` 已归零——`--strict` 闸门下非空即红
（防回升），可收缩条目同样红；非严格模式维持提示语义。

### 闸门基线纪律（#539 裁决，2026-10-18）

两类基线机制并存，语义不同，勿混用：

| 基线 | 机制 | 非空时判红？ | 收敛终点 |
|------|------|------------|---------|
| `tools/layering-baseline.txt`（层违规残留） | 归零纪律 | 红（`--strict` 非空即红） | 已归零，永不再涨 |
| `tools/qgs-audit-baseline.txt` / `tools/qgis-spread-baseline.txt`（Qgs 直连/散布，`--qgs-audit` / `check_qgis_spread.py`） | **初冻基线** | 否（现状冻结） | 按计划分批缩到零 |
| `tools/transitive-baseline.txt`（`--transitive` 四指标） | 初冻 + 计数口径 | 超基线才红 | 删 include 边后同步下调 |
| `tools/file-budget-baseline.txt` / `tools/ui-size-baseline.txt`（规模闸） | 初冻 + 冻结值 | 超冻结值才红 | 拆分后删行/下调冻结值 |

裁决要点：

1. **qgs-audit / qgis-spread 采用初冻基线，与 layering-baseline 的归零纪律
   不同期**——两者按「文件」计数（不是按 include/符号增量），已册文件是
   现状冻结区而非永久豁免区：新增直连/散布文件即红，已册文件收敛后
   `--strict` 下「可收缩」即红（强制删行）。不改检查器计数语义：增量计数
   要把「已册文件内的新增 include/符号」也打红，等价于把 149+116 个存量
   文件全部判红，闸门失去可行动性（风险可控性优先）；已册文件内的增量靠
   review 纪律 + 本表的收敛计划兜底。
2. **收敛计划（按目录分批缩到零）**，每批在 PR 描述里点名删行数：
   - 批 1（已落地，#523 收口）：导出链 4 个文件（geologicalexport.{cpp,h} /
     geologicalexportdialog.{cpp,h}）收进 src/qgis 封装后删行，qgis-spread
     153→149；
   - 批 2（下一步，优先 workflow/）：`src/workflow` 现存 49 个散布文件中
     与导出/版本登记同族的（mapexport / mapbookqueue / horizonbatchexport /
     layouttemplatelibrary）随批量版面 API 下沉 src/qgis；
   - 批 3（ui/ 侧 QGIS 原生控件族）：datapreview / layout / edittools 按
     「控件即视图」口径评估，确属视图原生的经 `qgs_audit_whitelist`
     **显式裁决**（带理由）而非静默留在基线；
   - 每批结束在 `docs/progress/` 记删行数与剩余条目数，剩余数只减不增。
3. **任何基线都只缩不涨**：判红后补登基线转绿（而非收敛代码）视为违约，
   除「上游演进导致的结构性增长」且必须在基线头注写明归因与批次
   （如 #516 的 +2、#474 接闸前 heavy_tus 83→87）——即便如此也优先修代码。
4. `--transitive` 的 func/ui 两指标需 QGIS 头目录，权威闸口在 CI lint 车道
   （Source gates 显式给 `QGIS_PREFIX`）；ctest 项 `layering_transitive` 不
   注入（同一信号不复制进 linux/windows 必过车道），本地全闸命令见
   CMakeLists.txt 该测试上方注释。

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
源码闸门（Python 检查器）统一注册为无标签 ctest 项，随 `ctest -LE 'core|perf'`
在 linux/windows 车道跑：layering 族（含 `layering_qgs_audit`）、
qgis_spread 族、ui_invariants / ui_size / **file_budget**（#538 函数体规模闸）、
i18n、ui_tokens、qgis_versions、docs_drift、ci_known_reds 等；CI lint 车道
的 Source gates 步是同一批脚本的权威闸口（见 .github/workflows/ci.yml），
`--transitive` 的 func/ui 两指标只在该步全闸（QGIS_PREFIX）。
并行度上限：构建/测试一律 `-j8` 以内（`cmake --build -j8`、`ctest -j8`）——
用户要求限制编译资源占用，不要用 `$(nproc)`。
依赖来源：尽量不依赖系统库，尽量自编译 vendored——superbuild 首选 /
钉哈希闭包加速档 / 系统包仅兜底（政策全文 BUILDING.md「依赖来源策略」）。

## Worktree 开发
独立分支使用独立 worktree/build；基线按任务明确指定的 master/origin/master
选择，保留主 checkout 的未提交改动。gitignored vendor 依赖不随 worktree
复制，共享依赖接线和显式 QGIS_PREFIX 命令见 BUILDING.md「独立 worktree 开发」。
默认分支前缀 codex/，任务指定名字优先；仅推工作分支、提 PR，不自行合并。

## PR 合并
用户要求合并 PR 时直接合并——**默认不等 CI、不要求检查全绿**；
只有请求明文写明「等 CI / 检查通过再合」才等待。
required checks 挡路时的既定流程：
`DELETE branches/master/protection/required_status_checks`
→ `gh pr merge --squash --admin`
→ `PUT branches/master/protection` 原样恢复
（checks = lint/linux/windows，strict=true，enforce_admins=true）
→ `git fetch` + `git reset --hard origin/master` 本地跟上，
并清理已合并分支（本地+远程）。
