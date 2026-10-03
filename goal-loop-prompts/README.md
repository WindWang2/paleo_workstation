# Goal-Loop Prompts — 23 个方向（自治迭代任务包）

第一批 01–05 已发 fleet 并落地（PR #70–#73 全合）。第二批 06–11 为追加方向。

本目录保存四批 01–23 的任务书快照；目录名为 `goal-loop-prompts/`，
编号文件与下表一一对应。表中「新增」「在飞」是任务书发出时的状态，
当前交付与递延以 `docs/progress/` 对应账本及 `TODOS.md` 为准。

每个 .md 是一个完整的 /goal-loop 开放 prompt：交给一个自治 agent 会话直接执行。
协议与既有 wave 一致（见 `.goal-loop-ledger*.md` 先例）：worktree 分支 → 迭代账本 →
Oracle 验收 → push + `gh pr create`。全自动，无人工确认，不等 CI。

## 通用纪律（所有 prompt 均含，此处统一说明）

- **基线**：任务指定的 `master` 或 `origin/master` HEAD（未指定时用最新 `origin/master`；
  本地/远程不一致时明确记录所选 commit）。含 vendor 收口：构建期 prefix 烧进二进制、ctest 注入
  `QGIS_PREFIX_PATH`、vendored .so 全 `$ORIGIN`。
- **worktree**：必须自建独立 worktree 分支开发（`git worktree add .worktrees/<slug>
  -b goal/<name>-<日期>`，从上述基线起），禁在主 checkout 写代码。
  gitignored vendor 依赖需另行接线，见 BUILDING.md「独立 worktree 开发」。
- **构建**：`QGIS_PREFIX_PATH=<repo>/vendor/superbuild/prefix cmake -S . -B build
  -DCMAKE_BUILD_TYPE=RelWithDebInfo`；增量 `cmake --build build -j8`。
  本机工具链偶发 ld/gcc 崩溃——重试续传即可，坏 `.o` 删后重编。
- **资源**：构建/测试并行度均不超过 8，`ctest --test-dir build -j8`；Windows ctest 串行。
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
| 15 | `15-well-logset.md` | 多文件井曲线汇聚：一井多 LAS 并集曲线表（带溯源）、canonical 仲裁、导入序修正、消费面统一 | 功能深化 |
| 16 | `16-catalog-sqlite.md` | catalog 持久层迁 SQLite：四表 schema、JSON→sqlite 迁移器、journal→事务原子化、备份语义保持（读面不动） | 架构深化 |
| 17 | `17-fault-surface.md` | 断层立体化：断棒→三角网断面、断距量算（heave/throw 沿交线）、断面持久化与 3D 渲染、体域阻断升级 | 新功能 |

### 第四批（新增，四方向各对应一个面）

| # | 文件 | 方向 | 性质 |
|---|------|------|------|
| 18 | `18-geostat-methods.md` | 地质统计学方法包：实验变差函数+三模型拟合、普通克里金、序贯高斯模拟、方法择优诚实面 | 方法优化 |
| 19 | `19-well-trajectory.md` | 井轨迹空间化：测斜域模型、最小曲率法 MD↔TVD、定向井贯穿剖面/平面/3D/属性建模消费面 | 操作逻辑 |
| 20 | `20-job-framework.md` | 任务框架统一：JobRunner 抽出散落 5 处三段式、workflows.cpp/datapreviewtabs.cpp 巨兽拆分、GUI 线程探测面扩展 | 重构 |
| 21 | `21-data-perf.md` | 数据路径性能攻坚：SEG-Y 索引缓存发布修复、LAS 读面加速、catalog sqlite 基线重测、冷启动 profile | 性能提升 |
| 22 | `22-seismic-inversion.md` | 地震反演：子波提取与库、层位约束低频阻抗模型、带限道积分反演、稀疏脉冲反演→波阻抗体 DERIVED | 新功能 |
| 23 | `23-singlefactor-completion.md` | 单因素语义补完+约束线交互绘制：走廊/分区/FaultPathMetric/surfer_idw/分区等值线/缓冲过渡移植 + 五语义线绘制编辑吸附 | 功能补完 |

## 使用方式

每个 prompt 直接喂给一个自治 agent 会话（如 devin/claude 新 session）：
它会自建 worktree 分支、按 ledger 协议迭代到 Oracle 全绿、push + 开 PR 收尾。
文件面互不重叠，可并行起多个；20 与 single-factor 迭代面有交集，宜等其收官或隔离实现。
