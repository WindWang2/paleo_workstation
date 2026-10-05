# Goal-Loop 方向 56：主窗壳层拆分——attach 3,816 + datalist 3,839 双巨兽解体

## 背景（实测事实，勿再勘察；行号为 2026-10-05 master `adf2be7` 口径）

视图层 279 文件 / 96,029 行（占全仓 43.2%），其中两座 UI 巨兽：

- `src/ui/paleomainwindow_attach.cpp` **3,816 行**——W4 壳瘦身产物
  （attachWorkflows 按页拆 + attachMapping 三段），但每页 attach
  函数仍在单 TU 里继续膨胀（每新页/新面板都往里加）。
- `src/ui/pages/datalist.cpp` **3,839 行**——P3 数据操作重构产物
  （多选/过滤/拖拽/批量/撤销，按 OPERATIONS.md 矩阵实现）——
  操作矩阵每扩一条就长一段。
- **拆分先例**：同文件族 `paleomainwindow.cpp` 本体 1,897 行（§42
  shell 六页 ribbon+层树+面板+日志）+ `_workbench` 712 + `_sections`
  250 + `_faults` 27——「主窗按页/域拆 TU + 聚合头」模式仓内已
  验证（W4 把主窗 3,386→1,349 行）；方向 20 workflows.cpp 4,086→153
  是功能层同款。
- **消费关系**：attach TU 内函数是 PaleoMainWindow 的成员函数
  实现拆片——类定义不动，拆的是实现文件；datalist 是
  DataListWidget 类实现——同类拆 TU 需谨慎（成员函数可拆文件，
  私有类型/辅助类可提独立头）。
- **相邻协调面**：`paleomainwindow_attach.cpp` 30 处 QMessageBox
  ——若方向 54（ErrorHub）在飞，按「谁先合谁为准」处理，不
  丢弃对方改动；datalist 的 QMessageBox 同理。

## 环境接线（Windows 本机实测口径，源 goal/sf-kriging 账本 R0）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\shell-split -b goal/shell-split-20261006 origin/master
cd .worktrees\shell-split
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 48。UI 测试 offscreen（tst_panels/tst_shells 先例）。

## 预算（开发量）

- **Agent tokens**：上限 3 亿，预计 1.2–1.8 亿（双巨兽搬迁 + 壳层
  测试面广）。
- **执行花费**：15–20 轮构建+测试，预计墙钟 3–4 个工作日会话。

## 目标形态（建议按序）

1. **R0 结构勘察**：attach 按函数族盘点（attachDataPage/
   attachPredict/attachMapping/…）画段位图；datalist 按操作矩阵
   域盘点（选择/过滤/拖拽/批量/撤销）——每域的行区间+外部耦合
  （信号连接/跨页调用）列表；切分线记 ledger 定案。
2. **attach 拆分**：按页域拆 TU（先例命名：paleomainwindow_attach_
   <域>.cpp，如 _data/_predict/_mapping/_compose/…）+ 聚合头；
   每 TU ≤900 行目标；类定义与公共 API 零变更。
3. **datalist 拆分**：操作域拆 TU（datalist_<域>.cpp：_select/
   _filter/_dnd/_batch/_undo/…）+ 辅助类型提独立头（datalistops.h
   之类，若先例已有则扩展）；每 TU ≤900 行。
4. **行为红线对拍**：拆分前后壳层测试（tst_panels/tst_shells/
   tst_datalist 若有）全绿；信号连接序不变的抽查断言（关键页
   attach 顺序——构造序影响初始化语义的场景记档证明等价）。
5. **收口**：两巨兽 ≤1,000 行（聚合+胶水）；CMake 源清单更新；
   include 纪律（域间不互 include 实现细节，经公共头）。

## 通用纪律（方向内全程有效）

- **分层**：全部留在 `src/ui/`（视图层）；新文件头三行
  `// 层：视图`；`check_layering.py --strict` 绿。
- **行为保留红线**：类定义/公共 API/信号序/初始化序逐条不变；
   「顺手改进」违规，改进想法记 TODOS。
- **UI 纪律**：纯结构拆分零视觉变更——不触 DESIGN.md token；
   截图对照不适用（无视觉 diff 是验收项）。
- **资源**：`-j8`；ctest 串行；全量构建后再 ctest（陈旧链接
   假红教训）。
- **无人值守**：切分线自行定案记 ledger；与 52/53 并行在飞时
  冲突按语义合。
- **ledger**：`.goal-loop-ledger-shell-split.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（API
   等价/信号序/include 纪律/TU 边界/无死代码 五维）→ 修复 →
   再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 类零变更：PaleoMainWindow/DataListWidget 类定义（公共区+信号）
   拆分前后 diff 零变更；消费方零改动（rg 证据）。
2. 体量：paleomainwindow_attach.cpp 与 datalist.cpp 各 ≤1,000
   行；新 TU 各 ≤900 行；无新文件 >1,200 行（wc 证据）。
3. 行为等价：壳层/数据页既有测试全绿两遍；关键信号连接序
   抽查断言（≥5 页域）通过。
4. 全量 ctest 两遍无新增失败（对照 R0 基线红清单）；全量构建
   后再 ctest 证据入 ledger。
5. check_layering 三档绿；构建无新警告；git diff --check 干净；
   include 纪律（域间零实现互 include，rg 证据）。
