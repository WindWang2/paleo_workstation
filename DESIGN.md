---
# gstack: design-md-format=spec
name: Paleo Workbench
description: QGIS-native 专业工具（浅色缺省/深色可选双主题）— 工作流即产品；密度与秩序感承载专业性，无装饰。
colors:
  primary: "#1B73D0"          # 工作流当前步 / 主按钮 / 选中 chip —— 仅此三色用途
  on-primary: "#FFFFFF"
  primary-hover: "#1565B8"
  surface: "#FFFFFF"          # 面板、卡片、dock 内容区
  surface-alt: "#EDF1F5"      # 窗口底色、标题栏
  border: "#DFE5EC"           # 面板分割、卡片描边
  text: "#24303E"
  text-muted: "#5D6E80"          # 深于原型灰 —— 在 surface-alt 上亦满足 4.5:1 对比度
  text-disabled: "#9AA7B4"       # 禁用文本/图标（非活跃组件，WCAG 豁免）
  focus-ring: "#1B73D0"          # = primary；键盘焦点 2px 描边，不用 Fusion 虚线框
  placeholder: "#5D6E80"         # = text-muted；输入占位与次级说明
  accent: "#1B73D0"           # = primary；交互色不做装饰用
  success: "#43A047"          # 验证通过
  warning: "#F29900"          # 待复核
  error: "#E53935"            # 不一致 / 阻断错误
typography:
  display:
    fontFamily: Noto Sans SC
    fontWeight: 500
    fontSize: 15pt           # 图件标题 chip、页级标题
    letterSpacing: 0
  body:
    fontFamily: Noto Sans SC
    fontSize: 9pt            # Qt pointSize —— 跟随 OS DPI 缩放
    lineHeight: 1.5
  label:
    fontFamily: Noto Sans SC
    fontSize: 8pt            # ribbon 组名、图例、表格次级信息
    letterSpacing: 0.02em
  mono:
    fontFamily: JetBrains Mono
    fontSize: 9pt
    fontFeature: tnum        # 坐标 / 深度 / TWT 数值列等宽对齐
rounded:
  sm: 4px
  md: 8px
  lg: 12px
  full: 9999px               # 层位 chip 胶囊形
spacing:
  xs: 4px                    # 工具栏按钮间距
  sm: 8px                    # 面板内边距、表单项间距
  md: 16px                   # 折叠组间距
  lg: 24px
  xl: 32px
  2xl: 48px
components:
  workflow-tab:              # 唯一签名元素：编号工作流标签
    activeUnderline: "{colors.primary}"
    activeText: "{colors.primary}"
    inactiveText: "{colors.text-muted}"
  ribbon-button:
    style: icon-over-text    # QToolButton.ToolButtonTextUnderIcon
    hoverBackground: "{colors.surface-alt}"
    primaryText: "{colors.primary}"   # 仅运行类动作
  dock-panel:
    backgroundColor: "{colors.surface}"
    borderColor: "{colors.border}"
  chip:
    backgroundColor: "{colors.surface}"
    borderColor: "{colors.border}"
    textColor: "{colors.text-muted}"
    rounded: "{rounded.full}"
  chip-active:
    backgroundColor: "{colors.primary}"
    textColor: "{colors.on-primary}"
  status-tag:
    successBg: "#E8F5E9"
    warningBg: "#FFF4E0"
    errorBg: "#FDEBEB"
    successText: "#2E7D32"     # 胶囊文字深色变体（浅底上语义原色仅 3.0:1，不足 AA）
    warningText: "#9A5B00"     # 原色 2.1:1 → 深色变体 5.0:1
    errorText: "#C62828"       # 原色 3.7:1 → 深色变体 4.9:1
  dark:                        # 2026-09-29 翻案：深色变体（UI chrome；数据符号色不跟随）
    surface: "#252C36"         # 面板、卡片、dock 内容区
    surface-alt: "#1B212A"     # 窗口底色、标题栏、页签行
    surface-alt-raised: "#2A313B"  # hover/选中浮起面（浅色由 surface-alt 兼任）
    border: "#3B4552"
    text: "#E4EAF2"
    text-muted: "#A3B1BF"
    text-disabled: "#66717E"
    focus-ring: "#5FA5F0"      # 暗色提亮焦点环（= primaryText）
    primary-text: "#5FA5F0"    # 文字位主色（浅色=primary，暗色提亮保对比度）
    primary-hover: "#2F82DD"
    success: "#5CB860"
    warning: "#FFB74D"
    error: "#F76A61"           # 胶囊底上 5.2:1（AA）
    status-tag:
      successBg: "#1F3524"
      warningBg: "#3A2E15"
      errorBg: "#3A1D1D"
      successText: "#5CB860"   # = 语义色（深底上已 AA）
      warningText: "#FFB74D"
      errorText: "#F76A61"
    note: primary/on-primary 填充色两主题同值；placeholder=text-muted
---

# Paleo Workbench

## Overview

**Creative North Star:** 这就是干古地理编图活儿的工具 —— 五步工作流链永远可见，其余一切让位于 QGIS 原生控件的专业质感。
**Product context:** 石油地质学家的古地理编图工作台；Qt6 Widgets 桌面应用（非 QML、非 Web），嵌入 vendored QGIS；五页：数据管理 → 预测编图 → 单因素图 → 智能编图 → 验证（#43 页签文案裁决）。
**Mode per surface:** 全部 Operate（任务操作面）；无 Persuade/Read 面。
**Reference:** `原型/` 五张设计稿 + QGIS 主窗口解剖（docs.qgis.org qgis_gui）。
**Key characteristics:** 编号工作流标签是唯一的非 QGIS 签名元素；浅色为缺省的双主题（暗色见 2026-09-29 翻案条）+ 单一蓝色 accent；密度对齐专业 GIS 工具而非 Web 惯例。

## Colors

**Strategy:** Restrained —— 一个蓝色 + 中性色阶；颜色稀有即有意义。
**Light or dark:** 浅色缺省（原型即浅色，野外办公室强光环境）；深色变体已交付（2026-09-29 翻案，用户明确要求）——UI chrome 全量跟随 `dark` token 块，缺省浅色、仅用户显式切换时写 QSettings；数据符号色（DESIGN.md:93 域配色）与 wellcomposite 柱状图「纸面」画布不跟随（柱状图是地质文档隐喻，2026-09-29 决策）。
- `#1B73D0` 只用于：当前工作流步、主操作按钮、选中态 chip。**禁止**用于装饰性背景——交互色不兼装饰。
- 语义色仅承载状态：绿=通过、橙=待复核、红=不一致/错误，且永远配文字（不单独用色）。
- 地图域配色（相色标、物源线红虚线、展布线蓝虚线）是**数据符号**，由 QGIS 样式系统管理，不属 UI token。

## Typography

- **UI/正文/标签:** Noto Sans SC（vendor 打包，保证跨平台一致 + §33 渲染回归测试稳定）；Windows 退化 Microsoft YaHei UI，Linux 退化 Noto Sans CJK SC。
- **数值/坐标/深度:** JetBrains Mono（vendor），tnum 等宽数字。
- **字阶（Qt pointSize）:** 正文 9pt 基准；标题 12pt；图件标题 15pt；次级标签 8pt。**有意偏离** Web 的 16px 规则：这是 Operate 面的专业工具密度，与 QGIS 本体一致；用 pointSize 而非 pixelSize 以跟随系统缩放。
- 全部用户可见字符串走 `tr()`/翻译机制（§41.5 i18n 规则）。

## Layout

QGIS 标准解剖：左 dock（资源管理器/图层树）、中央 `QgsMapCanvas`、右 dock（页签参数面板）、底 dock（任务/日志/验证记录）、状态栏。

**唯一签名元素：编号工作流标签栏** —— 文件菜单右侧依次为 `数据管理 | 预测编图 | 单因素图 | 智能编图 | 验证`（#43 裁决文案），当前步蓝色下划线+蓝色文字（暗色用 primaryText 提亮）。它把产品的工作流链钉在顶部，是"这就是干这事儿的工具"的载体。

层位切换条（C3 C6 D53…chips）在 ribbon 之下、画布之上，checkable `QToolButton` 组。

## QGIS 控件映射（"尽量用原生控件还原原型图"的实现契约）

| 原型区域 | 实现 |
|---|---|
| 工作流编号标签 | `QTabBar`（定制样式，签名元素）+ `QMenuBar`（文件/视图/帮助） |
| Ribbon 分组 | `QToolBar` + `QToolButton`(icon-over-text) + 分隔 + 组名行 |
| 搜索命令 Ctrl+K | `QgsLocator`（QGIS 原生 status-bar locator） |
| 资源管理器树 | `QTreeView` + 工区数据模型（QgsBrowserModel 模式） |
| 图层树（勾选/图标/右键） | `QgsLayerTreeView` —— 原生直接用 |
| 层位 chips | checkable `QToolButton` 组 |
| 地图画布 | `QgsMapCanvas` + `QgsMapTool*` |
| 右侧参数/样式页 | `QDockWidget` + `QStackedLayout`（#43：QgsPanelWidgetStack/QgsLayerStylingWidget 属 libqgis_app，未导出）；折叠组自研 CollapsibleSection |
| 底部 任务/日志/验证记录 | `QTabWidget` dock：自定义 QTreeWidget 任务面板（#43）+ `QgsMessageLogViewer`（libqgis_gui 可用）+ 发布/属性表面板 |
| 状态栏（坐标/比例尺/CRS/层位） | `QStatusBar` + 画布信号驱动的 mono 数字标签（#43：QgsScaleWidget 等专有状态栏控件为 QGIS 应用内实现，libqgis_gui 未导出） |
| 画布装饰（指北针/比例尺/图例） | `QgsMapCanvasItem` 自研 —— QGIS 装饰类在 src/app 层，属 §39 app-only 审计项 |
| 连井剖面/地震剖面/井对比 | 自定义 `QWidget`（护城河组件），dock 于画布下方 |
| 禁用工具的 reason 提示 | tooltip 显示 `ToolAvailabilityService::reason`（§35） |

## Shapes & Elevation

- 面板直角或 4px 圆角；卡片/对话框 8px；层位 chip 全圆角胶囊。
- 深度用 1px border + 极轻 offset 阴影（dock 浮起时）；**禁止**零偏移彩色光晕。
- 嵌套面板内圆角 = 外圆角 − 间距。

## Components

- **ribbon 按钮:** hover = surface-alt 底色；运行类动作（▶计算单因素）文字用 primary；禁用态置灰 + tooltip 给出 reason。
- **dock 面板:** 可拖动/可关闭/可恢复（QGIS dock 惯例），标题栏 12px 标题 + ✕。
- **折叠组:** `QgsCollapsibleGroupBox`，▼ 展开态为默认。
- **状态标签:** 浅色底+深色字的小胶囊（通过/待复核/未执行），永远带数字。
- **画布内装饰:** 指北针左上、比例尺左下、图例右上、图件标题 chip 左上——白底半透明卡片承载。

## Do's and Don'ts

- Do: 一切可复用的 QGIS 控件直接用原生类（图层树/任务面板/日志/样式面板/定位器）。
- Do: 工作流标签栏保持唯一签名元素地位——别再发明第二个自定义 chrome。
- Do: 禁用控件必须带 reason tooltip（§35）。
- Don't: 自绘按钮/树/停靠控件去"更像原型图"——视觉逼近到 token 级即止。
- Don't: 把交互蓝用作装饰底色；不要把相图色标混进 UI token。
- Don't: 堆积卡片套卡片、渐变 CTA、居中一切——这是工具不是营销页。

## Motion

- **Approach:** minimal-functional —— Qt Widgets 以即时切换为主；无编排动画。
- 任务进度走状态栏/底部任务面板（`QgsTaskManagerWidget`），>1s 显示进度条，>10s 给预计时间。
- 无 Qt 内建 reduced-motion；如未来加动画，走项目级设置开关（QSettings 绑定）。

## Decisions Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-09-25 | 初始设计系统 | /design-consultation + /qt-ui-design；控件级复用+主题还原（D2）；工作流即记忆点（D4）；AI mockup 不可用走 HTML 预览 |
| 2026-09-29 | 暗色模式翻案 | 用户明确要求交付暗色：推翻 2026-09-25「V1 不交付暗色」决定。落地=UI chrome 全量 token 双值（frontmatter `dark` 块）；缺省浅色、仅显式切换写 QSettings（`ui/theme`）；SARibbon 调色板 isDark 双份；图标暗色再着色（QGIS default 深 glyph 逐像素提亮，PaleoIcons）；数据符号色（§93）不跟随；wellcomposite 柱状图画布保持纸面白底（地质文档隐喻），仅面板/对话框 chrome 跟随。对比度全 AA（text/surface 11.6:1、muted 6.4:1、error 胶囊 5.2:1）。 |
| 2026-09-29 | 翻译源语言口径 = 中文 | 全部用户可见串以中文为源串走 tr()/translate（layoutdesignershell 的 22 条英文源串同日改写为中文源串）；lupdate 骨架 translations/paleo_zh_CN.ts（1611 条），更新走 tools/update_translations.sh。 |
| 2026-09-29 | status-tag 胶囊文字色补 token | 浅色 status-tag 底+语义原色实测不足 AA（warning 2.1:1/success 3.0:1/error 3.7:1），补 successText/warningText/errorText 深色变体（4.5+）；暗色深底上语义提亮色本已 AA，文字=语义色。capsuleLabel 同步改活体注册（运行中切主题即时跟随）。 |
| 2026-09-28 | 控件映射与页签文案偏离确认 (#43) | 1. 工作流页签文案根据用户裁决确定为「数据管理 / 预测编图 / 单因素图 / 智能编图 / 验证」五页；2. 状态栏采用 QStatusBar + 坐标/比例尺/CRS（QGIS 4.2 中 QgsScaleWidget 等专有状态栏控件为 QGIS 应用内实现，libqgis_gui 未导出）；3. 右侧 dock 采用 QDockWidget+QStackedLayout，任务面板采用 QTreeWidget 以满足非模态展示与无头测试需求。 |
