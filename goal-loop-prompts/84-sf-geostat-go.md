# Goal-Loop 方向 84：方向 74 执行——克里金约束消费 + 协克里金接线（已有任务书，本方向为发车令）

## 背景（实测事实，勿再勘察；行号为 2026-10-08 master `e3c8d31d` 口径）

**方向 74 任务书已在仓**：`goal-loop-prompts/74-singlefactor-geostat.md`
（提交 74396674，2026-10-07 入仓），账面尚未开工（无对应
ledger/分支，PR 列表零 open）。本轮盘点确认其剩余面**全部
原样成立**，且价值升高——方向 73（#295）已把 UI 两族入口
立好，克里金下不生效成为单因素链**最实质的语义断点**：

- **克里金族不消费约束线**：`src/algorithms/singlefactor/
  krigingsurface.cpp:272-277` 仍逐条记
  `direction_guide_not_used_by_kriging` /
  `soft_boundary_not_used_by_kriging` /
  `well_cluster_locality_not_used_by_kriging`——同批约束在
  IDW 族是真消费，语义不对称。
- **协克里金仍是死代码**：`CoKrigingSolver`/
  `ConstrainedKrigingSolver`（方向 67 交付，
  `src/algorithms/geostat/cokriging.{h,cpp}`）只被自己的
  测试引用；排除 algorithms 目录后全仓 grep `cokriging`
  **零命中**（workflow/ui 无接线）。
- 方向 74 任务书已把方案空间写清（局部张量 vs 分区拟合）、
  ConstrainedKrigingSolver 接线或销账、逐硬隔断分量拟合
  核实、TODOS P2 对账——**本方向 = 按 74 任务书执行**。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\sf-geostat -b goal/sf-geostat-20261009 origin/master
cd .worktrees\sf-geostat
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。算法验证用解析解夹具
（先例：tst_singlefactor_faultpath / tst_geostat_cokriging）。

## 目标形态

**以 `goal-loop-prompts/74-singlefactor-geostat.md` 为准**——
本方向不发新任务书，74 的背景/目标形态/通用纪律/Oracle 全部
有效。执行要点重申（详文见 74 号文件）：

1. 方向线/软边界进克里金权重（局部张量或分区拟合，74 已
   写方案空间；`krigingsurface.cpp:272-277` 三条
   `*_not_used_by_kriging` issue 终态）。
2. 协克里金词表项 + workflow 接线（CoKrigingSolver 从死代码
   变可选用法；UI 需协变量选择面）。
3. ConstrainedKrigingSolver 接线或销账（若接线价值低，销账
   也要有论证）。
4. 逐硬隔断分量拟合核实（方向 67 交付的 barrier 感知
   fitVariogram 档与 74 要求的「逐硬隔断分量」对账）。
5. TODOS P2 对账收尾（`TODOS.md:105-107` 剩余项终态）。

## 通用纪律

沿用 74 号任务书全部纪律（分层/诚实面/解析解先行/禁绝对
毫秒/无人值守/ledger/多轮 review 两轮零 High/Medium/原子
提交），补充两点：

- **ledger 命名**：`.goal-loop-ledger-sf-geostat.md`（与
  分支 goal/sf-geostat-20261009 对应）。
- **基线**：origin/master e3c8d31d（74 号任务书写的 dc26c0eb
  之后的又一批合并——R0 先确认 #295 的 constraintpage/
  constraintwellfactors 增量与 74 方案无冲突；UI 面如需
  协变量选择，从方向 73 的两族布局里长出来，勿回退布局）。

## Oracle 验收

以 74 号任务书 Oracle 为准，补一条对账项：

- `krigingsurface.cpp` 三条 `*_not_used_by_kriging` issue
  终态（消费后消失或按方法档保留且有 UI 提示）；方向 73
  的 UI 链（井点提取→两族→插值→等值线）在克里金档端到端
  回归绿。
