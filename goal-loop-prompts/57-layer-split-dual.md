# Goal-Loop 方向 57：功能层与数据层巨兽拆分——constraintfactorjobs 3,693 + dataimportservice 2,437 双文件解体

## 背景（实测事实，勿再勘察；行号为 2026-10-05 master `adf2be7` 口径）

两个中等体量巨兽，同属「方向 20 拆分模式未推及的下一站」，合一个
方向收口（文件面零重叠，可串行两阶段）：

**A：`src/workflow/constraintfactorjobs.cpp` 3,693 行**——功能层
最大文件（workflow 层 88 文件 27,568 行中的 13%）。单因素
约束×因素批作业编排：作业构建/参数解析/分域执行/结果收集/
失败隔离都在一个 TU。双高信号：体量 Top4 + TODO 命中 16 处
——膨胀与递延同步累积区。语义红线：失败隔离（单因素失败
不拖垮批次）、进度上报节流、取消检查点（batch-jobqueue
方向 33 口径）。

**B：`src/io/dataimportservice.cpp` 2,437 行**——io 层最大文件。
多格式导入编排（井表/坐标/分层/时深/LAS/SEG-Y/GeoJSON/图片/
PDF）+ catalog 登记挤一个类。**附加问题**：`:152`
`m_pdfProc->waitForFinished(2000)` 主线程阻塞等 PDF 进程
（位于 `:149-163` kill() 后清理段——交互频率 R0 确认所在函数
调用链后定性）。

**共同先例**：方向 20 `workflows.cpp` 4,086→153；方向 41
sf-kriging 的 `cmake/extra-sf-kriging.cmake` 挂接模式；importledger
失败诚实面（方向 44 口径）。**共同消费约束**：公共 API 不变，
消费方零改动。

**协调面**：方向 50（io-robustness）触 segyreader/wellfileparsers
（读核），本方向触 dataimportservice（编排壳）——文件不重叠；
若撞 importledger 消费按「谁先合谁为准」。

## 环境接线（Windows 本机实测口径，源 goal/sf-kriging 账本 R0）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\layer-split -b goal/layer-split-20261006 origin/master
cd .worktrees\layer-split
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 48。

## 预算（开发量）

- **Agent tokens**：上限 3 亿，预计 1.6–2.4 亿（双文件搬迁 +
  两域回归面）。
- **执行花费**：18–24 轮构建+测试（factorworkflow/mappingpages/
  singlefactor 族 + folderimport/dataimport 族），预计墙钟
  4–5 个工作日会话。**本批最大单方向**。

## 目标形态（建议按序；A 阶段→全绿→B 阶段→全绿）

1. **A·R0 结构勘察**：constraintfactorjobs 按函数族盘点（作业
   构建/参数域解析/执行编排/结果收集/失败隔离/进度上报）画
   段位图与共享状态依赖；16 处 TODO 逐条定性（过时的删、真实
   的归 TODOS.md）；切分线记 ledger 定案。
2. **A·公共 API 冻结 + 逐族搬迁**：头公共区 diff 对拍零变更；
   先无状态族（参数解析）后有状态族；预计 5–7 个 TU
  （constraintfactorjobs_<域>.cpp）；每步成图域测试绿。
3. **A·收口**：主文件 ≤800 行；TU 各 ≤1,000 行；批作业吞吐
   比率门不回退（对照 R0）。
4. **B·R0 结构勘察**：dataimportservice 按格式族盘点画段位图；
   登记路径与 importledger 交互列表；切分线定案。
5. **B·逐族搬迁**：每格式族一 TU（dataimport_<族>.cpp），共享
   登记基座（catalog 写入/importledger 记账/进度上报）提取；
   每步导入测试绿。
6. **B·阻塞解除**：PDF waitForFinished 改异步回调
  （QProcess::finished 信号链）；超时/失败语义保留并立测试。
6. **总收口**：两主文件各 ≤800 行；CMake 源清单更新；include
   纪律（族间经公共基座头）。（io 层旧式 SLOT() 宏实为零残留——
   2026-10-05 复核全仓 6 处均在 ui 层，归方向 48 清理，本方向
   不涉；dataimportservice.cpp:110/:1189 的 `assignResolvedWellLogSlot`
   是井日志主标记函数非 connect 宏，勿误改。）

## 通用纪律（方向内全程有效）

- **分层**：A 留在 `src/workflow/`（`// 层：功能`），B 留在
  `src/io/`（`// 层：数据`）；两目录均无 QtWidgets 无 ui
  include；`check_layering.py --strict` 绿。
- **行为保留红线**：失败隔离/进度节流/取消/导入登记/ledger
  记账语义逐条不变；PDF 异步化是显式授权的行为变更（阻塞→
  回调），语义等价单独立测试；「顺手改进」违规记 TODOS。
- **诚实面**：importledger 记账路径全保留，零静默丢弃。
- **资源**：`-j8`；ctest 串行；全量构建后再 ctest（陈旧链接
  假红教训）。
- **无人值守**：切分线与 TODO 定性自行定案记 ledger。
- **性能断言**：批作业吞吐/导入吞吐比率门（禁绝对毫秒）。
- **ledger**：`.goal-loop-ledger-layer-split.md`（A/B 两阶段
  分节记）。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（API
  等价/语义保留/异步正确性/TU 边界/无死代码 五维）→ 修复 →
  再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. API 等价（双文件）：头公共区 diff 零变更；消费方零改动
  （rg 证据）。
2. 体量：两主文件各 ≤800 行；新 TU 各 ≤1,000 行（wc 证据入
   ledger）。
3. A 语义：失败隔离/取消/进度节流回归用例零改动通过；TODO
   16 处终态清单入 ledger；批作业吞吐比率无 >10% 劣化。
4. B 异步：PDF 导入路径非阻塞（事件循环断言）；超时/失败面
   回归用例；本文件 waitForFinished/QEventLoop 零命中（rg）。
5. 回归：成图域 + 导入域测试全绿两遍；全量 ctest 无新增失败
  （对照 R0 基线红清单）；check_layering 三档绿；构建无新
  警告；git diff --check 干净。
