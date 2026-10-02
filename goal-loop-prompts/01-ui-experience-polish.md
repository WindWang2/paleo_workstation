# Goal Loop — goal/ui-experience-polish：UI/UX 全仓深化打磨

你接手一个自治迭代循环。方向：**把工作站 UI 从「功能正确」打磨到「桌面级专业体验」**——
以 `DESIGN.md` 为唯一权威，系统性消灭不一致、补齐交互细节。这是深度迭代任务，
预期多轮勘察→实现→验证循环，预算与收尾标准见文末。

## 背景事实

- 仓库 `/home/kevin/projects/paleo_workstation`。`DESIGN.md` 是设计系统真源（字体/色彩/
  间距/美学方向），AGENTS.md 明令「视觉决策先读 DESIGN.md，QA 模式标记偏离项」。
- UI 层 `src/ui/**`：paleomainwindow 壳 + dock 面板群（DataListPanel/DataOps、层树、
  属性、预览图、剖面、wellcomposite、layout designer、chips/工具条等 50+ 测试名
  对得上名），主题 `PaleoTheme::applyLightTheme()`（浅色钉死 + vendor 字体注册）。
- 真机实跑过：窗口渲染正常、真工区 20 井/层位/966MB SEG-Y 上图、WebEngine dock 在位。
- 现有测试：54 个 ui 标签测试（tst_ui/tst_panels/tst_*panel/tst_*tools…）+ ux/theme
  专项（tst_uxtheme）；`QT_QPA_PLATFORM=offscreen` 可跑全量。
- 已知粗糙面（勘察起点，非穷举）：
  - 各面板空态/加载态/错误态口径不一（部分静默空表、部分状态栏文案）；
  - 键盘可达性不全：焦点链、Esc/Enter/Delete 在各 panel 行为不一致；
  - 模态对话框驱动存在时序脆性（tst_panels 并行红先例：singleShot(0) 抢模态就绪）；
  - dock 尺寸/浮动/复位无持久化或恢复路径不统一；
  - 高密度信息面板缺少密度切换（comfort/compact）；
  - 图标混用（部分文本占位）；HiDPI 缩放下控件间距未审计；
  - 滚动条/选中态/hover 反馈在三类列表控件（QTableWidget/QListView/QTreeView）间不一致。

## Oracle（验收条件）

1. **设计一致性审计表**落地 `docs/progress/ui-polish.md`：面板×维度矩阵（间距/字体/
   色彩/空态/加载态/错误文案/键盘/焦点/hover/选中态），每格给「符合|修复|豁免+理由」；
   全部非豁免格修完。
2. 新基建按 DESIGN.md 落地其一至多：`UiDensity`（comfort/compact 密度切换持久化）、
   统一 `emptyStateWidget` 组件、统一 busy/错误 overlay 语义（信号驱动，视图不干活——
   面板发信号、服务干活的分层不破）。
3. 键盘/a11y：主导航 Ctrl+Tab/面板切换/Delete/Esc 清单化，每面板至少一条 ctest 断言
   （offscreen `QTest::keyClick` 可验）。
4. 动效克制落地：QPropertyAnimation/QEasingCurve 只允许 DESIGN.md 许可场合
   （如无许可则不加动效，审计删除已有任意动效）。`tst_uxtheme` 家族扩充通过。
5. 截图证据：offscreen `grabWindow()`/真机截图对比至少 6 个主要面板修前/修后入 ledger。
6. `ctest` 全绿（含你新增的 UI 断言测试），`check_layering --strict` 绿，
   `git diff master` 自 review 干净；ledger 完整；push + `gh pr create`。

## 勘察指引

- `src/ui/paleotheme.*`：主题/palette/字体入口；`src/ui/paleomainwindow.cpp`：壳装配。
- 测试样板：`tests/tst_uxtheme.cpp`、`tst_panels.cpp`（driveModalNextTick 模态驱动器
  可复用修时序脆性）、`tst_layoutdesigner_full.cpp`。
- 审计抓手：`grep -rn "setStyleSheet" src/ui` 找散落内联样式（应收敛进主题 token）；
  `grep -rn "QInputDialog\|QMessageBox\|QFileDialog" src/ui` 盘点原生对话框裸露面
  （统一委托/包装层是否缺失）。
- 视图层只允许 `io/lasdoc.h` + 四个 metadata 头白名单——修 UI 时顺手核对面板
  include 是否越界（越界即违规要收）。

## 禁区

- 不改 DESIGN.md 本身（色板/字体/间距 token 不可擅改；发现 token 缺失先在 ledger
  提案，注释标记 TODO 可，实现按现有 token 走）。
- 不在视图层写业务逻辑（信号-only 红线）；不新增 `ui/` 之外的 include 违规。
- 不重排主窗口信息架构（dock 拓扑不变）——本轮是打磨不是重构。
- 性能项另有人管：本方向若发现 UI 线程同步阻塞 IO，只记录不动手（移交 perf 方向）。

## 迭代协议

- 轮0：全仓勘察 + 审计表骨架（可并行调查子任务刷面板清单）。
- 中段轮次：按面板簇推进（数据页簇/层树簇/剖面簇/编图簇/井综合簇），每簇：
  审计 → 修复 → 截图 → 测试 → ledger。
- 每轮至少一个可验证产出（构建绿+新增断言过）。
- 完成定义 = Oracle 6 条全绿。禁止「差不多了」收尾。
