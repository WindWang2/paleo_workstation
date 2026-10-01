# goal/ui-experience-polish — UI/UX 全仓打磨进度底账

> 方向：以 DESIGN.md 为唯一权威，把 UI 从「功能正确」打磨到「桌面级专业体验」。
> 版图：全仓 `src/ui/**`（前轮 wave/ux-polish 的 seam 表已由各方向清偿，本轮收剩余面）。
> 分支：`polish/ui-experience-r2`（工作树为多 goal 会话共享，监护循环会把中间态
> 收割进 master——1b1e5be/e157800 等即收割提交；最终 PR 以本分支为准）。
> 基线：master 2d33c5e，54/54 ui 标签测试绿。收尾：全部闸门绿。

## 0. 豁免编号（矩阵中 E* 引用此处理由）

| # | 豁免项 | 理由 |
|---|--------|------|
| E1 | QMessageBox/QInputDialog/QFileDialog 无统一包装层（全仓 73+23+17 处裸调） | 原生对话框是 Qt 桌面惯例，DESIGN.md 未要求包装；建委托层属跨 28 文件架构改动，收益（弹窗样式统一）已被全局 palette+焦点环覆盖大半。递延提案见 §5。 |
| E2 | 数据符号色字面量（直方图条色/相名色/相关分析笔色/井曲线色/剖面波形色/剖面图表笔色） | DESIGN.md「地图域配色是数据符号，由 QGIS 样式系统管理，不属 UI token」——两主题都不跟随，字面量合规。 |
| E3 | QGIS 原生控件内建键盘行为不再造 | QgsLayerTreeView F2 重命名、QgsLocatorWidget ↑↓Enter、QgsLayoutView 方向键微调/Space 平移为上游能力。 |
| E4 | seismic3d GL 视口 offscreen 渲染断言 | 无 GL 环境渲染为黑是已知（ux.md §7），键盘断言走 fallback/事件直发。 |
| E5 | wellcomposite 柱状图画布纸面白底 | DESIGN.md 决策日志 2026-09-29：地质文档隐喻，仅面板 chrome 跟随主题。 |
| E6 | 同步短加载（感知 <1s）不设 busy 指示 | DESIGN.md Motion：任务 >1s 才显示进度条；属性表/实体面板同步加载在阈值内。 |
| E7 | 静态调色板/常驻内容面板无三态 | LayoutItemPalette 等恒定内容，无空/载/错语义。 |
| E8 | dock 浮动/复位布局持久化 | QMainWindow::saveState/restoreState 属壳级状态管理，涉及会话生命周期设计，记 §5 递延（非本轮 Oracle 维度）。 |

## 1. 设计一致性审计矩阵（面板 × 维度）

图例：**✓** 符合 · **R** 本轮已修复（落地记录见 §2）· **E#** 豁免（理由见 §0）。

| 面板 | 间距 | 字体 | 色彩 | 空态 | 加载态 | 错误文案 | 键盘 | 焦点 | hover | 选中态 |
|------|----|----|----|----|----|----|----|----|----|----|
| 主窗壳（ribbon/状态栏/页签） | ✓ | ✓ | ✓ | ✓ | ✓ | E1 | R(Ctrl+Tab) | ✓ | ✓ | R(全局) |
| DataListPanel（树/表/搜索） | R(密度) | ✓ | R(#E6F0FA) | ✓ | ✓ | E1 | R(Space/Delete) | ✓ | R(全局) | R(收敛) |
| DataPage 命令面板/快捷键表 | ✓ | ✓ | ✓ | ✓ | — | — | R(Ctrl+K→Ctrl+Shift+P) | ✓ | R(全局) | ✓ |
| EntityPanel | ✓ | ✓ | ✓ | ✓ | E6 | E1 | ✓(DataPage Tab 环路) | ✓ | R(全局) | R(全局) |
| DataPreviewTabs（预览壳） | ✓ | ✓ | R(全 token) | ✓ | ✓ | ✓(+E1) | ✓ | ✓ | R(全局) | R(全局) |
| PreviewMapPage/States | ✓ | ✓ | R(8 处) | ✓ | — | ✓ | ✓ | ✓ | R(全局) | R(全局) |
| PreviewIdentifyPanel | ✓ | ✓ | R(2 处) | R(空表指引) | E6 | E1 | ✓ | ✓ | R(全局) | R(全局) |
| PreviewTocPanel | ✓ | ✓ | R(3 处) | R(列表指引) | — | — | ✓ | ✓ | R(全局) | R(全局) |
| PreviewProfilePanel | ✓ | ✓ | R(2 处) | R(空序列指引) | — | E1 | ✓ | ✓ | R(全局) | R(全局) |
| PreviewHistogramWidget | ✓ | ✓ | R(1 处+E2) | ✓ | — | — | ✓ | ✓ | R(全局) | R(全局) |
| LayerTreePanel | ✓ | ✓ | ✓ | ✓ | E6 | E1 | R(Delete) | ✓ | R(全局) | R(全局) |
| LayerProfileBar | ✓ | ✓ | ✓ | ✓(页面指示) | E6 | E1 | ✓ | ✓ | R(全局) | ✓ |
| HorizonChipBar | ✓ | ✓ | ✓ | ✓(禁用+tooltip) | — | ✓(refused 信号) | R(Space 断言) | ✓ | ✓ | ✓ |
| AttributeTablePanel | ✓ | ✓ | ✓ | ✓ | E6 | E1 | R(导航断言) | ✓ | R(全局) | R(全局) |
| TaskPanel | ✓ | ✓ | ✓ | ✓(行内提示) | ✓(行内进度条) | ✓(行内失败) | R(断言) | ✓ | R(全局) | R(全局) |
| ReleasePanel | ✓ | ✓ | ✓ | ✓(树内指引) | E6 | ✓(状态栏+降级行) | R(断言) | ✓ | R(全局) | R(全局) |
| CorrelationPanel | ✓ | ✓ | E2(笔色) | ✓(+R 错误档) | E6 | R(lasLoadError 接壳) | R(断言) | ✓ | R(全局) | R(全局) |
| WellCompositePanel 族 | ✓ | ✓ | R(6 处) | ✓(画布自绘) | E6 | ✓(顶栏状态) | ✓(全键盘+Ctrl+G) | ✓ | R(全局) | R(全局) |
| SeismicSectionDock | ✓ | ✓ | R(5 处) | ✓(画布占位) | ✓(进度条) | ✓(标题内嵌)+E1 | R(断言) | ✓ | R(全局) | R(全局) |
| SeismicPickPanel | ✓ | ✓ | R(kBtnStyle) | R(空拾取指引) | — | E1 | R(断言) | ✓ | R(全局) | R(全局) |
| Seismic3DViewPanel | ✓ | ✓ | R(4 处) | R(空体画布占位已有+R 失败行内告警) | ✓(LOD 标签) | R(切片失败可见化) | ✓(E4) | ✓ | R(全局) | R(全局) |
| LayoutDesignerShell | ✓ | ✓ | ✓ | ✓(暂无页面) | — | ✓(状态栏) | R(Delete) | ✓ | R(全局) | ✓ |
| LayoutItemPanel/Palette | ✓ | ✓ | ✓ | ✓/E7 | — | — | R(Tab 断言) | ✓ | R(全局) | ✓ |
| ComposePage | ✓ | ✓ | ✓ | ✓ | ✓ | ✓(内联) | ✓ | ✓ | R(全局) | R(全局) |
| ConstraintPage | ✓ | ✓ | ✓ | ✓(thicknessHint) | — | ✓(内联不弹框) | ✓ | ✓ | R(全局) | R(全局) |
| ValidatePage | ✓ | ✓ | ✓ | ✓ | ✓(按钮 busy) | ✓(胶囊) | R(断言) | ✓ | R(全局) | R(全局) |
| MappingWorkbench/Predict/WellPrediction | ✓ | ✓ | ✓ | ✓ | ✓(进度条) | ✓(内联) | ✓ | ✓ | R(全局) | R(全局) |
| WebViewPanel | ✓ | ✓ | ✓ | ✓ | ✓(薄进度条) | ✓(降级面) | ✓ | ✓ | ✓ | ✓ |
| 对话框族（layerproperties/curveconfig/sectionsetup/stratassign/goto） | ✓ | ✓ | R(2 处) | — | — | E1 | ✓ | ✓ | R(全局) | R(curveconfig) |

「R(全局)」= `PaleoTheme::itemViewStyleSheet()`（壳级统一选中/hover/斑马纹/密度 padding）覆盖。矩阵全部非豁免格已修（本轮 six commit，见 §2）。

## 2. 修复清单（已全部落地）

### 2.1 新基建 ✓
- `PaleoTheme::Density{Comfort,Compact}`：`ui/density` QSettings 持久化、面板菜单「紧凑密度」
  checkable action（`densityToggleAction`）、树/列表 padding 档（3→1px）+ 表缺省行高档
  （26→20px，`applyDensityToViewTree` 补扫）随密度/主题活体重算。
- `PaleoTheme::itemViewStyleSheet()`：三类条目视图统一——选中 = primary 底 + onPrimary 字
  （palette 同款，QGIS 惯例）；`::item:hover` = surfaceAltRaised（此前全仓零 hover）；
  斑马纹底 token；树/列表 padding 密度档。收敛 datalist `#E6F0FA` 与 curveconfig
  surfaceAltRaised 两个自写变体（三分叉 → 一）。
- `tools/check_ui_invariants.py` + 3 ctest 项（`ui_invariants{,_strict,_selftest}`）：
  src/ui 零 QPropertyAnimation/QVariantAnimation/QEasingCurve/setAnimated(true) +
  setStyleSheet 零裸 hex。**零 baseline**（day-one 全绿，strict 防回升）。
- `PaleoTheme::sectionTitleStyleSheet()` 新出口（font-weight 600 + text 色，收敛 datapreview 六处字面量）。

### 2.2 键盘/a11y ✓（含快捷键清单化）
修复与新增：
- **Ctrl+K 双注册冲突**（真 bug：主窗定位器与 DataPage 命令面板同键同 WindowShortcut
  上下文，Qt 歧义消解下两键全哑）→ 命令面板改 **Ctrl+Shift+P**；DESIGN.md 把 Ctrl+K
  钉给 QgsLocator。
- **Ctrl+Tab / Ctrl+Shift+Tab** 循环五个工作流页（含末→首环绕）。
- **DataListPanel**：Space 切换当前行选中态（D6.4 注释声称未实现，本轮补实现）；
  Delete → `batchRemoveSoft` 软删流（确认模态 + 撤销）；图标/分组视图同语义。
- **LayerTreePanel**：`m_removeAction` 挂 Key_Delete（QGIS 默认动作语义：摘树节点）。
- **LayoutDesignerShell**：Delete → `QgsLayoutView::deleteSelectedItems()`。

收尾后快捷键全集（src/ui 扫描，QShortcut + QAction::setShortcut）：

| 键 | 动作 | 位置 |
|----|------|------|
| Ctrl+1..5 | 直切五个工作流页 | paleomainwindow.cpp（W5） |
| Ctrl+Tab / Ctrl+Shift+Tab | 工作流页循环（本轮新增） | paleomainwindow.cpp |
| Ctrl+K | 聚焦定位器（DESIGN.md 钉死） | paleomainwindow_attach.cpp |
| Ctrl+Shift+P | 数据页命令面板（本轮改键） | pages/datapage.cpp |
| Ctrl+S | 保存工程（QAction） | paleomainwindow_attach.cpp |
| Ctrl+Z/Y | 数据面板撤销/重做；版面撤销/重做（分窗口上下文不冲突） | datalist / layoutundostack |
| Ctrl+Shift+A/F | 反选 / 按过滤选中 | datalist |
| Ctrl+F | 聚焦资产搜索 | datalist |
| Ctrl+Alt+V | Vim 风导航开关 | datalist |
| F2 | 实体重命名意图 | datalist |
| ? | 快捷键表 | datalist → DataPage |
| Ctrl+G | 井综合跳深度对话框 | wellcompositepanel |
| Space | 数据资产行选中切换（本轮）；chip 切换；QGIS 上游（locator/视图内建） | datalist / chips / E3 |
| Delete | 数据资产软删 / 图层树删组或层 / 版面删项（本轮三处） | datalist / layertreepanel / layoutdesignershell |
| Esc | 捕获中止/工具停用/拖拽取消（edittools/maptools/画布族既有） | 既有 |
| ↑↓/Enter | 列表导航/激活（Qt 内建 + 命令面板） | 各列表 |

每面板 ≥1 条 keyClick 断言（11 个测试文件新增）：
tst_ui（Ctrl+1..5/Ctrl+Tab 环绕/密度往返/Ctrl+Shift+P 键序）、tst_panels（Space 双向/
Delete 软删模态流/命令面板改键）、tst_layertreepanel（Delete 摘树节点）、tst_taskpanel/
tst_releasepanel（列表 ↑↓）、tst_attrpanel（QgsAttributeTableView ↑↓）、tst_chips
（Space 切层位）、tst_correlation（曲线浏览器树导航）、tst_seismic_sectionui（拾取表
导航安全网 + 空态可见）、tst_wellcomposite（Ctrl+G 模态可达）、tst_layoutdesigner_full
（Delete 删 layout 项）、tst_layoutitempanel（Tab 焦点链）。

### 2.3 三态与 token ✓
- UI chrome 裸 hex 全部清偿为 token 活体注册：datapreview 族 24 处、seismicsection 5 处、
  seismic3d 4 处、wellcomposite 6 处、datalist 树选中 1 处、鹰眼小图 1 处（暗色下硬编码
  浅色的可读性崩坏随之消灭——证据见 §3 previewmap_error_dark）。
- datalist 树展开动画移除（DESIGN.md Motion 只许即时切换）；curveconfig 自写选中 QSS 收敛。
- 静默面板补空态：SeismicPickPanel（pickEmptyHint）、PreviewTocPanel（共享
  PaleoEmptyStateLabel 第二消费方）、PreviewProfilePanel（空序列画面心指引）、
  PreviewAttributeTableDialog（过滤空结果指引）。
- **CorrelationPanel lasLoadError 静默失败修复**：信号此前全仓无消费者——壳层接状态栏 +
  MessageLog；面板空载时切错误档空态（errorState 属性，setWells 复位）。
- Seismic3D 切片失败从 QgsMessageLog 独占提升为面板内 warning 行（showInlineWarning）。

### 2.4 动效 ✓
全仓 src/ui 零 QPropertyAnimation/QVariantAnimation/QEasingCurve（唯一违例 datalist
`setAnimated(true)` 已移除）；`ui_invariants_strict` ctest 钉住防回升（DESIGN.md Motion：
「Qt Widgets 以即时切换为主；无编排动画」——无需项目级 reduced-motion 开关）。

## 3. 截图证据（修前/修后）

`docs/progress/ui-polish-shots/{before,after}/`（同一套 `uipolish_capture.h` 钩子、
`PALEO_UI_CAPTURE` 门控、offscreen 双主题）。暗色档为主要证据面（token 化前硬编码浅色
在暗色下可读性崩坏）。像素差异 = ImageChops 差异 >30/通道像素占比：

| 面板 | 主题 | 差异点 | dark 差异 |
|------|------|--------|-----------|
| PreviewMapPage 错误页 | dark | 错误框/文案 token 化（#FDEBEB 残留清零） | **13.1%** |
| DataPage（数据列表树/表） | dark | 树选中 QSS 收敛 + chrome token | **3.6%** |
| SeismicSectionDock（含拾取面板） | dark | 显示条按钮/标签 token + 新空态 | **4.2%** |
| WellCompositePanel | dark | 顶栏按钮/读数/提示 token（画布纸面白底 E5 不变） | **1.2%** |
| attrpanel/correlation/chips/layertree/taskpanel/validatepage | dark | 0% —— 修复是行为态（键盘/空态触发型）或该面板本已 token 合规（与 §1 矩阵 ✓ 格一致）；键盘与空态由测试断言钉住 | 0% |

## 4. 测试与验收

- tst_uxtheme 扩充 +3 用例（densityRoundTripAndLiveRelayout /
  itemViewSheetUnifiesSelectionHoverZebra / densityAppliesRowHeightsToTables），22/22 绿。
- ui 标签全量 55/55 绿（基线 54 → 55 为并行会话新增 tst_seismic_3dviz）。
- `check_layering --strict` 绿；`check_ui_invariants --strict` 绿（零 baseline）。
- 已知环境项：tst_correlation_full 的 50 井渲染预算门（<3s）在共享机高负载
  （load>30，多会话并行编译）假红——单独跑通过（与 ux.md §9 记录一致，非回归）。

## 5. 递延提案（不改 DESIGN.md，只记录）

- 对话框委托层 `PaleoDialogs::`（question/warn/fileOpen/fileSave 薄包装 + 集中可测点）：
  113 处裸调收敛，估 2 人日，建议下轮。
- dock 布局 saveState/restoreState 会话持久化（E8）。
- token 缺口提案：surfaceAlt 底上的 hover/pressed 中性档（本轮用 border 档代替，
  见 previewmappage 工具条注释）；#E8F0FE 蓝染选中底 → 统一 primary 描边 + surfaceAltRaised 范式。
- 图标资产审计（HiDPI 2x 资源齐全性）需真机 retina 屏验证，offscreen 无法断言——
  本轮仅核对 QIcon 缺失即文字占位的按钮为零。
