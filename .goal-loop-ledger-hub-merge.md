# 方向 76 ledger：双 ErrorHub 合流

分支 `goal/hub-merge-20261009`（基线 origin/master `f31ee461`；任务书行号为
`e3c8d31d` 口径，`f31ee461` 相对 `e3c8d31d` 仅 `src/ui/paleomainwindow.cpp`
有 +156/-59 改动，errorhub.h / notifications / appcontext / errorhub 系测试
零改动，背景事实全部复核成立）。

## R0 消费面全量盘点（2026-10-08，rg 证据）

### 全局 `::ErrorHub`（errorhub.h:30，方向 64）
- 装配：`src/app/appcontext.cpp:182-183` 构造 + `installGlobal`（AppContext
  先于 PaleoMainWindow 构造——main.cpp:175 ctx → :264 window）。
- 上报真源：`src/ui/notifications/paleonotify.cpp` 经 `ErrorHub::global()`
  入账；全仓 `PaleoNotify::` 引用 **150 处**（任务书口径 144，+6 为
  `f31ee461` 后新增调用面；Oracle「零改动」以 diff 为准，计数以当日实数记档）。
- 呈现栈（64 系，已装配）：`notificationcenter` / `toastcard` /
  `errorhistorypanel`，由 `paleomainwindow_attach_shell.cpp:648-660`
  `attachErrorHub` 挂窗（main.cpp:320 调用）。

### `paleo::services::ErrorHub`（errorhub.h:171，方向 54/#251）
- 生产代码消费面（全部）：
  - `src/ui/notifications/notificationmanager.{h,cpp}`（54 系呈现）
  - `src/ui/notifications/notificationcard.{h,cpp}`（54 系呈现）
  - `src/ui/notifications/errorhistorymodel.{h,cpp}`（54 系呈现）
  - `src/ui/notifications/errorhistorydock.{h,cpp}`（54 系呈现）
  - `src/ui/paleomainwindow.cpp:1013-1028`（状态栏徽标订阅——接线错位）
- 装配构造点：`new NotificationManager` / `new ErrorHistoryDock` /
  `new ErrorHistoryModel` 在 src/ 全仓 0 处（唯一 `new ErrorHistoryModel`
  在 errorhistorydock.cpp:140 内部自连）。**呈现栈从未装配，死代码。**
- 测试消费面：tst_errorhub_d54 / tst_errorhub_storm /
  tst_errorhub_challenger_m4 / tst_errorhub_challenger_m5_harness /
  tst_error_history_dock / tst_notification_card /
  tst_notification_m2_challenger（7 个测试文件）。

### 接线错位（功能后果复核成立）
- `paleomainwindow.cpp:1013` 订阅 `paleo::services::ErrorHub::instance()`
  ——懒汉单例现造一个壳 hub，`count()` 恒 0 → `m_statusErrorBtn`（:967
  `setVisible(false)` 起）永久隐藏。
- 徽标点击打开的错误历史 dock 接的是全局 hub（attach_shell.cpp:652-658）
  ——可达但计数信号接错对象。

## 语义差异核对（54 hub vs 全局 hub，定案依据）

| 维度 | 54 hub | 全局 hub | 裁决 |
|------|--------|----------|------|
| 去重窗 | 从**末次发生**滑动（errorhub.cpp:262 `timestamp.secsTo`，聚合时 timestamp 被刷新） | 从**首次发生**固定 60s（errorhub.h:17-20，DESIGN.md:246 同文） | **保留全局语义**——DESIGN.md:246 明文「自首次出现起 60 s 固定窗口，不随重复滑动」；54 变体从未装配上线 |
| historyCleared | 有 | 已有（errorhub.h:88） | 无需并入 |
| historyChanged | 有（新增/聚合/逐出/清空全发） | 无；errorRaised(entry, firstInWindow) + historyCleared 两信号 | **不并入**——全局消费面（ErrorHistoryPanel :93-95、徽标）都是「信号到→全量重查」模式，errorRaised 每次上报（含聚合、含逐出引发的新写入）都发，historyCleared 覆盖清空；加第三信号是冗余能力，违反「不加新能力」红线 |
| 4 级 Critical | ErrorLevel::Critical | 3 级 + severe 标记 | 保留全局——paleonotify.cpp:74 已映射 critical→Error+severe，DESIGN.md:244 呈现矩阵即按 severe 口径 |
| 可变容量/可变去重窗 | setMaxCapacity/setDedupWindowSecs | 定值 kCapacity/kDedupWindowMs + setClockForTest | 不并入——生产零消费；测试侧全局 hub 用时钟注入达到同等可测性 |
| query 族/countByLevel/entryById | 有 | entries(Filter) 已覆盖级别/来源/文本过滤（ErrorHistoryPanel 在用） | 不并入——生产零消费 |
| ErrorDomain 词表 | 有 | source 自由字符串 | 不并入——生产零消费 |

**结论：54 hub 无任何存活消费面所需独有能力；合流 = 删除，零能力并入，
零行为变更（除徽标修复）。**

## 删除清单（每件零消费证据）

| # | 件 | 零消费证据（rg 全仓） |
|---|----|----------------------|
| D1 | `src/ui/notifications/notificationmanager.{h,cpp}` | `new NotificationManager` 在 src/ 0 处；仅 tst_* 4 文件构造 |
| D2 | `src/ui/notifications/notificationcard.{h,cpp}` | 仅 notificationmanager.{h,cpp} 与 tst_notification_card 引用 |
| D3 | `src/ui/notifications/errorhistorymodel.{h,cpp}` | 仅 errorhistorydock.cpp:140（同批删除）与 tst_error_history_dock 引用 |
| D4 | `src/ui/notifications/errorhistorydock.{h,cpp}` | `new ErrorHistoryDock` 在 src/ 0 处 |
| D5 | `paleo::services::ErrorHub` 全部声明+实现（errorhub.h:105-255 / errorhub.cpp:189-末尾） | 删 D1-D4 后 src/ 引用仅剩 paleomainwindow.cpp:1013-1028（批 A 改接全局） |

## 测试归属裁决

| 测试 | 归属 | 处置 |
|------|------|------|
| tst_errorhub | 全局 hub 单元 | 保留；并入 d54/storm 中有独值的 hub 级用例 |
| tst_errorhub_d54 | 54 hub 单元 | 删除；独值用例（8 线程高并发聚流、风暴下环形不超限）迁入 tst_errorhub；重复用例（去重窗/环形逐出/清空）已有等价覆盖（dedupWithinWindowAggregates / ringEvictsOldestAtCapPlusOne / clearResetsButIdsMonotonic） |
| tst_errorhub_storm | 54 呈现风暴 + hub 风暴 | 重定向保留：去掉 NotificationManager，纯 hub 风暴（500 连发 + 环形上限断言），**RUN_SERIAL + TIMEOUT 120 布线保留**（Oracle #5 明文） |
| tst_errorhub_challenger_m4 | 54 呈现 UI 交互 | 删除——被测呈现件删除；hub 侧语义（去重聚合）tst_errorhub 覆盖 |
| tst_errorhub_challenger_m5_harness | 54 呈现极端压力 | 删除——同上；风暴语义 tst_errorhub_storm（重定向后）+ tst_notifications::stormDoesNotFreezeMainThread（64 栈）覆盖 |
| tst_error_history_dock | 54 呈现历史面板 | 删除——64 栈 ErrorHistoryPanel 的等价面 tst_notifications::historyFilterCopyClear/historyRingEvictionVisible 覆盖 |
| tst_notification_card | 54 呈现通知卡 | 删除——64 栈 tst_notifications::cardAppearsAndExpires/stackCapEvictsOldest/sameKeyBumpsNoNewCard 覆盖 |
| tst_notification_m2_challenger | 54 呈现 M2 | 删除——同上 |

新增：tst_ui `TestUiShell` 加状态栏徽标用例（Oracle #2：注入错误 → count>0 →
徽标可见；清空 → 隐藏）。

## 批次

- 批A：徽标接线修正（paleomainwindow.cpp:1013-1028 → ErrorHub::global()，
  订阅 errorRaised+historyCleared 与 ErrorHistoryPanel 同源，计数 hub->size()）。
- 批B：删 paleo::services::ErrorHub（errorhub.h/cpp 收单类；头注释改写）。
- 批C：删 D1-D4 + CMake 691-694 源登记。
- 批D：测试迁移（上表）+ tst_ui 徽标用例。
- 批E：文档（errorhub.h 契约注释、54/64 账本尾注互指）。

## 轮次记录

| 轮 | 改动 | 验证 | 结果 | 下一步 |
|----|------|------|------|--------|
| 0 | 盘点+方案定案（本文件） | rg 证据全量落档 | — | 基线 build+ctest 拿 R0 红集合 |
| 1 | 无（基线构建尝试） | `paleo-dev.ps1 build` 链接失败：`paleo_workflow(aiassistworkflow.cpp.obj)` 引用 `suggestHorizonTracking`/`runTileInference`，实现仅在 CMake ORT 分支（CMakeLists.txt:347-349）；本 worktree 无 gitignored `vendor/onnxruntime` → 走 else 空壳分支（:360）→ LNK2019。**基线既有问题，非本任务引入**（两符号在 e3c8d31d..f31ee461 零改动） | 未通过 | 按 manifest 钉版下载 ORT 1.30.0（CI 同款）到 vendor/onnxruntime，重建基线 |
| 2 | 按 manifest 钉版拉取 vendored ORT 1.30.0（SHA256 校验通过）→ 重建 | 链接再次失败：`PaleoOnnxService` moc 符号缺失——两阶段切换（先无 ORT 后有）留下 AUTOMOC 陈旧态（mocs_compilation 未含 moc_onnxpredictionservice） | 未通过 | 删 `build/paleo_ai_autogen` 陈旧产物重生成 |
| 3 | 清 autogen 后 `paleo-dev.ps1 build` 续建 | **基线构建全绿**（304/304 链接完成，ORT DLL 1.30.0 staged）；起基线 ctest（串行） | 通过（构建） | 等 ctest 完成记 R0 红集合 |
| 4 | 批A-E 全部落盘（徽标接线/收单类/删 8+6 文件/CMake/测试迁移/文档）；静态自检：paleonotify.{h,cpp} 零改动、`paleo::services::ErrorHub` rg 零命中、diff --check 净、diff 外科式（+12/-523） | 待编译验证 | — | 基线 ctest 完成后重建 + ctest 对照 |

## R0 红集合（基线 ctest 结果）

369 项全量串行跑完（exit 1），红 26 项（`r0-red-set.txt`，多为本机性能预算
/环境类红：tst_singlefactor_perf 300s 超时、tst_ui mappingWorkbench 300s
超时崩、PROJ/GDAL 环境噪音等）：

boot, tst_aiassist_perf, tst_cache_las, tst_composeworkflow,
tst_crossplot_controller, tst_dockmanager, tst_fspath, tst_import,
tst_mapping, tst_metastore, tst_panels, tst_perf_regress,
tst_perf_segyindex, tst_pyramid_consume, tst_runtime,
tst_shell_projectlifecycle, tst_singlefactor_perf, tst_startup_trace,
tst_stratigraphicweb, tst_ui, tst_uxtheme, tst_wellcomposite_perf,
tst_wellfacies, tst_wellfaciesconfig, tst_wellsection_ui, tst_workflows

**errorhub 家族基线全绿**：tst_notifications 1.43s / tst_errorhub 0.31s /
tst_errorhub_d54 0.49s / tst_notification_card 1.23s /
tst_notification_m2_challenger 2.75s / tst_error_history_dock 0.96s /
tst_errorhub_challenger_m4 0.68s / tst_errorhub_storm 15.81s /
tst_errorhub_challenger_m5_harness 1.71s——合流后存活三件
（tst_errorhub / tst_errorhub_storm / tst_notifications）必须保持绿。

| 轮 | 改动 | 验证 | 结果 | 下一步 |
|----|------|------|------|--------|
| 5 | 记 R0 红集合（本节） | 基线 ctest 369 项完整跑完 | 通过（R0 已固化） | 重建改动版 + 全量 ctest 对照 |
| 6 | **事实修正（重要）**：编译暴露 `paleomainwindow.cpp:796`（e3c8d31d 起 :784 即在）`new paleo::ui::ErrorHistoryDock(this)` —— 任务书「`new ErrorHistoryDock` 0 处」与 R0 盘点同因**限定名漏检**（`new ErrorHistoryDock` 匹配不到 `new paleo::ui::ErrorHistoryDock`）。实况：54 系 ErrorHistoryDock 在主窗口构造（隐藏 + toggleViewAction `actionViewErrorHistory` 从未挂任何菜单 + accessor `errorHistoryDock()` 零外部消费者 + 背后僵尸 hub 恒空），与 64 系 `m_errorHistoryPanelDock` **同 objectName "errorHistoryDock" 并存** | rg 证据：`actionViewErrorHistory` 仅 :799 自身；`errorHistoryDock()` 在 src/tests 零调用；面板菜单「错误历史」action 走 64 系 showErrorHistory（paleomainwindow.cpp:1272-1274） | 事实修正落档 | 处置（Option 3 定案）：删 54 dock 构造块 + 头文件前置声明/accessor/成员——该 UI 用户不可达且恒空，删除属修复级清理，与徽标接线同类记档；64 系面板 dock 成为唯一「错误历史」dock 且 objectName 恢复唯一 |
| 7 | 删 54 dock 块 + 补 errorhub.cpp `#include <QDateTime>`（.h 收单类后转递断裂） | 重建 383/383 全绿 | 通过 | 全量 ctest 对照 |
| 8 | 合流版全量 ctest | 363 项（−6 删除）：layering 三档 + i18n + ui_tokens + qgis_versions 全绿；**tst_errorhub / tst_errorhub_storm / tst_notifications 三家族全绿**；tst_ui 徽标用例 `statusErrorBadgeTracksGlobalErrorHub` **PASS**（Oracle #2 ✓）；PaleoNotify 150 处计数不变、`-G'PaleoNotify'` 代码面零命中（Oracle #3 ✓） | 通过（见下方红集合对照） | 归因 + mutation |
| 9 | **红集合对照**（R0 26 → R1 22） | R0→R1 diff：5 项 perf 类基线红在 R1 变绿（tst_perf_regress / tst_perf_segyindex / tst_pyramid_consume / tst_singlefactor_perf / tst_wellfacies——本机负载噪音向好方向）；新增 1 项 tst_perfbudget（535ms > 500ms 目录枚举预算） | 已归因 | 见下方 |
| 10 | tst_perfbudget 归因 | 隔离重跑两次均 Passed（4.39s / 4.08s）——纯计时抖动 | 通过（非本任务引入） | 完成 |
| 11 | **tst_ui 归因（stash 实验）** | `git stash` 全部合流改动回 origin/master 态重编（705/705）后跑 tst_ui：**同样 2 个 FAIL**（pageShortcutsTabCycleAndDensityToggle 密度设置往返 / windowStateAndExtentPersist 几何持久化，33 passed 2 failed 9 skipped）。R0 全量时 tst_ui 为 Exception 崩（mappingWorkbench 300s 超时 abort 掩盖用例级结果）——**两态皆红、失败用例集合同，非合流引入** | 通过（归因闭环） | 恢复改动 |
| 12 | **mutation 验证**（删测试不回退覆盖的证据） | M1 去重聚合恒不命中 → tst_errorhub FAIL + tst_errorhub_storm FAIL；M2 环形满不逐出 → 两件均 FAIL；M3 findLive 活区间下界松动 → tst_errorhub_storm FAIL（id 连续性断言）。三 mutation 均被存活测试抓住；恢复后三件复跑全绿 | 通过 | review 收口 |
| 13 | **文档同步（批E 补）**：DESIGN.md Tier 1/2/3 描述从 54 系件（NotificationCard/NotificationManager/PaleoConfirmDialog/ErrorHistoryDock/ErrorHistoryModel）改写为 64 系实际形态（NotificationCenter/ToastCard/PaleoNotify::ask*/ErrorHistoryPanel）；Decisions Log 追加 2026-10-08「错误呈现唯一真源（方向 76）」条目（历史条目不改写） | 门禁全绿：layering 三档 + ui_invariants 三档 + i18n + ui_tokens 三档 | 通过 | 提交 |

## Review 轮次记录（五维：分层/行为对照/删除证据/测试覆盖/文档同步）

- **轮 1（落盘后静态）**：`paleo::services::ErrorHub` 全仓零命中（src/tests/CMakeLists）；PaleoNotify 150 处计数不变、`git diff -G'PaleoNotify'` 代码面零命中；paleonotify.{h,cpp} 零 diff；diff --check 净（仅 LF→CRLF 提示）；diff 外科式（+243/−6436）。**结论：零 High/Medium。**
- **轮 2（编译+全量 ctest 后）**：构建 383/383 绿；363 项 ctest 红集合对照（见轮 9/11 归因）；errorhub 三家族绿；徽标用例 PASS；layering/i18n 绿。**结论：零 High/Medium**（tst_perfbudget 抖动与 tst_ui 两 FAIL 均已归因为基线既有）。
- **轮 3（收口）**：分层（errorhub.h 无 QtWidgets、头三行层标记齐）；行为对照（notifications 目录仅删除无修改，64 栈四件原位不动）；删除证据（4 呈现件 src 内 include 引用全 0）；测试覆盖（mutation 三连抓）；文档同步（errorhub.h 契约注释 + 54/64 账本互指 + DESIGN.md 三级段改写 + Decisions Log）。**结论：零 High/Medium。**

## 最终状态

- 唯一 hub：`src/services/errorhub.{h,cpp}` 单类（`::ErrorHub`），AppContext installGlobal。
- 呈现栈（64 系，装配不变）：NotificationCenter / ToastCard / ErrorHistoryPanel / PaleoNotify。
- 徽标：`paleomainwindow.cpp` buildShell 订 `ErrorHub::global()` 的 errorRaised+historyCleared，计数 hub->size()。
- 「错误历史」dock 唯一：64 系 `m_errorHistoryPanelDock`（objectName errorHistoryDock 恢复唯一）；54 系 dock 构造块删除。
- 测试：tst_errorhub（16 例，含 d54 迁入 2 例）、tst_errorhub_storm（重定向 3 例，RUN_SERIAL+TIMEOUT 120 保留）、tst_notifications（不变）、tst_ui（+徽标 1 例）。
- 文件账：删 8 呈现件 + 6 测试 = 14；改 9 + 文档 3 = 12。

## 环境事实（Windows 本机）

- `powershell.exe`(5.1) 解析不了 paleo-dev.ps1 —— 必须用 `pwsh`。
- PATH 无 cmake；用户级 pip cmake 4.4.2 + ninja + ctest 在
  `~/AppData/Roaming/Python/Python313/Scripts`（构建/测试命令统一前置该 PATH）。
- worktree 不带 gitignored vendor 依赖（AGENTS.md 明示）；本机 localdeps 链
  （conda Qt 6.11.2 + QGIS prefix）可用，但 ORT 需按 `vendor/manifest.json`
  `deps.onnxruntime_win` 钉版拉取（SHA256 校验），否则 master 链不过。
