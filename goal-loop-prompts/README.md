# Goal-Loop Prompts — 11 个方向（自治迭代任务包）

第一批 01–05 已发 fleet 并落地（PR #70–#73 全合）。第二批 06–11 为追加方向。

每个 .md 是一个完整的 /goal-loop 开放 prompt：交给一个自治 agent 会话直接执行。
协议与既有 wave 一致（见 `.goal-loop-ledger*.md` 先例）：worktree 分支 → 迭代账本 →
Oracle 验收 → push + `gh pr create`。全自动，无人工确认，不等 CI。

## 通用纪律（所有 prompt 均含，此处统一说明）

- **基线**：本地 `master` HEAD（含 vendor 收口：构建期 prefix 烧进二进制、ctest 注入
  `QGIS_PREFIX_PATH`、vendored .so 全 `$ORIGIN`）。
- **worktree**：必须自建独立 worktree 分支开发（`git worktree add .worktrees/<slug>
  -b goal/<name>-<日期>`，从最新 `origin/master` 起），禁在主 checkout 写代码。
- **构建**：`QGIS_PREFIX_PATH=<repo>/vendor/superbuild/prefix cmake -S . -B build
  -DCMAKE_BUILD_TYPE=RelWithDebInfo`；增量 `cmake --build build -j8`。
  本机工具链偶发 ld/gcc 崩溃——重试续传即可，坏 `.o` 删后重编。
- **ledger**：worktree 根建 `.goal-loop-ledger-<slug>.md`，每轮记「改动/验证/判定/下一步」。
- **分层护栏**：src/ 文件头三行 `// 层：<词表>`（新顶层模块先 `scripts/new_module.sh`
  登记）；`tools/check_layering.py` + `--strict` 必须绿。
- **vendor 补丁**：改 `vendor/sbm` 必须登记 `vendor/sbm/PATCHES.md`（P 编号递增）。
- **UI 纪律**：视觉决策先读 `DESIGN.md`，不偏离；截图前后对照入 ledger。
- **提交**：原子提交，中文 conventional 前缀，对齐 git log 风格。
- **收尾硬门禁**：完成后**先仔细 review 确认无问题才开 PR**——diff 全量自审
  （无调试残留/死代码）、分层 strict 绿、构建无新警告、ctest 全绿、Oracle 逐条
  有验证证据。发现问题先修再验直到干净。然后 push + `gh pr create`
  （标题 conventional，body 含 `## Summary`/`#### Test plan`/自审结论清单）。
  不等 CI、不等人审。
- **性能断言纪律**：禁绝对毫秒墙钟（TEST-02 教训）——一律基线比率门或
  `PALEO_REAL_PROJECT_AREA` 类 env 门控实测。
- **冲突处理**：与在飞分支撞文件时按语义合，不丢弃他人改动；解决不了就隔离实现层。

## 方向清单

### 第一批（已落地，PR #70–#73 / AI 在飞）

| # | 文件 | 方向 | 性质 |
|---|------|------|------|
| 01 | `01-ui-experience-polish.md` | UI/UX 全仓打磨 | UI 优化 |
| 02 | `02-seismic-attributes.md` | 地震属性引擎 | 新功能 |
| 03 | `03-seismic-3d-viz.md` | 3D 深化 | 新功能 |
| 04 | `04-perf-systematize.md` | 性能体系化 | 优化 |
| 05 | `05-ai-geological-assist.md` | AI 辅助解释 | 新功能（在飞） |

### 第二批（新增）

| # | 文件 | 方向 | 性质 |
|---|------|------|------|
| 06 | `06-time-depth-velocity.md` | 时深转换与速度建模：tops/checkshot→速度模型→层位/剖面深度域转换 | 新功能 |
| 07 | `07-fault-interpretation.md` | 断层解释框架：FaultSet 数据模型、剖面断层棒拾取、层位切割多边形、管理面板 | 新功能 |
| 08 | `08-petrophysics-logs.md` | 测井岩石物理：Vsh/孔隙度/Sw 公式库、曲线计算器、多井批处理、QC | 新功能 |
| 09 | `09-horizon-autotrack.md` | 层位自动追踪：种子点同相轴追踪、置信度 QC、断层停追、2D→有限面扩散 | 新功能 |
| 10 | `10-gridding-surface-ops.md` | 网格化与面运算：新网格化算法、栅格代数、等厚/体积量算、不确定性面 | 功能深化 |
| 11 | `11-mapbook-reporting.md` | 批量制图与报告：地图册 AOI 序列、蒙太奇版面、批量导出队列 | 新功能 |

### 第三批（新增）

| # | 文件 | 方向 | 性质 |
|---|------|------|------|
| 12 | `12-synthetic-welltie.md` | 合成地震记录与井震标定：LAS→波阻抗→反射系数→子波褶积→井旁道对比→checkshot 回灌 | 新功能 |
| 13 | `13-property-modeling.md` | 三维地层格架与属性建模：层位面格架→井曲线粗化上格→地层坐标充填（断层阻断）→DERIVED 属性体 | 新功能 |
| 14 | `14-crossplot-facies.md` | 交会分析与无监督相分类：曲线/属性/栅格多维交会图、lasso 选区、k-means/GMM 聚类→分类栅格 DERIVED | 新功能 |

## 使用方式

每个 prompt 直接喂给一个自治 agent 会话（如 devin/claude 新 session）：
它会自建 worktree 分支、按 ledger 协议迭代到 Oracle 全绿、push + 开 PR 收尾。
五个方向文件面互不重叠，可并行起多个。
