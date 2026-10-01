# goal/ui-experience-polish — UI/UX 全仓打磨进度底账

> 方向：以 DESIGN.md 为唯一权威，把 UI 从「功能正确」打磨到「桌面级专业体验」。
> 版图：全仓 `src/ui/**`（前轮 wave/ux-polish 的 seam 表已由各方向清偿，本轮收剩余面）。
> 分支：`polish/ui-experience`。基线：master 2d33c5e，54/54 ui 标签测试绿。

## 0. 豁免编号（矩阵中 E* 引用此处理由）

| # | 豁免项 | 理由 |
|---|--------|------|
| E1 | QMessageBox/QInputDialog/QFileDialog 无统一包装层（全仓 73+23+17 处裸调） | 原生对话框是 Qt 桌面惯例，DESIGN.md 未要求包装；建委托层属跨 28 文件架构改动，收益（弹窗样式统一）已被全局 palette+焦点环覆盖大半。递延提案见 §5。 |
| E2 | 数据符号色字面量（直方图条色/相名色/相关分析笔色/井曲线色/剖面波形色） | DESIGN.md「地图域配色是数据符号，由 QGIS 样式系统管理，不属 UI token」——两主题都不跟随，字面量合规。 |
| E3 | QGIS 原生控件内建键盘行为不再造 | QgsLayerTreeView F2 重命名、QgsLocatorWidget ↑↓Enter、QgsLayoutView 方向键微调/Space 平移为上游能力。 |
| E4 | seismic3d GL 视口 offscreen 渲染断言 | 无 GL 环境渲染为黑是已知（ux.md §7），键盘断言走 fallback/事件直发。 |
| E5 | wellcomposite 柱状图画布纸面白底 | DESIGN.md 决策日志 2026-09-29：地质文档隐喻，仅面板 chrome 跟随主题。 |
| E6 | 同步短加载（感知 <1s）不设 busy 指示 | DESIGN.md Motion：任务 >1s 才显示进度条；属性表/实体面板同步加载在阈值内。 |
| E7 | 静态调色板/常驻内容面板无三态 | LayoutItemPalette 等恒定内容，无空/载/错语义。 |
| E8 | dock 浮动/复位布局持久化 | QMainWindow::saveState/restoreState 属壳级状态管理，涉及会话生命周期设计，记 §5 递延（非本轮 Oracle 维度）。 |

## 1. 设计一致性审计矩阵（面板 × 维度）

图例：**✓** 符合 · **R** 本轮修复 · **E#** 豁免（理由见 §0）。矩阵为本轮验收契约：所有 R 格收尾时必须已修（对应测试/截图引用在 §2/§4）。

| 面板 | 间距 | 字体 | 色彩 | 空态 | 加载态 | 错误文案 | 键盘 | 焦点 | hover | 选中态 |
|------|----|----|----|----|----|----|----|----|----|----|
| 主窗壳（ribbon/状态栏/页签） | ✓ | ✓ | ✓ | ✓ | ✓ | E1 | R(Ctrl+Tab) | ✓ | ✓ | R(全局) |
| DataListPanel（树/表/搜索） | R(密度) | ✓ | R(#E6F0FA) | ✓ | ✓ | E1 | R(Space/Delete) | ✓ | R(全局) | R(收敛) |
| DataPage 命令面板/快捷键表 | ✓ | ✓ | ✓ | ✓ | — | — | R(Ctrl+K 冲突) | ✓ | R(全局) | ✓ |
| EntityPanel | ✓ | ✓ | ✓ | ✓ | E6 | E1 | ✓(Tab 断言) | ✓ | R(全局) | R(全局) |
| DataPreviewTabs（预览壳） | ✓ | ✓ | R | ✓ | ✓ | ✓(+E1) | ✓ | ✓ | R(全局) | R(全局) |
| PreviewMapPage/States | ✓ | ✓ | R(8 处) | ✓ | — | ✓ | ✓ | ✓ | R(全局) | R(全局) |
| PreviewIdentifyPanel | ✓ | ✓ | R(2 处) | R(空表) | E6 | E1 | ✓ | ✓ | R(全局) | R(全局) |
| PreviewTocPanel | ✓ | ✓ | R(3 处) | R(列表) | — | — | ✓ | ✓ | R(全局) | R(全局) |
| PreviewProfilePanel | ✓ | ✓ | R(2 处) | R(空序列) | — | E1 | ✓ | ✓ | R(全局) | R(全局) |
| PreviewHistogramWidget | ✓ | ✓ | R(1 处+E2) | ✓ | — | — | ✓ | ✓ | R(全局) | R(全局) |
| LayerTreePanel | ✓ | ✓ | ✓ | ✓ | E6 | E1 | R(Delete) | ✓ | R(全局) | R(全局) |
| LayerProfileBar | ✓ | ✓ | ✓ | ✓(页面指示) | E6 | E1 | ✓ | ✓ | R(全局) | ✓ |
| HorizonChipBar | ✓ | ✓ | ✓ | ✓(禁用+tooltip) | — | ✓(refused 信号) | R(Space 断言) | ✓ | ✓ | ✓ |
| AttributeTablePanel | ✓ | ✓ | ✓ | ✓ | E6 | E1 | R(导航断言) | ✓ | R(全局) | R(全局) |
| TaskPanel | ✓ | ✓ | ✓ | ✓(行内提示) | ✓(行内进度条) | ✓(行内失败) | R(断言) | ✓ | R(全局) | R(全局) |
| ReleasePanel | ✓ | ✓ | ✓ | ✓(树内指引) | E6 | ✓(状态栏+降级行) | R(断言) | ✓ | R(全局) | R(全局) |
| CorrelationPanel | ✓ | ✓ | E2(笔色) | ✓ | E6 | R(lasLoadError 静默) | R(断言) | ✓ | R(全局) | R(全局) |
| WellCompositePanel 族 | ✓ | ✓ | R(6 处) | ✓(画布自绘) | E6 | ✓(顶栏状态) | ✓(全键盘+Ctrl+G) | ✓ | R(全局) | R(全局) |
| SeismicSectionDock | ✓ | ✓ | R(5 处) | ✓(画布占位) | ✓(进度条) | ✓(标题内嵌)+E1 | ✓ | ✓ | R(全局) | R(全局) |
| SeismicPickPanel | ✓ | ✓ | R(kBtnStyle) | R(静默) | — | E1 | R(断言) | ✓ | R(全局) | R(全局) |
| Seismic3DViewPanel | ✓ | ✓ | R(4 处) | R(无线框占位说明) | ✓(LOD 标签) | R(切片失败不可见) | ✓(E4 断言直发) | ✓ | R(全局) | R(全局) |
| LayoutDesignerShell | ✓ | ✓ | ✓ | ✓(暂无页面) | — | ✓(状态栏) | R(Delete) | ✓ | R(全局) | ✓ |
| LayoutItemPanel/Palette | ✓ | ✓ | ✓ | ✓/E7 | — | — | R(Tab 断言) | ✓ | R(全局) | ✓ |
| ComposePage | ✓ | ✓ | ✓ | ✓ | ✓ | ✓(内联) | ✓ | ✓ | R(全局) | R(全局) |
| ConstraintPage | ✓ | ✓ | ✓ | ✓(thicknessHint) | — | ✓(内联不弹框) | ✓ | ✓ | R(全局) | R(全局) |
| ValidatePage | ✓ | ✓ | ✓ | ✓ | ✓(按钮 busy) | ✓(胶囊) | R(断言) | ✓ | R(全局) | R(全局) |
| MappingWorkbench/Predict/WellPrediction | ✓ | ✓ | ✓ | ✓ | ✓(进度条) | ✓(内联) | ✓ | ✓ | R(全局) | R(全局) |
| WebViewPanel | ✓ | ✓ | ✓ | ✓ | ✓(薄进度条) | ✓(降级面) | ✓ | ✓ | ✓ | ✓ |
| 对话框族（layerproperties/curveconfig/sectionsetup/stratassign/goto） | ✓ | ✓ | R(2 处) | — | — | E1 | ✓ | ✓ | R(全局) | R(curveconfig) |

「R(全局)」= 由本轮新增的 `PaleoTheme::itemViewStyleSheet()`（选中/hover/斑马纹/密度统一）覆盖，非逐面板改。

## 2. 修复清单（R 格落地记录）

进行中——随实现更新。

### 2.1 新基建
- [ ] `PaleoTheme::Density{Comfort,Compact}` + `ui/density` QSettings 持久化 + 面板菜单切换 action。
- [ ] `PaleoTheme::itemViewStyleSheet()`：三类条目视图（QAbstractItemView 派生）统一选中态（palette primary 一致化）/`::item:hover`（surfaceAltRaised）/斑马纹底 token/密度化 padding；随主题与密度活体重算。
- [ ] `tools/check_ui_invariants.py` ctest 钉子：src/ui 无 QPropertyAnimation/QVariantAnimation/QEasingCurve（DESIGN.md Motion 只许即时切换）；setStyleSheet/QSS 串零裸 hex（baseline 防回升机制）。

### 2.2 键盘/a11y
- [ ] Ctrl+K 双注册冲突修复：DESIGN.md 把 Ctrl+K 钉给 QgsLocator；DataPage 命令面板改 Ctrl+Shift+P（同窗口级歧义 → 现状两键全哑，真 bug）。
- [ ] 主窗 Ctrl+Tab / Ctrl+Shift+Tab 循环切换五个工作流页。
- [ ] DataListPanel：Space 勾选（注释声称未实现）补实现；Delete → dataops.removeSoft 软删流。
- [ ] LayerTreePanel：m_removeAction 挂 Key_Delete。
- [ ] LayoutDesignerShell：Delete → QgsLayoutView::deleteSelectedItems()（QAction 挂壳）。
- [ ] 每面板 ≥1 条 keyClick 断言（tst_ui/tst_panels/tst_layertreepanel/tst_taskpanel/tst_attrpanel/tst_releasepanel/tst_chips/tst_correlation/tst_seismic_*/tst_layout*/tst_wellcomposite）。

### 2.3 三态与 token
- [ ] 全部 UI chrome 裸 hex → token（datapreview 族 24 处、seismic 9 处、wellcomposite 6 处、datalist 选中 1 处）。
- [ ] datalist 树 `setAnimated(true)` → false（DESIGN.md 无动画许可）；curveconfigdialog 自写选中 QSS 收敛。
- [ ] SeismicPickPanel/PreviewToc/PreviewProfile/属性表对话框 静默空态补齐。
- [ ] CorrelationPanel lasLoadError 信号接壳层（状态栏可见化，消灭静默失败）。
- [ ] Seismic3D 切片失败从 QgsMessageLog 提升到面板内黄条（复用内存超限条模式）。

## 3. 截图证据（修前/修后）

进行中。落 `docs/progress/ui-polish-shots/{before,after}/*.png`，offscreen `PALEO_UI_CAPTURE` 门控。

| 面板 | 主题 | 差异点 | before | after |
|------|------|--------|--------|-------|
| DataListPanel | dark | 选中态/树动画 | 待 | 待 |
| LayerTreePanel | dark | 空态卡/选中 | 待 | 待 |
| TaskPanel | dark | 选中/hover/密度 | 待 | 待 |
| SeismicPickPanel | dark | 新空态 + 按钮样式 token | 待 | 待 |
| PreviewMapStates | dark | 错误页 token 化 | 待 | 待 |
| ValidatePage | dark | 选中/hover/密度 | 待 | 待 |
| WellCompositePanel | dark | hint token 化 | 待 | 待 |
| CorrelationPanel | dark | lasLoadError 错误态 | 待 | 待 |

## 4. 测试与验收

- [ ] tst_uxtheme 扩充：Density 往返/持久化、itemViewStyleSheet 双主题断言、（新增用例全绿）。
- [ ] 全量 ctest 绿 + `check_layering --strict` 绿。
- [ ] `git diff master` 自 review；ledger 完整；push + `gh pr create`。

## 5. 递延提案（不改 DESIGN.md，只记录）

- 对话框委托层 `PaleoDialogs::`（question/warn/fileOpen/fileSave 薄包装 + 集中可测点）：113 处裸调收敛，估 2 人日，建议下轮。
- dock 布局 saveState/restoreState 会话持久化（E8）。
- 图标资产审计（HiDPI 2x 资源齐全性）需真机 retina 屏验证，offscreen 无法断言——本轮仅核对 QIcon 缺失即文字占位的按钮为零。
