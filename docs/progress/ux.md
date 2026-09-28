# wave/ux-polish — UX 方向进度与移交报告

> 版图：src/ui 根级文件、{correlation,wellcomposite,layout,locator,decorations}/、dialogs/（folderconfirm 除外）、
> pages/{datapage,datalist,entitypanel}.* 与 {pageshared.h,pagepanels.h}、DESIGN.md、resources/、translations/、
> 相关 tests。TODOS.md 禁改，本文是该方向的进度底账。
> 分支：`wave/ux-polish`（worktree ../pw-ux）。

## 1. 暗色模式翻案（已落地）

DESIGN.md 决策日志 2026-09-29 翻案条：缺省浅色，用户显式切换深色（视图菜单「深色模式」），
QSettings `paleo/paleo → ui/theme`（唯一写者 = 用户切换动作；测试经 `QSettings::setPath` 隔离，不产生新写者）。

实现链路：

- `PaleoTheme::Theme{Light,Dark}` + `ThemeTokens` 双值全集（`src/ui/paleotheme.{h,cpp}`）；
  QSS 出口（focusRing/capsule/shell/mutedCaption/ribbonPalette/ribbonStyle）全部按主题参数化，缺省实参取 `currentTheme()`。
- 换主题 = `applyTheme()`（palette + Fusion + 字体）+ `ThemedStyleSheetRelay::reapplyAll()`——
  实测 Qt6 `setPalette` 不给隐藏子控件发 `ApplicationPaletteChange`（见 /tmp 复现结论），活体样式由 applyTheme 显式直调重算，
  不依赖事件广播。`applyThemedStyleSheet(widget, builder)` 一行注册、随主题自动重算、widget 销毁自动出表。
- SARibbon：`ribbonPaletteJson(Dark)`（isDark=true + 暗色阶；white/black 不翻转，对齐官方 office2021-dark 参考）；
  `PaleoRibbon::applyTheme` 改整体替换（不再追加），主题来回切换不累积。
- 图标：`PaleoIcons::qgisTheme()` 暗色下逐像素提亮（仅 alpha>0 像素，RGB += lift，alpha 不动——
  QPainter Plus 合成会把透明区叠成不透明底，不可用，见 tst_uxtheme::iconsRetintForDarkTheme）；
  自绘图标墨色随主题翻。已构造图标不回填（新取的带新色），完整刷新随重启。
- 壳接线：`PaleoMainWindow` 构造按设置钉主题；面板菜单（右键 dock 标题栏 / 右上「面板」钮）尾附
  checkable「深色模式」（objectName `themeToggleAction`）→ `setDarkThemeEnabled`。
- 版图内 chrome token 化（浅色输出与旧字面量逐字节一致，tst_panels/tst_datapreview 断言保持绿）：
  paleomainwindow（壳 QSS/crsLabel）、attach（crsNote）、webviewpanel、correlationpanel + correlation/*
  （pen/brush 绘制时现取 token）、layout/layoutitempalette、pages/{datalist,entitypanel}、pageshared caption。
- **wellcomposite 柱状图画布保持纸面白底**（决策日志同条）：柱状图是地质文档隐喻（井位legend/图例头/道头全部
  按打印惯例设计），仅面板顶栏/曲线配置对话框 chrome 跟随主题；道内地质配色（相带/年代色带/岩性图案）是
  数据符号色（DESIGN.md:93），两主题都不动。

对比度（WCAG，程序化验证）：暗 text/surface 11.6:1、muted/surface 6.4:1、primaryText/surface 5.5:1、
focusRing/surfaceAlt 6.3:1、success 胶囊 5.3:1、warning 7.7:1、error 5.2:1（#F76A61，首版 #EF5350 只有
4.39:1 已调）；白字/primary 填充 4.76:1。全部 ≥AA。

测试：tst_uxtheme 新增 8 个用例（darkPalette/darkTokens/defaultArgs/relay/icons/emptyState/settings/双主题截图），
19/19 绿；`PALEO_UI_CAPTURE=<dir> ./tst_uxtheme` 落 uxtheme_{light,dark}.png。

## 2. Seam 表（他方向版图硬编码浅色样式——合并时由对应方向消化）

| file:line | 现状 | 所需改动 | 归属 |
|---|---|---|---|
| src/ui/horizonchipbar.cpp:13-17 | kChipStyle 白底/`#DFE5EC` 边/`#5D6E80` 字；checked `#1B73D0`/白字 | 底/边/字换 `PaleoTheme::tokens()`（checked 蓝保留）；建议 `applyThemedStyleSheet` | 编图(P2) |
| src/ui/layers/layertreepanel.cpp:83 | `LayerTreePanel { background: #FFFFFF; }` | background → tokens().surface | 编图(P2) |
| src/ui/layers/layertreepanel.cpp:108-110 | 空态卡片复制版（`rgba(255,255,255,0.9)` 底） | **迁到共享组件 `PaleoEmptyStateLabel`**（src/ui/paleoemptystate.h，本方向已建）；tst_layertreepanel:85/100 依赖 objectName `layerTreeEmptyState`——迁移时保留 objectName 或同步改断言 | 编图(P2) |
| src/ui/layers/layerprofilebar.cpp:103 | `color: #5D6E80` | `PaleoTheme::mutedCaptionStyleSheet()` + 活体注册 | 编图(P2) |
| src/ui/pages/panelshared.h:41-52 | CollapsibleSection 折叠钮 `#24303E`/`#EDF1F5`/`#DFE5EC`/hover `#E2E8F0` | 同 entitypanel.cpp 已做的 token 化模板（本方向 entitypanel 的同款复制版已迁移，可对照） | 编图(P2) |
| src/ui/pages/panelshared.h:86 | caption `color: #5D6E80` | 同 pageshared.h 的 mutedCaptionStyleSheet 方案 | 编图(P2) |
| src/ui/pages/constraintpage.cpp:317 | thHint `#5D6E80` | mutedCaptionStyleSheet | 编图(P2) |
| src/ui/pages/validatepage.cpp:294/301 | `#24303E`/`#5D6E80` | text/textMuted token | 编图(P2) |
| src/ui/datapreview/**（相名色/预览壳） | datapreviewtabs 等含浅色样式与相名色 | 相名色=数据符号色不动；壳样式 token 化待 datapreview 方向 | 数据方向 |

**属性键漂移隐患**：`src/ui/pages/pageshared.h:16-17`（namespace `paleo::pagesinternal`）与
`src/ui/pages/panelshared.h:20-22`（namespace `PaleoPanel`）各持一份字面量完全相同的
`"paleo.page.layers"`/`"paleo.page.wf"`（后者多一个 `kTaskSvcProp`）。两份各自编译能过纯属字面量巧合；
单边改键会静默断链（panelshared 侧 4 个页在用，pageshared 侧 datalist/entitypanel/datapage/validatepage 在用）。
本方向未单方面改 panelshared.h（他方向版图）；建议合并后收敛为单一定义（pageshared.h 已是壳侧聚合头，
panelshared.h 改 include 它即可，`paleo.page.tasksvc` 一并搬）。

## 3. i18n（tr 覆盖 + lupdate 骨架）

- 源语言口径 = 中文（DESIGN.md 决策日志 2026-09-29）；layoutdesignershell.cpp 的 22 条英文源串已改写为中文源串
  （映射见 diff；无测试断言英文文案，已核实）。
- 本轮 tr 缺口清零（豁免口径内）：paleomainwindow（启动页/页签/dock 标题/状态栏 ≈41 处；kPageLabels/kPageDockTitles
  改首调构建的函数——命名空间级常量会在 main 前静态初始化漏翻译）、attach 7 处、wellcompositepanel ≈34 处、
  curveconfigdialog ≈21 处、wellcompositetrack 11+10 处（WellTrack 非 QObject，走
  `QCoreApplication::translate("WellCompositeTrack",…)`）、canvas 2、legend 7、vertexeditorshim 表头、
  水印默认文案（装饰类非 QObject，默认值移到 PaleoDecorationManager 构造）。
- 豁免核对：相名词表/岩性词表/层位词表/相名色带数据、objectName、`paleo.page.*` 属性键、比例值（1:200）、
  单位（m/ft）、"LOG"/" · " 助记符均不包 tr——与 lupdate 输出一致。
- lupdate：`tools/update_translations.sh`（PATH+Qt 专有路径双探测）+ `translations/paleo_zh_CN.ts`
  （骨架 1611 条源串）；CMake 侧 `cmake/extra-ux.cmake` 挂 `ninja paleo_translations` 手动 target，
  不进默认构建，无 LinguistTools 不断构建。
- 已知小尾巴：lupdate 报 seismicsectiondockwidget.cpp:550 命名空间限定警告（地震方向，先于本方向存在）。

## 4. a11y 收口

新增 accessibleName 17 处（全部 tr）：比例尺下拉（wellcomposite）、曲线配置树、顶点坐标表、井位图例表、
展开/折叠树按钮（datalist）、发布名称/备注/对比 A/B/差异列表（releasepanel）、任务列表（taskpanel）、
底部面板页签（mainwindow）、页码 spin（layoutdesigner）。

键盘/焦点：

- 焦点环 = 2px focus-ring token（浅 `#1B73D0` / 暗 `#5FA5F0`），全局 QSS 覆盖可聚焦控件，替代 Fusion 虚线框。
- QLineEdit 审计结论：版图内全部「placeholder 或 accName 至少其一」齐备，无双缺。
- 焦点陷阱审计：无模态循环新引入；WebViewPanel 降级面按钮可 tab 退出。

快捷键表（现有全集，QShortcut + keyPressEvent 扫描）：

| 键 | 动作 | 位置 |
|---|---|---|
| Ctrl+K | 聚焦定位器（搜索井位/层位） | paleomainwindow_attach.cpp:1075 |
| Ctrl+S | 保存工程 | paleomainwindow_attach.cpp:1110 |
| Ctrl+Z / Ctrl+Y | 版面撤销/重做 | layout/layoutundostack.cpp:44-45 |
| Esc | 结束捕获/中止编辑（editAborted） | edittools 全系 keyPressEvent |
| Esc | 停用活动地图工具（deactivateTool 同语义） | 画布工具链 |
| ↑/↓/Enter | locator 结果导航/触发 | QgsLocatorWidget 原生（上游） |

## 5. 空态/错误态/降级态三态规范（已落地）

共享组件 `src/ui/paleoemptystate.{h,cpp}`（原 paleomainwindow.cpp 匿名类收敛）：

- 三态：Empty（text-muted 字）/ Error（error 字）/ Degraded（warning 字）；objectName
  `emptyStateCard`/`emptyStateCardError`/`emptyStateCardDegraded` 供测试与辅助技术分辨。
- 卡片 = surface 90% 不透明底 + border 描边 + 8px 圆角（DESIGN.md 卡片档）；样式活体注册随主题重算；
  宿主 resize 保持居中；文案永远带下一步动作指引。
- 消费方：主窗地图空态（objectName 仍为 `mapEmptyState`，tst_ui 依赖）。layertreepanel 复制版迁移见 seam 表。
- 表格空态（datalist refreshAssetEmptyState）与 WebViewPanel 降级面语义不变，样式已 token 化。

## 6. locator / webview polish

- locator（paleolocatorfilters）：结果按 filter 分节（`QgsLocatorResult::group=displayName()`、groupScore）；
  打分（精确 1.0 > 前缀 0.9 > 包含 0.6）；井位结果的 name 字段进 description 副行。键盘 ↑↓/Enter 导航
  为 QgsLocatorWidget 原生能力（上游维护），无需自研。IssueLocatorFilter 维持未注册（无 issue 存储，
  导航走验证页 issueTable → ThreeWayLocator，attach:1068 注释）。
- webview（webviewpanel）：三条降级路径（无效 URL / 无 WebEngine / 无屏平台）+ 渲染进程终止回收，
  全部落「内嵌浏览器不可用：%1」降级面 + `loadFailed` 信号，宿主不拖死；状态文字色统一走
  PaleoTheme muted token（活体）。offscreen 下走降级面（tst_webviewpanel 断言 fallback 语义）。

## 7. 视觉审计（offscreen 截图循环）

黑白名单（事实校正节口径）：

- **预期黑**：seismic3d GL 视口（offscreen 无 GL，归地震方向）。
- **预期降级面**：WebViewPanel（可审，降级文案 + 外开按钮）。
- 截图先例：tst_ui:788 `PALEO_UI_CAPTURE`、tst_uxtheme::pinRenderEnvironment、
  tst_wellcomposite 三处个人绝对路径写盘已清（`captureIfAsked`，PALEO_UI_CAPTURE 门控）。

双主题对照（`PALEO_UI_CAPTURE=/tmp/ux_audit ./tst_uxtheme`）：

- 浅色 uxtheme_light.png：背景 84% = #FFFFFF（surface token 精确命中）。
- 暗色 uxtheme_dark.png：背景 84% = #252C36（surface token 精确命中），纯白残留 0%。
- 字阶/间距断言走 tst_uxtheme 既有 typography 用例（kDisplayPt…kMonoPt）。

## 8. 简化版 composer 评估（不写实现——TODOS 递延纪律）

现状底座（已在 master）：`QgisLayoutService::exportPdf` 从模板建 QgsPrintLayout（qgislayoutservice.cpp:62-68）；
z1 版面设计器壳（layoutdesignershell：页面导航/缩放/标尺/模板面板/属性对话框）；
`PaleoLayoutExportActions`（DPI/页码范围导出动作）；A4/A0 模板在 docs/templates/。

最小可用路径（建议，估 3–4 人日）：

1. 「快速出图」对话框（compose 页 ribbon 入口）：模板选择（docs/templates 三张 .qpt）+
   图件标题 chip 文本 + 装饰开关（指北针/比例尺/图例——PaleoDecorationManager 同语义映射到 layout item）+ 输出路径。
   预览 = `QgsLayout::renderPage` 到 QLabel 缩略（offscreen 可测）。约 1.5 日。
2. 复用 `QgisLayoutService`（模板 → layout → exportPdf）；PNG 出口补一个 `exportImage`（QgsLayoutExporter
   同一导出器，~20 行）。约 0.5 日。
3. 版面地图项钉页面档案（pinLayoutTheme 已有）保证导出内容与画布一致；标题/装饰字段写进 layout item。约 1 日。
4. 测试：tst_layoutexport 增「快速出图」端到端（模板→PDF 字节非空+页数断言）。约 0.5–1 日。

不需要：完整 designer UI（z1 已有）、拖拽编辑、多页编排——「简化版」的边界就是模板驱动 + 字段填充 + 一键导出。
完整评估结论：**值得做，全部构件已存在，无新架构**；递延到 TODOS 对应项由编图方向排期。

## 9. 测试与验收

- 新增/扩展：tst_uxtheme（+8 暗色用例，19/19）；tst_wellcomposite 绝对路径清理（13/13）。
- 版图内测试全绿：tst_ui 23、tst_panels 40、tst_correlation 15、tst_correlation_full 16+11+14+17+13+15、
  tst_layoutshell 9、tst_layoutdesigner_full 11、tst_layoutundo 14、tst_layoutpalette 13、
  tst_layoutitempanel 11、tst_layoutexport 10、tst_webviewpanel 8、tst_locator 7、tst_decorations 10、
  tst_wellcomposite 13、tst_datapreview 31、tst_entityview 12、tst_attrpanel 4、tst_edittools 56。
- `python3 tools/check_layering.py` 绿（新文件 paleoemptystate.* 带 `// 层：视图` 头三行）。
- 已知环境项：tst_correlation_full 的 100k 采样渲染计时门（<300ms）与 50 井构建渲染门（<3s）在共享机
  高负载（load>28，多方向并行编译）下会假红——基线在低负载时通过（本轮 893ms/2.1s 实测），
  归属环境而非回归（与 wave4-runtime-resilience 记录一致）。
