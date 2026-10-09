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

**Creative North Star:** 这就是干古地理编图活儿的工具 —— 工作流链永远可见，其余一切让位于 QGIS 原生控件的专业质感。
**Product context:** 石油地质学家的古地理编图工作台；Qt6 Widgets 桌面应用，嵌入 vendored QGIS；六页：数据管理 → 地层对比 → 智能预测 → 单因素图 → 智能编图 → 验证。地层对比按用户 2026-10-05 的要求，以 QtWebEngine 承载独立 Web 工作台。
**Mode per surface:** 全部 Operate（任务操作面）；无 Persuade/Read 面。
**Reference:** `prototype/` 五张设计稿 + QGIS 主窗口解剖（docs.qgis.org qgis_gui）。
**Key characteristics:** 编号工作流标签是唯一的非 QGIS 签名元素；浅色为缺省的双主题（暗色见 2026-09-29 翻案条）+ 单一蓝色 accent；密度对齐专业 GIS 工具而非 Web 惯例。

## Colors

**Strategy:** Restrained —— 一个蓝色 + 中性色阶；颜色稀有即有意义。
**Light or dark:** 浅色缺省（原型即浅色，野外办公室强光环境）；深色变体已交付（2026-09-29 翻案，用户明确要求）——UI chrome 全量跟随 `dark` token 块，缺省浅色、仅用户显式切换时写 QSettings；数据符号色（DESIGN.md:93 域配色）与 wellcomposite 柱状图「纸面」画布不跟随（柱状图是地质文档隐喻，2026-09-29 决策）。
- `#1B73D0` 只用于：当前工作流步、主操作按钮、选中态 chip。**禁止**用于装饰性背景——交互色不兼装饰。
- 语义色仅承载状态：绿=通过、橙=待复核、红=不一致/错误，且永远配文字（不单独用色）。
- 地图域配色（相色标、物源线红虚线、展布线蓝虚线）是**数据符号**，由 QGIS 样式系统管理，不属 UI token。

## 单因素地图符号

单因素充填面保持 `FactorStyleWriter` 因子语义色带（连续线性渐变），明暗 UI 主题切换保持相同数值配色。等值线与数值注记共用该模块的常量规范：深墨 `#24303E` 主线 0.25 mm，白色底描边 0.55 mm；沿线数字 JetBrains Mono 9 pt，白色缓冲 0.6 mm。线和字均在浅/深画布及深色带端点上具有白/墨双重对比，不跟随 UI token。

注记识别结构图层 `level` 与 GDAL 图层 `ELEV`，显示两位小数，沿线重复间隔 60 mm；长度不足 10 mm 的碎段不注记，交叠由 QGIS 标签引擎避让。解释性绕行线使用相同可读性规范，但图层名称与来源保留“解释性”等标记。上述尺寸采用纸面 mm / pt，不随屏幕 DPR 手动缩放。

## Typography

- **UI/正文/标签:** Noto Sans SC（vendor 打包，保证跨平台一致 + §33 渲染回归测试稳定）；Windows 退化 Microsoft YaHei UI，Linux 退化 Noto Sans CJK SC。
- **数值/坐标/深度:** JetBrains Mono（vendor），tnum 等宽数字。
- **字阶（Qt pointSize）:** 正文 9pt 基准；标题 12pt；图件标题 15pt；次级标签 8pt。**有意偏离** Web 的 16px 规则：这是 Operate 面的专业工具密度，与 QGIS 本体一致；用 pointSize 而非 pixelSize 以跟随系统缩放。
- 全部用户可见字符串走 `tr()`/翻译机制（§41.5 i18n 规则）。

## Layout

QGIS 标准解剖：左 dock（资源管理器/图层树）、中央 `QgsMapCanvas`、右 dock（页签参数面板）、底 dock（任务/日志/验证记录）、状态栏。

**唯一签名元素：编号工作流标签栏** —— 文件菜单右侧依次为 `数据管理 | 地层对比 | 智能预测 | 单因素图 | 智能编图 | 验证`（2026-10-05 增加地层对比），当前步蓝色下划线+蓝色文字（暗色用 primaryText 提亮）。它把产品的工作流链钉在顶部，是"这就是干这事儿的工具"的载体。

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
- **面板布局:** 主窗口、数据预览与图件设计器共用原生 Qt 停靠管理策略；数据列表也是独立 dock，默认只呈现搜索行与列表，低频工具收在「选项」。中央画布使用停靠面板之外的剩余空间，长工具栏单独横向滚动；层位元数据与版本选择收归数据属性区。标题栏拖动提供及时的四边目标与停靠预览，允许浮动、分割和标签式停靠。「布局」入口统一显隐、移动、自适应与保存/恢复。内容溢出使用双向滚动条，内容刷新和切页不主动改变外层尺寸；窗口/停靠区域变化交给 Qt 布局，手动自适应仅在用户调用时执行。
- **折叠组:** `QgsCollapsibleGroupBox`，▼ 展开态为默认。
- **状态标签:** 浅色底+深色字的小胶囊（通过/待复核/未执行），永远带数字。
- **画布内装饰:** 指北针左上、比例尺左下、图例右上、图件标题 chip 左上——白底半透明卡片承载。

## 错误呈现 (Error Presentation)

为彻底解决散弹式弹窗导致的交互打断与状态丢失问题，Paleo Workbench 采用统一的 ErrorHub 汇聚与三级呈现（3-tier hierarchy）体系：

### 1. 三级呈现架构 (3-Tier Hierarchy)

- **Tier 1 (轻量通知): 通知卡片 (`NotificationCenter` & `ToastCard`)**
  - **交互语义**: 非模态、右下角悬浮堆叠呈现。适用于常规操作反馈、后台任务告警及非破坏性错误。
  - **生命周期与倒计时**: 支持基于严重级别的自动倒计时关闭（info 5s 状态栏 / warning 8s / error 12s）。
  - **去重与聚合角标**: 相同去重键（`ErrorHub` 缺省键 = 级别|来源|标题|正文）的高频消息不重复弹出新卡片，就地更新卡片文案并累加聚合角标（`×N`），且重置倒计时。
  - **屏幕并发屏障**: 预建池 4 张卡片并发上限；第 5 张到来时最旧一张提前收起（不排队、不溢出）。
  - **动效规范**: 零 `QPropertyAnimation` / `QEasingCurve` 动效（严格符合 `minimal-functional` 即时切换规范）。

- **Tier 2 (阻断决策): 模态确认对话框 (`PaleoNotify::ask*`)**
  - **交互语义**: 仅用于不可逆、破坏性操作二次确认或致命业务阻断。
  - **收敛契约**: 全局统一走 `ui/notifications/paleonotify.h` 的 `ask` / `askSaveDiscard` 收口，消除散弹式裸 `QMessageBox` 调用。
  - **去重抑制保护**: severe 错误（原 `critical`）的模态呈现受 60 s 去重窗口保护（相同去重键 60 秒内只弹一次，后续相同错误降级为非模态卡片或静默记录），杜绝错误风暴下的模态弹窗洪泛。

- **Tier 3 (全局审计): 错误历史抽屉面板 (`ErrorHistoryPanel`)**
  - **交互语义**: 集中式全局停靠（Dock）面板，挂载于主窗口底栏及「布局与面板」菜单。
  - **环形缓冲区**: 底层由 `ErrorHub` 维护上限为 500 条的环形历史缓冲区（`ErrorHub::kCapacity = 500`），满额时严格按 FIFO 逐出最早记录。
  - **多维过滤与检索**: 支持按级别（下拉）与关键字文本（标题/正文/来源子串，大小写不敏感）实时组合过滤。
  - **多行详情复制与清空**: 支持选中行（无选中则全部可见行）的格式化错误详情一键复制到剪贴板（TSV），支持全局一键清空历史。
  - **实时联动与刷新合并**: 订阅 `ErrorHub::errorRaised` / `historyCleared` 信号，高频注入下只置脏 + 单发 150 ms 定时器整表重建（隐藏时不重建），无越界风险。

### 2. 错误呈现 UI 令牌与布局规范 (UI Tokens & Layout)

| Token / 属性 | 取值规范 | 设计系统对齐依据 |
|---|---|---|
| **通知卡片宽度 (Card Width)** | `340px` | 保证多行排版下在主画布右下角的紧凑感与可读性 |
| **通知卡片高度 (Card Height)** | `64px ~ 140px` (自适应) | 最小高度 64px 保证单行标题与操作栏布局；最大高度 140px 容纳详细错误说明 |
| **卡片圆角 (Border Radius)** | `8px` (`rounded.md`) | 严格遵守 `DESIGN.md` 中卡片/对话框 `rounded.md: 8px` 规范 |
| **卡片描边与阴影** | 1px border (`border: #DFE5EC` / 暗色 `#3B4552`) + 轻量 drop-shadow (4px offset, 12px blur) | 统一走 `PaleoTheme::tokens()` 动态注入 |
| **倒计时条 (Countdown Bar)** | 底部 `2px` 矩形填充 | 对应语义强调色，不设圆角或外发光，50ms 步进渲染 |
| **悬浮堆叠容器宽度 (Overlay Width)** | `360px` | 容纳 340px 卡片及左右 margin/shadow 缓冲 |
| **悬浮堆叠定位锚点 (Overlay Anchor)** | 屏幕右下角 (Right-Bottom) | `right: 16px` (距窗口右边缘), `bottom: 36px` (避让底部状态栏高度) |
| **卡片垂直堆叠间距 (Card Spacing)** | `8px` (`spacing.sm`) | 垂直线性布局，紧凑不粘连 |
| **Info / Primary 语义色** | 浅色 `#1B73D0` / 暗色 `#5FA5F0` | WCAG AA 文本对比度符合 (4.5:1+) |
| **Warning 语义色** | 浅色 `#F29900` / 暗色 `#FFB74D` | 文本位置采用 `warningText: #9A5B00` 保障 AA 对比度 |
| **Error / Critical 语义色** | 浅色 `#E53935` / 暗色 `#F76A61` | 文本位置采用 `errorText: #C62828` 保障 AA 对比度 |


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

地层对比页占用中央工作区，收起 Paleo 的停靠面板；离页恢复原可见状态。ribbon 宿主与连接反馈遵守以上 token；宿主注入样式适配 Web chrome 的字体、主题与焦点环，SVG 地质图件保持原有数据符号。独立 Web 源码、模型、井资料与解释工程不纳入主仓库。

## 错误呈现

方向 64（2026-10-07）。错误/提示统一经 `services/errorhub`（`ErrorHub`）入账，再由 `ui/notifications` 按级别分流呈现；**文本、出现时机、失败语义与迁移前逐点一致，只换呈现通道**。

| 级别 | 通道 | 时长 / 规则 |
|---|---|---|
| info | 状态栏 `showMessage` | 5 s；不入通知卡 |
| warning | 右下角非模态通知卡 | 8 s 自动消失 |
| error | 右下角非模态通知卡 | 12 s 自动消失 |
| error + severe（原 `critical`） | 窗口模态 `QMessageBox::critical`（`open()`，不嵌套事件循环） | 同去重键 60 s 内只弹一次；其余次数只聚合计数 |

- **去重/聚合（单点事实在 `ErrorHub`）：** 去重键缺省 = 级别|来源|标题|正文；同键自**首次出现起 60 s 固定窗口**内再次上报只累加计数（不新增历史行、不新弹卡/模态）。窗口不随重复滑动——持续风暴下每 60 s 至多再呈现一次。
- **通知卡：** 宽 360 px，距主窗口右/下边缘 `spacingMd`(16)，卡间距 `spacingSm`(8)，自下而上堆叠，**最多同时 4 张**；第 5 张到来时最旧一张提前收起（不排队、不溢出）。卡面 = `surface` 底 + 1 px `border` + `radiusMd`(8) 圆角 + 左侧 4 px 语义色条（warning 橙 / error 红）+ 级别文字（「警告」/「错误」，颜色永远配文字）+ 加粗标题 + 正文（自动换行）+ ✕ 关闭。同键卡在场时只把右上角计数胶囊更新为「×N」并重置计时，不另起一张。
- **动效：** 遵守 Motion 节 minimal-functional——出现/消失即时切换，无滑入/淡出动画；卡片控件预建池复用（4 张），通知路径无逐帧分配。
- **模态保留清单：** ① 需要用户裁决的确认（是/否、确定/取消、保存/放弃/取消、重试/取消）——结果决定后续分支，保留模态，统一经 `PaleoNotify::ask*` 收口；② severe 错误（原 `critical`）。其余 warning/information 一律走非模态。逐点清单见 `.goal-loop-ledger-errorhub.md`。
- **错误历史：** 「布局与面板」菜单（主窗口无字面「视图」菜单，此菜单即视图入口）→「错误历史」dock：列 = 时间 / 级别 / 来源 / 次数 / 标题 / 内容；级别下拉 + 文本过滤；「复制」把选中行（无选中则全部可见行）按 TSV 进剪贴板；「清空」清 `ErrorHub` 环形历史（上限 500，满则逐出最旧）。
- **降级：** 未安装全局 `ErrorHub`（单测、组装根之前）时 `PaleoNotify` 原样回落到迁移前的 `QMessageBox` 调用，行为与迁移前完全一致。
- **独立顶层窗：** 通知卡只锚定主窗口。调用方身处另一个可见的非对话框顶层窗（如独立的版面设计器主窗口，可能整个盖住主窗口）时，保留旧模态呈现、只入账历史；对话框（叠在主窗口之上）照常走通知卡。
- **info 不去重：** 状态栏是非打扰通道，同键 info 每次都刷新状态栏（历史里仍聚合计数）。

## High DPI

渲染正确性条款（非视觉决策；色彩/字体口径仍以 Colors、Typography 节为准，pointSize 已跟随系统缩放）。

- **口径：** Qt Widgets 栈按逻辑坐标绘制，backing store 为物理像素——普通 QWidget 的 QPainter 路径天然 dpr 正确，**禁止**自行乘除 devicePixelRatio（画蛇添足会二次缩放）。需要手算设备像素的只有两类：
  1. **QOpenGLWidget 子类的 `resizeGL`**：`glViewport` 必须用物理像素（`qRound(w * devicePixelRatioF())`），因为 FBO 是物理尺寸而 `resizeGL(w, h)` 收到的是逻辑尺寸。视口按逻辑设置 = 高 DPI 屏渲染内容只占左下角。已由 `tools/check_ui_invariants.py` 的 `gl-dpr` 规则静态把关。
  2. **手动构建屏幕分辨率位图**（图标 pixmap、剖面地震栅格缓存）：按 `devicePixelRatioF()` 生成并 `setDevicePixelRatio`，参考 PaleoIcons 与 wellsectionscene 的 deviceTransform 口径。
- **拾取一致性：** 屏幕坐标↔NDC 换算要求分子分母同单位（鼠标逻辑坐标配视口逻辑 rect，或物理配物理）。尺度因子在除法中相消，混用才是错位根源；新拾取代码沿用逻辑单位。
- **图像出口分两类**：数据位图（地震切片 rgba、CPU 拼接栅格、图件导出）以**数据分辨率**为准，不做 dpr 换算——dpr 是屏幕概念；屏幕抓帧（视口截图、GL sweep 帧的 `grabFramebuffer`）按**物理像素**回读（所见即所得，dpr>1 时即高分辨率帧），同样不做额外缩放补偿。
- **测试约定：** 高 DPI 回归用 `QT_SCALE_FACTOR=2`（Qt 官方机制，offscreen 平台下有效）模拟分数缩放；断言 `grab()` 输出物理尺寸 = 逻辑×2。offscreen 下 QOpenGLWidget 不建 GL 上下文，GL 路径的 dpr 验证走 QOffscreenSurface+FBO 直渲（tst_seismic_highdpi 先例）。dpr=1 路径必须逐像素不变（既有 golden 对拍）。

## Decisions Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-10-09 | Office 原件改用本机 ranuts/document 编辑页 | Calligra 只能把原件画成页面图，操作面不够。六种 Office 原件在 Qt WebEngine 中打开钉死版本的 ranuts/document（OnlyOffice WASM，无文档服务器）。编辑器沿用上游界面和 ONLYOFFICE 标识；外围状态行使用主题字体、颜色和间距。不转 PDF。保存另登记 DERIVED 版本，不覆盖 RAW。无 WebEngine 或无屏时降级为系统浏览器打开同一本机地址。 |
| 2026-10-09 | Office 离线预览使用 Calligra / Qt 页面控件 | 用户排除 LibreOffice，选择本地离线开源方案，并保留不转 PDF 的要求。Calligra 26.08.2 在独立进程解析和按需渲染六种格式，Qt 原生控件提供页码/工作表切换、缩放和双向滚动；沿用字体、主题与间距。文档纸面颜色保留原件样式。该页图路径已被同日的编辑页决定替换。 |
| 2026-10-09 | 辅助 XML 默认井道图，可切换数据列表 | 非工程测井的辅助 XML 使用既有视图切换工具行，按钮为「井道图」「数据列表」，初始选择井道图；加载中可以切换，解析失败保持用户选择并显示失败态。SpreadsheetML 列表保留工作表，普通 XML 列表显示路径与内容；沿用主题字体、颜色与间距。 |
| 2026-10-08 | 单因素方法升为主流程，等值线/面符号统一 | 用户要求方法参数明面化、主题可读；FactorStyleWriter 同时管理语义色带与双描边数字线，数据符号两主题保持同值。 |
| 2026-10-07 | 预测页统一命名为「智能预测」 | 用户明确要求；页签、页面标题、快捷键说明、帮助及工作流指南统一使用新名称。 |
| 2026-10-07 | 错误呈现三级分流（方向 64） | 169 处 QMessageBox 连弹无聚合、无历史；统一 ErrorHub + 通知卡/模态/状态栏，确认类与 severe 保留模态，详见「错误呈现」节 |
| 2026-10-06 | 高 DPI 条款成文（High DPI 节） | 3D 视口 resizeGL 未乘 dpr 为确定性缺陷（方向 60）；口径=普通 QWidget 路径禁止手乘 dpr、GL resizeGL 必须物理像素、图像出口两类（数据位图=数据分辨率、屏幕抓帧=物理回读）均不额外补偿；护栏入 check_ui_invariants.py `gl-dpr` 规则；测试约定 QT_SCALE_FACTOR=2 offscreen。 |
| 2026-10-05 | 数据管理后增加地层对比 ribbon 页 | 用户明确要求嵌入独立 Web 前端。QtWebEngine 只作页面宿主，连接与进程编排留在功能层；本机配置保存服务地址、外部目录和 Python，源码/模型/数据不进入 GitHub。 |
| 2026-09-25 | 初始设计系统 | /design-consultation + /qt-ui-design；控件级复用+主题还原（D2）；工作流即记忆点（D4）；AI mockup 不可用走 HTML 预览 |
| 2026-09-29 | 暗色模式翻案 | 用户明确要求交付暗色：推翻 2026-09-25「V1 不交付暗色」决定。落地=UI chrome 全量 token 双值（frontmatter `dark` 块）；缺省浅色、仅显式切换写 QSettings（`ui/theme`）；SARibbon 调色板 isDark 双份；图标暗色再着色（QGIS default 深 glyph 逐像素提亮，PaleoIcons）；数据符号色（§93）不跟随；wellcomposite 柱状图画布保持纸面白底（地质文档隐喻），仅面板/对话框 chrome 跟随。对比度全 AA（text/surface 11.6:1、muted 6.4:1、error 胶囊 5.2:1）。 |
| 2026-09-29 | 翻译源语言口径 = 中文 | 全部用户可见串以中文为源串走 tr()/translate（layoutdesignershell 的 22 条英文源串同日改写为中文源串）；lupdate 骨架 translations/paleo_zh_CN.ts（1611 条），更新走 tools/update_translations.sh。 |
| 2026-09-29 | status-tag 胶囊文字色补 token | 浅色 status-tag 底+语义原色实测不足 AA（warning 2.1:1/success 3.0:1/error 3.7:1），补 successText/warningText/errorText 深色变体（4.5+）；暗色深底上语义提亮色本已 AA，文字=语义色。capsuleLabel 同步改活体注册（运行中切主题即时跟随）。 |
| 2026-09-28 | 控件映射与页签文案偏离确认 (#43) | 1. 工作流页签文案根据用户裁决确定为「数据管理 / 预测编图 / 单因素图 / 智能编图 / 验证」五页；2. 状态栏采用 QStatusBar + 坐标/比例尺/CRS（QGIS 4.2 中 QgsScaleWidget 等专有状态栏控件为 QGIS 应用内实现，libqgis_gui 未导出）；3. 右侧 dock 采用 QDockWidget+QStackedLayout，任务面板采用 QTreeWidget 以满足非模态展示与无头测试需求。 |
| 2026-10-06 | 统一错误呈现架构与 QMessageBox 收敛 | 建立统一 ErrorHub 服务与三级错误呈现（Tier 1 通知卡片非模态浮动 / Tier 2 PaleoConfirmDialog 模态阻断二次确认 / Tier 3 错误历史 Dock 全局审计面板）。将 UI 层现存 169 处 QMessageBox 散弹调用降幅达 81.66%（保留 31 处关键决策并在 NotificationManager 设立 60s 模态去重抑制与 setConfirmHookForTesting 无头自动化应答钩子）。通知卡片并发上限 5 张、排队上限 50 条 FIFO 逐出、零 QPropertyAnimation 动效；ErrorHub 500 条环形缓冲区 FIFO 逐出。严格符合 DESIGN.md 配色、圆角与 minimal-functional 动效规范。 |
| 2026-10-07 | 工程配准与底图（用户要求） | 文件菜单增加「工程坐标与底图…」，使用原生 Qt 表单与 QgsProjectionSelectionWidget，沿用字体、间距和次级文字 token。有效配准工程的状态栏显示地图 CRS、WGS84 经纬度及底图来源；无配准工程保留局部米坐标文案。 |
| 2026-10-08 | 错误呈现唯一真源（方向 76） | 方向 54 并行 ErrorHub 及未装配呈现栈（NotificationManager/NotificationCard/ErrorHistoryModel/ErrorHistoryDock）删除；全局 `ErrorHub` 为唯一错误真源，三级呈现归 `NotificationCenter`/`ToastCard`/`ErrorHistoryPanel`/`PaleoNotify`；状态栏错误徽标改订全局 hub（此前误订未装配 hub，count 恒 0 永久隐藏）|
