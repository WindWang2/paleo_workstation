# goal/perf-maintenance-20261027 — 构建与维护性能：首配早失败 + 重链接收窄 + perf 证据链

> 基线 origin/master `d7abab8d`（2026-10-27）· Linux x86_64（CachyOS）·
> GCC 16.2.1 · Qt 6.12.0（系统）· QGIS 4.2.3 vendored superbuild prefix ·
> cmake 4.4.4 / Ninja 1.13.2 · ccache 4.14.1（用户共享 25GB，本任务未清缓存；
> worktree 源码路径与主仓不同 → ccache 大面积 miss，全量构建按「冷」口径记录）。
> 分支 codex/perf-maintenance-20261027。测量均为本机 RelWithDebInfo、-j8；
> 绝对时长跨机不可比，比率与计数是主口径。

## 一、任务 A：新 worktree 首次 configure（TODOS P2 收口）

### 1.1 复现结论（当前 master，Linux）

| 路线 | 全新构建目录 | 首配结果 | 四条 vendor include（glm/saribbon/sbm/segyio） |
|---|---|---|---|
| `-DQGIS_PREFIX`（superbuild prefix） | build | 成功 | 全在（1627/1635 条编译命令等） |
| env `QGIS_PREFIX_PATH` | build-env | 成功 | 全在 |
| 无 prefix | build-noprefix | 成功（静默走系统兜底） | 全在 |
| 显式悬空 `-DQGIS_PREFIX=/nonexistent` | build-dangling | **成功但错误** | —— |

TODOS P2 原症状「首配漏 4 条 vendor include、再跑同一条恢复」在当前
master 不复现：三条路线全新目录首配的 compile_commands 均完整，CI 每次全
新 checkout 亦绿（#780–#793）。该症状的记载出自 2026-10-03 Windows 本机
记录（与 QScintilla/Qt 混链时代同段落），本任务无法在本机复验 Windows，
不宣称 Windows 已验收。

实测确认的**同族真实缺陷**：显式 `QGIS_PREFIX`（cache 或 env）悬空时，
find_* 静默回落系统栈——`QGIS_CORE_LIB=/usr/lib/libqgis_core.so` 无任何
警告，且 `PALEO_QGIS_PREFIX_DEFAULT` 编译成悬空路径（链接系统库、运行时
找 vendor 前缀的分裂二进制）。这正是 TODOS 候选修法「configure 期早失败
并报明确原因」针对的缺口。

### 1.2 修复（cmake/paleo-dependencies.cmake，三处）

1. 显式 QGIS_PREFIX 不存在 → FATAL_ERROR，报文含缺失路径 +
   BUILDING.md「独立 worktree 开发」/ bootstrap 修复入口（早失败，
   替代静默回落）。
2. prefix 存在但 QGIS 头命中在 prefix 之外 → FATAL_ERROR（布局不完整
   同族混栈）。
3. 无显式 prefix → `message(STATUS)` 明示命中的 QGIS 栈路线（系统兜底
   档不再无声）。

### 1.3 护栏（cmake/verify-first-configure.cmake → ctest `first_configure`，UNIX）

- G1 依赖存在：合成 stub prefix（头 + stub 库）全新目录首配成功 +
  `QGIS_INCLUDE_DIR` 命中 stub（不静默换栈）+ compile_commands 四条
  vendor include 计数过阈（原症状直接断言）。
- G2 依赖缺失（显式 -D 悬空）：非零退出 + 报文点名路径 + 含
  BUILDING.md/bootstrap 修复入口。
- G3 幂等复用：同一目录重跑同命令 → 同样失败，且无 build.ninja 残留。
- G4 依赖缺失（env 路线悬空）：同 G2（覆盖 env→cache 写入路径）。

实测：修复在位全绿（本机 13.2s）；`git stash` 还原修复后 9 断言红
（反向验证，检查器不假绿）。实现踩坑记录：CMake `if()` 不对 `${var}`
展开值重新分词——条件串经宏参数传递会整串成为单个非布尔参数（NOT 恒
真），断言必须内联 `if()`；`-DVAR="带引号值"` 会被 PATH 型 cache 条目按
CWD 相对解析。两者均已写入脚本头注。

Windows：护栏仅注册 UNIX（stub 库文件名/分隔符按 POSIX 写死）；Windows
首配行为不在本任务验收面，PR 明示。

## 二、任务 B：增量编译与重链接收窄

### 2.1 测量基线（改前，build.ninja 传递闭包，441 exe = 437 tst + 4 工具）

各静态库被链接的 exe 数（tst/全部）见 `.task-notes/fanout-BEFORE.json`：
domain/sbm/segyio 435/439（底层，devex.md 已知「收益有限」），store 348/351，
algorithms 338/341，io 319/323，services 270/274，qgis 232/236，ai 211/213，
linkage 200/202，workflow 198/201，attribute_table 135/137，ui/saribbon
134/136，app 24/26。

文件编辑频率（git log 2026-08 起）：paleomainwindow.cpp 117 次（ui 族）、
datapreviewtabs 73、appcontext.cpp 65（app，扇出仅 26）、
mappingworkbenchpage 51、mappingworkbench.cpp 46（workflow，扇出 201）、
dataimportservice 43（io，323）、datacatalog 42（store，351）、datalist 41（ui）。

26 个全闭包 exe 中：5 个 devex.md 记录的刻意伞式兼容样本 +
tst_panels_dataops/tst_mappingworkbench（注释明示伞式）+ paleo/demo +
tst_aiwiring/crossplot_controller（LIBS paleo_app，组装根闭包合法）+
tst_startupdatabases（直接用 AppContext，paleo_app 即最小）——以上不动；
**7 个无标记单参注册**按「直接 include 的顶层模块」收窄（devex T1 口径）：

| 测试 | 原 | 现 LIBS | 链接闭包（paleo 档案数） |
|---|---|---|---|
| tst_seismic_perf | 伞式 | paleo_sbm | 13+ → 3 |
| tst_seismic_engine | 伞式 | paleo_services | 13+ → 7 |
| tst_seismic_transcode | 伞式 | paleo_services | 13+ → 7 |
| tst_ingestplan | 伞式 | paleo_qgis | 13+ → 8 |
| tst_keychainslot | 伞式 | paleo_ai | 13+ → 6 |
| tst_curvescale | 伞式 | paleo_ui | 13+ → 13−app |
| tst_sections_alignment | 伞式 | paleo_ui | 13+ → 13−app |

（数字以改后 build.ninja 快照为准——见 2.2 对账表。）

### 2.2 改后对账（fanout BEFORE → AFTER，两代 build.ninja 快照精确计数）

| 库 | tst 扇出（改前→改后） | Δ | 触发场景（编辑频率前三的该库文件） |
|---|---|---|---|
| paleo_app | 24 → 17 | **−7** | appcontext.cpp 65 次 |
| paleo_ui | 134 → 129 | −5 | paleomainwindow.cpp 117 / datapreviewtabs 73 / datalist 41 |
| paleo_saribbon | 134 → 129 | −5 | （随 ui） |
| paleo_qgis_attribute_table | 135 → 130 | −5 | （随 ui） |
| paleo_linkage | 200 → 195 | −5 | |
| paleo_workflow | 199 → 194 | −5 | mappingworkbench.cpp 46 / workflows 38 |
| paleo_ai | 211 → 207 | −4 | |
| paleo_qgis | 233 → 229 | −4 | |
| paleo_io | 319 → 317 | −2 | dataimportservice 43 |
| paleo_services | 271 → 269 | −2 | |
| paleo_algorithms | 338 → 336 | −2 | |
| paleo_store | 347 → 346 | −1 | datacatalog 42 |
| paleo_domain | 435 → 434 | −1 | wellsection 等（底层，架构性） |
| paleo_sbm / paleo_segyio | 435 → 435 | 0 | （seismic_perf 保留） |

计数口径：build.ninja 传递闭包（改前快照 /tmp/build.ninja.BEFORE 留档
.task-notes/），441 exe 链接边两侧一致；与 scripts/relink-surface.py 的
真实构建计数口径等价（计数是确定性的，两法一致）。

<!-- TIMING_MATRIX -->

### 2.3 增量构建计时矩阵（AFTER 图，同机 -j8，全量构建后 settled 状态）

| 场景（touch 目标） | 墙钟 | 重编 TU | exe 重链接 | 备注 |
|---|---|---|---|---|
| no-op | 1.1s | 0 | 0 | 5 步 = CONFIGURE_DEPENDS glob 复查 |
| src/app/appcontext.cpp | 74.7s | 1 | 19 | appcontext.cpp 本身是重 TU |
| src/app/appcontext_layers.cpp | 156.5s | 1 | 19 | |
| src/app/appcontext.h | 81.1s | 16 | 19 | 头包含面 16 TU，收敛良好 |
| src/workflow/mappingworkbench.cpp | **487.8s** | 1 | **196** | 改前 201（−5，实测 196 与图计数一致） |
| src/ui/pages/datalist.cpp | **462.3s** | 1 | **131** | 改前 136（−5，实测 131 与图计数一致） |
| src/catalog/datacatalog.cpp | **561.6s** | 1 | **350** | store 层，改前 351 |

单次顺序测量（BUILD_OPT.md 交错三轮纪律用于优化档对比；本矩阵是结构
计数验证场景，重链接数为确定性值与图计数逐一吻合，墙钟仅记录）。
ccache 状态：全量构建为冷（worktree 源码路径与主仓不同，preprocessed
内容不一致 → miss）；矩阵各场景均无缓存复用干扰。

### 2.4 结论

**实际提速**：app 族触发的重链接 26→19 exe（appcontext.cpp 场景按每链接
~2.4-3.5s 折算省 ~15-25s）；ui/workflow/linkage 族每触发省 5 个 exe 的
重链接（~12-18s）；tst_seismic_perf 自身重链接从 13+ 档案降到 2。
**仅减少依赖**（无墙钟提速声明）：qgis/ai 族 −4、io/services/algorithms
族 −2、store/domain 族 −1 的扇出计数；7 个测试的链接命令行显著变短。
**未测量**：跨机可比性（绝对时长只对本机有效）。

**实测确认的结构性瓶颈（遗留，超出本任务最小范围）**：单 exe 链接
~2.4-3.5s（RelWithDebInfo、静态档案 + QGIS .so 链），底层库一次触发的
重链接扇出 350-435——mappingworkbench.cpp 一改 = 8.1 分钟、
datacatalog.cpp 一改 = 9.4 分钟，其中 >95% 是重链接而非重编译。这是
静态库 DAG + 调试信息链接的架构成本（devex.md 已裁决不做 PCH/unity/
LTO 默认档；换 linker/共享库化是政策级变更）。头文件重编风暴最大热点
datacatalog.h：41 次编辑（2026-08 起）× 379 TU 传递包含面（roleregistry
380 / layermanifest 334 同族）——按依赖边界拆门面是独立方向。

## 三、任务 C：perf 证据链维护

已确认缺陷与修复：

1. **tools/ci_perf.sh 假绿族**（linux-perf 是 continue-on-error 车道，证据
   链是唯一记录）：
   - `ctest -N -L perf` 失败或 0 命中 → 原 `::warning` + exit 0（注册/
     发现漂移静默丢全部趋势数据）→ 现 ::error + 非零；未配置/损坏的
     build 目录同样被打红（实测 BUILD_DIR 指向不存在目录 → RC=1）。
   - 构建步 `cmake --build` 原不限并行（Ninja 缺省吃满核，违反 -j8
     纪律）→ 现 `--parallel ${PALEO_JOBS:-8}`。
   - JUnit 原经 "$@" 二次传 `--output-junit`，与 paleo-dev 自带 flag 叠加
     （生效与否取决于参数序）→ 现统一取 paleo-dev 落盘的
     `Testing/ctest-junit.xml` 拷贝到 artifact 位；跑绿但 junit/summary
     缺失或为空 → 判红（防「绿了但没产物」）。
2. **tools/ci_ctest_gate.sh**：选择器 0 命中时 ctest 退出 0 直接放行 →
   现 "No tests were found" → ::error 红（区分「没跑」与「通过」）。
3. **ci.yml 并行度**：linux-asan Build 步 `cmake --build build`（全仓最重
   构建，未限并行）→ `--parallel 8`；lint autogen stub 步同。
4. **scripts/relink-surface.py**：裸 `ninja -C` 不限并行 → `-j`（默认 8，
   PALEO_JOBS 可调；relink 计数与并行度无关）。

perf 套件现状核对：`ctest -N -L perf` 33 项（tst_cache_las/
tst_singlefactor_perf/tst_startup_trace 均在注册且现行启用）；tst_startup_trace
经 `add_dependencies(… paleo)` 保证单编 target 时二进制在位（#399 已修）。

**端到端实测（修复后 tools/ci_perf.sh，本机 superbuild 路，2026-10-28）**：
发现 33 项 → 构建（--parallel 8）→ 串行执行 427s → 退出码 8 如实透传。
产物三件齐全：perf-junit.xml（15.5KB，自 paleo-dev 落盘的
Testing/ctest-junit.xml 拷贝）/ perf-summary.txt / perf-ctest.log——红路径上
证据链同样完整（旧脚本在此场景无 JUnit 拷贝步骤、参数序依赖）。结果：
32/33 绿，含历史线索 tst_cache_las 0.59s、tst_singlefactor_perf 155.61s、
tst_perf_las 0.64s、tst_seismic_perf 0.84s；1 红 = tst_startup_trace
`qgis_init_share_max` 中位 0.2463 > 门 0.2200（基线×2.5）——机器/环境敏感
比率门，2026-10-27 22:59 全系统升级（loader/init 构成变化）所致环境漂移；
该测试属非阻断 perf 车道（continue-on-error by design）且在 envdebt 豁免
名单，阈值不动、不据此宣称回归。本分支未触碰该测试与 paleo 二进制任何
链接边（见 §四.5 归因方法）。

## 四、验证记录（全部 Linux 本机，RelWithDebInfo、-j8、`./paleo-dev test` 注入 superbuild 环境）

1. **全量构建**：1931 步 440 exe 链接 0 失败（ccache 冷口径；中断一次，
   见 §五环境事件，续跑完成）。
2. **任务 A 护栏**：`first_configure` 绿（13.2s→~130s 全量档，含 4 次
   合成 configure）；反向验证：stash 修复后 9 断言红。
3. **7 个收窄测试 + 8 个相关项回归**：全绿（经 paleo-dev test，229s）。
   注意：裸 `ctest`（不经 paleo-dev）在 superbuild 路线下因
   libqgis_core.so 的传递 NEEDED（libdraco.so.9 等）缺 LD_LIBRARY_PATH
   而 load 失败——既定入口差异（BUILDING.md「QGIS prefix 解析顺序」），
   deb 闭包路同样由 paleo-dev 注入。
4. **源码闸门批次**（layering 族/qgis_spread/ui_*/file_budget/i18n/
   qgis_versions/docs_drift/cpp_mainline/ci_known_reds/flaky_ledger/
   env_redset/lo_pin/role_tables）：除 layering_transitive 外全绿。
   - layering_transitive 红（ui_qtwidgets_notes 6 > 基线 5）**为基线既有**：
     同命令在新 origin/master（0b9c0a7a #815）git archive 树上复现同样
     红；本分支 src/ 与基线 d7abab8d 逐字节一致，未触碰任何 include 边。
     按「基线只缩不涨」纪律不调基线不修码，如实登记 master 侧既有债。
5. **快速道全量回归**（`-LE 'heavy|perf'`，445 项）：438 绿 / 7 红。
   7 红 = layering_transitive（上述既有）+ 6 个功能测试
   （tst_assetops / tst_versions / tst_wellfacies / tst_derivationgraph /
   tst_impactrecompute / tst_welllog_ui，均为 PDF 导出/文件写/幂等类断言）。
   **归因（气密验证）**：① 这 6 个测试的链接边在改前/改后 build.ninja
   中逐字节一致；② stash 本分支全部改动后（纯基线接线）同环境同命令
   串行复跑——同样 6 红且失败集完全一致；③ 2026-10-27 22:59 本机全系统
   大升级（libstdc++/gcc-libs/mesa/qt6-declarative/qt6-webengine/noto 字体
   栈/poppler/draco/gdal/qgis 等 150+ 包）后环境漂移。结论：非本分支引入。
6. **transitive 全指标**（权威口径，显式 QGIS_PREFIX）：见上——仅
   ui_qtwidgets_notes 超 1（基线既有），heavy_* 三指标与基线持平或更优
   （include 边零增）。

## 五、环境事件记录（非本 PR 代码问题）

2026-10-27 22:59 系统 pacman 全量升级：poppler 26.08→26.10（SONAME
163→165）、draco 1.5.7→1.5.9（SONAME 9→11）——共享 superbuild prefix 的
libqgis_core.so.4.2.2/libgdal.so.39（构建时链旧 SONAME）链接失败。恢复
（方案 1 of 3，additive、可逆、不清缓存不重建）：从 /var/cache/pacman/pkg
提取同版本旧库放入共享 prefix/lib（libpoppler.so.163.0.0 + libdraco.so.9.0.0），
链接与运行时（$ORIGIN RUNPATH + LD_LIBRARY_PATH）同时恢复；主仓共享受益。
备选方案（未启用）：2) 降级系统包回 26.08（影响整机）；3) superbuild 重编
QGIS/GDAL（≥2h，违反「不重建已可用依赖」）。根治属 superbuild 自包含性
缺口（prefix 未 vendor 传递依赖），登记遗留。

## 六、残留与递延（如实）

1. **datacatalog.h 头风暴**（41 次编辑 × 379 TU 传递重编面；roleregistry
   380 / catalogindex 379 / layermanifest 334 同族）——按依赖边界拆 catalog
   门面是独立方向，本任务只测量未动手。
2. **重链接架构成本**：单 exe 链接 ~2.4-3.5s × 底层库扇出 350-435，单改
   mappingworkbench.cpp = 8.1min（>95% 是链接）。缓解需 linker/共享库化/
   DAG 分层政策级变更，本任务不裁。
3. **layering_transitive ui_qtwidgets 6>5**：master 既有红（#815 合并后），
   修法是删 include 边，属 master 侧工作。
4. **6 个环境漂移红测试**：需在稳定环境（或 CI）复核归属；本分支不改其
   行为。CI linux 车道若复现即为独立环境债。
5. **Windows 首配**：first_configure 护栏未注册 Windows（stub 按 POSIX 写
   死）；paleo-dev.ps1 localdeps 路线显式 -DQGIS_PREFIX 不受本次早失败
   影响的静态审查已做，实跑验收待 Windows 车道。
6. **superbuild 传递依赖自包含缺口**（draco/poppler 事件根因）：建议
   superbuild 增加「链接后传递 NEEDED 全部在 prefix/LD_LIBRARY_PATH 内」
   的自检（另行立项）。
