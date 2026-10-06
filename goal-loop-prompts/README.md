# Goal-Loop Prompts — 57 个方向（自治迭代任务包）

第一批 01–05 已发 fleet 并落地（PR #70–#73 全合）。第二至七批为追加方向。
第八批 48–57 为 2026-10-05 三路深度代码审计（技术债/架构/质量）后的
优化提升批：方向源自实测证据（巨兽文件/AUDIT 未修项/翻译空白/Mock 路由/
错误面分散等），每个 prompt 内含开发量预算（agent tokens 上限 3 亿 +
执行花费轮次/墙钟估计）。

本目录保存八批 01–57 的任务书快照；目录名为 `goal-loop-prompts/`，
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
  -b goal/<name>-<日期>`，从最新 `origin/master` 起），禁在主 checkout 写代码。
  gitignored vendor 依赖需另行接线，见 BUILDING.md「独立 worktree 开发」。
- **构建**：gitignored vendor 三件（`vendor/superbuild/prefix`、`vendor/onnxruntime`、
  `vendor/prefix`）从主仓**绝对路径** symlink 进 worktree——相对 `../../` 会自环；
  `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DQGIS_PREFIX=<repo>/vendor/superbuild/prefix`；增量 `cmake --build build -j8`。
  本机工具链偶发 ld/gcc 崩溃——重试续传即可，坏 `.o` 删后重编。
- **资源**：构建/测试并行度均不超过 8，`ctest --test-dir build -j8`；Windows ctest 串行。
  多方向并行执行时全机编译槽会叠加（N 个并发 ≈ 8N 线程）——建议**同时重构建
  不超过 2 个**，或各会话降 `-j4`；review/测试等轻活不受限。
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

### 第五批（新增，功能扩展五方向）

| # | 文件 | 方向 | 性质 |
|---|------|------|------|
| 24 | `24-interactive-editing.md` | 交互式约束编辑与地图数字化：顶点编辑、形状数字化、捕捉、undo/redo、属性批量改型 | 功能补完 |
| 25 | `25-layout-publishing.md` | 图件编制与出版输出：布局模板库、标准图件元素、地图绑定、PNG/PDF/SVG 批量导出、设计器补全 | 新功能 |
| 26 | `26-wellsection-deep.md` | 连井剖面深化：基准面拉平、剖面编辑、曲线充填、栅状图、剖面-平面联动、导出 | 功能深化 |
| 27 | `27-facies-automapping.md` | 沉积相自动编图辅助：优势相统计、候选相界提取、证据合成、编图 QA、快速成图、版本对比 | 新功能 |
| 28 | `28-sequence-framework.md` | 层序地层格架：格架树/标志层管理、井间归属建议、编图单元校验、格架柱状视图、一致性诊断 | 新功能 |
| 29 | `29-ui-visual-polish.md` | UI 视觉一致性二轮：token 违例普查、图标收口、面板一致性、状态视觉规范、主题对比度、截图档案 | 打磨 |
| 30 | `30-data-pipeline.md` | 工区数据管线与健康：批量导入残差（实体归位预览/导入台账/未决归位）、CRS 管理、XYZ/Excel 格式扩展、体检仪表盘、回收站收编、版本对比回滚 | 功能深化 |

### 第六批（新增，领域扩展五方向）

| # | 文件 | 方向 | 性质 |
|---|------|------|------|
| 31 | `31-geological-symbols.md` | 地质符号库与花纹体系：岩性/相花纹、井别符号、线型规范、图例自动生成、符号选择器 | 新功能 |
| 32 | `32-well-tops-editor.md` | 井分层编辑与质量：分层表编辑器、批量修正、校验器、版本化回滚、导入合并、下游失效 | 功能补完 |
| 33 | `33-batch-jobqueue.md` | 批处理与作业队列：层位×方法批次定义、调度、断点恢复、进度报告、失败隔离、编排 UI | 新功能 |
| 34 | `34-well-siting.md` | 新井部署辅助：覆盖空洞诊断、候选点位、方案评估对比、planned 实体隔离、导出 | 新功能 |
| 35 | `35-sedimentary-evolution.md` | 沉积体系多期演化：相邻期对比引擎、迁移矢量、演化剖面、多期动览、演化报告 | 新功能 |

### 第八批（新增，2026-10-05 审计驱动：优化/完善/提升十方向，按开发量升序）

| # | 文件 | 方向 | 性质 | 预算（tokens/执行） |
|---|------|------|------|---------------------|
| 48 | `48-audit-closure.md` | 审计清账：AUDIT_ISSUES 未修 5 项 + 待证 15 项复核 + 假绿测试清零 | 债务清理 | 0.6–0.9 亿 / 1–2 天 |
| 49 | `49-arch-closure.md` | 架构收口：文档漂移修复（四头→六头等）+ checker 传递闭包/符号抽检/例外 ratchet | 架构治理 | 0.6–0.9 亿 / 1–1.5 天 |
| 50 | `50-io-robustness.md` | IO 稳健性：SEG-Y NaN/Inf 清洗、井哨兵词表、深度单位同义、BIZ-07/10/12 复核 | 债务清理 | 0.7–1.0 亿 / 1.5–2 天 |
| 51 | `51-ai-assist-upgrade.md` | AI 辅助面：Mock 路由接线退场 + LLM 地质对话助手骨架（流式 SSE）+ 阻塞解除 | 新功能 | 0.9–1.5 亿 / 2–3 天 |
| 52 | `52-test-deepening.md` | 测试深化：零测试头清零（atomicfile 优先）+ dataChanged 增量通道 | 质量提升 | 0.9–1.3 亿 / 2 天 |
| 53 | `53-zh-cn-l10n.md` | zh_CN 本地化：5,196 条翻译清零 + 术语表 + lrelease 产物链 + 门禁 | 质量提升 | 1.0–1.6 亿 / 1–2 天 |
| 54 | `54-errorhub.md` | 错误面统一：ErrorHub 服务 + 通知/模态/状态栏三级呈现 + 169 处弹框收敛 | 新功能 | 1.0–1.4 亿 / 2–3 天 |
| 55 | `55-seismic-svc-split.md` | seismictaskservice 5,041 行 God-service 拆分（API 零变更） | 重构 | 1.2–1.8 亿 / 3–4 天 |
| 56 | `56-shell-split.md` | 主窗壳层拆分：attach 3,816 + datalist 3,839 双巨兽解体（行为零变更） | 重构 | 1.2–1.8 亿 / 3–4 天 |
| 57 | `57-layer-split-dual.md` | 功能/数据层巨兽双拆：constraintfactorjobs 3,693 + dataimportservice 2,437 + PDF 阻塞解除 | 重构 | 1.6–2.4 亿 / 4–5 天 |

第八批并行纪律：48/49/50 文件面互不重叠可并行；51 独占 ai+appcontext；
52 与 51 在 remotepredictionservice 测试面上有移交注记；53 只动 translations/
与 CMake；54/55/56/57 均触 UI/服务大文件但拆分面互不重叠——与在飞
方向撞文件时按「谁先合谁为准，后者 rebase」处理。同机并行重构建建议
≤2 个会话（编译槽叠加），或各降 -j4。

## 使用方式

每个 prompt 直接喂给一个自治 agent 会话（如 devin/claude/zcode 新 session）：
它会自建 worktree 分支、按 ledger 协议迭代到 Oracle 全绿、push + 开 PR 收尾。
文件面互不重叠，可并行起多个；20 与 single-factor 迭代面有交集，宜等其收官或隔离实现。
第五批（24–28）同样互不重叠可全并行；catalog 词表与层位序两处为潜在共触面，
各方向按「最小改动面」约束避让。
