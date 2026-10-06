# Goal-Loop 方向 54：错误面统一——ErrorHub 错误通道 + 状态栏/通知收敛

## 背景（实测事实，勿再勘察；行号为 2026-10-05 master `adf2be7` 口径）

- **UI 层 QMessageBox 169 处**（`rg -o "QMessageBox::\w+" src/ui -g "*.cpp"`
  调用点口径）：集中区 `paleomainwindow_attach.cpp` 30、
  `welltops/welltopseditordialog.cpp` 24、`paleomainwindow.cpp` 14、
  `seismicsection/seismicsectiondockwidget.cpp` 11、
  `seismicsection/seismicpickpanel.cpp` 9、`pages/wellsitingpanel.cpp` 9。
- **无统一错误通道**：错误经 `QString *error` 出参 + 结构体 error
  字段（分层正确），但到 UI 后逐点弹框 + statusBar（`paleomainwindow.cpp:424`
  `showMessage(reason, 8000)`）——同类错误（导入失败×N 个文件）
  会连弹 N 个框，无聚合、无静默期、无错误历史。
- **专项通道各自为政**：crashreport（信号处理器+脏退出检测）、
  cataloghealth（→ dialog + datalist 消费）、taskpanel/batchjobpanel
  （任务进度）——互不复用。
- **结构化日志**：qInfo/qWarning 101 处；专用前缀仅
  `PALEO-SEISMIC-TRANSCODE`——无统一 logging facade。

本方向做「错误面收敛」：一个 ErrorHub 服务（视图层可见的唯一错误
汇聚点），弹框策略集中化（聚合/去重/分级），错误历史可查。**不是**
重写业务错误语义——底层 `QString *error` 出参契约不动。

## 环境接线（Windows 本机实测口径，源 goal/sf-kriging 账本 R0）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\errorhub -b goal/errorhub-20261006 origin/master
cd .worktrees\errorhub
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 48。UI 测试用 offscreen（先例：tst_panels）。

## 预算（开发量）

- **Agent tokens**：上限 3 亿，预计 1.0–1.4 亿（169 处调用点收敛
  是逐点机械改造 + 新服务 + 测试）。
- **执行花费**：12–16 轮构建+测试，预计墙钟 2–3 个工作日会话。

## 目标形态（建议按序）

1. **ErrorHub 服务**：`src/services/errorhub.{h,cpp}`（数据层，
  无 QtWidgets）——错误条目（分级 error/warning/info、来源域、
  时间戳、去重键、聚合计数）、环形历史（上限如 500，防无界增长）、
   查询面（按域/级别过滤）。信号 `errorRaised` 供 UI 订阅。
2. **呈现层**：`src/ui/notifications/`（视图层）——非模态通知
   （右下角堆叠卡片，自动消失分级时长）+ 严重错误才弹模态框
   （策略：同去重键 60s 内只弹一次）+ 状态栏轻量集成（statusBar
   通道保留为 info 级）。DESIGN.md token 遵守。
3. **调用点收敛**：169 处 QMessageBox 按语义分流——致命/需决策
   →模态（走 ErrorHub 策略）；可恢复/后台 →通知卡；信息 →状态栏。
   逐文件迁移按集中区降序（attach 30 → welltopseditor 24 →
   mainwindow 14 → seismicsectiondock 11 → …），保持每批 ctest 绿。
4. **错误历史面板**：ErrorHub 环形历史的查看 dock/面板（过滤/
   复制/清空），挂主窗「视图」菜单。
5. **测试**：ErrorHub 单元测试（去重/聚合/环形上限/分级路由）；
   呈现层 offscreen 测试（通知出现/消失/堆叠上限）；抽样调用点
   行为保留断言（原来弹框的路径现在走通知——信号断言）。
6. **文档**：DESIGN.md 补「错误呈现」节（通知/模态/状态栏三级
   口径 + token），tst_panels 新增用例走新口径。

## 通用纪律（方向内全程有效）

- **分层**：ErrorHub 归 services（数据层，无 QtWidgets）；呈现归
  ui/notifications；装配 appcontext；`check_layering.py --strict` 绿。
- **行为保留**：错误文本、错误时机、失败态语义逐点不变——只改
  呈现通道；每批迁移附「迁移前后行为对照」记 ledger。
- **DESIGN.md**：通知卡视觉先提案 token（配色/圆角/动效时长）
  再实现——视觉决策读 DESIGN.md，无对应节则本方向补节并守之。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：弹框策略分级边界自行裁决记 ledger（记「哪些
  场景保留模态」清单及理由）。
- **性能断言**：通知路径无每帧 alloc；错误风暴（100 条/秒注入）
  下 UI 不冻结（计数断言，非毫秒墙钟）。
- **ledger**：`.goal-loop-ledger-errorhub.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/
  行为保留/DESIGN 一致/去重语义/i18n 五维）→ 修复 → 再 review，
  至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. ErrorHub 单测：去重键 60s 窗口、聚合计数、环形上限逐出、
   分级路由——全绿并覆盖边界（0 条/上限+1/同键并发）。
2. 通知呈现：offscreen 测试通知卡出现/到时消失/同键不重复弹；
   模态保留清单（及理由）入 ledger。
3. 调用点收敛：`rg -o "QMessageBox::\w+" src/ui -g "*.cpp"` 计数
   终态 ≤ 起始 169 的 20%（≤35，保留项为模态策略清单内）；每处迁移有行为对照记录。
4. 错误历史：面板可过滤/复制/清空；环形上限后旧条目逐出（测试）。
5. 错误风暴：100 条/秒 × 10 秒注入，主线程不阻塞（事件循环
   处理计数断言），通知堆叠有上限不溢出屏幕。
6. 回归：全量 ctest 两遍无新增失败；check_i18n 绿（新增文案
   tr() 全覆盖）；DESIGN.md「错误呈现」节与实现一致。
