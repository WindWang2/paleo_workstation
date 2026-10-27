# QGIS/UI 工作流深化（任务 B：goal/qgis-ui-deepening-20261012）

日期：2026-10-12。基线：origin/master `d230a5e7`。worktree：
`.worktrees/qgis-ui-deepening-20261012`（独立 build/，vendored superbuild
prefix 共享）。

范围：**既有** GIS 工作流的 UI/QGIS 质量深化——不新开业务方向、不替换
QGIS、不建第二套 composer/vector 编辑器。主责 `src/ui/`、`src/qgis/`；
A 任务主责目录（domain/catalog/metadata/services/algorithms/app、
mappingworkbench 核心）零写入；Open Issues #351–#421 全量排除（矩阵见
`docs/ledger-archive/goal-loop-qgis-ui-deepening.md`）；Open PR #422/#423/#424 占用
文件零触碰。

## 交付内容（7 commits）

| Commit | 主题 | 对应任务线 |
|---|---|---|
| `c95a8feb` | 发布门随层位 chip 重算 + 合成编图门控与失败反馈 + 约束编辑静默路径落地 + 验证 busy tooltip | B-R1 |
| `7e2908b2` | 窄面布局两处裁字（预览样式侧栏表单换行 / 井纸面缩放读数去 QSS min-width） | B-R3 |
| `c8805c05` | 标准导出流图签块（TODOS P3 规范图件四要素收口） | B-R4 |
| `77fcf5f0` | 层位状态一致性（页面下拉填充/chip 联动/验证页工程复位/切页单次档案应用）+ 新 shell 测试 | B-R1/B-R5 |
| `3d1cde81` | renameTheme 原生 renameMapTheme + page:* 服务层硬拒 | B-R2 |
| `28d3817f` | TODOS/账本/进度文档 | 文档 |
| `72198262`/`73078358` | attach 改动事故恢复（详见下）+ Review 注释修正 | 修复 |

### B-R1 页面状态与动作统一

- **发布门层位联动**（`attach_mapping.cpp`）：`refreshPublishGate` 补
  `activeHorizonChanged` 接线——发布按钮/版本胶囊/保存按钮文案不再滞留
  旧层位状态（对照同函数块内 D8 厚度按钮既有联动）。
- **合成编图门控**（`composepage.cpp`）：fuseButton 随勾选态门控 + §35
  原因 tooltip（经列表模型信号驱动，不侵入 refreshFactors——#363 立案
  函数零接触）；`attach_compose.cpp` fuseFactors 失败原因落地 statusLabel
  （此前静默吞掉）。旧契约「空清单也发 fuseRequested」按 DESIGN.md §35
  「启用的控件必须可行动」改为禁用+原因。
- **horizonCombo 生产链填充**（`paleomainwindow.cpp`/`constraintpage.*`）：
  工程打开按 `mappingHorizons()` 填充预测/单因素页层位下拉（此前生产链
  从未填充、只有测试手工补数据——两页全部意图恒携带空层位）；chip 切换
  → 两页下拉跟随（单向，chip 是唯一真源；双向同步留待产品决策）。
- **约束编辑三处静默失败**（`attach_constraint.cpp`）：删除/语义切换/
  顶点编辑的 beginConstraintEdit 失败补状态行提示（同文件已有纪律）。
- **验证页 busy tooltip**（`validatepage.cpp` ctor）：§35 对齐 predictpage
  先例；恢复路径同步清空。

### B-R2 QGIS 图层档案

- `renameTheme` 从 insert+remove（两次 mapThemesChanged）改原生
  `renameMapTheme`（≥3.14 原子操作 + 专用 mapThemeRenamed 信号）；
  `page:*` 前缀（任一侧）服务层硬拒——此前只有头注释约定，.qgz 版面
  地图项钉的 followVisibilityPresetName 会随改名悬空。
- TODOS P3「主题重命名」条目对账：服务 API 早已由 `edc892c5` 交付、
  管理 UI 随 `c2769d27` 移除——本轮只做服务面收尾，避免重复造轮子。

### B-R3 窄面/裁字（TODOS P3 遗留，方向 29 实证基线）

- **预览样式侧栏**（`previewtocpanel.cpp`）：三个快调表单
  WrapLongRows+AllNonFixedFieldsGrow（mappingworkbenchpage 先例）——
  1100×700 下 dock 压到 ~240px 时长标签换行到字段上方；dock 结构/页签
  层级/视觉体系零改动。
- **井纸面缩放读数**（`panelsetup.cpp`）：去掉 QSS `min-width: 65px`、
  数值面改 mono 字族。**实证**（独立 Qt 最小程序）：QSS min-width 使
  QLabel sizeHint 与文本长度脱钩（实测 hint=78 而文本需 93px），深缩放档
  「1000% (1:2000)」必裁字；移除后自然 minimumSizeHint 钉住完整文本宽。
- 右侧动作超出可见宽度：PaleoToolRow 横向滚动已可达；结构性分组重排
  需产品决策（TODOS 条目更新如实记录）。

### B-R4 标准导出流图签块（TODOS P3 收口）

`buildHorizonMapLayout`（`layoutexport.cpp`）在 CRS 页脚同行右段
（x=157–287mm, y=190–200，与既有元素零重叠）加 `signatureBlock`
（QgsLayoutItemLabel 8pt 两行）：编制/审核读工程 customVariables
（`paleoAuthor`/`paleoReviewer`——captureProjectSnapshot 保留该面），
缺省留空线供手签（不虚构责任方、不加输入 UI）；日期取当日。规范图件
四要素（图名/图例/比例尺/图签）就此闭合，未建第二套 composer。

### B-R5 测试验收

- **新增 `tests/tst_shell_horizonstate.cpp`**（真实 AppContext + 主窗壳 +
  attachWorkflows/attachMapping 同 main.cpp 接线）：
  - 工程打开 → 两页 horizonCombo 按 mappingHorizons() 填充（逐项对拍）；
  - chip 切换 → 两页下拉跟随；
  - 发布门随层位重算：hA 存版本（saveVersion 直灌）→「编辑中 · vN」，
    切 hB →「还没有保存的版本」，切回 hA 恢复——修复前该断言必红。
  - 注册：新文件 `cmake/extra-qgis-ui-deepening.cmake`（并行方向挂载点
    惯例，零 CMakeLists 冲突面；include 列表加一行）。
- 扩测：tst_mappingpages（fuse 门控）、tst_panels（验证 busy tooltip +
  clearResults 工程边界）、tst_previewmap_assets（240px 侧栏几何）、
  tst_wellcomposite_a11y（1200×700 缩放标签几何）、tst_mapping（图签块
  在场/空线/日期/几何/署名往返）、tst_layerplatform（page:* 双向拒绝）。

## 验证记录（R7 全量重建后复核；此前一轮绿标因 stash 事故二进制陈旧而重做）

- 构建：全量 `-k 0` 333 目标，仅两个 **master 基线链接红**
  （`tst_singlefactor_contours`/`_contourbudget`——#349 factorcontour 引入
  PaleoTask 依赖所致，#422 账本原文确认并已在其分支修复；CMakeLists 属
  其占用面）；其余含 `paleo` 与全部受影响测试二进制构建成功。
- 受影响 7 套件（tst_mappingpages/tst_panels/tst_previewmap_assets/
  tst_wellcomposite_a11y/tst_mapping/tst_layerplatform/tst_shell_horizonstate）
  + 回归 5 套件（tst_ui/tst_shell_projectlifecycle/tst_composeworkflow/
  tst_mapbook/tst_uxtheme）+ tst_wellcomposite_visual（golden）→ **13/13
  通过**。
- 护栏：`git diff --check` 0；`check_layering.py --strict` 通过；
  `check_ui_invariants.py --strict` violations=0；`check_l10n.py` 通过。

### 事故记录（独立 Review 发现并已修复）

bisect 排查 tst_ui 基线红时，一次 `git stash pop` 因 Bash 持久 cwd 在主
checkout 误执行，导致三份 attach_* 改动滞留 stash、首版 c95a8feb 未包含
其内容（提交信息与代码不一致）。独立 Review（B5 角色）以时间戳证据定案；
改动经逐行核对后由 stash 恢复并以 `72198262` 独立提交，全部验证用恢复后
的新二进制重做。主 checkout 现存未提交改动经文件集比对为并行任务（A）领域
内容，未触碰。

### 双基线说明（master 自带红，非本任务引入）

- **tst_ui `mappingWorkbenchCanvasRibbonAndReferences`**：主 checkout（干净
  master `d230a5e7`）同样失败（同一断言）；本分支 36/1/8 与 master 逐项
  一致。#422/#423 修复范围。
- **ui_tokens stale exception**（`trackpatterns.cpp`）：master 同样存在；
  #423 修复即此。本分支 violations=0。
- **contour 两测试链接失败**：#349 引入、#422 已修（见上）。

### 双基线说明（master 自带红，非本任务引入）

全量 ctest（362 项 -LE perf）：355 通过 / 7 红，**七项均为主线基线红**
（#422/#423 修复范围，逐项证据见账本「全量 ctest」节）：

- tst_ui（`mappingWorkbenchCanvasRibbonAndReferences` 断言，master 同红，
  双侧 36/1/8 逐项一致）；tst_attrpanel（#422 账本列出）；
  tst_mappingworkbench（**SEGFAULT 在本 worktree 切纯净 origin/master
  重建后同函数同指令偏移复现**——非本分支引入）；
  tst_singlefactor_contours（链接，#349 引入 PaleoTask 依赖，#422 已修）；
  tst_singlefactor_asynccontour（#423 账本列出）；ui_tokens/ui_tokens_strict
  （stale exception，master 同红，#423 修复对象）。

## 排除与边界

- Open Issues #351–#421：零触碰（排除矩阵 + 四路只读调查逐项核对；详见
  账本）。#362/#363 与本任务改动的共享文件（validatepage/composepage）
  仅新增无交集函数（clearResults/fuse 门控），未碰立案行为。
- Open PR：#422/#423（attributetablepanel、五个测试文件、ui-token 工具）、
  #424（datapage/datalist*/datapreviewtabs/ribbonpanels/datagraphpage 等）
  零触碰；新测试文件走独立 cmake 挂载点。
- A 主责目录零写入；`src/workflow/mappingworkbench*`、`src/app/appcontext*`
  零改动（ensureWellBaseLayers 每页重跑一项因落在 mappingworkbench.cpp
  放弃，记录于账本后续接口需求）。
