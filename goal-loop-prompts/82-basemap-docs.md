# Goal-Loop 方向 82：地震窗聚类与测区面文档化补账——零文档新增面立账 + 证据补全

## 背景（实测事实，勿再勘察；行号为 2026-10-08 master `e3c8d31d` 口径）

10-07/10-08 实战系新增了约 2,000+ 行核心逻辑**不在任何方向
任务书内、零 progress 文档、零账本**（三轮盘点的 W 组清单）。
文档是 agent 协作的契约面——这批面会被后续 prompt/会话当
「未知领域」误判。本方向立账补文档 + 补证据：

1. **`src/services/seismichorizoncluster.{h,cpp}`**（40398d20
   新建 196 行、f0e7b10d 重写 +188）——地震窗聚类服务；
   **零设计文档**；与方向 23/43（属性体）及交会分类
  （projectclassifier）的关系未对账；「直接按原始层位提取
   地震窗」的取窗契约（f0e7b10d 修复点）无记录。
2. **`src/metadata/paleoprojectfile.{h,cpp}`**（#261 新建
   +238、3e8988cd 再扩 +32）——工程文件新契约（配准节/
   mapreference）；仅随提交带 docs/PROJECT_FILE_DESIGN.md
   +34 行；与方向 16 catalog-sqlite 的 schema 关系未对账。
3. **`src/qgis/projectmapreference.{h,cpp}`**（+212，3e8988cd）
   ——仿射→PROJ pipeline（affine + 逆等距圆柱 R=6378137）、
   三目标 CRS 注册、正反向往返校验 ≤0.001（projectmapreference.cpp:60-118）；
   仅 PROJECT_AREA_PLAN.md +7 行。
4. **`src/qgis/qgisprojectservice_async.cpp`**（+268）、
   `src/services/previewdoc_survey.cpp`（+58）——异步工程
   服务面，零文档。
5. **离线底图链路**（3e8988cd + vendor/basemap/fetch-tiles.py
   + 10cdb8cb）——有 README 合规节但无 progress 主文档；
   源管理/缓存策略/坐标系范围散记。
6. **数据导航树四组重构**（#262 61fdd897）——零文档，重构
   后连补多次（#269/#270/#12346066）；分组词表（顶级五组 +
   计划井/标签条件组）只活在代码里。
7. **SEG-Y 方言补齐**（e9d17e80，+186/-76 号域语义）——零文档。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\basemap-docs -b goal/basemap-docs-20261009 origin/master
cd .worktrees\basemap-docs
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。

## 目标形态（建议按序）

1. **八面各立 progress 文档**：`docs/progress/` 按既有风格
  （What/口径/证据/递延四段）——seismichorizoncluster.md、
   project-file-mapreference.md（2/3 合篇）、
   qgisprojectservice-async.md、offline-basemap.md、
   datanav-tree.md、segy-dialects.md。
2. **聚类证据补全（唯一标「证据最弱」的域）**：读
   seismichorizoncluster 实现定算法口径（取窗契约/聚类参数/
   号域语义）；与 projectclassifier 的关系定案（复用同一
   分类核还是独立链——rg 消费点核验）；写进文档；若发现
   与既有方向的语义冲突（号域 vs 原始层位）记 TODOS。
3. **交叉对账**：新面 × 既有方向交集清单（paleoprojectfile×
   方向 16、horizoncluster×方向 23/43、底图×配准×方向 12
   时深）——每对给「正交/依赖/潜在冲突」结论。
4. **补测试锚**（轻）：文档化的关键契约若无测试锚，最小
   补充——取窗契约（f0e7b10d 语义）与 projectmapreference
   往返校验已有测试则引用，无则补最小用例（不追全覆盖）。
5. **README/AGENTS 挂钩**：goal-loop-prompts/README 九批表
   后加「实战系补账注记」；docs/progress 索引（若有）同步。

## 通用纪律（方向内全程有效）

- **分层**：文档 + 最小测试锚；src/ 改动仅限补测试锚（不重构
  不修 bug——发现的问题记 TODOS）。
- **诚实面**：推测与证据分离标注（聚类算法口径若读不透，
  写「待证」清单而非编造）；递延照实登记 TODOS。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行。
- **无人值守**：文档结构与对账结论自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-basemap-docs.md`。
- **多轮 review（硬要求）**：每批 → 文档事实核对（逐条对
  源码）→ diff 自审（证据真实/对账完整/测试锚最小/风格
  一致/TODOS 同步 五维）→ 修复 → 再 review，至少两轮零
  High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 八面文档齐（docs/progress 新增 ≥5 篇），每篇四段结构、
   事实有文件:行号支撑（抽检比对）。
2. 聚类口径定案：算法入口/参数/取窗契约成文；与
   projectclassifier 关系结论（复用或独立）有 rg 证据。
3. 交叉对账：≥3 对交集有「正交/依赖/冲突」结论；冲突项
   进 TODOS。
4. 测试锚：取窗契约与往返校验有测试引用（新增或既有）；
   新测试绿。
5. README 挂钩与 progress 索引同步；layering 三档绿；
   全量 ctest 对照 R0 红集合 diff 为空。
