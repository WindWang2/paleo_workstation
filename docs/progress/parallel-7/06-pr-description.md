## 摘要

方向 06 第 4 阶段增量——已合并 #453（squash `2fc29d35`）的审查跟进，基线 `3df72b80`
（#437 已并入）。本 PR 只含 3 个新提交，把工程门的覆盖面补齐到经 handler 核实过的全集，
并修掉审查发现的三处帮助措辞不实。

1. **门控覆盖面补齐**（`0451dfe5`）：初版只门五个导入 ribbon 命令——审查发现同一入口的
   **页内按钮**和**非镜像 action** 仍是漏的，且拖拽导入整条绕过。现在：
   - 门控集扩到 12 个 ribbon 命令：导入 ×5 + 保存版本 / 导出 PDF / 图件设计 / 运行预测 /
     生成单因素 / 运行验证 / 生成等厚图——均逐条核实「无工程必败或只有误导性反馈」。
   - 页内按钮硬门限恒使能入口（导入 ×5 + 编图三入口）；门控前把原 tooltip 存进
     `paleoGateSavedTip` 属性、开门还原——不再把发布态文案清成空串。
   - `PaleoRibbon::applyGate` 支持非镜像 action（图件设计）：直接写 enabled/tooltip，
     同样的存取恢复语义；owner 管使能的命令（勾选/层位/忙）门控只关不放，
     不覆盖更具体的既有禁用理由。
   - **catalog 打开失败**这道既有禁令补上漏掉的 `importWellLogs`，并与工程门取与、
     谁的禁令写谁的理由——`availabilityChanged` 重放不再把目录原因冲掉。
   - **拖拽入队与按钮同一道门**：`DataListPanel` 新增壳注入的工程态探针
     （catalog 在工程关闭后仍 `isOpen()`，分辨不了「工程已关」）；无工程先写
     「先新建或打开工程」不入队、不弹预估对话框。
2. **帮助措辞校正**（`4e4bcf12`）：
   - `directionButton`/`breakLineButton` 原文按单一路径写死——实际语义由右侧下拉决定
     （方向引导/软边界/制图绕行、硬屏障/等值停止），且只有约束感知的插值方法消费；
     改为如实描述线族。
   - 预览最大化原文写「列表留一行」，实际把数据列表与右侧面板一起隐藏——按真实行为改写。
   - `thicknessChainButton` 补目录帮助（藏在旧版编图页签、可见性测试覆盖不到），
     文案取按钮自身 tooltip 的真语义。
3. **记账**（docs only）：阶段 4 审查结论与处置。

### 有意不动的

- 地图册 `ribbonMapBookAction`、剖面工作台 `sectionWorkbenchAction`：无工程时打开的是
  基本空的功能面，不致败只显得空——owner 决策项，本 PR 不门。
- 自报告/自闭环命令（发布、数据图谱、刷新、未决过滤、导入台账/体检/存储、震道查看器、
  打开剖面、版本发布、`ribbonFuse`/`ribbonPolygonize` 既有输入态门）：分析无真实缺陷，不门。
- owner 管使能的页内按钮（预测 runButton、验证 runButton、IDW 按钮、等厚图按钮）：
  门控只作用于 ribbon 侧 action，页内按钮由 owner 的禁用理由接管。

## 证据（均为实际执行）

- base：`origin/master` = `3df72b80`（含 #453 squash + #437）。无冲突。
- 红→绿：`importCommandsAreGatedUntilProjectOpens` 扩到 12 action + 8 页按钮 + DnD
  三阶段断言（无工程/工程开/工程关）——扩门前新增断言为红，修复后全绿；
  DnD 断言首次暴露「工程关后仍可入队」（catalog 不关），探针接线后绿。
- 构建：rebase 后整树重编退出码 0；`tst_shell_chrome` 18 用例全过；
  相关 10 测试集其余 9 项全过。
- `python3 tools/check_layering.py --strict`、`git diff --check`：退出码 0。
- `paleo_zh_CN.ts`：仅 ±语义改动 + 3 条新增 message，`xml.dom.minidom` 验证、
  lrelease 成功（6271 条）。
- 全程 `flock -x /tmp/paleo_workstation_global_build_j4.lock`，`-j4`，offscreen。
- 基线与逐轮记录：`docs/progress/parallel-7/06-baseline.md`、`06-ui-shell-help.md`。

## CI 状态（如实，非本地推断）

- 合并提交 `2fc29d35`（#453 squash）上：`linux` ✅、`lint` ✅、`linux-asan` ❌——
  失败原因 `final link failed: No space left on device`（2133/2134 步才挂），
  **runner 磁盘满的基础设施问题，非代码**；`windows`/`linux-perf` 当时在跑。
- 本分支 CI 以本 PR 检查结果为准。

## 已知基线问题（非本 PR 引入，如实记录）

- **`tst_shell_projectlifecycle::surfaceOpsGateFetchesProjectAtCallTime` 在 pristine
  `3df72b80` 确定性失败**：期望 ≥2 个已声明栅格后弹参数表，实测弹出
  「需要至少两个已声明的栅格图层」通知。stash 本分支全部改动后同法复现——
  #437 合并带入的回归，本 PR 不修（属 direction-01 域）。

## Review 记录与限制

- 实现与评审分离：生产改动 lead 逐行读完 diff；新增断言由 lead 编写，
  有效性靠修复前红→绿证明。没有另起独立 reviewer 子代理。
- `tst_ui` 既有挂死用例 `testSimultaneousWellsAndSurveyAreaVisibility`（pristine master
  即挂死）未修未跳过。
- 最小窗口 1280×800 与小逻辑屏冲突的三处越界是设计决策，本 PR 不改下限。
- 地图册/剖面工作台门控、逐层 `layerBusy` 显示、约束页 `constraintDeleteButton`
  禁用无原因提示：均为 owner 决策项，已记录未动。

## 本 PR 没做的

- F：dock 内无名 Tab 停靠点、dialog 重复弹窗、动态主题焦点环、tab order 顺序未审计。
- G：没有手工视觉验收；真实双屏、dock 拖拽浮动未测。

不合并。

Generated with [Devin](https://devin.ai)
