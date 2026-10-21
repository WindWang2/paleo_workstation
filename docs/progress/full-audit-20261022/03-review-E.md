# Agent E 深审报告 — 构建系统、测试、架构规范、安全与独立审计（full-audit-20261022）

- 基线：`b301f2e8`（对照 `093346e6`，20 个 PR 增量）
- 模式：只读深审（未构建、未改源码；只跑只读检查器脚本）
- Agent：E（构建/测试/架构/安全/独立审计）

---

## 1) 覆盖清单

### 1.1 构建系统（#609 拆分 + #608/#620/#622 增量）

| 文件/对象 | 方式 | 风险级 | 结论 |
|---|---|---|---|
| `CMakeLists.txt`（1749→118 行）+ `cmake/paleo-*.cmake` 7 文件 | 逐行等价对拍：剥离注释/空行后新旧内容做有序 diff + 无序（sort）diff | 高 | 拆分逐字等价；唯一删项 `find_package(Qt6 REQUIRED COMPONENTS Core5Compat)` 为声明冗余（Core5Compat 已在 paleo-dependencies.cmake:59 的 REQUIRED 列表），commit 自述一致 |
| `include()` 顺序（build-options → dependencies → modules-data/ui → enable_testing → test-infra → tests → extra 清单循环 → test-labels） | 语义对拍（add_paleo_test 先定义后使用；labels 文件在 extras 之后 APPEND） | 高 | 顺序保持拆分前语义；`_paleo_extra_manifest` 防漏对账 GLOB+FATAL_ERROR 机制原样保留 |
| `cmake/paleo-test-infra.cmake`（XDG 沙箱/add_paleo_test/36 源码闸注册） | 全文 Read | 高 | 契约完好；发现 E-03（缩进误导，见 Findings） |
| `cmake/paleo-test-labels.cmake`（perf/golden/heavy/envdebt 标签） | 全文 Read + 与 extra-seismic 新 tst_horizontracksvc_perf 接线交叉验证 | 中 | tst_horizontracksvc_perf 进 `_paleo_perf_tests`（LABELS APPEND perf + RUN_SERIAL），与 extra-seismic 本地 LABELS services 不冲突（APPEND 语义） |
| `cmake/extra-{seismic,idw-hullclip,import-ux,well-logtext,wellcomposite}.cmake`（range 内改动/新增 5 个） | 全文 Read | 中 | tst_horizontracksvc_perf 手工注册带全四件套（sandbox/link/FIXTURE_GPKG/首跑输出），与 add_paleo_test 等价 |
| `#620` tst_workflows ORT 门控（`#if !PALEO_HAVE_ORT → QSKIP`） | Read + 宏传播链验证：paleo_workflow PUBLIC→paleo_ai PUBLIC `PALEO_HAVE_ORT=1`；未定义时 `#if !X` 语义为真 | 中 | 修复正确，无 ORT 构建可编译、有 ORT 构建真跑 |
| `#622` src/app/main.cpp QSG_RHI_BACKEND=opengl 全启动模式 | diff Read | 中 | 从便携包分支提升为无条件（env 已设则保留）；在 QgsApplication 构造前设置，正确 |
| src/app/officepreviewrenderer.cpp PostGate（#610） | diff + 并发语义分析 | 中 | mutex 门 + close-后拒绝 post + std::exit 跳过析构，无 UAF 窗口；PostGate 故意泄漏合理（reader 线程寿命） |
| `.github/workflows/ci.yml`（#608 disk guard + flaky ledger cache） | diff Read | 中 | linux/linux-perf 双 job 25GB 水位闸；ledger restore（restore-keys 前缀）/save（run 唯一 key + always + hashFiles 条件）接线正确 |
| `tools/ci_ctest_gate.sh`（strict 档不再 `|| true`） | diff Read | 中 | strict 非零退出置 rc2=1（真升红）；警告档行为不变 |
| translations（paleo_zh_CN.ts +299）+ `cmake/extra-ux.cmake` lrelease 单一嵌入点 | Read + `check_l10n.py` 实跑（PASS） | 低 | 唯一 :/i18n 嵌入点（qt6_add_resources），ALL 目标；ts 6340 message |
| 安装规则（extra-install.cmake / CalligraRenderer.cmake） | grep 定位 + Read | 低 | 未在 range 内变化，结构一致 |

### 1.2 测试质量（43 个改动测试文件）

| 文件 | 方式 | 结论 |
|---|---|---|
| tests/tst_import_ux.cpp（新 1178 行，43 用例/428 断言） | 深读含崩溃恢复三连（interruptedUnregisteredBytesAreRemovedOnReopen / interruptedRegisteredBytesSurviveReopen / recoveryRefusesTamperedBytesAndPreservesEvidence） | 真实行为测试：真实文件 move、字节级对拍、QSignalSpy；无 mock 绕过、无 QSKIP |
| tests/tst_algorithms.cpp（+131） | 深读 preparedHullMatchesUnpreparedContains / constraintIdwHullClipPreparedIdentical | >5000 探针含顶点/边中点退化 + 端到端 GeoTIFF 逐位 memcmp 对拍 + 双类覆盖断言，质量高 |
| tests/tst_idw_hullclip_perf.cpp（新） | 全文 Read | 掩码逐格一致硬断言 + 只做方向性墙钟断言（prepared≤unprepared），perf 车道 RUN_SERIAL，纪律正确 |
| tests/tst_workflows.cpp（#620） | Read writeOnnxRaster 用例 | 写真 GeoTIFF 回读像素断言（NaN→-9999 非 class0、argmax），真实路径 |
| tests/tst_import.cpp（-450/+重写） | 断言计数（-83/+131）+ 用例名对拍 | 井名解析语义随 #603 演进重写，断言净增；六个旧用例由更强等价用例替代，无弱化 |
| tests/tst_basemap_rendering.cpp（新，#625） | 全文 Read | 本地瓦片夹具 + QgsMapRendererSequentialJob 真渲染像素色断言（220,30,30）+ 可见性状态机 + 失败回退保持原选；不联网 |
| tests/tst_wellcomposite_visual.cpp + tests/golden/wellcomposite/golden_syn_full.png | Read 基线机制 | 8×6 网格均值色容差 22、≤4 外点；PALEO_UPDATE_GOLDEN 写构建目录不写源码树；GOLDEN-SUMMARY 供分诊器核数 |
| tests/tst_import_performance.cpp（新，371 行） | Read 计时断言面 | 墙钟只 qInfo 记录（record-only）；唯一时间断言 QTRY 60s（relaxedBudgetMs ×3 under sanitizer）为活性界；未打 perf 标签可接受 |
| tests/tst_welllog_defaultview.cpp（新 513 行/97 断言） | 用例名 + 抽读 | 滚轮/Ctrl 缩放、宽度编码、颜色口径、粒度量子真行为断言 |
| tests/tst_panels.cpp（+523） | diff 抽读 | folderConfirm 三用例对真实对话框 widget 状态断言 |
| tests/tst_crossplot_samples.cpp（+45） | diff 全读 | feet→m 精确对拍（304.8 @1e-8）+ 不安全轴拒绝 |
| tests/tst_seismic_index.cpp（米弧长断言迁移） | Read 相关段 | columnDistancesM 断言 9.0×25.0×√2 ±1m + 单调累计，真值校验 |
| tests/tst_horizontracksvc.cpp（#608 perf 车道拆分） | diff + CMake 接线 | propagationCostRatioGate 以 PALEO_HZ3D_PERF_LANE 编进 perf 二进制，功能门保真 |
| 其余（tst_catalog/tst_catalogstore/tst_seismic_3d*/tst_shell_chrome/tst_ui/tst_datagraphpage/tst_io_importrows_derivation/tst_datapreview/tst_mappingpages/tst_mappingworkbench/tst_wellcomposite_framework/tst_wellcomposite_depth/tst_wellfacies/tst_welllog_consumers/tst_welllogtext/tst_aichattoolloop/tst_folderimport/tst_previewmap_assets/tst_wellsection_workflow/tst_seismic_3dui/tst_seismic_3dviz/tst_horizontracksvc/tst_welllogset/tst_wellbasemap/tst_import_preview/tst_wellcomposite_visual） | diff 形状 + 断言计数 + 抽读 | 无 QSKIP 逃逸（除 #620 合法门控）、无永真断言、无断言删除性弱化；#611 tst_catalog 变更是行为对齐且新断言（无幻影行）更强 |

### 1.3 架构规范

| 项 | 方式 | 结论 |
|---|---|---|
| `check_layering.py`（默认/--strict/--selftest/--symbol-audit/--transitive/--qgs-audit --strict） | 实跑（全部 rc=0） | 零层违规；strict 零残留（layering-baseline 归零维持） |
| `check_qgis_spread.py`（默认/--strict） | 实跑 | 172 文件冻结（新增 0、可收缩 0） |
| `check_ui_invariants/ui_size/file_budget/ui_tokens/i18n/qgis_versions/docs_drift/flaky_ledger selftest/ci_triage_reds --validate/check_l10n` | 实跑（全部 rc=0） | 全绿；file_budget 57 项持平（回升 0）；ui_tokens 350/350 |
| 基线只缩不涨（range 内 diff） | 逐文件 diff | file-budget（删 3 行 + 下调 5 值）、transitive（ui 6→5）、ui-size（删 2 行）、ui-token（354→350 + 删 5 例外）——全部只缩；qgs-audit/qgis-spread/layering 基线文件 range 内零改动 |
| 层标注（新增 27 个 src 文件头三行） | 逐文件 head 检查 | 全部合规（数据/QGIS 封装/视图） |
| #539 裁决合规 | 基线头注核查 | E-2 扫描面扩张（前轮）在基线头注明归因；range 内无补登转绿行为 |
| 前轮修复回归 | 抽查 B-1（Ricker π² 注释在位）、B-2（columnDistancesM 迁移）、E-6（check_qgis_versions 块界截断修复在位）、A-01（ShaCache setDiskSaveThrottle 在位）、#610 四修（faciessliver qsizetype+int 界、normalizeDepthCurve、horizonGridFetch 路径快照、fault 层泄漏） | 均已落地且实现正确 |

### 1.4 安全/UB 扫描（重点新代码）

| 面 | 方式 | 结论 |
|---|---|---|
| vendor/sbm SgySectionBuilder.h（range 内唯一 vendor 改动） | diff + 全消费点追踪 + PATCHES.md/README 核查 | 改动纯增量、消费侧正确（见 E-01：PATCHES.md 漏记）；SgySectionStats 不跨 SdkC C ABI（静态库内部 C++ 结构，无 ABI 风险）；license 姿态不变 |
| src/services/seismictaskservice_slice.cpp columnDistancesM 填充 | 深读 200-330 行 | mapper.valid() + inlineNos/xlineNos 尺寸守卫 + double 累加 + 仿射不可得留空（never-synthesized 契约成立）；回落路径 clear |
| src/io/welllogtext{,_convert}.cpp（新） | 全文 Read | kMaxBytes 64MB 双重检查（size 预检 + read(kMax+1) 复检抗 TOCTOU）、kMaxCells 4M、kMaxColumns 256、引号状态机严格、UTF-8 验证、严格递增索引、qint64 乘积预算（无 int 溢出）、并集映射循环边界正确（子集必然命中等值） |
| src/io/importrecovery.cpp（新，崩溃恢复=删除路径） | 全文 Read + resolvedVersionPath 审计 | 删除受双约束：catalog 在册保护 + sha256 逐文件复核 + symlink 拒绝；路径经 resolvedVersionPath 硬化（绝对路径/反斜杠/段词法/symbolic link/canonical 包含五重拒绝）——无可逃逸 projectDir 的删除 |
| src/io/outsourceworkbook.cpp | 限额 grep + 抽读 | QXmlStreamReader（pull 模型天然无 XXE 外部实体）+ 512MB/4M 格/16384 列/1M 行四预算 |
| src/domain/wellrecords.cpp（XML 嗅探下沉） | 全文 Read | 100000 元素上限，安全 |
| src/io/ingestplan.cpp / dataimport_folder.cpp（文件夹导入） | 符号链接逃逸逻辑核查 | canonical 包含判定 + 读字节前 canonical 复检（目标改指向降级跳过），双时点防护 |
| src/domain/lithologystyle.cpp / welllogfacies.cpp（#610/#614 新） | 全文 Read | 纯函数；quantum>0 守卫、qBound 钳制、单位词表 NaN 哨兵返回 |
| src/algorithms/paleoalgorithms.cpp PreparedHull（#609） | 实现 + 生命周期审计 | QgsGeos 持有 hull.constGet() 裸指针，hull 与 PreparedHull 同作用域且 hull 之后无修改——生命周期正确；unique_ptr 不可拷贝 |
| src/qgis/onlinebasemap.cpp（#624 预设） | 凭据扫描 | 无嵌入 key/token（且本文件属 EXCLUDED_ACTIVE_WORK，只做安全边界扫） |

### 1.5 仓库卫生

| 项 | 方式 | 结论 |
|---|---|---|
| 根目录垃圾（Viijwr/PyplfI/nFiFrO_styles.db、' - zoom_level=2.aux.xml'、r0/r1-red-set.txt、goal-loop-prompts.zip、leak-test-root/） | `git log --follow` 逐文件溯源 + `git ls-tree -r b301f2e8` 复核 | 全部为 range **之前**引入（#598/#307 等），#608 在 range 内统一 git rm + .gitignore 规则（`/*_styles.db`、`/*.aux.xml`、`/r[0-9]*-red-set.txt`、`/leak-test-root/` 等）——基线零残留，任务书所述「range 内被提交」实为「range 内被清理」 |
| 最大 blob 扫描 | `git ls-tree -r -l` 排序 | 最大为 vendor/mamcl zip 13MB（vendored 依赖，既有）与字体/测试数据，无异常新增二进制 |

### 1.6 未核查区（如实申报）

- 未实际 configure/build（主 Agent 持锁）；#609 声称的 ctest -N/compile_commands 等价性未独立复现，以逐行文本对拍替代。
- tst_panels_dataops / tst_panels 全量 92 用例未逐条读（抽读新增段）。
- tst_wellcomposite_{framework,depth} 仅抽读（+144/+54 行）。
- Windows 侧（注册表 hive 沙箱、OSGeo4W）无法在本机验证，仅静态读接线。
- tst_onlinebasemap.cpp 全文深读 skipped——EXCLUDED_ACTIVE_WORK（PR #627）。
- perf 车道实际墙钟稳定性未复跑（RUN_SERIAL 纪律以接线审查为准）。

---

## 2) Findings

### E-01 · vendor/sbm 本地补丁漏记 PATCHES.md（上游同步回放契约破洞）

- **Severity**: P3　**Confidence**: VERIFIED
- **Module**: vendor 边界纪律
- **File:Line**: `vendor/sbm/src/Data/Sgy/SgySectionBuilder.h:30-33`（#611，07193ce9）
- **Root Cause**: `SgySectionStats` 新增字段 `columnDistancesM`（米弧长口径，前轮 B-2 修复的承接面），属对上游 pinned commit `aae56c77` 的有意分叉；`vendor/sbm/PATCHES.md` 头注承诺「全部有意分叉……同步上游时需逐一重放」并已记 P1–P13，本改动未登记（缺 P14 条目）。
- **Trigger**: 下次按 README「Upstream sync」流程（替换 src/ → 重放 PATCHES.md）同步上游时，该字段被静默冲掉，`seismictaskservice_slice.cpp:262-286` 的填充代码编译失败（或更糟：上游若自行加同名字段则语义漂移）。
- **Observed/Expected**: 实测 `grep columnDistancesM vendor/sbm/PATCHES.md` 零命中 vs 应有一条与 P1-P13 同格式的补丁说明。
- **Reproduction**: `git show 07193ce9 -- vendor/sbm/`（唯一 vendor 改动）+ `grep -c columnDistancesM vendor/sbm/PATCHES.md` → 0。
- **Impact**: 上游同步成本/正确性风险；不违反 license（README 明示上游无许可证、按内部资产处置，本改动纯增量不改变姿态）。
- **IssueMatch/PRMatch**: 新发现（前轮 register 无此项）。
- **FixStrategy**: PATCHES.md 增 P14：SgySectionStats 新增 columnDistancesM（米弧长、仿射可得才填、never-synthesized），消费侧 seismictaskservice_slice。
- **RegressionTest**: 无需（文档项）。

### E-02 · 闸门基线计数文档漂移（AGENTS.md/checker 注释滞后于 E-2 扩面后的冻结值）

- **Severity**: P3　**Confidence**: VERIFIED
- **Module**: 架构规范（基线纪律文档面）
- **File:Line**: `AGENTS.md:51`（"149+116 个存量文件"）、`AGENTS.md:57`（"qgis-spread 153→149"）；`cmake/paleo-test-infra.cmake:175`（"116 个直连文件全部在册"）
- **Root Cause**: full-audit-20261021 的 E-2 把 qgs-audit/qgis-spread 扫描面从 workflow+ui 扩到全部非视图模块并初冻入册（qgs-audit 116→137、spread 149→172，基线头注已写归因）；AGENTS.md「闸门基线纪律」节（最后更新 #564，2026-10-18）与 test-infra 注释未随之更新。
- **Trigger**: 审计/开发者以 AGENTS.md 数字核对「只缩不涨」时会看到 172>149、137>116 的表面增长；或把 149/116 误当当前红线。
- **Observed/Expected**: 基线实测 172（ui100+workflow49+其他23）/137 vs 文档 149/116。
- **Reproduction**: `python3 tools/check_qgis_spread.py`（172）对照 AGENTS.md:51。
- **Impact**: 纯文档面——权威闸口读基线文件本身（实跑全绿），但「初冻基线 + 收敛计划」以这些数字跟踪批次进度，漂移会污染下一批（批 2 workflow 49 文件）的删行对账基线。
- **IssueMatch/PRMatch**: 新发现（文档面；与 #591 EXCLUDED 的「实测不符」不同载体）。
- **FixStrategy**: AGENTS.md 该节补一行 2026-10-21 E-2 扩面归因（172/137 现值）；顺手更新 paleo-test-infra.cmake:175 注释。
- **RegressionTest**: 无需。

### E-03 · paleo-test-infra.cmake 大跨度 if 块内缩进错位（维护陷阱）

- **Severity**: P3　**Confidence**: VERIFIED
- **Module**: 构建系统
- **File:Line**: `cmake/paleo-test-infra.cmake:181-190`（qgis_spread 三个 add_test 与 set_tests_properties 以列 0 缩进呈现，实际仍在 137 行 `if(PALEO_PYTHON3)` 至 292 行 `endif()` 块内；块内其余 40+ 行均为两空格缩进）
- **Root Cause**: #532 接线时插入位置的排版遗留；CMake 忽略缩进故无功能影响，且 python3 缺失在 134 行 FATAL_ERROR 兜底。
- **Trigger**: 后续在同一区域增删闸门时按缩进误判块归属（例：以为 qgis_spread 注册在 if 外而把新 add_test 放到 endif 之后，或重构时把 endif 误挪到 181 行前）——一旦 134 行的 FATAL_ERROR 防线被放松即成真配置错误。
- **Observed/Expected**: 列 0 缩进 vs 块内两空格惯例。
- **Reproduction**: Read cmake/paleo-test-infra.cmake 137-192 行。
- **Impact**: 低（当前死安全）；纯可维护性。
- **IssueMatch/PRMatch**: 新发现。
- **FixStrategy**: 统一两空格缩进；或把 qgis_spread 族挪进独立的 include 片段。
- **RegressionTest**: 无需。

### 无更高等级发现的依据（正面结论备案）

- **P0/P1/P2 空**：构建拆分逐字等价（含 CMAKE_BUILD_RPATH 目录级语义保留 + 注释声明）；#610 四处 UB/寿命修复实现复核正确（faciessliver qsizetype+INT_MAX 界、horizonGridFetch 主线程快照+context 连接自动断开、PostGate 闭门、fault 层所有权）；导入链信任边界（预算、路径包含、XML pull 解析、双时点 canonical 复检）在本轮新代码中系统在位；43 个测试文件抽查无一弱化（净增 48 断言于 tst_import，全量无 QSKIP 逃逸）；全部 17 项源码闸门实跑绿；基线只缩不涨纪律 100% 执行。

---

## 3) 模块健康结论

**总体：健康（绿）**。本 range 20 个 PR 在我的四个职责面上均为净改善：

1. **构建系统**：#609 拆分经逐行对拍确认语义零漂移（唯一删项为声明的冗余清理），extra-*.cmake 防漏对账与 add_paleo_test 契约完好；#620 ORT 门控修复正确且宏传播链闭合；#608 的 CI disk guard/flaky ledger strict 升红接线正确。
2. **测试质量**：新增测试（import_ux 43 用例、basemap 真渲染像素断言、prepared hull 逐位对拍、golden 抽样容差、crossplot 单位归一）全部走真实生产路径，崩溃恢复/篡改拒绝等场景有字节级断言；无永真断言、无 mock 绕过、无弱化性改写；计时类正确分流 perf 车道或 record-only。
3. **架构规范**：17 项闸门实跑全绿；四个基线文件在本 range 内只缩不涨（file-budget 删 3 行下调 5 值、transitive ui 6→5、ui-size 删 2 行、ui-token 354→350）；新增 27 个 src 文件层标注 100% 合规；前轮 P2 修复（B-1/B-2/E-6/A-01）抽查全部落地。
4. **安全/UB**：新 IO 面（welllogtext/outsourceworkbook/importrecovery/ingestplan）预算与路径硬化系统在位；vendor/sbm 唯一改动功能正确但补丁漏记（E-01）。
5. **仓库卫生**：任务书关注的根目录垃圾文件实为 #608 在本 range 内清理（含 .gitignore 防回升规则），基线零残留。

登记 3 项 P3（E-01 补丁漏记 / E-02 基线计数文档漂移 / E-03 缩进错位），全部为文档与维护性债务，无一影响当前构建正确性、测试有效性或运行时安全；均给出一次性小修路径。
