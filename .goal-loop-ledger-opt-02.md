# goal-loop ledger · opt-02 井震剖面方向（测井/地层对比/连井剖面/地震多视图联动）

- 分支：`goal/paleo-opt-02-well-seismic-correlation-20261016`
- 基线 BASE_SHA：`dbb03269ca1809145e1ee39b699c6fd08916a317`（= origin/master @2026-10-16 快照）
- worktree：`../paleo_workstation-opt-02-well-seismic-correlation`（独立 build/，vendor 只读 symlink）
- 构建：Ninja RelWithDebInfo，全部经跨 worktree 锁 `$(git-common-dir)/paleo-shared-build-j4.lock` 以 `-j4` 执行；测试 `QT_QPA_PLATFORM=offscreen`，`ctest -j2`
- 基线构建：31m06s（域测试目标集，813 步）；域内 ctest 基线：所选 68 目标全绿（Not Run 项为未纳入首批目标集，非失败）

## 排除矩阵（R0 冻结）

- Open Issues：#460–#485 全文已读（正文落 /tmp/opt02-issue-bodies/）。域内占用指纹：
  - #466 LasParser::parse 旧入口；#476 P3 welllogfacies 采样 / timedepthmodel:26-39 首点负深度 / wellsectionstore:334 splitEscaped；#477 laswriter fmtValue / wellcompositexml；#480 wellsectionworkflow:359 与 sectionworkbench:396 LAS 静默 continue；#481 paleomainwindow_wellsection delete/new store + syncSeismicVolumeToDocks + 进度 connect；#482 seismicsection canvas render:357 wiggle / interact:85 hover / paint:91 标签 + wellcomposite compositebody 旗宽 / wellcompositestore JSON；#463/#486 seismic3d 书签；#479 ai/services 清单
  - Open PR：#486（welllogfacies.cpp / seismic3dviewpanel.* / tst_wellfacies.cpp）、#487（qgisstyleservice.cpp / tst_services2.cpp）——文件级禁改
- 域内经审计**未占用**且本任务落地的候选全部逐条核对了上述指纹（见各单元「排除核对」）。

## 审计（R2）

四路只读 subagent（S1 井数据模型 / S2 连井与对比 UI / S3 地震多视图 / S4 选择同步与 E2E）产出 21 条带 file:line 证据候选；主 agent 逐条亲验代码全部属实。定稿 13 个落地单元 + 3 条域外记录：

### 落地单元（实现 + Oracle + 红绿验证）

| # | 候选 | 文件 | Oracle |
|---|------|------|--------|
| A/U1 | setSection 无条件清地震缝+重发（无关刷新闪断/反复取消重提取） | ui/wellsection/wellsectionpanel.cpp | tst_wellsection_ui::setSectionKeepsStripWhenSeismicIdentityUnchanged |
| B/W2 | 时深表只有一侧域列时 tops 走常速把 TVD 当深度（与曲线错位） | workflow/sectionworkbench.cpp | tst_sections_alignment::singleDomainTimeDepthTableTopsUseExactModel |
| C/W3 | topsTable NoSurvey 井 TVD 列冒充（违反 .h 三态契约） | domain/wellsection.cpp | tst_wellsection::topsTableTvdColumn（扩展） |
| D/W4 | 时深装载失败零告警（workbench status 丢弃 + setCheckshots 无 else + TVD-only 表不可用无解释） | workflow/wellsectionworkflow.cpp | tst_wellsection_workflow::timeDepthLoadFailuresWarn |
| E/Q1 | 拾取 undo/redo 不刷画布叠加（幽灵点/消失点） | ui/seismicsection/seismicsectiondock_internal.h | tst_seismic_sectionui::pickUndoRedoSyncsCanvasOverlay |
| F/Q2 | 道头卡列号直当文件道序（非首线全读错道） | ui/seismicsection/seismicsectiondock_bookmark.cpp | tst_seismic_sectionui::traceHeaderCardResolvesColumnToPhysicalTrace |
| G/Q3-Q5 | 卷帘切时间片/任意线不关帘 + zone/attr 叠加换线/任意线不清（#224 同族补漏） | ui/seismicsection/seismicsectiondockwidget.cpp | tst_seismic_sectionui::curtainDisabledOnTimeSliceMode / overlaysClearedOnLineSwitchAndArbitraryLine |
| H/Q6-Q7 | 追踪/断层拾取无时间片方向闸（伪身份拾取落盘） | ui/seismicsection/seismicsectiondock_track.cpp + canvas_interact.cpp | tst_seismic_sectionui::trackingGatedOnTimeSlice |
| I/W5 | valueAtDepth 双 NaN 返回 0（把缺失伪造成合法物理量）+ paintBody 深度 NaN 不断笔 | domain/wellcompositemodel.cpp + ui/wellcomposite/trackcurves.cpp | tst_wellcomposite_framework::valueAtDepthNanGapReturnsNan |
| J/W6 | TimeDepthModel 构造器/isValid 速度上界缺失（与 setVelocity 口径分裂） | domain/seismic/timedepthmodel.cpp:15-18,173 + sectionworkbench 显示口径 | tst_timedepthmodel::velocityBoundsSymmetric |
| K/V1 | 工程边界不清 SelectionContext ids/horizon（旧选择污染新工程 + 等值短路吞广播） | workflow/evidenceinteraction.cpp | tst_evidenceselection::projectResetClearsSelectionIdsAndHorizon |
| L/U2 | 版头右键菜单捕获陈旧列号（popup 存续期井集重建删错井） | ui/wellsection/wellsectionscene.cpp | tst_wellsection_ui（见 M/L 合并用例） |
| M/U3 | 格架 removeUnit 级联删子单元但标志层 unitId 悬空（持久化 + 诊断告警） | ui/sequenceframeworkpanel.cpp | tst_sequenceframework_ui（级联用例扩展） |
| N/U4 | 栅状图落库失败静默 + persisted 快照照常前进（编辑丢失永不重试） | ui/wellsection/fencewidget.cpp | tst_wellsection_ui（失败注入用例） |
| O/V4 | 剖面保存→恢复 E2E 零覆盖（含体缺失分支） | tests only | tst_sectionlifecycle::sectionSaveRestoreRoundTrip |

### 域外/不修记录（证据保全，不立项）

- W1：datapreview 井预览 LAS 深度 FT→m 不折算（datapreviewtabwelllog.cpp:316 / tabs_internal.h:686）——`src/ui/datapreview` 不在本任务写入域；建议另行立项（消费侧其余四路均折算，仅此路径漏）。
- V2：SeismicMapLink::setSeismicLayer 死接线 + O(N) 全表扫——接线属产品决策（接进 SelectionContext 的时机），本轮只读分析。
- V3：证据选择 writeEntry 每次 targetChanged 把 QgsProject 写脏（切工程误弹保存框）——修复需改恢复点存储通道（非脏写或显式落盘），设计级变更，记录待立项。
- S2 弃选清单（WellCorrelationPanel 隐藏态交互缺陷等）见 S2 报告「弃选说明」。

## 验证记录

- 红（修前旧代码）：六用例 stash 验证全红（见「红验证」节）
- 绿（修后）：受影响 10 个测试二进制全量绿；收官第二遍（含 correlation/
  crossplot/linkage/datapreview/deviation/welllogset 等扩域共 26 目标）
  **100% tests passed**（ctest -j2，offscreen）
- 静态门禁：`check_layering.py --strict` 通过、`check_ui_invariants.py
  --strict` clean（violations=0）、`check_i18n.py` 通过、`git diff --check`
  干净
- 离屏渲染验证：既有像素断言用例（renderImage/renderCanvas/imageDiff、
  captureEvidence/captureTokenSectionEvidence 截屏族）随套件绿

## 红验证

stash 旧 src → 重建六目标 → 直跑新用例（QGIS_PREFIX_PATH=vendor superbuild + offscreen）→ 六个新 Oracle 全部按预期失败（各自命中预期判据），恢复改动后全绿：

| 用例 | 旧代码失败判据（实测） |
|------|------------------------|
| singleDomainTimeDepthTableTopsUseExactModel | 井 A twt 80ms（常速把 TVD100 当深度）/ 井 B 120ms ≠ 修复后 105ms |
| topsTableTvdColumn | NoSurvey 行 TVD 单元格写出 "1000.00" ≠ 空 |
| timeDepthLoadFailuresWarn | `blob.contains("A1")` 为 FALSE（零告警） |
| valueAtDepthNanGapReturnsNan | `std::isnan(...)` 为 FALSE（返回 0.0） |
| velocityBoundsSymmetric | `TimeDepthModel(30000).DepthToTwtMs(2500)=166.67` ≠ 缺省口径 2000 |
| setSectionKeepsStripWhenSeismicIdentityUnchanged | seisSpy==1（无条件重发）≠ 0 |

E/F/G/H/K/L/M/N/O 用例因锁竞争窗口未做 stash 级红验证（判据在用例注释标明「先红」语义；S5 review 已复核判据与实现因果一致，含 Q6 手写 4×4 体的线号域论证）。

## 独立 Review（S5，非作者 subagent）

结论：**可以合并**，无阻断项。应修 A-1（fence 失败提示行重复累积）+
建议 B-1（空曲线仍伪造 0）/B-2（注释措辞）/B-4（push 后双重刷新冗余）
四项全部采纳，修复于 commit `6bc7b171`；修复后受影响套件复跑全绿。
S5 同时复核：无 UAF/线程/越界/伪断言问题；keepStrip 门控与 faultsRequested
无条件重发的语义论证成立；与 #476/#480/#481/#482 占用 hunk 零越界。
复审（对 6bc7b171）因平台 5 小时配额上限（06:30 重置）无法再派发独立
agent——改由主 Agent 逐项二次核验（A-1 截断键含 note 专属前缀不影响基础
文本分隔符；B-4 保留的 :67/:178 两处均非 undo push 路径；B-1 空曲线分支
消费方均有 isEmpty 前置守卫）并如实记录此证据缺口。

## 已知限制 / 后续建议（不在本 PR 扩面）

- topsTable NoSurvey 留空是对外 CSV 的诚实化破坏性变更：外部脚本需按
  「空 = 无实测垂深」适配（S5 提示，PR body 已注明）
- SeismicStrip 未携带体身份：切活动体与去抖重取同帧到达的窄窗口会保全
  旧体缝（既有缺陷的窗口扩大；治本 = strip 附体世代，建议后续立项）
- 域外三项（datapreview FT 折算 / SeismicMapLink 死接线 / 证据选择
  writeEntry 脏工程）证据在账本，未修

## 阶段

| 阶段 | 状态 |
|------|------|
| R0 基线冻结 + 排除矩阵 | 完成 |
| R1 构建/测试基线 | 完成 |
| R2 四路审计 + 亲验定稿 | 完成 |
| R3 实现（A–O 单元 + AddPicks id 记账） | 完成（8 commit） |
| R4 独立 review（S5） | 完成（A-1/B-1/B-2/B-4 修复于 6bc7b171） |
| R5 收官 Oracle + PR | 完成 |

## 提交

```
9509817d fix(well,seismic): 时深单域表回退精确换算 + 导出/装载/边界三处诚实性
1de08656 fix(wellcomposite): NaN 空档读数不再伪造 0 + 折线对深度 NaN 断笔
a7095991 fix(wellsection): 地震缝保全去冗余 + 版头菜单 id 锚定 + 栅状图落库失败可见
a631e935 fix(sequenceframework): removeUnit 级联删除子体系域时清空其名下标志层归属
4ac1e6ca fix(seismicsection): 拾取 undo 同步/道头索引域/卷帘与叠加生命周期/时间片方向闸
cd3239f0 fix(linkage): 工程边界复位清空选择 ids 与活动层位
933b14fb test(sections): 剖面保存→恢复 E2E 往返（恢复侧此前零覆盖）
6bc7b171 fix(review): S5 复审四项收口
（+ docs/ledger commit）
```

## 阶段

| 阶段 | 状态 |
|------|------|
| R0 基线冻结 + 排除矩阵 | 完成 |
| R1 构建/测试基线 | 完成 |
| R2 四路审计 + 亲验定稿 | 完成 |
| R3 实现（A–O 单元） | 完成 |
| R4 独立 review（S5） | 进行中 |
| R5 收官 Oracle + PR | 待 |

## 提交

（按单元分 commit，见 git log）
