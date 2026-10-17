# Agent E — 构建/测试/架构规范/安全与独立审计（full-review-20261017）

- 审查对象：`/home/kevin/projects/paleo_workstation-full-review-20261017`（HEAD=origin/master c720aef5，只读）
- 范围：CMakeLists.txt、cmake/*.cmake、tools/、tests/、src/app/、src/selfcheck/、resources/、translations/、.github/workflows/、BUILDING.md 一致性
- 本轮主攻：#496 (a1efbd74) 测试侧改动（tst_constraintediting +179 / tst_singlefactor_constraintcrs +70 / tst_singlefactor_constraintreceipts +89）
- 排除遵守：#475/#484/#497/#498/#501/#502/#503/#504、PR #506、codex/audit-remediation、前轮审计（COMPREHENSIVE 2026-10-16 / SWARM 2026-10-15）根因重合项只记不报

## 总体判断

#496 的三个测试文件**真实性良好**：计划侧（constraintsemantics Liang–Barsky 闭矩形谓词）与引擎侧（constraintbarriers supercover 栅格化）是独立实现，一致性测试是真交叉验证而非同码对拍；编辑会话测试用真实 GDAL 显式 FID 构造冲突；注入测试断言语义（1 行 vs 2 行）而非子串。未发现永真断言或 mock 绕过生产路径。发现的问题集中在**测试自述与实现不符**（声称四边缘只测三边缘）、**负载敏感测试未挂既有的串行/perf 分类机制**（CI 实证红）与若干工具规则盲区。构建布线整体健康（沙箱三腿齐全、extra-*.cmake 防漏登记对账、vendor 许可证可溯），新发现多为 P3 级规则/接线盲区。

---

## Findings

### E-01 [P2][HIGH] tst_wellsiting_kernel 时序比率门未挂 RUN_SERIAL/perf 分类——CI 并行轮实证红、串行洗白

- Module/File/Function：`tests/tst_wellsiting_kernel.cpp:415-449` `TestWellSitingKernel::largeFieldThroughputRatio()`；`CMakeLists.txt:1048`（注册行）与 `CMakeLists.txt:1495-1507`（`_paleo_perf_tests` 清单缺席）
- Root Cause：断言 `large >= 0.5 * reference` 依赖两次 QElapsedTimer 墙钟测量之比。参照跑 100×100 网格（亚毫秒级）、大网格 500×500，无预热、无重复取中、无最小时长下限——`max(elapsed,1)` 只防除零不防调度噪声。仓内对同类「墙钟预算/比率门」有两套既定缓解：`_paleo_perf_tests` 清单统一打 `LABELS perf` + `RUN_SERIAL TRUE`（注释明言「比率门……本机负载敏感」），以及 tst_panels/tst_correlation_full 的 RUN_SERIAL 先例注释。tst_wellsiting_kernel 两者皆未享受。
- Trigger：CI linux 车道 `-j8` 并行轮（`-LE 'core|perf'` 包含它，label=algorithms），负载下参照跑被调度器放大或大网格跑被抢占即比率塌方。
- Observed vs Expected：实证 run 38835970978 linux 并行轮 `40 - tst_wellsiting_kernel (Failed)`（本文件唯一的负载敏感断言即该函数），随后 `-j1` 重跑 0.26s 通过被 gate 放行（::warning 留痕）。Expected：要么按清单语义打 perf 标签/RUN_SERIAL，要么断言改为噪声鲁棒（多次采样取中/最小时长闸）。
- Impact：与 #501 的「串行重跑放行」机制叠加后，该测试在 CI 里是确定性噪声源：红了不拦合入（#497），绿了也不证明吞吐比——两头失真；同时每次并行失败浪费一轮串行重跑。
- Existing Match：#501 持有「重跑放行机制」本身；**本条持有根因**（测试设计 + 分类缺失），机制修复不消除此根因。NEW
- Fix Strategy：把 `tst_wellsiting_kernel` 加进 `_paleo_perf_tests`（或单独 `set_tests_properties(... RUN_SERIAL TRUE)`）；根治则把比率断言改为 ≥3 次采样取中位数并要求参照跑 elapsed ≥ 某最小值（不足则放大循环体）。
- Regression Test：无（它本身即测试）——修复验证 = CI 连续 N run 无并行轮红。

### E-02 [P3][VERIFIED] #496 edgeAligned 测试：函数名与断言相反、注释与实现不符、四边缘缺一边缘

- Module/File/Function：`tests/tst_singlefactor_constraintreceipts.cpp:640-725` `edgeAlignedBarrierIsOutsideForAllGridEngines()`
- Root Cause：该测试经半开→闭契约 rebase 后改了断言与部分注释，但（1）函数名仍叫 `...IsOutsideForAllGridEngines`，而全部断言是 `consumed == true`（闭契约 = 贴边在域内）；（2）行内注释「四条贴边线（x==±110 / y==±110 域界）」——井点 0..100 + 10% pad 的真实域是 `[-10,110]`（`src/algorithms/paleoalgorithms.cpp:533-535`：pad = 宽高 ×0.1），不存在 ±110；（3）commit message 与注释声称「四边缘 × 三引擎」，实际只加了 3 条贴边线（x=110、y=110、y=-10）+ 1 条域内对照线（x=109），**左边缘 x=-10 缺测**。
- Trigger：静态审查 + 域计算核对。
- Observed vs Expected：名称/注释/提交信息三方自述与代码行为不一致；声称的覆盖面（四边缘）未达成。
- Impact：低（断言本身有效、三引擎交叉真实），但误导后续维护者对闭契约边界的理解，且左边缘（x==xMin）恰是半开/闭契约在 x 方向的判定边界之一，缺测使「四边缘一致」的证据链有一格空洞。
- Existing Match：NEW（#496 未经任何审计，属本轮主攻）。
- Fix Strategy：改名 `edgeAlignedBarrierIsConsumedForAllGridEngines`；注释改 `[-10,110]`；补 `LINESTRING(-10 0, -10 100)` 第四条贴边线并在 `sawX` 断言组入表。
- Regression Test：即本测试自身的修正。

### E-03 [P3][VERIFIED] #496 storeFidIndex 在 snapshot() 每要素循环内全表扫描 + 重开连接——O(K·N) 提交开销

- Module/File/Function：`src/qgis/qgisconstrainteditsession.cpp:79-106`（`storeFidIndex`）、`:140-176`（`snapshot()` 内 per-added-feature 调用）
- Root Cause：F4 修复把「占用复核」放进 `snapshot()` 的要素循环：每个已映射新增要素各调一次 `storeFidIndex()`——每次 `GDALOpenEx` 新开数据集 + 全 constraints 表扫描建 `fid→身份` 表。K 个新增要素的保存 = K 次连接打开 + K×N 行读取；占用表每要素重建又随即丢弃。
- Trigger：一次编辑会话画 K 条约束后保存（每次 `finish()`/快照）。
- Observed vs Expected：K=50、表 5000 行时每次保存约 50 次连接 + 25 万行扫描。Expected：每次 snapshot 最多一次扫描（循环外取 `owners`/`live` 一次复用）。
- Impact：约束库大（导入成百上千条）时保存卡顿可感知；纯性能，无正确性问题（复核逻辑本身正确且测试覆盖）。
- Existing Match：NEW（#496 新增代码）。
- Fix Strategy：`snapshot()` 开头对 `added && known` 存在的情况做一次 `storeFidIndex`，循环内复用同一 `owners` 表与 `live`。
- Regression Test：`redoSurvivesFidReuseByOtherWriter` 已覆盖行为语义，重构后照跑即可。

### E-04 [P3][VERIFIED] check_ui_invariants 规则盲区：`setAnimated( true )` 带空格形式绕过、setStyleSheet >400 字符截断窗口、QTimeLine 不在 motion 词表

- Module/File/Function：`tools/check_ui_invariants.py:33`（MOTION_TOKENS）、`:105-122`（400 字符窗口）、`:131`（字面子串 `"setAnimated(true)"`）
- Root Cause：(1) motion 检查用字面子串匹配，`setAnimated( true )`（括号内带空格，clang-format 常见形态）不命中；(2) `setStyleSheet` 实参只取调用后 400 字符窗口，更长的样式串中 400 字符以外的裸 hex 检测不到（实测当前树恰有 1 处超窗调用、其 hex 未超窗，属潜在盲区）；(3) `QTimeLine`（QtCore 动画驱动类）不在 MOTION_TOKENS，用它编排动画不违规，与 DESIGN.md「无编排动画」语义有缝。
- Trigger：静态分析 + 对 src/ui 实测（当前零命中——均为潜在而非现行 exploited）。
- Observed vs Expected：规则宣称「禁止 setAnimated(true)」，实际只禁无空格写法。
- Impact：低——防回升闸门的词面口径比文档口径窄，回归以特定格式写入即漏网；selftest 夹具也未覆盖这些变体。
- Existing Match：#484 P3 已持有「QTWIDGETS_CLASSES 缺 QStyle 家族 / FWD_RE 不认 struct」同族词表缺口——本条是该盲区族的**新增三个实例**，根因（词表/匹配形态不完备）同族但条目不同，标记 EXCLUDED_PARTIAL(#484 语义族)，建议并入其整改。
- Fix Strategy：motion 行改正则 `setAnimated\s*\(\s*true\s*\)`；hex 窗口改为配对括号全文截取（去 400 上限）；MOTION_TOKENS 增补 `QTimeLine`、`QAbstractAnimation`。
- Regression Test：`--selftest` 夹具补对应反例。

### E-05 [P3][VERIFIED] check_ui_tokens ratchet 只数例外条目数不数覆盖次数——per-entry count 膨胀不触发闸门

- Module/File/Function：`tools/check_ui_tokens.py:197`（`ratchet_lines(len(exceptions), ...)`）vs `:78`（`used[key] < exception["count"]` 消费计数）、`tools/ui-token-baseline.txt`（=358）
- Root Cause：strict ratchet 以「exceptions.json 条目数」对 baseline 358 做只降不升；但单条例外可经 `count` 字段覆盖多次出现。当前 358 条实际覆盖 **495** 处字面量（94 条 count=2、15 条 count=3、3 条 count=4、1 条 count=5）。把某条 count 从 1 提到 5 并复制对应源码行 4 次，条目数不变、ratchet 绿、豁免面 +4。
- Trigger：静态核对 exceptions.json 聚合。
- Observed vs Expected：baseline 文件自述「例外计数只降不增」，实际闸的是条目数而非（条目数或总覆盖次数）。
- Impact：低（需有意钻空才触发）；stale 检测（`used != count`）保证了 count 不能虚挂，但不防真实膨胀。
- Existing Match：NEW。
- Fix Strategy：ratchet 改为 `sum(e["count"] for e in exceptions)` 对 baseline（或双计数都记），baseline 注释同步语义。
- Regression Test：selftest 的 ratchet 段补「同条目 count 抬升 → strict 红」用例。

### E-06 [P3][VERIFIED] verify_l10n_offscreen.py（20 项运行时 l10n 验收器）无任何闸门接线——「有工具无闸门」第 4 例

- Module/File/Function：`tools/verify_l10n_offscreen.py`（全文）；grep 全仓：CMakeLists/cmake/.github 零引用
- Root Cause：脚本覆盖 --lang/PALEO_LOCALE 优先级、zh 变体归一、注入尝试回落（`zh_CN; reboot`）等 20 项运行时行为，比 ctest 的静态 `check_l10n.py`（.ts 完整性）强一档，但既不在 ctest 也不在 CI——main.cpp 的 locale 解析回归只能靠人记着手跑。
- Trigger：静态 grep。
- Observed vs Expected：main.cpp:34-146 的 locale 解析/翻译装载链（含安全相关的注入回落）无自动回归防线。
- Impact：低概率高隐蔽——i18n 装载链回归在静态门（check_i18n/check_l10n）全绿的情况下静默失效为英文 UI。
- Existing Match：swarm review「有工具无闸门 ×3」（symbol-audit #474 / transitive #484 / continue-on-error #484）为同族主题但未列本工具。NEW（第 4 例，并入该族整改）
- Fix Strategy：接一个 ctest 项（依赖 build/paleo 与 build/translations/paleo_zh_CN.qm 存在性，缺失 SKIP），或至少进 CI linux job 非阻断步。
- Regression Test：即脚本本体。

### E-07 [P3][VERIFIED] lrelease 缺失仅 WARNING：中文 UI 静默回落英文，无任何闸门验证 .qm 资源真的进了二进制

- Module/File/Function：`cmake/extra-ux.cmake:68-70`（else 分支 WARNING）；`src/app/main.cpp:239-249`（运行时 qWarning 回落）
- Root Cause：`qt6_add_resources` 只在找到 lrelease 时执行。未装 qttools 的本地构建 configure 只发 WARNING，`:/i18n/paleo_zh_CN.qm` 不存在，中文用户拿到英文 UI + 一条运行时 qWarning。没有任何 ctest/CI 断言「构建产物包含翻译资源」（E-06 的工具恰有此项但未接线）。
- Trigger：无 qt6-linguisttools 环境配置构建（CI 装 qt6-tools-dev 故不受影响）。
- Observed vs Expected：期望缺 lrelease 时至少在 selfcheck/冒烟层可见（selfcheck 8 项里无 i18n 项）。
- Impact：低（CI 不触发）；本机/新贡献者环境静默丢翻译。
- Existing Match：NEW。
- Fix Strategy：paleo_selfcheck qgis 组加一项「PALEO_HAVE 翻译资源存在性」（或 lrelease 缺失时把 target 定义为占位并让 selfcheck 红）；至少把 WARNING 升为「构建后 selfcheck 必红」。
- Regression Test：selfcheck 新项。

### E-08 [P3][SUSPECTED] 工具门禁 ctest 注册整体静默依赖 `find_program(PALEO_PYTHON3)` 成功——无 python 的 configure 零告警丢全部闸门

- Module/File/Function：`CMakeLists.txt:944-1018`（`if(PALEO_PYTHON3)` 包住 layering/ui_invariants/i18n/ui_tokens/qgis_versions/docs_drift/lo_pin 全部注册与 WORKING_DIRECTORY）
- Root Cause：python3 未找到时整块跳过，无 message(WARNING)，ctest 清单里这些门静默消失；`l10n` 注册（extra-ux.cmake:76-90）独立再找一次 Python3，同样静默。CI 镜像有 python3 所以现网无感。
- Trigger：极简构建环境（无 python3 的交叉/容器配置）。
- Observed vs Expected：期望缺 python3 时 FATAL_ERROR 或至少 STATUS/WARNING + 一项「gates_missing」占位测试。
- Impact：低概率、高失效面（一次丢 20+ 个门禁项且不可见）。
- Existing Match：NEW（#484 清单未列）。
- Fix Strategy：`else()` 分支 `message(WARNING "python3 未找到——layering/i18n/ui_tokens 等 20+ 门禁未注册")`，并注册一个恒红的占位 ctest 项防静默。
- Regression Test：占位项自身。

### E-09 [P3][VERIFIED] run-target-tests.sh（#496 新入库）固定写 `/tmp/<test>.log`——并发/多用户互踩与名字碰撞

- Module/File/Function：`run-target-tests.sh:15-18`
- Root Cause：日志路径硬编码 `/tmp/$t.log`（$t 是可执行名），两个会话/两个 worktree 同时直跑同一测试即互相覆盖日志；共享主机上可被预置符号链接种植（低敏感：只写不执行）。
- Trigger：dev 双 worktree 并行使用该脚本（正是本仓标准工作流）。
- Observed vs Expected：应 `mktemp` 或写到 `build/`。
- Impact：低——仅开发体验/日志可信度。
- Existing Match：#504 持有仓库根脚本卫生面；该文件本身入库的取舍归 #504 族；**/tmp 固定路径缺陷**为 NEW 小项，修一行。
- Fix Strategy：`log="$(mktemp -t paleo-tgt-$t.XXXXXX.log)"`。
- Regression Test：无。

### E-10 [P3][VERIFIED] make_appimage.sh 从 GitHub `continuous` 浮动 tag 下载 AppImage runtime 后 chmod +x，无校验

- Module/File/Function：`tools/make_appimage.sh:33-37`
- Root Cause：`releases/download/continuous/runtime-x86_64` 是可变引用；下载后直接 `chmod +x` 并作为产物运行时嵌入。对照同仓 `ensure_qgis_resources.py:118-128`（sha256+size 校验）与 vendor manifest 的钉哈希政策，这是唯一无校验的外部执行件下载。
- Trigger：运行 make_appimage.sh（非 CI，本地发布动作）。
- Observed vs Expected：与「依赖来源策略：钉哈希」精神不符。
- Impact：低（发布工具、非默认路径），供应链面一角。
- Existing Match：NEW。
- Fix Strategy：钉具体 release tag + sha256（AppImage 官方 runtime 有版本 release）；或至少记录 sha 并提示人工核对。
- Regression Test：无。

### E-11 [P3][VERIFIED] #496 CRS 迁移「用户可达路径」的门面与 UI 按钮路径零测试（底层 store 已测）

- Module/File/Function：`src/workflow/constraintworkflow.cpp`（`constraintLayerCrsState`/`migrateConstraintLayerCrs` 新增门面）、`src/ui/pages/constraintpage_constraints.cpp:328-361/506-520`（状态行 + 迁移按钮）
- Root Cause：#496 的核心主张之一是「inspectCrsStorage/migrateLegacyCrs 此前只有测试调用→补可达性」，但补的可达层（workflow 门面 + 页面按钮/确认框/状态显示）无任何测试引用（grep tests/ 零命中）；现有覆盖停在 `tst_singlefactor_store.cpp:157-221` 的 store 层。门面是薄委托，但「legacy 库显示 ⚠ + 按钮、新库不显示」的显示分支（crsState 0/1/2 → visible）无回归防线。
- Trigger：静态 grep。
- Observed vs Expected：同 commit 为其余五节都配了测试，独可达性节没有。
- Impact：低（薄代码）；UI 显示分支回归（如 crsState==2 仍显示按钮）无人拦。
- Existing Match：NEW。注意 `constraintpage*` 七文件在排除矩阵 D 区被主 checkout 会话硬排除——本条只报测试缺口不改其代码。
- Fix Strategy：tst_constraintediting 或 mappingpages 侧补一条：legacy 库 → 门面返回 1；迁移后 → 2；无库 → 0（门面三态可直接测，不碰 UI 文件）。
- Regression Test：即所补测试。

---

## 排除核对记录（根因重合，只记不报）

| 观察 | 持有方 |
|---|---|
| UBSAN `halt_on_error=0`（CMakeLists.txt:904） | 前轮 TEST-P0-01 + codex/audit-remediation 已改 |
| ASAN 车道 `detect_leaks=0` | 文档化口径（CMakeLists:38 注释），非缺陷 |
| ci_ctest_gate.sh 断言类失败串行重跑放行 | #501 |
| lint 缺 layering --selftest；check_layering transitive 债无防增长闸 | #484 P2 |
| QTWIDGETS_CLASSES 缺 QStyle 家族 / FWD_RE 不认 struct | #484 P3 |
| tst_procdialog 等 8 文件 QSettings::setPath /tmp 残留 | #484 P3 |
| label 正则 `^paleo_[a-z]+$` 不匹配带下划线库 | #484 P3 |
| CalligraRenderer.cmake + officepreviewrenderer 零引用 bitrot | #484 P3 |
| `tst_predict_challenger_r2` challenge5 三函数零断言（-9999 缺陷绿灯化） | 前轮审计 5.1（仍未修，现状与之一致） |
| `/tmp/wellsection-shots` mkpath、grab().save("/tmp/...") 截图 | 前轮审计 5.3（仍在） |
| golden 8×6 采样 4/48 容差（tst_wellcomposite_visual） | #475 |
| windows 车道 TestMappingWorkbench/TestVersions QFile::remove 红（run 38835970978） | #499 |
| 仓库根 goal-loop-prompts.zip / leak-test-root / r0-r1-red-set / prototype/*.png / artifacts/l10n 追踪垃圾 | #484/#504 |
| CI 门禁失效（master 连红仍合入） | #497（+PR #506 活跃） |
| tst_evidencee2e 同 run 并行红/串行绿 | 根因未定位：文件内无墙钟断言（仅 qInfo BASELINE 计时），无法归因于时序断言；其放行机制归 #501，若再现建议取 artifact `test-logs-linux` 的 qtest-first-run 输出定位后单独立项。记 SUSPECTED 待证 |

## 覆盖清单

### 深读（真实性/规则逐条核）
1. tests/tst_constraintediting.cpp（1313 行：#496 新增三测 + 既有槽位全列 + 408 断言密度核对）
2. tests/tst_singlefactor_constraintreceipts.cpp（828 行全文：helpers/16 行约束夹具/5 组 plan-vs-engine/edgeAligned/rasterization 系列）
3. tests/tst_singlefactor_constraintcrs.cpp（#496 diff 全量 + toStoreWkt/PROJ affine 往返）
4. tests/tst_receiptsemantics.cpp（306 行全文）
5. tests/tst_impactrecompute.cpp（fixture 全栈真实服务栈核对）
6. tests/tst_evidenceindex.cpp（槽位清单 + 150 断言密度）
7. tests/tst_evidencee2e.cpp（354 行主要路径 + 计时面排查）
8. tests/tst_wellsiting_kernel.cpp（452 行全文，定位唯一负载敏感断言）
9. tests/tst_onnxworkflow.cpp（22 处 QSKIP 条件核对——fixture 均在库，回退式跳过）
10. tests/tst_wellcomposite_visual.cpp（golden 采样/容差机制）
11. tests/tst_attrpanel.cpp（nullProviderSafe 无断言槽——crash-smoke 型，可接受）
12. tests/tst_test_sandbox.cpp（Windows hive 契约）
13. tests/tst_predict_challenger_r2.cpp（challenge5 零断言现状核对）
14. tests/tst_cache_core.cpp（lru 并发/detached thread 安全边界）
15. tests/tst_seismic_3dviz.cpp（GL 环境 QSKIP 先例核对）
16. tests/tst_sections_alignment.cpp + 17. tests/tst_layoutexport.cpp（/tmp 写面核对——后者两处均走失败路径不落盘）
18. tests/mockremotepredictionservice.h（mock 边界核对——网络边界镜像，合法）
19. tests/tst_singlefactor_store.cpp（CRS inspect/migrate 覆盖定位）

### 工具/构建/CI
- tools/check_layering.py（930 行：规则实现 vs AGENTS.md 词表逐条一致；--selftest 实跑通过；baseline 空）
- tools/check_ui_invariants.py（全文 + 400 字符窗口实测）
- tools/check_i18n.py（全文 + 未扫入口族实测：QListWidgetItem/addItem 现树均为数据值或已 tr，无实漏）
- tools/check_ui_tokens.py（全文 + ratchet 计数核对 358 条/495 次）
- tools/check_env_redset.py（头部/分类语义）、tools/check_ci_scripts.py（解析口径）、tools/ci_ctest_gate.sh（全文）、tools/update_translations.sh、tools/verify_l10n_offscreen.py、tools/ensure_qgis_resources.py（下载校验）、tools/make_appimage.sh、tools/batch_translate.py（subprocess 列表式）
- CMakeLists.txt（1548 行通读：sandbox/link-sandbox/add_paleo_test/extra 清单对账/perf 清单/envdebt 标签/ORT ENVIRONMENT_MODIFICATION）
- cmake/extra-ux.cmake（全文）、cmake/extra-data.cmake、extra-xplot-sup-*.cmake（抽样）
- .github/workflows/ci.yml（447 行全文）
- src/app/main.cpp（全文）、src/app/mkproject.cpp（路径处理面）、src/selfcheck/main.cpp（全文）
- resources/（fonts OFL/geology/qrc）、translations/（.ts 6274 message 零 unfinished/零 vanished；.qm 构建期生成不入库）
- BUILDING.md（384 行全文 vs CMakeLists/CI 对拍：prefix 解析、-j8、沙箱、层护栏、perf 豁免名单均一致）
- vendor 许可证面（saribbon MIT/segyio/glm/sbm 无上游 license 已文档化/mamcl 13MB zip 入库）

### 统计
- 新发现：P2×1（E-01）、P3×10（E-02..E-11）
- 排除核对：16 项
- CI 实证取证：run 38835970978 linux/windows 日志（78068 行）
