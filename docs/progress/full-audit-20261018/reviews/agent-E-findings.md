# Agent E 只读审查报告（组装根 / 构建布线 / 测试有效性 / 工具链 / 资源 / CI / 分层横切）

- 基线：366aef97（worktree paleo_workstation-full-review-20261018）
- 审查员：Agent E（独立审计视角，只读；未运行 cmake/编译/ctest，全部结论基于静态阅读 + 只读 python3 脚本核对）
- 排除输入：01-conflict-matrix.md、99-external-issues-snapshot.txt（44 个开放 issue 全文通读）、/tmp/paleo-audit/issue-*.txt

## 覆盖清单

| 区域 | 口径 | 实际覆盖 |
|---|---|---|
| src/app/（12 文件，3676 行） | 全读 | main.cpp / appcontext.cpp / appcontext_layers.cpp / mkproject.cpp / officepreviewrenderer.cpp / crossplotcontroller.cpp / aiwiring.cpp / onnxwiring.cpp 全文 + 4 头文件标记核对 |
| CMakeLists.txt（1586 行） | 逐行 | 全文通读；add_paleo_test 注册与磁盘 392 个 tst_*.cpp 交叉对账（全注册、无孤儿） |
| cmake/（49 文件，1174 行） | 全读 | 全部 49 个 fragment 通读（CalligraRenderer + 48 extra-*） |
| tests/（401 项） | 抽查 ≥30 | 深读约 22 个（tst_perf 全文、tst_startup_trace、tst_ui、tst_wellcomposite_visual、tst_factorworkflow、tst_onnxworkflow、tst_import、tst_cache_async、tst_wellsiting_kernel、tst_panels_dataops、layout 族 3 个、tst_threeway、tst_metastore/tst_futuretask/tst_commitcoord/tst_test_sandbox/tst_errorhub 断言密度抽查等）+ 全仓脚本化扫描（永真断言、QSKIP 191 处、msleep 79 处、if 守卫断言、QVERIFY 赋值、禁用槽位——均逐一分类） |
| tools/（31+ 项）+ scripts/ | 全读（分档） | check_layering.py（主体+transitive/symbol-audit 段）、ci_ctest_gate.sh、ci_perf.sh、check_flaky_ledger.py、check_tidy.py、check_file_budget.py、check_ci_scripts.py、make_appimage.sh、measure_incremental.sh、relink-surface.py、audit_ui_visual_polish.py、batch_translate.py、check_lo_pin.py 全/近全读；new_module.sh、其余 checker 头部+职责核对；baseline 文件（file-budget/ui-size/qgs-audit 127 项/transitive/layering）计数核对；env_redset 16 条 vs 测试注册对账 |
| resources/ | 一致性核对 | geology.qrc 343 条目 vs 磁盘全存在；facies-hierarchy.json schema（7 条目 8 键）vs faciestaxonomy.cpp/path()/place() 消费一致；texture 引用全部在 qrc；paleo_fonts/seismic_shaders 的 Q_INIT_RESOURCE 落点核对；translations.qrc 死文件（FE-005） |
| .github/workflows/ | 全读 | ci.yml（463 行）逐段、deb-lock.yml 全文；引用脚本存在性独立复核（fetch-deps.sh / ci_perf.sh / mkproject mini 等） |
| 分层横切 | 批量实证 | 只读 python 脚本扫 src/ 全部 1122 个 .h/.cpp：头三行层标记 0 缺失（marker 全一致）；数据/功能层 QtWidgets include 0 命中；非视图层 ui/ include 0 命中；qgis→ui 0 命中；视图层 io/metadata 白名单外 include 0 命中——与 layering 闸门全绿互证 |
| 安全横切 | 定向扫描 | QProcess 参数（scriptrunner/pythonenv/pythonrepl/soffice：全部 argv 列表式无 shell 拼接）；SQL 拼接（全仓 .exec( 非字面量 0 命中，一律 prepared）；XML（Qt6 QDom/QXml 无外部实体解析面）；zip（zipExtractBytes 仅按名取字节无落盘解压；MAMCL 解包 SHA-256 前置 + python -m zipfile）；临时文件（QTemporaryDir 族为主，FE-004 一处例外） |

## Findings（新问题）

### FE-001 永真断言：lithoLexicon 序列化用例把同一表达式比给自身
- **Severity**: Medium
- **Confidence**: High
- **Module**: tests（测试有效性）
- **File:Line**: tests/tst_factorworkflow.cpp:429
- **Root Cause**: `lithoLexiconSerializationRuleManagementAndParsing` 首行断言写错——把同一调用比给自己：
  ```cpp
  QCOMPARE(LithoLexicon::groupDisplayName(LithoGroup::Sand), LithoLexicon::groupDisplayName(LithoGroup::Sand));
  ```
  注释声明意图是「枚举解析/显示名（往返一致）」，紧随其后的循环对 6 个中文名正确做了 `parseGroup(name)` 往返，唯独 Sand 这行两边是同一个表达式，任何实现（含打印错误名/返回空串的实现）都恒过。
- **Trigger**: 静态存在；`LithoLexicon::groupDisplayName` 对 Sand（及一切枚举）的回归从此无断言保护。
- **Observed/Expected**: 观察到恒真断言；预期应为一侧写期望字面量（如 `QStringLiteral("砂族")`）或做 `parseGroup(groupDisplayName(Sand)) == Sand` 往返。
- **Impact**: 词表引擎显示名回归逃逸。与 #476 P3「litholexicon.cpp 无专属 tst」相邻但不同根因（那是缺测试文件，这是现存覆盖中的坏断言）——修复时应一并补专属测试。
- **Existing Issue Match**: 无（#476 记的是「无专属 tst」盲区，非本行）。
- **Active PR Match**: 无（tst_factorworkflow 不在 PR #518/#521/主仓未提交簇清单内）。
- **Fix Strategy**: 改为往返断言（`parseGroup(groupDisplayName(Sand)) == Sand`）+ 对每个枚举值补期望字面量；同文件自查其余序列化断言。
- **Regression Test 思路**: 变异验证——临时让 `groupDisplayName` 返回空串，该用例应红（当前必绿）。

### FE-002 tst_perf 两个槽位是不触生产代码的「策略自证」：公式与实现相悖且永真
- **Severity**: Medium
- **Confidence**: High
- **Module**: tests（测试有效性）/ 构建布线（该测试挂 perf 标签 + RUN_SERIAL，占 CI 串行槽位）
- **File:Line**: tests/tst_perf.cpp:82-90（concurrencyCap）、:130-145（defaultCellSize）；生产对照 src/services/paleotaskservice.cpp:220
- **Root Cause**:
  - `concurrencyCap()` 在测试内本地计算 `qMax(2, cores/4)` 后断言其 ≥2——全仓 grep 证实生产代码没有该公式；PaleoTaskService 实际是 `m_pool->setMaxThreadCount(4)`（固定 4，与核数无关）。§41.6 声称的「NFR 策略」与实现相悖，而该槽位对两者都无检测力（`max(2, anything)` 数学上恒 ≥2；`cap <= cores` 被 `if (cores > 0)` 守卫）。
  - `defaultCellSize()` 同样在测试内重算 `ext.width()/2048.0` 再断言 ≈0.0049——「默认像元 = 范围/2048」这一策略在生产代码零命中（grep `2048` 仅命中 segy/渲染/kriging 的无关常量），唯一有效成分是 golden 夹具 extent==10.0 的数据校验。
  - 文件头注释自认「Policy/budget checks … trivially met」「the manager itself is not part of this slice」，但它以 perf 标签 + RUN_SERIAL 进入 CI linux-perf 车道，消耗串行预算换零缺陷检测力。
- **Trigger**: 静态存在；生产 `setMaxThreadCount(4)` 改成任何值、或派生像元策略漂移，两槽位照常绿。
- **Observed/Expected**: 观察到两个不可失败的槽位冒充 §41.6 验收；预期要么断言真实生产值（如 `workerPool()->maxThreadCount() == 4`），要么从 tst_perf 移除并在策略落地时以真实现为被测面。
- **Impact**: 误导性覆盖声明（§41.6 四条 NFR 中两条无任何绑定）+ CI perf 槽位空转。
- **Existing Issue Match**: 无（#503 记的是体量/墙钟，#484 记的是布线清单，均未点名此文件）。
- **Active PR Match**: 无（tst_perf 不在活动簇内）。
- **Fix Strategy**: concurrencyCap 改测 `PaleoTaskService::workerPool()->maxThreadCount()`（或删除并在 TODOS 登记 §41.6 公式与实现分叉的裁决）；defaultCellSize 把 2048 策略落到生产常量后再测真值。
- **Regression Test 思路**: 变异 `setMaxThreadCount(4)`→2，改造后用例应红。

### FE-003 make_appimage.sh 下载未钉版的 type2-runtime「continuous」产物且无校验
- **Severity**: Low（手动打包路径，非 CI 必经；但产物是分发件）
- **Confidence**: High
- **Module**: tools（打包/供应链）
- **File:Line**: tools/make_appimage.sh:44-48
- **Root Cause**:
  ```bash
  curl -fL --retry 3 -o "$RUNTIME" \
    https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
  ```
  可变 tag（continuous）+ 无 sha256 校验 + 无版本记录。与本仓既定纪律相悖：ci.yml 全部 actions 按 commit-SHA 钉死（防 tag 投毒，ci.yml:31-33 注释自述）、vendored 依赖一律哈希钉死（manifest.json / MAMCL #142 / deb-closure.lock / lo_pin 门禁）。
- **Trigger**: 打包机执行 make_appimage.sh 即从远端拉取当下任意内容并直接嵌进 AppImage 的启动器。
- **Observed/Expected**: 观察到未钉版下载；预期钉 release tag + sha256（对齐 check_lo_pin 口径），或改为从本地 vendor 缓存取。
- **Impact**: 打包链路供应链缺口——AppImage 运行时是第一段被执行的代码；上游 continuous 被替换即产出被污染的分发件，且事后无从审计（无版本留痕）。
- **Existing Issue Match**: 无（44 个 issue 无打包/AppImage 条目）。
- **Active PR Match**: 无。
- **Fix Strategy**: 钉具体 release 资产 + `echo "<sha256> ..." | sha256sum -c`；runtime 版本写进打包日志。
- **Regression Test 思路**: 脚本自检段（或 check_ci_scripts 类静态检查）断言 URL 非 continuous 且后跟校验行。

### FE-004 LibreOffice 转换 profile 目录用可预测固定名落在共享 /tmp 且从不清理
- **Severity**: Low
- **Confidence**: High（事实）/ Medium（可利用性——需要本机多用户/恶意本地用户场景）
- **Module**: src/io（安全与健壮横切）
- **File:Line**: src/io/dataimport_document.cpp:300-301
- **Root Cause**:
  ```cpp
  const QString profileDir = QDir::temp().filePath(
      QStringLiteral("paleo-lo-profile-%1").arg(QCoreApplication::applicationPid()));
  ```
  ① 固定可预测名（`/tmp/paleo-lo-profile-<pid>`）——多用户机上可被预先布设（symlink/占位目录），LibreOffice 随后往该路径写 profile 数据；② 全仓仅此一处引用，无任何 `removeRecursively`/清理——每个做过文档转换的应用进程都在 /tmp 永久遗留一个 profile 目录。同仓对照：previewrendercache.cpp:34-35 已示范 `QTemporaryDir(... paleo-preview-cache-XXXXXX)` 正确口径。
- **Trigger**: 任一 doc/docx/ppt/pptx 导入预览触发 soffice 转换即创建；跨会话不清理持续累积。
- **Observed/Expected**: 观察到 pid 固定名 + 零清理；预期 QTemporaryDir（或 QStandardPaths::AppDataLocation 下按会话目录）+ 转换结束/退出清理。
- **Impact**: 多用户机器上的写原语弱化面 + /tmp 垃圾累积；单用户桌面场景影响主要为磁盘残留。
- **Existing Issue Match**: 无（#477 是 io 域清单但无此条）。
- **Active PR Match**: 无（dataimport_document.cpp 不在活动簇）。
- **Fix Strategy**: 换 `QTemporaryDir(QDir::temp().absoluteFilePath("paleo-lo-profile-XXXXXX"))`（保活到服务析构）或 AppDataLocation 隔离目录；退出时 removeRecursively。
- **Regression Test 思路**: tst_io_workbook_edges/tst_import 族补断言：转换后 QDir::temp() 不残留 paleo-lo-profile-* 条目。

### FE-005 resources/translations.qrc 是引用不存在文件的死资源清单（嵌 qm 双轨陷阱）
- **Severity**: Low
- **Confidence**: High
- **Module**: resources / 构建配置
- **File:Line**: resources/translations.qrc:4；对照 cmake/extra-ux.cmake:450-453、src/app/main.cpp:168
- **Root Cause**: qrc 唯一条目指向 `../translations/paleo_zh_CN.qm`——源码树中不存在（.qm 是 lrelease 生成进构建目录，由 extra-ux.cmake 的 `qt6_add_resources(paleo_ui "translations" ...)` 嵌入，main.cpp 的 `Q_INIT_RESOURCE(translations)` 初始化的正是后者）。translations.qrc 在 CMakeLists/cmake/src/tools 全部引用为零：纯死文件。
- **Trigger**: 任何人在 paleo_ui（或新 target）源列表里加上该 qrc，configure/rcc 立刻因文件缺失而红；更隐蔽的风险是有人把生成的 .qm 复制到源树 translations/ 后两套 `:/i18n/paleo_zh_CN.qm` 嵌入路径并存漂移。
- **Observed/Expected**: 观察到死 qrc；预期删除该文件（单一嵌入路径=qt6_add_resources），或改为文档注释指向 extra-ux.cmake。
- **Impact**: 构建卫生 + 误导（读者会以为翻译经此 qrc 进二进制）。
- **Existing Issue Match**: 无。
- **Active PR Match**: 无（translations.qrc 不在活动簇；#518 触碰的是 ci/CMakeLists/tools 主题）。
- **Fix Strategy**: `git rm resources/translations.qrc`；在 extra-ux.cmake 的 qt6_add_resources 处加一句「唯一 .qm 嵌入点」注释。
- **Regression Test 思路**: 无需测试；可选在 check_ci_scripts 类静态检查中加「qrc 条目文件必须存在于源码树或显式生成清单」规则。

## 排除项汇总（命中外部工作/既有 issue，只记录）

按 01-conflict-matrix.md 判定规则逐条核对后排除：

1. **EXCLUDED_EXISTING_ISSUE（#484 P3）**：`tools/check_layering.py` QTWIDGETS_CLASSES 词表——复核确认仍缺 `QStyle/QCommonStyle/QProxyStyle`（有 QStyleFactory/QStyleOption，无 QStyle 本体）；仓库根仍追踪 goal-loop-prompts.zip、leak-test-root/、r0/r1-red-set.txt；8 个测试文件（tst_procdialog/tst_ui/tst_uxtheme/tst_shell_chrome/tst_seismic_sectionui/tst_seismicattrui/tst_constraintediting/tst_shortcuts_shell）仍带 QSettings::setPath /tmp 重定向（实测 grep 全数在位）；`CalligraRenderer.cmake`+`src/app/officepreviewrenderer.cpp` 零 CI 车道。均与 #484 原文同根因。
2. **EXCLUDED_EXISTING_ISSUE / EXCLUDED_ACTIVE_WORK（#472/#474/#502/#518）**：check_layering.py 已新增 `--qgs-audit`（qgs-audit-baseline.txt 127 条）、`--transitive` 防增长闸（transitive-baseline.txt）、`--symbol-audit`；ci.yml lint 已接 `--symbol-audit`+`--selftest`。但 lint 仍未跑 `--transitive`/`--qgs-audit`，ctest 也未注册这两档——「有工具未接线」仍是 #474 原文根因，且该文件/ci.yml 正是 PR #518 活动面。
3. **EXCLUDED_EXISTING_ISSUE（#502/#518）**：`tools/check_file_budget.py`（视图层规模闸，含 baseline+selftest）在 CMakeLists、ci.yml、check_ci_scripts.py 中零引用——与上一条同族（#502 的闸 + #518 活动面）。
4. **EXCLUDED_EXISTING_ISSUE（#501/#518）**：flaky 台账 `check_flaky_ledger.py` 仅在 ci_ctest_gate.sh 重跑放行分支内 `|| true` 调用，阈值≥3 只打 warning（`PALEO_FLAKY_STRICT` 默认关）；env_redset STALE 默认 warn 档——#501 原文要求且 #518 正在接线。
5. **EXCLUDED_EXISTING_ISSUE（#475/#501/#503 家族）**：golden 17/48 超容差通道、perf 标签 `-LE perf` 非阻断、windows 串行 ctest（ci.yml:427-433 注释仍援引注册表理由，方向 81 hive 已落地——正是 #503 二次复核记录的疑点）。
6. **EXCLUDED_EXISTING_ISSUE（#498）**：linux-asan 磁盘——ci.yml 已加 40GB 水位闸 + -gz + 扩容清理（PR #518 修复中），非新问题。
7. **EXCLUDED_EXISTING_ISSUE（#483 P3 已修复项复核）**：main.cpp --prefix CLI（现 :148-153 写回 env）、mkproject 绕过 QgisRuntime（现 :701-713 resolvePrefixPath+exitQgis 护栏）、projectOpened 巨 lambda（已抽 bindProjectOpened 具名方法）——三处均已按 #483 修复，无需再报。
8. **EXCLUDED_EXISTING_ISSUE（CMakeLists F10 注记）**：tst_wellsiting_kernel largeFieldThroughputRatio 的 `max(elapsed,1)` 无测量下限（:437-447）——CMakeLists.txt:1533-1536 注释明记为 full-audit F10 已挂 perf+RUN_SERIAL 处置；测量地板块本身的根因修复属既知递延。
9. **EXCLUDED_ACTIVE_WORK（PR #521）**：tools/*-baseline.txt、tst_edittools/tst_faciespartition/tst_mappingworkbench/tst_ui 相关面未审。
10. **EXCLUDED_ACTIVE_WORK（主仓未提交导出簇）**：tst_dataexport/tst_exportpipeline 及 CMakeLists 导出相关挂点未审。
11. **观测不计分**：tst_layoutshell.cpp:90/100、tst_layoutdesigner_full.cpp:125、tst_layoutexport.cpp:64 的 `QCOMPARE(x,x)` 是注释声明过的幂等性检查（伴随非空断言与互异断言），非缺陷；tst_cache_async 的 11 处 msleep 均为 worker 内蓄意占位 + spinUntil 轮询收口，非盲睡同步。

## 未覆盖区域

- vendor/ 全树（superbuild/deb 闭包/manifest 哈希核对未做，仅经 check_qgis_versions/check_lo_pin 的存在性间接覆盖）。
- 根脚本 paleo-dev / paleo-dev.ps1 主体（仅经 ci.yml/check_ci_scripts 的引用面交叉验证；paleo_dev_ps1_selftest.ps1 未读）。
- tools/ 中 ~12 个 checker（check_i18n/check_l10n/check_ui_size/check_ui_invariants/check_ui_tokens/check_qgis_versions/check_docs_drift/check_env_redset 正文/ensure_qgis_resources/pin_osgeo4w/make_segy_fixture/verify_l10n_offscreen）只做头部+职责核对，未逐行审。
- tests/ 约 350 个文件未逐一深读（伞式大测试按代表性抽样；tst_panels/tst_rehydrate/tst_shell_* 只做结构与关键断言抽查）。
- translations/paleo_zh_CN.ts 内容质量、golden PNG 实际像素、build/ 目录产物均未验证（只读约束下不运行构建）。
- .github/workflows 的 GitHub Actions 运行时行为（secret/权限运行面）仅静态审。
