# Agent E 审查报告（full-audit-20261021）

审查范围：CMakeLists.txt + cmake/extra-*.cmake（测试注册/依赖接线/平台分支）、
tools/ 全部源码闸检查器（自身正确性 + 基线与现实同步）、tests/ 抽样 18 个测试
（真实性/flaky 模式/共享状态）、src/app/（组装根初始化/关闭序）、src/selfcheck/、
resources/ 与 .github/workflows/ci.yml（结构与已知红登记对应）。
工作树 BASE e6837e14，只读；实跑了全部可只读执行的检查器与三个针对性
python 复现脚本（证据见各条）。

汇总：8 项新发现（P2×4、P3×4）。已知红（qgs-audit 118/116、qgis-spread
151/149、file_budget 回升 3+新增 1、ui_tokens、docs_drift 27 TU）复跑均在，
但均为已知 147 项 E.2–E.9/H6 同根因，不再列入。

---

## E-1 提交进 master 的 582 行测试从未被注册编译——整文件零保护，且无孤儿测试防漏机制

- **Severity**: P2 ｜ **Confidence**: VERIFIED ｜ **Module**: tests / build wiring
- **File:line**: `tests/tst_horizonbatchexport_diag.cpp`（全文 582 行）；
  对照 `CMakeLists.txt:1295`（只注册了 `tst_horizonbatchexport`）。
- **根因**: 提交 `5a3cbe83`（PR #564，"批量导出健壮性与定向测试"）新增
  `tests/tst_horizonbatchexport_diag.cpp`（同 PR 给已注册的
  `tst_horizonbatchexport.cpp` 追加 147 行），但**没有在任何
  `add_paleo_test` / `add_executable` / `add_test` 登记该 `_diag` 目标**。
  `add_paleo_test` 按名取 `tests/${name}.cpp`，无 GLOB；CMakeLists 的
  FATAL_ERROR 对账（:1623-1632）只覆盖「磁盘上的 cmake/extra-*.cmake 是否
  在清单内」，不覆盖「tests/*.cpp 是否有注册方」。
- **验证**: `git ls-files` 确认已提交；全仓（CMakeLists.txt + cmake/*.cmake）
  grep `horizonbatchexport_diag` 零命中；`comm` 对账 403 个 tests/*.cpp 与
  全部注册名，唯此文件落单（tst_sanitizer_budget_on 为双目标共用源、tst_foo
  为文档示例，均正常）。文件内容含像素一致性断言、版面复原断言、catalog
  OUTPUT 登记断言——正是 #564 声称补的「定向测试」面，实际一行都没跑过。
- **Observed vs Expected**: 期望定向回归被钉死；实际 PR 声称的测试保护为零，
  且后续维护者会误以为该面有覆盖。
- **影响面**: 批量出图链（layoutexport/standardelements/horizonbatchexport）
  的回归保护缺口；更广地，任何「提交测试文件但忘注册」都会同样静默消失
  ——P1-14/TEST-01 的「测试漏注」宣称根治，实际只根治了 extra-*.cmake 漏注。
- **排除比对**: 不在 147 项（H5 是 tst_assetpaging 断言过期，不同根因）；
  上两轮修复清单无此文件根因。文件在 02-excluded-files.txt:235（活跃分支
  触碰中），可能正是修复分支在处理——若该分支已注册/删除此文件，本条按
  EXCLUDED_ACTIVE_WORK 归并。
- **修复建议**: 注册或删除该文件；根治面是给 CMake 加一条对账：`tests/tst_*.cpp`
  磁盘清单 vs 注册名集合，未注册即 FATAL_ERROR（与 extra-*.cmake 对账同构）。
  回归测试：对账本身即测试。

## E-2 Qgs 直连/散布两闸只扫 workflow+ui——algorithms/linkage/services 的直连 qgs* 面对三道闸全部隐形

- **Severity**: P2 ｜ **Confidence**: VERIFIED ｜ **Module**: tools/check_layering.py, tools/check_qgis_spread.py, 架构闸门
- **File:line**: `tools/check_layering.py:707`（`collect_qgs_audit` 的
  `iter_files_under(("workflow", "ui"))`）、`tools/check_qgis_spread.py:48`
  （`SCAN_DIRS = ("workflow", "ui")`）；盲区数据面：`src/algorithms/`
  16 个文件直接 include 外部 qgs*.h（`welldistance_internal.h:4-12`、
  `mincurvature.cpp:6-23`、`faciespolygonize.cpp:5-8`、`distancetransform.cpp`
  等）、`src/linkage/` 4 个（`wellmaplink.{h,cpp}`、`seismicmaplink.{h,cpp}`）、
  `src/services/crashreport.cpp:16`（`<qgsconfig.h>`）。
- **根因**: 三道闸的覆盖矩阵有洞：
  1. `check_layering` 的 data-qgis-include 规则（:241）只拦「归一后落在
     repo `qgis/` 目录」的 include；外部 `<qgsfeature.h>` 经 `norm_include`
     （:179-188）被归一成**不存在的同目录幽灵路径**（`algorithms/qgsfeature.h`），
     `dst == layer_dir`，任何规则都不触发；
  2. `--qgs-audit` 与 qgis-spread 的扫描面硬编码 workflow+ui。
  本会话以 check_layering 自身函数实跑证明：`// 层：数据` 文件里
  `#include <qgsvectorlayer.h>` + `class QgsVectorLayer;` + `new QgsMapCanvas`
  → violations 为**空**（临时树 + `L.SRC` 重定向，见审查记录）。
- **触发条件**: 在 src/algorithms / src/linkage / src/services 新增或扩散
  直接 qgs* include / Qgs 符号——三道闸全绿，基线不变。
- **Observed vs Expected**: `docs/PALEO_QGIS_PLAN.md` §2 明文「应特别避免
  业务代码直接散布 QGIS API 调用……只修改这一层」；AGENTS.md 闸门纪律也
  以该口径立闸。实际现状冻结面只覆盖 2 个目录，其余 ~21 个直连文件
  （algorithms 16 + linkage 4 + services 1）既不在册也无闸，属**未冻结的
  已存债 + 可静默增长的面**。
- **排除比对**: 已知 E.9（147 项）是「identifytool 使 workflow/ui 面新增 2
  文件致闸红」——同闸不同根因（计数超标 vs 扫描面缺口）；#558 是 transitive
  四指标债，不相交。非已知项。
- **修复建议**: 二选一或并行：① 把 `SCAN_DIRS`/qgs-audit 扫描面扩到全部
  non_view 目录（algorithms/linkage/services 现状 `--emit-baseline` 初冻，
  进 AGENTS.md 收敛计划批次）；② 至少在 check_layering 给数据层加
  「basename 匹配 qgs*.h 的外部 include」规则（与 QtWidgets 禁令同款词表制）。
  回归：E-2 的实跑脚本可直接改造为 selftest 夹具。

## E-3 ci_triage_reds.classify 对「车道缺席」fail-open：空 jobs 载荷与改名车道都静默全绿

- **Severity**: P2 ｜ **Confidence**: VERIFIED ｜ **Module**: tools/ci_triage_reds.py（triage job）
- **File:line**: `tools/ci_triage_reds.py:275-281`（`conclusion = jobs.get(lane)`
  为 None 时落入 `if conclusion not in ("failure",): continue`，cancelled/skipped
  有 NOTE，**None 无任何提示**）；`:231-235`（`parse_jobs` 对合法 JSON
  `{"jobs": []}` 返回 `{}` 而非 None）；对照 `:227-230` 文档自称
  「空映射不能当『全绿』静默放行」。
- **验证（本会话实跑）**: 以 `classify(base_reds, {}, {"linux": <含
  LastTestsFailed.log: tst_important 的日志树>})` 直调 → **rc=0**、零输出
  （一个实际存在测试级红的车道被完全无视）；再以
  `{"renamed-linux": "failure"}` 模拟车道改名 → 同样 rc=0、无 NOTE。
- **触发条件**: ① jobs.json 为合法但空的 jobs 列表（gh api 返回结构变化/
  过滤/分页截断——当前 5 job < per_page=100，分页暂不可达，但 payload
  形状变化即中）；② ci.yml 里 job 改名而 known-reds lanes 未同步（没有任何
  对账拦这种漂移）。
- **Observed vs Expected**: triage 的存在意义（#497「红得明白」）是未知红
  必 exit 1；实际上述两种形态下它对全部车道静默放行。fail-open。
- **影响面**: `triage` job 本身（按 ci.yml 注释尚未列入 required checks，
  故不构成门禁绕过，但信号面失效——正是 #497 要恢复的东西）。
- **排除比对**: 上轮已修的 E-10/E-12/E-13 是「闸门接线」（ctest 项注册/
  --validate 接线），本条是 classify 自身的 fail-open 逻辑洞，不同根因。
- **修复建议**: ① `parse_jobs` 对空 jobs 映射返回 None（走 exit 2）；
  ② classify 开头对「gating 车道不在 jobs 键集」逐条 `::error` 或至少
  FATAL——车道改名必须显式同步 known-reds。回归：本条的两个实跑脚本
  直接进 `--selftest`。

## E-4 flaky 台账在 CI 上结构性无法累计——「重跑放行 ≥3 次点名/升红」只在开发者本机生效

- **Severity**: P2 ｜ **Confidence**: VERIFIED ｜ **Module**: tools/ci_ctest_gate.sh, CI 布线
- **File:line**: `tools/ci_ctest_gate.sh:25`（ledger = checkout 内
  `tools/flaky-ledger.jsonl`）、`.gitignore:33`（该文件 gitignored）、
  `.github/workflows/ci.yml` 全文 grep `flaky` **零命中**（缓存只覆盖
  ~/.cache/ccache 与 vendor/*；上传只覆盖 build/Testing）。
- **根因**: #501 的升级机制（`check_flaky_ledger.py` 按 JSONL 累计命中，
  ≥3 打 ::warning、PALEO_FLAKY_STRICT=1 升红）依赖台账跨轮持久；CI runner
  是一次性环境，每次运行从空台账开始，单轮内同一测试至多记 1 次
  （ci_ctest_gate.sh:97-109 只在「本轮重跑通过」时记一次）——阈值 3 在 CI
  上**永远达不到**。而重跑放行恰恰在 CI 并行负载下最常发生（脚本自己的
  注释就以 CI run 举过例）。
- **Observed vs Expected**: 期望「同一测试反复靠重跑放行会被累计点名/升级」
  （脚本头注释与 paleo-dev.ps1:508 的用户可见承诺）；实际 CI 上该承诺
  恒不触发，绿灯通胀防回归只剩单轮 ::warning 留痕。
- **影响面**: lint/linux/windows 车道的 flaky 升级通道（观察信号，不阻断）。
- **排除比对**: 已知 E.8 是 known-reds.json 登记与现实脱节（另一个名单）；
  #501 机制缺口不在已知 147 项。非已知项。
- **修复建议**: 最小改法——ci_ctest_gate.sh 把 `tools/flaky-ledger.jsonl`
  复制进 `build/Testing/`（随既有 test-logs artifact 上传），并在 job 开头
  从上轮 artifact 恢复（actions/cache 或 gh run download 最近成功 run）；
  或者接受「CI 不累计」现实，把承诺措辞降级为本机工具。回归：CI 侧人工
  验证连续 3 轮重跑放行后第 3 轮出现 ::warning。

## E-5 lint「Source gates」步缺 ui_tokens（及 qgis_spread selftest 等）——与「权威闸口」口径不一致

- **Severity**: P3 ｜ **Confidence**: VERIFIED ｜ **Module**: .github/workflows/ci.yml
- **File:line**: `.github/workflows/ci.yml:120-140`（Source gates 20 条命令）；
  对照 `CMakeLists.txt:1085-1090`（ui_tokens / ui_tokens_strict /
  ui_tokens_selftest 三个 ctest 项）与 AGENTS.md「CI lint 车道的 Source gates
  步是同一批脚本的权威闸口」。
- **根因**: Source gates 清单与 ctest 源码闸清单手工双维护，已漂移：缺
  `check_ui_tokens.py`（全部三档）、`check_qgis_spread.py --selftest`、
  `ci_triage_reds.py --validate`（ci_known_reds）、lo_pin、env_redset 族。
  其中 ui_tokens 有 ctest 车道兜底（无保护损失），但：①「权威闸口」的
  文档口径失实；② lint job-scope 已知红登记（lint-source-gates-red-20261018）
  的语义是覆盖 lint 步红——将来 ui_tokens 若只在 lint 侧修红会没有对应
  执行点，反向（ctest 红、lint 不跑）则 triage 靠 lint job-scope 条目降级时
  名不副实。
- **排除比对**: H6/E.7 是「闸门红」，本条是「权威闸口清单缺脚本」——相邻
  但不同根因（H6 未提及 lint 步缺 ui_tokens）。
- **修复建议**: 补齐 Source gates 与 ctest 项的单源对照（或直接在 lint 步
  逐 ctest 项枚举）；至少补 `check_ui_tokens.py --strict` + `--selftest`。

## E-6 check_qgis_versions 的 URL_HASH/补丁名正则绑定到「文件尾」而非 qgis 块尾——追加任一钉哈希工程后即静默错绑

- **Severity**: P3（当前潜伏） ｜ **Confidence**: VERIFIED（逻辑）/ 潜伏（当前不可达） ｜ **Module**: tools/check_qgis_versions.py
- **File:line**: `tools/check_qgis_versions.py:59-66`：`block = text[idx:]`
  从 `ExternalProject_Add(qgis` 一直切到**文件尾**，`URL_HASH SHA256=…` 与
  `PATCH_FILE=…qgis-<ver>-*.patch` 两个正则在该开区间上搜索。
- **根因**: 当前 `vendor/superbuild/CMakeLists.txt` 中 qgis（:127）恰好是
  最后一个 ExternalProject，故未错绑；一旦在其后追加任何带 `URL_HASH` 的
  依赖（zstd/新库），删除 qgis 自己的 URL_HASH 行后正则会命中**后继工程的
  哈希**——该闸存在理由（「URL 与哈希成对钉死」，方向 71）即被静默废掉，
  selftest 的合成树不会发现（其夹具 qgis 也是最后一个）。
- **修复建议**: `block` 截到下一个 `ExternalProject_Add(` 或用括号配对；
  selftest 夹具补「qgis 后还有别的工程」反例。回归：合成树 qgis 块去掉
  URL_HASH + 追加后续工程 → 必须红。

## E-7 triage 的 Test.xml「并集」把重跑洗白的 flaky 测试重新算成红——与 ci_ctest_gate 的 flaky 裁决口径互相矛盾

- **Severity**: P3 ｜ **Confidence**: VERIFIED ｜ **Module**: tools/ci_triage_reds.py × tools/ci_ctest_gate.sh
- **File:line**: `tools/ci_triage_reds.py:268`（`failed = parse_last_failed(logs)
  | parse_test_xml(logs)`，`parse_test_xml`（:155-171）`os.walk` 整个 artifact
  根，收**每一个** `Testing/<tag>/Test.xml` 里 Status="failed" 的名字）；
  `tools/ci_ctest_gate.sh:28+92`（同一 build 目录先并行轮后 `--rerun-failed`
  串行轮，ctest 各自产生新 tag 目录，旧 tag 保留）；`ci.yml:235-241`
  （artifact 上传整个 `build/Testing/`，含全部历史 tag）。
- **根因**: 一个测试并行轮失败、串行重跑通过时：gate 按协议记 flaky 并放行
  （rc2==0 时整个 job 都绿）；但只要**同车道别的测试**让 job 变红，triage
  解析该车道 artifact 时会把旧 tag 里那条 Status="failed" 的 flaky 记录并入
  红集——未登记的 flaky 于是被打 `::error 未知红项`，triage exit 1。
  「重跑洗白」在 triage 侧被无条件推翻，且与 FirstRunFailed.log 的 flaky
  汇总（:270-271 自己也在统计）自相矛盾。
- **影响面**: triage 噪声/误红（fail-closed 方向，非静默放行）；对依赖
  triage 判断的人产生「新未知红」假信号。
- **排除比对**: 无已知对应（E.8 是登记漂移）。#501 相关但不同件。
- **修复建议**: parse_test_xml 只取**最新 tag**（按 tag 目录名/时间排序），
  或将 Test.xml 口径降为「LastTestsFailed 缺失时的兜底」而非并集；至少把
  flaky_names（FirstRunFailed − 最终红）从红集里扣除。回归：合成两个 tag
  的 artifact 树进 selftest。

## E-8 check_ui_invariants 两处健壮性弱点（长样式表截断漏检 + 基线粒度掩盖同规则多处违规）

- **Severity**: P3 ｜ **Confidence**: VERIFIED ｜ **Module**: tools/check_ui_invariants.py
- **File:line**: `tools/check_ui_invariants.py:107`（`window =
  text[m.end():m.end() + 400]`——setStyleSheet 实参超过 400 字符时，落在
  窗口外的裸 hex 静默漏检；括号配对 cut 也被同一窗口截断）；`:215-217`
  （baseline 键 = `<relpath>:<rule>`，无行号/次数粒度——同一文件同一规则的
  第 2..N 处违规被一条基线条目全部吸收，且「可收缩」判定只在全清时触发）。
- **影响面**: qss-hex 闸对长样式表（本仓 ribbon/qss 面恰好偏长）存在结构
  性漏检面；基线粒度使 qss-hex 的实际存量被低估且收缩进度不可见。与
  layering 的 path:rule 粒度同款（家法一致），但 layering 已归零、本闸
  qss-hex 仍在冻结期，粒度损失实际生效。
- **排除比对**: 无已知对应（#502/#538 是规模闸，不是本闸）。
- **修复建议**: 窗口改为「配对括号全量」；基线键升级为 `<relpath>:<rule>:<次数>`
  （--emit 时生成）。回归：>400 字符且 hex 在尾部的人工夹具。

---

## 已核对、判定为已知/不报的事项（抽样）

- qgs-audit 118>116、qgis-spread 151>149、file_budget 回升 3 + 新增 1
  （updateEngineRows 230）、ui_tokens 1 violation + 2 stale、docs_drift 27 TU、
  layering_transitive 7>6：全部复跑在，均为已知 147 项（E.2–E.9/H6），
  EXCLUDED_ACTIVE_WORK。
- `check_i18n.py` 的词面盲区（只扫 setter/ctor/action 三类入口）：已知
  E-14（上轮登记未修池），本轮不重复报；另查得 `resetText(` 类子串误匹配
  仅理论可达（仓内无此前缀方法），不报。
- `ci_triage_reds.py --validate/--selftest`、`check_layering --selftest`、
  `check_qgis_spread --selftest`、`check_file_budget --selftest`、
  `check_ui_size --selftest`、`check_docs_drift --selftest`、
  `check_l10n --selftest`、`check_flaky_ledger --selftest`：逐个阅读，
  夹具覆盖与其语义一致，未发现假绿路径（除 E-3/E-6/E-7/E-8 所列）。
- `ci_ctest_gate.sh` 的 perf 重跑排除（`-LE perf`）与车道选择集一致、
  ENVIRONMENT_MODIFICATION 的 path_list_prepend 用法、崩溃正则：读码
  无新问题。
- tst_seismic_welltie / tst_pyramid_consume 墙钟断言、tst_ai_horizonsuggest
  冗余槽、tst_domain_importrows 自反槽、tst_sequenceframework_ui 空操作、
  tst_identifybrowse 永真断言、tst_assetpaging SEGFAULT：全部为已知
  L5.6–L5.8/H5，不重复。
- tst_errorhub_storm 的 50ms 事件循环 lag 断言：绝对墙钟类，但已按家法
  RUN_SERIAL + TIMEOUT 120 布线（CMakeLists:1521-1522），属已缓解口径，
  不另立条。

## 覆盖清单

**逐行精读**: CMakeLists.txt（全文 1717 行）；tools/check_layering.py、
check_qgis_spread.py、check_file_budget.py、check_ui_size.py、check_ui_tokens.py、
check_ui_invariants.py、check_i18n.py、check_docs_drift.py、check_qgis_versions.py、
check_l10n.py、check_flaky_ledger.py、ci_triage_reds.py（全文）、ci_ctest_gate.sh；
tools/layering_vocab.json、qgs-audit-baseline.txt / qgis-spread-baseline.txt /
transitive-baseline.txt / file-budget-baseline.txt / ui-size-baseline.txt（头注
+ 条目计数）；cmake/extra-ux.cmake、extra-mamcl.cmake（全文），其余 48 个
extra-*.cmake 逐条 grep 过 add_paleo_test/add_test/target_sources 登记
（tst_foo/tst_sanitizer_budget_on 两个 comm 差异均核实为正常）；
src/app/main.cpp、appcontext.cpp（全文 889 行）；src/selfcheck/main.cpp；
.github/workflows/ci.yml（全文 558 行）；resources/*.qrc（344+8+4 条 file
引用逐一存在性校验，零缺失）。

**接口级/结构级审读**: src/app/appcontext.h、appcontext_layers.cpp、aiwiring、
onnxwiring（头文件+调用面）；tools/check_env_redset.py（头部 60 行 + 数据面
文档）；documented_redset.json / env_redset.json（结构）；tests/ 抽样 18 个：
精读 tst_test_sandbox、tst_startup_trace、tst_qgisruntime、
tst_horizonbatchexport_diag、tst_dataopssidecar、tst_metastore（选段）、
tst_errorhub_storm（选段）、tst_seismic_3dviz/tst_seismic_highdpi（skip 条件
面）；断言密度/结构面扫读 tst_commitcoord、tst_background_open、tst_shacache、
tst_entityview、tst_catalog_integrity、tst_versions、tst_layermanifest、
tst_onnx、tst_arearules。全 tests/ 目录做了三机械化扫（恒真断言/QSKIP
分布/零断言文件）与注册对账（403 文件 vs 注册名）。

**实跑（只读）**: check_layering（默认/--qgs-audit --strict）、check_qgis_spread、
check_file_budget、check_ui_tokens、check_docs_drift（确认已知红现状）；
三个针对性 python 复现（E-2 幽灵归一、E-3 空载/缺席车道、均上文引证）。

**未覆盖**: tools/check_tidy.py、check_ci_scripts.py、check_mamcl_lock.py、
check_lo_pin.py、ensure_qgis_resources.py、pin_osgeo4w.py、check_env_redset.py
主体逻辑（超出时间预算，只读了头部/接线）；paleo-dev / paleo-dev.ps1 壳脚本
（仅 grep 消费面）；tests/ 其余 ~380 个文件未逐一精读（抽样 + 机械扫口径）；
vendored 代码内部（按纪律只看集成边界）。
