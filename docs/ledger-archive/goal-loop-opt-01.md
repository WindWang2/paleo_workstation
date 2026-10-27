# goal-loop 账本 — opt-01 数据管理 / 资产版本 / 资料图谱 / 导入预览深度闭环

- 分支：`goal/paleo-opt-01-data-catalog-graph-20261016`（远端无同名，未加后缀）
- worktree：`/workspace/audit/paleo_workstation-opt-01-data-catalog-graph`（独立 `build/`，Release，Ninja）
- BASE_SHA：`dbb03269ca1809145e1ee39b699c6fd08916a317`（启动时 `git fetch --prune` 后的 origin/master，= 任务快照）
- 角色：本会话无法派生 subagent，S1–S4 由作者本人串行承担；S5 独立 review 由父会话另派非作者 reviewer。

## 取舍记录（用户指令：不提问，取默认/保守路径并记档）

| # | 决策点 | 取舍 | 理由 |
|---|--------|------|------|
| T1 | 本机 GDAL 3.10 < 仓库要求 | 本地 `git apply` 一份未入库的 GDAL 3.10 兼容补丁（/workspace/audit 下，constraintstore.cpp / tst_realizationworkflow.cpp，行尾带本地专用 gdal3.10 标记）+ `git update-index --skip-worktree` 两文件 | 只为本机能编；两文件不在本方向写入域，skip-worktree 防误提交；每次提交前 `git show` 对本地专用标记 grep 复核为 0 |
| T2 | vendored 依赖 | 仅 `vendor/onnxruntime -> /workspace/ort/onnxruntime-linux-x64-1.30.0` 只读 symlink（同 sfdeep）；QGIS/Qt 走系统前缀（QGIS_PREFIX 空，与 build-sfdeep 一致） | BUILDING.md worktree 章节；不碰共享 prefix |
| T3 | 构建并发 | 一律 `flock -x $GIT_COMMON_DIR/paleo-shared-build-j4.lock` + `-j4`；ctest `-j2` | spec 硬约束；整机 15G 内存、可用 3–4G |
| T4 | 基线纯净 | 基线全量构建跑在未改动的 master 源上；改动先在 `/workspace/audit/opt01/scratch`（detached origin/master 的临时 worktree，**不构建**）起草，基线构建完成后再搬入 | 避免构建中途改源污染基线红/绿判定 |
| T5 | PR 是否 draft | 首批改进单元提交后即建 PR，**非 draft**（spec 允许），PR body 标注「独立 review 进行中」 | 父会话指示 review 后再续；非 draft 不影响「不合并、不请求 reviewer」纪律 |
| T6 | 新增测试文件 | 仅新增 `tests/tst_datagraphpage.cpp`（资料图谱页此前零页面级测试），注册在本域 fragment `cmake/extra-data-asset-deepening.cmake`；其余回归一律增补既有 tst_* | spec「优先增补本域已有 tests/tst_*，用本域 cmake fragment」 |
| T7 | importedWellHeadAsset 的歧义口径 | sourceUri 精确命中优先（最新登记者），无命中时文件名口径仅在唯一候选时采用，歧义返回空（不开预览） | 宁可不开预览也不开错资产；不改调用方签名 |
| T9 | 目录内多个 .qgz 且无同名主工程（R-9） | 拒绝并以非致命提醒列出全部候选，请用户直接选文件；同名主工程存在或恰一个 .qgz 时行为不变 | 字母序静默取首个可能打开备份/旧副本；用户指令「保守：拒绝并解释」 |
| T10 | 井口回查词法/规范路径次序（S5 U1-c） | 先全量词法 cleanPath 比较、命中即返回；无词法命中才算规范路径 | GUI 线程少 stat；代价：词法命中优先于更新资产的别名命中（同源同文件，结果等价的常见情形） |
| T11 | 台账形状不对/读不了（S5 U2-c/d） | 一律按损坏处理（备份后才写，读不了即拒写）；合法空数组仍为空历史 | 审计面零静默覆盖 |
| T12 | 查看器内警告（S5 U2-e） | importledgerdialog.h 不在写入域：从 datalist_undo 经对话框公开 QBoxLayout 插入警告标签 | 不越域；对话框布局若改为非 QBoxLayout 则退化为仅状态栏（不崩） |
| T13 | P2 优化范围 | 只优化实测热点（台账 load/record），保留 toVariant/fromVariant 公开接口；落盘字节须与旧实现 sha256 一致 | spec「性能只在实测后优化 + 正确性交叉校验」 |
| T14 | 导入在飞时切换工程（P5） | 不把作废批次记进新工程台账；旧工程也不补记（其 catalog 已关闭），只经回调如实报「工程已切换」 | 审计台账不串账；代价：旧工程台账缺这一次失败尝试 |
| T15 | E2E 的「更新」步 | 重导改过的源文件被产品登记为新资产（观察口径，未改）；资产 A 的版本追加用 catalog 受管 addVersion 原语，再 rollbackToVersion 恢复 | 不在本 PR 改导入版本化语义；如实记入摘要 sameAssetUpdate=0 |
| T8 | 损坏台账 | 不就地修复、不猜内容；覆盖前把原字节另存 `.corrupt-<UTC 时间戳>`，另存失败拒写 | 审计面零破坏；无 schema 变化 |

## R0 基线冻结（2026-10-16 01:51–02:40 UTC+8）

见 `docs/progress/opt-01-data-catalog-graph-20261016.md` §R0（排除矩阵、白名单、调用链映射）。

## R1 构建与基线测试（干净 master @ dbb03269 + 本地专用 gdal3.10 补丁）

- configure：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`（按 BUILDING.md 已知坑跑两遍），rc=0。
- 全量构建：`flock -x /workspace/audit/fix4/.git/paleo-shared-build-j4.lock bash -lc 'cmake --build build -j4'`
  → EXIT=0，2198 步，墙钟 2506 s（ccache 冷）。峰值内存未单独计量（整机 15G，构建期可用 3–6G，无 OOM）。
- 全量测试：`QT_QPA_PLATFORM=offscreen ctest --test-dir build -j2 --output-on-failure` → 408 项 **406 过 / 2 红**，
  real 694 s（墙钟 1108 s，含 perf 串行段）。红：`tst_singlefactor_fieldcontours`、`tst_horizontracksvc`
  ——与任务给出的本机环境基线一致（GDAL 3.10 环境项）。本次 `tst_singlefactor_store`、`tst_faciesclassify`、
  `tst_faciessupervised`、`tst_realizationworkflow`、`tst_rehydrate`、`tst_shell_projectlifecycle` 均**通过**
  （新 master 上已不红，记为基线漂移）。日志：`/tmp/opt01-ctest-baseline.log`。

## 轮次证据

### 轮 1（R3）：首批 6 个改进单元（U1–U6）

流程：先在临时 worktree（不构建）起草 → 基线构建/测试完成后搬入 → 定向构建
`cmake --build build --target tst_folderimport tst_io_importledger tst_assetops tst_assetpaging tst_datagraphpage paleo -j4`
（flock，rc=0）→ 首跑 2 红为**测试自身夹具问题**（`QFile::link` 目标父目录不存在；DataListPanel 未 show/refresh
时 m_ctx 未装载 → 查看器早退），修夹具后全绿。

**先红后绿（revert-check）**：把 6 个生产文件还原为 origin/master（folderimport.h 一并还原；importledger.h /
datagraphpage.h 保留新声明以便新测试编译），重建 5 个测试目标，结果（`/workspace/audit/opt01/revert-check-batch1.log`）：

| 单元 | 用例 | 旧码结果 |
|---|---|---|
| U1 | tst_folderimport::importedWellHeadAssetResolvesSameNameBySourcePath | FAIL：Actual "ast-1" / Expected "ast-2" |
| U3 | tst_folderimport::folderImportSurvivesServiceTeardownHonestly | **SIGSEGV**（悬垂 m_svc） |
| U2 | tst_io_importledger::corruptLedgerPreservedBeforeOverwrite / corruptLedgerUnbackupableRefusesWrite | FAIL ×2 |
| U2 | tst_assetpaging::importLedgerViewerReportsCorruptLedger | FAIL（损坏台账未给出原因） |
| U4 | tst_datagraphpage::lineageBackReturnsToOverview / relationsThenLineageBackIsStepwise | FAIL ×2 |
| U5 | tst_assetops::compareVersionsRefusesManagedPathOutsideProject | FAIL（sizeB 读到工程外文件） |
| U6 | tst_assetops::compareVersionsSamplesBothSides | FAIL（40/0） |

恢复新码后重建，定向 + 邻近回归 8 套（tst_folderimport/io_importledger/assetops/assetpaging/datagraphpage/
datagraph/storagegovernance/panels）**连跑两遍 8/8 全绿**（`/tmp/opt01-ct-b1-run{1,2}.log`）。
中间提交态校验：U5 单独态构建 tst_assetops 并跑 11/11 绿后才叠 U6。

提交（均已 push）：197af242 U1 · b5d7ad9a U3 · e99616e7 U2 · cb8e2105 U4 · f4125de7 U5 · 7c26fd69 U6。

HEAD 全量 ctest（`QT_QPA_PLATFORM=offscreen ctest --test-dir build -j2 --output-on-failure`，HEAD=7c26fd69 代码态）：
409 项 **407 过 / 2 红**，红集合与干净 master 完全相同（tst_singlefactor_fieldcontours、tst_horizontracksvc），
新增 tst_datagraphpage 通过；real 626.5 s。日志 `/tmp/opt01-ctest-head.log`。

静态门禁（HEAD）：`git diff origin/master --check` 0；`check_layering.py --strict` 通过；
`check_ui_invariants.py --strict` clean；`check_i18n.py` 通过。HEAD 全量构建 rc=0（增量 91 s）。

性能 A/B：本批无以性能为目标的改动，**未测量**（U1 回查为 O(资产数) 单次遍历，同旧阶）。

自审（作者两轮，非独立 review）：
- 第 1 轮：U3 同步发现「单文件作业 assetId 仍非空 = 假成功」，并入 U3 修复；U2 备份只做一次（清 loadWarning）。
- 第 2 轮：核对 U4 行为面——`openLineage("", "")` 且当前无焦点时旧码进入无焦点追溯态，新码回概览（Low，记 PR）；
  U1 每个 well_head 资产多一次 canonicalFilePath stat（Low，资产量级小）。High/Medium：0。

纪律核对：首个 docs 提交 12847382账本正文以散文形式写出了本地补丁标记字样（非代码 hunk），
后续 docs 提交已改写为不含该字样；不 force-push 故历史保留，PR body 如实说明。代码提交对本地补丁标记 grep 计数 = 0。

### 轮 2（批次 2，进行中）：R-9、R-7+P4、P2，以及 S5 第 1 轮 Low 修复

| 提交 | 单元 | 问题 → 修复 | 测试（旧码结果，日志） |
|---|---|---|---|
| f9b669c1 | R-9 | 多 .qgz 无同名主工程时字母序静默打开首个 → 拒绝并列候选（T9） | tst_folderimport::openPathRefusesAmbiguousQgzDirectory（红：openPath 返回 true；revert-check-r9.log） |
| cd2344fa | R-7 + P4 | 追溯/影响/展开关联/实体激活同一点击双发图谱查询 → QSignalBlocker 屏蔽模式框，显式 request 单发；索引统一 findData | tst_datagraphpage::nodeVersionPreviewBackRoundTrip（红：prepareCount 2≠1；revert-check-r7.log） |
| db876508 | P2 基准 | 台账有界规模往返基准（默认 1k；环境变量开 10k/100k），sha256 + 逐字段回读交叉校验 | tst_io_importledger::ledgerScaleRoundTrip |
| 7ee24b66 | S5 U1-c/U1-d | 井口回查每资产 stat → 两遍（T10）；修过时注释 | 符号链接别名断言（语义守护，旧码亦绿） |
| 298416fd | S5 U3-d | 服务析构用例行可能为空时空转 → 断言非空、逐行 Failed 且带原因 | 测试收紧 |
| a479c654 | S5 U2-c/U2-d | 台账形状不对/读不了被当空历史覆盖 → 按损坏处理（T11） | corruptShapesAreNotOverwritten ×3 行 + unreadableLedgerRefusesWrite（红 ×4；revert-check-u2cd.log） |
| 9bfc3e27 | S5 U2-b | 备份经 QFile::copy 未保证落盘 → QSaveFile 写备份 | 既有备份字节相等用例 |
| 9aa9dd94 | S5 U2-e | 损坏原因只上状态栏（被模态框遮挡）→ 查看器顶部警告（T12） | importLedgerViewerReportsCorruptLedger 增断言（红；revert-check-u2e.log） |
| 72ef6604 | S5 U4-c | 返回栈用例缺逐步断言 → 每步断言查询次数与恢复焦点真实 id | 测试收紧 |
| 1f482a9d | S5 U5-c | compareVersions 注释与 resolvedVersionPath 实现不符 → 修注释 | — |
| 91aa5fd6 | P2 优化 | 台账 QVariant 中转为实测热点 → 直接 JSON↔结构 | A/B 见下 |

**P2 同机同构建 A/B**（box：8 核 / 15G，load avg ≈1.0；A=91aa5fd6^ 的 importledger.cpp，B=91aa5fd6；
同一 tst_io_importledger 二进制仅换该文件重建；每实现 3 轮、每轮每档 3 次取中位数；日志
`/workspace/audit/opt01/perf-p2-ledger.log`）：

| 行数/批 | A record 中位 | B record 中位 | A 回读 3 批 | B 回读 3 批 | 落盘 sha256 A=B |
|---|---|---|---|---|---|
| 1k | 10–11 ms | 7–8 ms | 6–7 ms | 3–4 ms | ✓ 94880bbe… |
| 10k | 126–129 ms | 77–89 ms | 94–99 ms | 41–42 ms | ✓ 9f2a3d40… |
| 100k | 1846–1921 ms | 846–957 ms | 1893–2003 ms | 493–514 ms | ✓ 33e093de… |

批次 2 续：

| 提交 | 单元 | 问题 → 修复 | 测试 |
|---|---|---|---|
| fe15e9a2 | P5 | 导入在飞时切换工程把作废批次记进新工程台账 → 不入账（T14） | folderImportAcrossProjectSwitchDoesNotCrossPostLedger（红：新工程台账 1 批；revert-check-p5.log） |
| 6f736cd2 | P1 | 工程数据 E2E 双跑夹具 | projectDataEndToEndTwice（两遍摘要一致） |
| cd92a246 | P3 | 合并窗换代 + 取消回归 | rapidRefocusLandsOnLatestGeneration |

| 2700540e | P5 自审修正 | 切工程判定纯字符串比较可能误判同工程 → 先词法后规范路径 | 用例末段换写法重设同工程仍入账 |

批次 2 验证（HEAD 代码 = 2700540e）：
- 全量构建 `flock … cmake --build build -j4` rc=0（`/tmp/opt01-b2-final-build.log`）。
- 全量 `QT_QPA_PLATFORM=offscreen ctest -j2`（cd92a246 代码态 + 未提交的 P5 自审修正正在编辑中，`/tmp/opt01-ctest-b2.log`）：409 项 406 过 / 3 红 =
  {tst_singlefactor_fieldcontours, tst_horizontracksvc}（同基线）+ tst_singlefactor_perf（`proposedBudgets` L 档中位 15303 ms > 15000 ms 预算，
  单独重跑仍超 ≈2%；该测试只链 paleo_algorithms，本 PR 改动文件与之零交集，标签 envdebt_perf-host——记为本机负载/主机性能漂移，非本 PR 引起）。
- 定向 8 套（tst_folderimport/io_importledger/assetops/assetpaging/datagraphpage/datagraph/storagegovernance/panels）在 2700540e 上 `ctest -j2` **连跑两遍 8/8**（`/tmp/opt01-ct-b2-run{1,2}.log`）。
- 门禁：`git diff origin/master --check` 0；check_layering --strict 通过；check_ui_invariants --strict clean；check_i18n 通过；
  最终树中本地补丁标记 0；代码提交中 0（仅 12847382 账本散文出现，8456f9f1 已删除该句）。

自审（批次 2，作者两轮）：
- 第 1 轮：发现 fe15e9a2 同工程判定纯字符串比较可能漏记 → 2700540e 修复（自审时定为 Medium，已修复）；U2-c 修复后，部分批次无效的数组仍会静默丢弃无效项（既有行为，记 Low）。
- 第 2 轮：复核 R-7 各入口 findData 索引与旧硬编码一致；P2 JSON 直转对 null/非字符串/非数组 rows 的容错与旧 QVariant 路径逐一对照，sha256 A=B；
  U2-b 中 QSaveFile 遇同毫秒同名备份会覆盖（首备份后警告即清空，实际不会再写第二份），记 Low。High/Medium：0（剩余）。

S5 第 1 轮（dbb03269..0dd90e07）：0 High / 0 Medium；Low 处置见 PR「独立 review」节。
U5-e（versiondialog.h）不在写入域，未改；U1-b/U4-b/U5-b/U5-d/U6-b 记入 PR 已知限制。

### 轮 3：S5 第 2 轮（0dd90e07..935dcc55：0 High / 1 Medium / 若干 Low）修复

| 提交 | 项 | 修复 | 测试（旧码结果） |
|---|---|---|---|
| b3d849d6 | M-1 | 井口回查符号链接别名子例以 `#ifndef Q_OS_WIN` 包裹（Windows QFile::link 生成 .lnk） | — |
| eb9aee8c | Low 2 | projectopen 候选只数 QDir::Files，提示清单封顶 20 条并注明余数 | openPathRefusesAmbiguousQgzDirectory 增子例（红：26≠25） |
| 25acad5a | Low 4 | 下游影响/展开关联/实体节点/实体目录入口各断言单次一查询 | everyRefocusEntryPointSendsOneQuery（对 cd2344fa^ 红：2≠1） |
| 720e0534 | Low 16 | 去掉 qWait(300)：按 ready 携带焦点等待期望代；取消半段确定性断言 | rapidRefocusLandsOnLatestGeneration |
| a425e229 | Low 15 | E2E 参照摘要每行自算（run2 可单跑）；显式断言重导 = 新资产 | projectDataEndToEndTwice |
| 577dd9f9 | Low 14 | POSIX：prepareOpen(符号链接别名) + setProjectDir(真路径) 后同工程批次仍入账 | 对 fe15e9a2 红（台账 0 批），佐证 2700540e |
| a3d5bc61 | Low 9 | 台账部分批次无法识别也告警并先备份 | corruptShapesAreNotOverwritten(partly-invalid)（红） |
| f0947ac3 | Low 7 | 规模基准断言落盘字节 == toVariant 旧序列化路径字节 | ledgerScaleRoundTrip |
| de2521ff | Low 12 | 查看器警告标签 setTextFormat(Qt::PlainText) | importLedgerViewerReportsCorruptLedger（红） |

S5 第 2 轮 Low 8、Low 11：留作已知限制（内容见下「S5 第 3 轮与最终处置」）。

## 收尾门禁（R10，2026-10-16 05:2x UTC+8）

- `git fetch --prune origin`：origin/master 仍为 BASE_SHA dbb03269（未移动，无需合并）；open PR #486/#487/#488 与本 PR 文件交集为空；open issues 仍为 #460–#485（26）。
- 全量构建：`flock -x <git-common-dir>/paleo-shared-build-j4.lock bash -lc 'cmake --build build -j4'` rc=0（de2521ff 代码态；a5e7044c 之后仅文档，`ninja: no work to do`）。
- 全量 `QT_QPA_PLATFORM=offscreen ctest --test-dir build -j2`（de2521ff 代码态，04:44–05:17，real 641 s，`/tmp/opt01-ctest-final.log`）：**409 项 407 过**，红 = {tst_singlefactor_fieldcontours, tst_horizontracksvc}，与干净 master 基线完全相同。
- tst_singlefactor_perf 定性：该二进制 02:32 由干净 master 源构建、此后未重链（只链 paleo_algorithms，本 PR 零触及；libpaleo_algorithms.a 01:55）。基线 164.5 s 通过 → 批次 2 全量与单跑两次超预算（L 档 15303 ms > 15000 ms，box load avg 最高 ≈9）→ 收尾全量 165.6 s 通过。同一 master 二进制时过时不过 ⇒ **主机负载导致的预算抖动，与本 PR 无关**。
- 定向 8 套（tst_folderimport / io_importledger / assetops / assetpaging / datagraphpage / datagraph / storagegovernance / panels）`ctest -j2 -R …` **连跑两遍 8/8**（`/tmp/opt01-ct-final-run{1,2}.log`）。
- 静态门禁：`git diff origin/master --check` 0；`python3 tools/check_layering.py --strict` 通过；`python3 tools/check_ui_invariants.py --strict` clean；`python3 tools/check_i18n.py` 通过。
- 本地补丁标记扫描：最终树 0；src/tests/cmake 在全部提交中 0；仅文档散文出现于 12847382/8456f9f1（ef79b2ee 已删除）。两份本地补丁文件仍为 skip-worktree，未提交。

## PR

- https://github.com/WindWang2/paleo_workstation/pull/489（base master，非 draft，`gh pr view` 核验 OPEN；取舍 T5）。首批后暂停，等待独立 S5 review。

### S5 第 3 轮与最终处置（2026-10-16）

- S5 第 3 轮（935dcc55..b6a52c0c）：0 High / 0 Medium，第 2 轮修复全部验证。测试 Low：everyRefocusEntryPointSendsOneQuery 的实体节点子例紧接「展开关联」已处于资料关联模式，对旧码无鉴别力 → 172704a3 先切到实体目录再双击；只回退实体激活一处为旧写法时该子例红（2≠1，revert-check-r3-entity.log）；tst_datagraphpage 连跑两遍通过。
- R-7 口径更正：「追溯/下游影响/展开关联」为按钮单击；实体节点与「实体目录」分组为图上双击动作。
- 本地补丁标记历史：12847382 引入散文提及；8456f9f1 改写时又新增一行含标记字面量文字，ef79b2ee 删除。
- U5-b：datacatalog_versions.cpp 与 #476 同文件（非同 hunk），超出本任务写入域，未改。
- S5 r2 Low 8（已知限制）：10 万行级台账 load/record 仍在 GUI 线程同步（≈0.9 s record / ≈0.5 s 回读）。
- S5 r2 Low 11（已知限制）：备份读原件 readAll 无法发现短读；同毫秒同名备份会被覆盖；台账读不了时写入被永久拒绝，仅经 qWarning（及查看器警告）体现。
