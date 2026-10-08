# Goal-Loop 方向 83：paleomainwindow 壳层收尾——本体 2,137 行二次拆分 + 装配根归并审视

## 背景（实测事实，勿再勘察；行号为 2026-10-08 master `e3c8d31d` 口径）

方向 56 拆了 attach 家族（3,816→702），但**主窗本体未再分**，
且实战系提交持续往装配根堆料：

- `src/ui/paleomainwindow.cpp` **2,137 行**（当前全仓第一；
  十方向合并后从 1,780 涨回）——§42 shell 六页 ribbon +
  层树 + 面板 + 底部日志/任务 + 快捷键（方向 63 收编后
  `:1091-1103` Ctrl+1..N/Ctrl+Tab 注册）+ ErrorHub 装配
  （`:953-1013`）+ locator（Ctrl+K）。
- 家族全景：本体 2,137 + `_workbench` 850 + `_shell` 668 +
  `_sections` 250 + `_faults` 27 + attach 族 10 个 TU（740/
  704/632/489/388/385/343/323/310/206/161/36）——**16 文件
  合计 8,399 行**仍挤一个类。
- **装配根膨胀**：`src/app/appcontext.cpp` 被 4 个实战提交
  连触（#261/#40398d20/3e8988cd/#12346066）——AI 装配
  （aiwiring bindChatToolRunner）、底图、配准、图片道、
  ErrorHub 全在往里加；`src/app/` 现含 crossplotcontroller/
  aiwiring/mkproject（619 行）——组装根职责清单未审视。

**拆分先例**：方向 56 attach 按页域拆 TU（类定义零变更）；
方向 55 seismictaskservice 五 TU + internal.h；方向 65
canvas 2,192→297（12 TU + 2 内部契约头）——模式成熟。

**协调面**：方向 63 的 ShortcutRegistry 与方向 64 的
ErrorHub 装配都在主窗——本方向纯拆 TU 不动装配语义；
与在飞方向撞 paleomainwindow 按「谁先合谁为准」。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\mainwindow-split2 -b goal/mainwindow-split2-20261009 origin/master
cd .worktrees\mainwindow-split2
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。UI 测试 offscreen
（tst_panels/tst_shortcuts_shell/tst_notifications 全是主窗
回归面）。

## 目标形态（建议按序）

1. **R0 结构勘察**：本体 2,137 行按函数族盘点（ribbon 构建/
   页切换/层树接线/底部 dock 族/快捷键注册/状态栏/ErrorHub/
   locator）画段位图；16 文件家族全景图（谁该并入谁、
   谁该新拆）；切分线记 ledger 定案。
2. **公共 API 冻结**：PaleoMainWindow 类定义公共区 diff 对拍
   零变更；消费方零改动（rg 证据）。
3. **本体拆分**：按域拆 TU（paleomainwindow_<域>.cpp：
   _ribbon/_pages/_docks/_shortcuts/_status/…）——每 TU
   ≤700 行目标；快捷键注册/ErrorHub/locator 各自成域
  （方向 63/64 的装配面独立可测）。
4. **装配根审视**：appcontext.cpp 职责清单化——AI 装配已
   在 aiwiring（好先例），评估底图/配准/图片道装配是否
   各立 wiring 文件（app 内新 TU，不动 appcontext 类接口）；
   产出「组装根瘦身」最小方案（不追全量重构）。
5. **行为红线对拍**：tst_panels/tst_shortcuts_shell/
   tst_notifications/tst_uxtheme 族零改动通过；信号连接序
   抽查断言（≥5 页域）。
6. **收口**：本体 ≤1,200 行；家族文件各 ≤900 行；include
   纪律；拆分候选清单（若有未拆的）记 TODOS。

## 通用纪律（方向内全程有效）

- **分层**：全部留 `src/ui/` + `src/app/`（视图层/组装根）；
  新文件头三行 `// 层：<词表>`；`check_layering.py --strict` 绿。
- **行为保留红线**：类定义/公共 API/信号序/初始化序/键位
  逐条不变；「顺手改进」违规记 TODOS。
- **测试口径**：壳族测试每批全绿；全量构建后再 ctest；
   offscreen 截图对照不适用（无视觉 diff 是验收项）。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行。
- **无人值守**：切分线自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-mainwindow-split2.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （API 等价/信号序/装配语义/TU 边界/无死代码 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 类零变更：PaleoMainWindow 公共区 diff 零变更；消费方
   零改动（rg 证据）。
2. 体量：本体 ≤1,200 行；新 TU 各 ≤700 行；家族最大文件
   不超本体（wc 证据入 ledger）。
3. 行为等价：壳族测试（panels/shortcuts_shell/notifications/
   uxtheme）零改动通过×2 遍；信号连接序抽查断言通过。
4. 装配根：appcontext 职责清单 + wiring 拆分方案入 ledger
  （最小落地或如实递延）。
5. 全量 ctest 对照 R0 红集合 diff 为空；layering 三档绿；
   构建无新警告；git diff --check 干净。
