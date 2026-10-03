# Goal-Loop Ledger — 方向 25：图件编制与出版输出（布局设计器补全）

- 分支：`goal/layout-publishing-20261004`（worktree `.worktrees/layout-publishing`，基线 bd1eb0a）
- 日期：2026-10-04 起

## 现状勘察结论（实测）

- `layoutdesignershell` 已非空壳：菜单(4 顶)/工具栏/标尺/QgsLayoutView/元素面板/
  模板动作/undo/dock 管理都在；缺元素树、对齐分布、吸附开关、快照/实时绑定。
- `src/qgis`：QgisLayoutService（布局 CRUD 挂 QgsProject::layoutManager → 随 QGZ
  持久化）、PaleoLayoutExport（Png/Pdf/Svg + 页选克隆策略）、
  buildHorizonMapLayout（单图工厂样板）、mapbooklayout。
- `src/ui/layout/`：layoutitempalette / layoutitempanel（业务区：层位标题注入 +
  比例尺三预设）/ layoutexportactions（dpi 72–1200 自由值，无 150/300/600 预设）/
  layouttemplates（3 个页面 .qpt）/ layoutundostack / mapbookpanel。
- catalog：append-only（无 rename/delete API）→ 模板「重命名/删除」语义走
  project.sqlite sidecar 表（MapVersionStore 先例），内容走 catalog 版本（SHA/版本号）。
- QGIS 4.2 API 确认：QgsLayoutAligner(core)/QgsLayoutItemMapGrid/grids()->addGrid/
  QgsLayoutItemMapOverview/overviews()->addOverview/setLinkedMap/
  QgsLayoutItemLegend::setSyncMode(Qgis::LegendSyncMode)/QgsLayoutSnapper/
  QgsLayout::itemsModel()（原生元素树模型）。
- vendor symlink 陷阱：任务给的相对路径 symlink 自引用（cc1plus 符号链接层数过多），
  改绝对路径后构建恢复。

## 实现记录（改动/验证/判定）

### M1 版式模板库

- 改动：`src/qgis/standardelements.{h,cpp}` 三类图件骨架工厂（井位图/单因素图/
  沉积相图，populateFigureLayout 清场重建 + buildFigureLayout）；
  `docs/templates/a3_{landscape,portrait}.qpt`（A4 克隆改尺寸）；
  `layouttemplates` 增加复合键（kind@page，12 个）与图件模板子菜单；
  模板资产库 `metadata/layouttemplatestore`（layout_templates 表：名字/种类/规格/
  资产引用）+ `workflow/layouttemplatelibrary`（qpt→catalog INTERMEDIATE 受管版本，
  另存/重存/重命名/删除/套用）。
- 验证：tst_standardelements（9 用例：词表/页面/工厂/插图/三类清单/清场/XML
  round-trip 逐字段）；tst_layouttemplatelibrary（4 用例：save-list-apply-rename-
  remove 全链 + catalog 版本/SHA/只读 + 重名/空参拒绝）。
- 判定：✔ Oracle 1（round-trip）/Oracle 3（catalog 版本 SHA）有测试证据。

### M2 标准图件元素

- 改动：工厂五件套（比例尺数字+条式/图例 AllProjectLayers 同步+按图过滤/指北针
  SVG+N 退化/坐标网格整刻度+注记/标题块 15-9-8pt）+ 全套装配 addStandardSet；
  Items 菜单「标准图件元素」子菜单（7 动作，无地图项守卫 + §35 tooltip）。
- 验证：tst_standardelements::factoriesLinkAndStyle（联动/样式/定位约定）+
  tst_layoutdesigner_full::standardElementsMenuAddsFactories。
- 判定：✔（面板走既有 QGIS 原生属性控件 + PaleoTheme 壳样式，未新增裸样式）。

### M3 地图项绑定

- 改动：addInsetMap（右下 42×30mm，overview setLinkedMap 指主图——渲染层自动
  跟随）；比例尺 setLinkedMap；面板「锁定快照/实时跟随」按钮
  （followVisibilityPreset 切换 + checkedLayers 钉层集）。
- 验证：insetOverviewBindsMainMap + itempanel 快照/实时代码路径（tst_layoutitempanel
  既有用例不回归）。
- 判定：✔（快照=显式层集，实时=主题跟随，语义见头注释）。

### M4 导出链路

- 改动：导出对话框 dpi 预设行（150/300/600 一键回填，自旋框仍为真值）；
  `workflow/horizonbatchexport`（层位组→快照换图→全幅范围→标题替换→导出→
  catalog OUTPUT 登记（工程_层位_日期）→版面逐字段复原→undo 不污染）；
  ComposePage 批量出图按钮 + 主窗口接线（骨架=活动层位版面或第一个版面）。
- 验证：tst_horizonbatchexport（计数=层位组数/文件名规则/登记/复原/空层位如实
  失败/**导出-画布 5×5 像素抽样一致**）。
- 判定：✔ Oracle 2 两半（像素抽样 + 批量计数）有测试证据。

### M5 版式持久化

- 改动：无产品代码改动需要——QgsLayoutManager 挂 QgsProject，QGZ 读写原生带回；
  补验证测试 tst_layoutpersistence（元素/绑定/页面规格/标题逐字段 + 服务面 CRUD）。
- 判定：✔（既有架构已满足，测试钉住行为）。

### M6 设计器外壳

- 改动：元素树（QgsLayout::itemsModel() 原生模型 + QTreeView，双向选中同步，
  双击开属性面板）；Edit 菜单对齐(6)/分布(8)子菜单（QgsLayoutAligner + 选中数
  守卫）；View 菜单吸附三开关（参考线/项默认开，网格默认关）；ComposePage 版面库
  （名单 + 打开/删除/批量出图，设计器非孤岛）。
- 验证：tst_layoutdesigner_full::itemTreeSelectionSync / alignDistributeAndSnapping /
  figureBuiltinActionAppliesSkeleton / shellStructureMenusAndPanels（新菜单形状）。
- 判定：✔。

## 陷阱与翻案记录

- vendor symlink 相对路径自引用 → 绝对路径（见上）。
- QgsLayoutItemMap 顺序纪律：**先 attemptResize 定框再 setExtent**（attemptResize
  保比例尺重扩范围；setExtent 只按宽度调高度；mapbook 注释同款）——addMainMap/
  插图/测试全部遵守。
- QGIS 模板序列化 round-trip 非字节稳定（比例尺框重算）→ 模板「重存内容未变
  复用版本」探测撤案，改为每次保存记新版本（append-only 账本如实）。
- `QLatin1String("中文")` 永不相等（Latin-1 截断）→ CJK 比较一律 QStringLiteral。
- 完全重合的两项是分布的退化输入（QgsLayoutAligner no-op）→ 测试用互异位置 +
  DistributeLeft。
- highlightItem 需显式 setCurrentIndex（select 的 Current 旗标不动 currentIndex）。
- 批量导出 catalog 必填（原可选分支的成品会随临时目录焚毁，Result.file 指死路径）。

## 门禁/全量状态

- [x] layering --strict / ui_invariants --strict / i18n / git diff --check（工作区）
- [ ] 全量 ctest（进行中）
- [ ] ≥2 轮 diff 自审零 High/Medium
