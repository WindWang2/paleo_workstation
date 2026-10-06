# Goal-Loop 方向 64：错误面统一——ErrorHub 错误通道 + 通知/模态/状态栏三级呈现（方向 54 修订重发）

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

**方向 54（errorhub）从未启动**——原任务书
`goal-loop-prompts/54-errorhub.md` 的审计基线至今零消减，本
方向为修订重发（背景数据刷新 + 与八批合并结果对齐）：

- **UI 层 QMessageBox 169 处**（`rg -o "QMessageBox::\w+"
  src/ui -g "*.cpp"` 调用点口径）与 10-05 完全相同：warning 71 /
  information 24 / question 12 / critical 8（其余为按钮枚举）。
- **Top 集中区（刷新后）**：`welltops/welltopseditordialog.cpp`
  **24**、`paleomainwindow.cpp` 13、`seismicsection/
  seismicsectiondockwidget.cpp` 11、`paleomainwindow_attach_
  mapping.cpp` 11、`edittools/editingtoolbar.cpp` 10、
  `pages/wellsitingpanel.cpp` 9、`paleomainwindow_attach_
  compose.cpp` 9、`seismicsection/seismicpickpanel.cpp` 9。
  attach 家族拆分后总量守恒（7+2+9+1+11=30，分散进 5 个新
  TU——shell-split 未动错误面，符合其零变更承诺）。
- **无统一错误通道**：错误经 `QString *error` 出参 + statusBar
  （`paleomainwindow.cpp:424` showMessage）——同类错误连弹
  N 框、无聚合、无静默期、无错误历史。
- **专项通道各自为政**：crashreport / cataloghealth /
  taskpanel / batchjobpanel 互不复用。

**注意分工**：本方向只做错误呈现收敛；seismicsection 家族的
**文件拆分**归方向 65（两方向若并行，65 先行——拆完再迁弹框
避免同文件两轮大改；或按「谁先合谁为准」处理）。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\errorhub -b goal/errorhub-20261007 origin/master
cd .worktrees\errorhub
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。UI 测试 offscreen。

## 目标形态（建议按序）

1. **ErrorHub 服务**：`src/services/errorhub.{h,cpp}`（数据层，
  无 QtWidgets）——错误条目（级别 error/warning/info、来源域、
  时间戳、去重键、聚合计数）、环形历史（上限 500）、查询面、
  `errorRaised` 信号。
2. **呈现层**：`src/ui/notifications/`——非模态通知卡（右下
  角堆叠，分级消失时长）+ 严重错误模态（同去重键 60s 单弹）+
  状态栏 info 级。DESIGN.md 补「错误呈现」节。
3. **调用点收敛**：169 处按集中区降序迁移（welltopseditor 24
   → mainwindow 13 → seismicsectiondock 11（**若方向 65 已合
   则按拆后 TU 定位**）→ attach_mapping 11 → …），每批 ctest
   绿；错误文本/时机/失败语义逐点不变。
4. **错误历史面板**：环形历史查看（过滤/复制/清空），挂
  「视图」菜单。
5. **测试**：ErrorHub 单测（去重/聚合/环形上限/分级路由边界）；
   呈现 offscreen 测试（出现/消失/堆叠上限）；错误风暴
  （100 条/秒×10s）不冻结主线程。
6. **审计联动**：AUDIT 里若有 QMessageBox 相关条目同步终态。
7. **防误发注记**：原方向 54 任务书（`54-errorhub.md`）头部
   加「已修订重发为方向 64」注记（先例：69 对 38 的处理）。

## 通用纪律（方向内全程有效）

- **分层**：ErrorHub 归 services（无 QtWidgets），呈现归
  ui/notifications，装配 appcontext；`check_layering.py --strict` 绿。
- **行为保留**：错误文本/时机/失败语义逐点不变——只改呈现
  通道；每批迁移附行为对照记 ledger。
- **DESIGN.md**：通知卡视觉先补节再实现。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：模态保留清单（哪些场景保留模态及理由）自行
  裁决记 ledger。
- **性能断言**：通知路径无每帧 alloc；风暴场景用事件处理
  计数断言（禁毫秒墙钟）。
- **ledger**：`.goal-loop-ledger-errorhub.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/行为保留/DESIGN 一致/去重语义/i18n 五维）→ 修复 →
  再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. ErrorHub 单测：去重键 60s 窗口/聚合计数/环形逐出/分级路由
   全绿（含 0 条/上限+1/同键并发边界）。
2. 通知呈现：offscreen 通知卡出现/到时消失/同键不重复弹；
   模态保留清单入 ledger。
3. 调用点收敛：`rg -o "QMessageBox::\w+" src/ui -g "*.cpp"`
   终态 ≤35（169 的 20%，保留项为模态策略清单内）；每处迁移
   有行为对照。
4. 错误历史：过滤/复制/清空可用；环形上限逐出（测试）。
5. 错误风暴：100 条/秒×10s 注入，事件循环处理计数正常，
   通知堆叠不溢出。
6. 全量 ctest 对照 R0 红集合 diff 为空；check_i18n 绿；
   DESIGN.md「错误呈现」节与实现一致。
