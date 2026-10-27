# opt-02 · 测井/地层对比/连井剖面/地震多视图联动深化（2026-10-16）

基线 `dbb03269`（PR #459 后 master）。分支
`goal/paleo-opt-02-well-seismic-correlation-20261016`。完整账本见仓库根
`docs/ledger-archive/goal-loop-opt-02.md`。

## 范围与排除

写入域：`src/ui/{wellsection,wellcomposite,correlation,crossplot,seismicsection}/`、
`src/ui/sequenceframework*`、`src/workflow/{wellsectionworkflow,sectionworkbench}*`、
`src/domain/` 井深/地层非 issue 子文件、`src/linkage/`、`tests/` 既有用例增补。
完全避开 2026-10-16 快照全部 Open Issues（#460–#485）与 Open PR（#486/#487）
的文件/hunk 指纹；与 #476（timedepthmodel:26-39）、#480（wellsectionworkflow:359）、
#482（seismicsection canvas render/interact/paint 与 wellcomposite 四项）等同文件
不同 hunk 的单元在 PR body 单列说明。

## 交付单元（15 项，均带测试 Oracle 与修前红验证）

1. **井震对齐一致性（P1）**：时深表只有一侧域列（MD-only / TVD-only）时，
   剖面分层标记改按该行另一域值经可用模型精确换算（对称回退经测斜
   tvdAt），不再把 TVD 值当深度喂 2500 m/s 常速近似——同井曲线与分层
   标记在 TWT 剖面上不再系统性错位。`sectionworkbench.cpp`；
   `tst_sections_alignment::singleDomainTimeDepthTableTopsUseExactModel`。
2. **导出面诚实性（P1）**：连井剖面 tops 表 TVD 列对 NoSurvey 井留空
   （`.h` 三态契约「不得静默冒充」落到导出面）；坏表井既有留空口径不变。
   `wellsection.cpp`；`tst_wellsection::topsTableTvdColumn`。
3. **时深装载告警（P2）**：workbench 分支 mdTimeDepth 失败 status、主表
   setCheckshots 失败、TVD-only 表（连井剖面 MD 域契约用不上）三类失败
   如实进 warnings——井间地震缝「缺少时深关系」从此有因可查。
   `wellsectionworkflow.cpp`；`tst_wellsection_workflow::timeDepthLoadFailuresWarn`。
4. **地震缝保全（P0 交互）**：`setSection` 在井集「地震身份」（井序+井口
   坐标）不变时不再 clearStrip + 重发 seismicRequested——曲线/分层/岩性/
   模板/去抖刷新不再打穿地震缝（闪断 + 取消在途 512 道重提取）；坐标/
   井序变化照旧清缝重提，与 `setDepthDomain` 的保全承诺同口径。
   `wellsectionpanel.cpp`；`tst_wellsection_ui::setSectionKeepsStripWhenSeismicIdentityUnchanged`。
5. **拾取 undo/redo 画布同步（P2）**：`PickCommandBase::refresh()` 统一刷
   画布叠加——撤销「添加」不再残留幽灵拾取点、撤销「删除」恢复显示。
   `seismicsectiondock_internal.h`；`tst_seismic_sectionui::pickUndoRedoSyncsCanvasOverlay`。
6. **道头卡索引域（P2）**：点击道读道头按「剖面列 → 实际线号 →
   SgyIndex::FindTraceIndex → 文件道号」换算；时间切片/任意线如实给
   「无单一物理道」原因行，不再展示错道数据。
   `seismicsectiondock_bookmark.cpp`；`traceHeaderCardResolvesColumnToPhysicalTrace`。
7. **卷帘与叠加生命周期（P2）**：切时间切片/任意线显式关帘（旧实现 B
   半幅残留相邻线图像被拉伸进新几何）；换线清 zone 叠加、任意线清
   attr+zone 叠加（#224 只修了切片路径 attr）。
   `seismicsectiondockwidget.cpp`；`curtainDisabledOnTimeSliceMode` /
   `overlaysClearedOnLineSwitchAndArbitraryLine`。
8. **时间片方向闸（P2）**：时间切片上「追踪」被拒绝并给出面板原因
   （旧实现按 (Time, 采样号) 伪身份在陈旧垂直剖面追踪，垃圾拾取自动
   落盘）；断层拾取模式在时间切片不起草（旧实现用陈旧 t0/dt 反算伪
   TWT）。`seismicsectiondock_track.cpp` + `seismicsectioncanvas_interact.cpp`；
   `trackingGatedOnTimeSlice`（夹具线号 1..4 与采样号域重叠以暴露旧路径）。
9. **NaN 空档诚实性（P2）**：综合柱状图 `valueAtDepth` 双 NaN 空档返回
   NaN（0 是合法物理量，旧实现把缺失伪造成 0.00 污染道头读数/tooltip）；
   折线对深度 NaN 同样断笔。`wellcompositemodel.cpp` + `trackcurves.cpp`；
   `tst_wellcomposite_framework::valueAtDepthNanGapReturnsNan`。
10. **速度边界对称（P3）**：TimeDepthModel 单参构造器与 isValid() 补上界
    [100,20000)（与 setVelocity/setCalibration 同界）；手改时深 JSON 的
    界外速度按 2500 回落，sectionworkbench 显示口径同步。
    `timedepthmodel.cpp`；`tst_timedepthmodel::velocityBoundsSymmetric`。
11. **工程边界选择复位（P2）**：`EvidenceInteraction::reset()`（工程关闭/
    catalog 重绑）同时清 SelectionContext 的 ids 与 activeHorizon——旧工程
    选择不再污染新工程（撞名高亮错井/保存剖面写旧层位），也解除与
    setSelection 等值短路的复合吞广播。`evidenceinteraction.cpp`；
    `tst_evidenceselection::projectResetClearsSelectionIdsAndHorizon`。
12. **版头菜单 id 锚定（P2）**：右键「从剖面移除」捕获井 id、触发时按 id
    重定位（与 GapItem 菜单同口径）——popup 存续期间 catalog 去抖重建/
    删井后序号前移不再删错井。`wellsectionscene.cpp`；`tst_wellsection_ui`。
13. **格架级联悬空（P3）**：removeUnit 级联删除子体系域时同步清空挂在其
    名下的标志层 unitId——保存后诊断不再报 MarkerDanglingUnit。
    `sequenceframeworkpanel.cpp`；`tst_sequenceframework_ui`。
14. **栅状图落库失败可见（P2）**：fence 落库失败进提示行 + 消息日志，且
    不前进 persisted 快照（下次 rebuild 自动重试）——旧实现静默吞掉编辑、
    重开即丢。`fencewidget.cpp`；`tst_wellsection_ui`（失败注入）。
15. **剖面保存→恢复 E2E（测试深化）**：`tst_sectionlifecycle` 增
    save→clear→restore 往返（恢复回放折线、提取成功、band 重建）与体
    缺失分支（如实拒绝、不半还原）——恢复侧此前零覆盖。

## 域外记录（不修，证据在账本）

- datapreview 井预览 LAS FT 深度不折算（写入域外，建议另行立项）
- SeismicMapLink 测线层死接线 + O(N) 扫描（产品决策）
- 证据选择 writeEntry 写脏 QgsProject（需恢复点存储通道重设计）

## 验证

见账本「验证记录」节（静态门禁 + 关键测试两遍 + 红绿对照）。
