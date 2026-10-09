# Goal-Loop 方向 76：双 ErrorHub 合流——状态栏徽标接线修正 + 54 系呈现件归并

## 背景（实测事实，勿再勘察；行号为 2026-10-08 master `e3c8d31d` 口径）

三轮盘点**最重要的新发现**：`src/services/errorhub.h` 同文件
**两个同名类并存**，且装配错位：

1. **全局 `::ErrorHub`**（`src/services/errorhub.h:30`，方向 64
   交付）——AppContext 构造并 `installGlobal`（`src/app/
   appcontext.cpp:181-183`）；真实上报全走它：全仓
   `PaleoNotify::*` 调用 144 处经 `ErrorHub::global()` 入账
  （`src/ui/notifications/paleonotify.cpp:14-16,50,62,74,84`）。
2. **`paleo::services::ErrorHub`**（`src/services/errorhub.h:171`，
   方向 54/#251 并行实现）——头注释自认「与上方全局 ErrorHub
   并存……合并期保留两套」（`:106-108`）。其呈现栈**从未被
   装配**：`new NotificationManager` 全仓 0 处调用、
   `new ErrorHistoryDock` 0 处；外部生产者 ≈ 0。
3. **接线错位（功能后果）**：主窗口状态栏错误徽标订阅的是
   `paleo::services::ErrorHub::instance()`（`src/ui/
   paleomainwindow.cpp:999-1013`）——该 hub `count()` 恒 0 →
   `m_statusErrorBtn` 永久隐藏（构造时 `setVisible(false)`，
   paleomainwindow.cpp:953）。徽标点击打开的「错误历史」dock
   反而接的是全局 hub（`attachErrorHub`，paleomainwindow_
   attach_shell.cpp:648-660）——功能可达但计数信号接错对象。
4. `ErrorHistoryDock`/`ErrorHistoryModel`（方向 54 呈现件，
   `src/ui/notifications/errorhistorydock.cpp:27` 自连
   `paleo::services::ErrorHub::instance()`）在装配中无构造点，
   属死代码/仅测试可达。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\hub-merge -b goal/hub-merge-20261009 origin/master
cd .worktrees\hub-merge
./paleo-dev.ps1 build     # localdeps 统一链（Qt 6.11.2），自动 env
./paleo-dev.ps1 test      # 沙箱监狱 TEMP/TMP 已内置
```

等价手工接线见 BUILDING.md:140-152（统一口径四元组 + PATH
前置 + GDAL_DATA/PROJ_LIB；**旧 C:/deps/Qt/6.8.0 接线已退役**）。

## 目标形态（建议按序）

1. **R0 消费面全量核对**：两 hub 的生产者/消费者逐文件清单
  （`rg "ErrorHub::global|PaleoNotify|paleo::services::ErrorHub|
   NotificationManager|ErrorHistoryDock" src tests` 全量）——
   定案合流方向（推荐：全局 `::ErrorHub` 为唯一真源，
   `paleo::services::ErrorHub` 的能力（historyChanged/
   historyCleared 信号、去重窗语义若有差异）并入后删除）。
2. **徽标接线修正**：`paleomainwindow.cpp:999-1013` 改订阅
   `ErrorHub::global()`（null 降级路径保留）；徽标计数/隐藏
   语义与「错误历史」dock 同源。
3. **54 系呈现件归并**：`NotificationManager`/`ErrorHistoryDock`/
   `ErrorHistoryModel` 若与方向 64 的 `paleo::notifications`
   面重复——删除（含其测试）；若有独有能力（如 ErrorHistoryDock
   的某视图语义）先迁入 64 面再删；决策逐件记 ledger。
4. **测试迁移**：tst_errorhub/_d54/_challenger_m4/_m5_harness/
   _storm/tst_error_history_dock 等按归属重定向到全局 hub；
   被删呈现件的测试随之删除（删测试也要 mutation 验证被测
   语义仍被覆盖——避免覆盖回退）。
5. **文档**：errorhub.h 头注释「合并期保留两套」段改写为合流
   后契约；方向 54/64 账本尾部加合流注记互相指向。

## 通用纪律（方向内全程有效）

- **分层**：合流后唯一 hub 留 `src/services`（数据层，无
  QtWidgets）；呈现件在 `src/ui/notifications`；`check_layering.py
  --strict` 绿。
- **行为红线**：用户可见行为（通知卡/模态/历史面板）合流前后
  一致——徽标从「永久隐藏」变「真实计数」是**修复**而非行为
  变更，单独记档；其余零变化。
- **诚实面**：删除死代码逐件列清单入 ledger（每件给零消费
  证据）；不加新能力。
- **资源**：`./paleo-dev.ps1` 系（-j8 上限内）；ctest 串行。
- **无人值守**：归并/删除取舍自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-hub-merge.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/行为对照/删除证据/测试覆盖不回退/文档同步 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 唯一真源：`src/services/errorhub.h` 单类（rg 证据）；
   `paleo::services::ErrorHub` 零残留；`errorhub.h:106-108`
   「两套并存」注释删除。
2. 徽标修复：状态栏徽标订阅全局 hub（接线证据）；注入测试
   错误 → count > 0 → 徽标可见（offscreen 测试）。
3. 消费面守恒：PaleoNotify 144 处调用零改动（diff 证据——
   上报路径不动，只动装配与呈现件）。
4. 死代码清零：删除件清单（每件零消费 rg 证据）；被删测试的
   语义覆盖迁移记录。
5. 测试：errorhub 系测试重定向后全绿（含 storm 的
   RUN_SERIAL 布线保留）；全量 ctest 对照 R0 红集合 diff 为空。
6. layering 三档绿；check_i18n 绿。
