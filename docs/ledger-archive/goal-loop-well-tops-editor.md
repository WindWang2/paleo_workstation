# Ledger — 方向 32：井分层数据编辑与质量管理

Worktree: `.worktrees/well-tops-editor`，分支 `goal/well-tops-editor-20261004`（基线 `origin/master` dda2faa）。

## 勘察结论（实测事实）

- `well_stratification`（DC.dat）由 `src/io/wellfileparsers.cpp:79` 解析为 `WellTopRecord`
  （井名/层名/MD/X/Y/Z/TVD/Time，-99999 哨兵判空）；一份多井文件 = 一个资产 + 每井一条
  `tops` 链接；**无写侧序列化器**（全仓检索确认）。
- 消费端 `ProjectDataFacade::topsFor`（`src/services/projectdata.cpp:125`）读
  **primary 链接资产的 currentVersion（最高 versionNumber）**文件并现场重解析——
  编辑落新版本（更高号）即自动成为下游读到的内容。
- `addVersion` 的 supersede 语义（`src/catalog/datacatalog.cpp:776-787`）：同资产低号旧版本
  的下游闭包（DERIVED）随本次 addVersion 原子落 `extra["stale"]` ⇒ **下游失效免手接**。
- **陷阱**：编辑新版本若把旧版本记入 `parentVersionIds`，则新版本自身在旧版本闭包内、
  且是 DERIVED——会被自己的 addVersion 标 stale。⇒ 编辑版本血缘记 `extra`
  （`editBaseline` 等），parentVersionIds 留空。
- `DerivedAssetRegistrar::stage` 按 (type, displayName) find-or-create——传既有资产的
  displayName 即在同一资产上发新 DERIVED 版本，路径 `artifacts/derived/...`，commit 现算
  sha + 置只读。可整体复用。
- 层序格架词表：`AreaRules::active().sequenceBoundaries`（浅→深有序）。
- UI 惯例：纯代码建 UI、objectName 锚点、tr() 中文源串、paleotheme token 单一真源、
  无动画；入口模式 = datalist 右键 → `topsEditRequested(assetId)` 意图信号 → 壳
  （paleomainwindow_attach）开对话框（同 gridHorizonRequested 先例）。

## 设计定案

| 件 | 位置 | 职责 |
|---|---|---|
| 纯函数校验/差异/合并/批量 | `src/domain/welltopsedit.{h,cpp}` | 数据层；行级 issue、diff 摘要、合并取舍、批量变换（无 IO，worker 安全） |
| DC.dat 写侧 | `src/io/wellfileparsers.{h,cpp}` 追加 `writeWellTopsText` | 自适应精度（3..9 位小数往返精确）+ -99999 空值 + `#` 头 |
| 版本化提交编排 | `src/workflow/welltopseditorworkflow.{h,cpp}` | 功能层；GUI 线程单事务 commit（stage+commit 复用 registrar）、回滚=新版本、合并落库、校验上下文（TD/格架） |
| 编辑器视图 | `src/ui/welltops/` | 表格 CRUD/脏行/校验面板/批量菜单/版本回滚/合并对话框 |

口径裁决（记档）：
- DC.dat 是**点式**分层（每行=一个层位顶点），无底深列。「顶<底」校验映射为：
  同井 MD 严格递增（同深=叠置）、TVD≤MD（域冲突）、MD∈[0, 井 TD]。备注列格式不存在，
  不伪造（PR 遗留项）。层间「底深」由下一行 MD 隐含，排序即整理。
- 空洞/倒置锚定：有格架词表时——缺失层位（空洞）锚到按层序应插入处的相邻行；
  名单外层名（悬空）与层序深度倒置逐行检出；无词表跳过（诚实降级）。
- 编辑版本 = 同资产新 DERIVED 版本（versionNumber+1），extra 记
  `editKind/editWell/rowsAdded/rowsRemoved/rowsChanged/editBaseline`；与当前版本字节相同
  ⇒ no-op 不发版本。
- 回滚 = 用目标旧版本文件字节发新版本（extra 记 `rollbackTo`），currentVersion 指新号。
- 批量修正差异计算 = domain 纯函数（无共享态，天然可下 worker）；落库 = GUI 线程
  一次 addVersion 单事务（produce-then-commit 口径；数据量级万行内同步计算为毫秒级，
  不引入任务池——记 PR 说明）。
- 导入融合 = 编辑器内「从文件合并…」：逐冲突行 保留旧值/采用新值，合并结果进编辑表
  待审，保存才落库（不默认覆盖）。

## 批次日志

### B1 环境与勘察
- [x] worktree/vendor symlink/cmake 配置；全量构建通过。
- [x] 三路勘察（解析链/版本链/UI 模式）+ 关键源码精读定案。
- 磁盘事故：/home/kevin/projects 挂载盘 100% 满（10 个 worktree build 所致）——
  本 worktree build 迁至 /home/kevin/paleo_builds/well-tops-editor（大盘），旧 build 删除。

### B2 实现（四件 + 接线 + 测试）
- [x] domain/welltopsedit：校验（9 种 issue，行级锚定）/差异/合并/批量纯函数。
- [x] io：writeWellTopsText 写侧（自适应精度 3..9 位、-99999 哨兵、CRLF + '#' 头）。
- [x] workflow/welltopseditorworkflow：GUI 线程单事务 commit（DerivedAssetRegistrar
  stage+commit 复用）、unchanged no-op、回滚=新版本、合并行集、validateAllWells。
- [x] ui/welltops：编辑器对话框（8 列表格 CRUD/脏行/校验阻断/批量/版本回滚/合并）
  + 合并对映对话框（冲突默认保留旧值）。
- [x] 接线：datalist 右键「编辑分层…」→ topsEditRequested 信号 → 壳开模态对话框。
- [x] tst_welltopsedit（20 用例）+ tst_welltopseditor_ui（8 用例）。

### B4 评审轮 2（独立 agent）
- 确认轮 1 十四项修复全部落地、无 High 回归；新发 1M+6L，修复于 1690a09：
  合并值列全字段展示、reloadAll 清合并溯源、Z 有值而 X/Y 缺失阻断、空文件
  首井名输入、同深改排序链式比对（容差与 sameTop 一致）、脏行按同名出现序
  对位、issue 按行号稳定排序、哨兵文案对齐判定域。
- 测试锚：sentinel/wellKey '_'/validateAllWells/merge 溯源（tst_welltopsedit），
  precision+Z round-trip（tst_welltopseditor_ui）。

### B5 评审轮 3（独立 agent）
- 轮 2 七项修复的纯逻辑面全部验证正确；发现 3 个收口不完整 Medium，修复于
  db6ff1b：合并值列改单行 + ResizeToContents + tooltip（固定行高裁掉双行）、
  写侧 Z 门控改 hasX||hasY（半坐标组保 z）+ HalfCoordinateGroup 警告、新井名
  循环校验拒空白；另修 待修正 状态、只读禁行操作、algorithm include、stable_sort。
- 测试锚：sameDepthChainWithinTolerance（1e-6 链）、halfCoordinateGroupPreservesZ、
  zWithoutXYBlocksSave。

### B6 评审轮 4 + 5 + 6（独立 agent ×3）
- 轮 4 发现 2 Medium（半组 hasX||hasY 语义只传导到 3/6 处门控），修复于
  bc15131：collectRows/refreshRowStatus 改「双缺才拦」+ 文案、applyShift 半组
  z 同位移、只读禁版本/回滚菜单。测试锚：halfGroupRowLoadsCleanAndSaves、
  applyShift 半组 z 断言。
- 轮 5 抽检：6 处门控一致性核对表全过——**0 High / 0 Medium**。
- 轮 6 广域终扫（外围件/接壳/CMake/测试质量）：**0 High / 0 Medium**（连续
  两轮清零达成硬要求）；Low×3 已随后修掉（见 B7）。

### B7 收尾 Low 清理
- ledger 落后 → 本段补记全部轮次与对账（即此）。
- editTops 菜单项无测试锚 → tst_panels 菜单矩阵加正反断言（对齐 gridHorizon
  先例）。
- 新井名守卫漏 '#' 前缀（行首 # 写盘即注释）→ 守卫与文案补齐。

### B3 评审轮 1（独立 agent + 自查）
发现并修复（全部带测试锚）：
- H1 Z 列丢失→编辑器加 Z 列（8 列），fillTableFrom/collectRows/refreshRowStatus 同步。
- H2 显示 'f',3 截断→numericCell 走 WellTopsEdit::formatDepth（与写侧同出口）。
- M1 空基线井回退失败→m_currentWell（combo 显示形）回退。
- M2 哨兵值可输入→validator 新增 SentinelValue（错误级，TVD/X/Y/Time）。
- M3 批量不过校验→commitBatch 先走 validateAllWells（错误阻断，井名前缀）。
- M4 merge editKind 死代码→m_pendingMergeNote 溯源，保存传 editKind=merge + note。
- M5 wellKey 缺 '_'→对齐 DataCatalog::normalizeWellName。
- L1 空文件误报→readAllBytes 区分开失败与空内容；L2 .partial 失败回收；
  L3 padNum 负宽保护；L4 stage 资产一致性校验；L5 diff/merge 首见为准；
  L6 空井名行错误级（EmptyWellName）；L7 切井刷新状态行。
新增测试：sentinel/wellKey '_'/validateAllWells/merge 溯源/precision+Z round-trip。

## 验收对账（Oracle）

| # | 验收项 | 证据 | 状态 |
|---|---|---|---|
| 1 | 编辑 round-trip：改分层→保存→重开逐字段一致，catalog 新版本/SHA 正常 | tst_welltopsedit::commitRoundTrip（新 DERIVED 版本/sha/extra、他井不扰动、重开 catalog 持久）+ tst_welltopseditor_ui::editMarksRowDirtyAndSaveRoundTrips / precisionAndZSurviveEditorRoundTrip | ✅ |
| 2 | 校验召回：叠置/同深/空洞/倒置/悬空各一，全部行级检出 | tst_welltopsedit::validatorRecall（含行号断言）+ sameDepthChainWithinTolerance + sentinelValuesAreValidatorErrors + halfCoordinateGroupPreservesZ | ✅ |
| 3 | 导入合并：同井二次导入 → 冲突行逐条分别取舍，结果符合预期 | tst_welltopsedit::mergeDiffAndApply + mergeViaWorkflow（外部文件按井过滤、取舍落库、editKind=merge 溯源）+ tst_welltopseditor_ui::mergeDialogDefaultsAndResolutions（默认保留旧值 + 逐行改选） | ✅ |
| 4 | 下游失效：编辑提交后消费面失效标记（版本 bump） | tst_welltopsedit::downstreamStaleAfterEdit（父=RAW 的 DERIVED 被 supersede 标 stale、新版本自身干净） | ✅ |
| 5 | 全量绿 | ctest 全量（最终代码 exit 0）+ layering×3/ui_invariants×3/i18n×2 + git diff --check 全过；tst_constraintstore/tst_datapreview/tst_wellsection×3 绿 | ✅ |

评审纪要：六轮独立评审（轮 1-4 各发现并修复 2H/5M/7L → 1M/6L → 3M/4L → 2M/1L；
轮 5+6 连续 0 High/0 Medium）。Low 未修项（评审 Info 级，记 PR body）：X/Y 双哨兵
而 z 实值的外来行重写时 z 丢（格式无 hasZ 模型固有）、applyShift 计数断言对新旧
门控等价（区分度在 z 断言）。
