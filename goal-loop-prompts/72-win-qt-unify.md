# Goal-Loop 方向 72：Windows 环境债根治——Qt 6.8/6.11 混链收敛，恢复本机全量回归可信度

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径，多账本交叉佐证）

**问题**：Windows 本机开发环境的 Qt 双版本混链——可执行文件
按 **Qt 6.8**（`C:/deps/Qt/6.8.0/msvc2022_64` 头/链接）编译，
但运行时加载的 QGIS 相关 DLL（`paleo-qgis-deps`）按
**Qt 6.11** 构建。两版 Qt 同进程混载导致确定性失败：

- 方向 57 账本 `:178-198`：全量回归 **185 红 / 293**——
  环境红，非代码红；R0 起即存在。
- 方向 51 账本 `:153-171`：5 个伞式测试 0xc0000139
  （ENTRYPOINT_NOT_FOUND），与无 AI 改动的基线分支逐项一致。
- 当前所有本机「全绿」结论只能靠**红集合 diff 法**承载
  （改前/改后红集合一致 = 无新增失败），无法直接跑绿。

**根因链**（BUILDING.md 与账本口径拼合）：QGIS prefix
（`C:/Users/wangj.KEVIN/paleo-qgis-prefix`）与依赖包
（`C:/Users/wangj.KEVIN/paleo-qgis-deps`，含 Qt 6.11 运行时）
来自 OSGeo4W 布局，其 Qt 与编译用 Qt 6.8 不同源；PATH 顺序
技巧（`paleo-qgis-deps/Library/bin` 前置）只是让进程能启动
（加载 6.11 的 Qt6Core），但 exe 的导入表按 6.8 链接——
ABI 混链下部分测试进程直接倒。

**目标**：统一编译与运行的 Qt 版本，让本机 ctest 能真正
全绿（或红集合收敛到沙箱已知的极小集），恢复「本机全量
回归」作为 goal-loop 验收口径的可信度——这是后续所有方向
验收质量的放大器。

## 环境接线（Windows 本机实测口径——本方向自身就是修这个）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\win-qt-unify -b goal/win-qt-unify-20261007 origin/master
cd .worktrees\win-qt-unify
```

（构建命令在本方向内本身就是被修对象——R0 勘察后按定案
的新接线执行并回写本节。）

**已知事实（R0 起点）**：当前 configure 用
`-DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;
C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;..."` +
`-DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix`；
运行时 PATH 需 `paleo-qgis-deps/Library/bin` 前置。

## 目标形态（建议按序）

1. **R0 勘察定案**：盘点三方 Qt 来源与版本
  （`paleo-qgis-prefix` 的 Qt、`paleo-qgis-deps` 的 Qt 6.11、
   `C:/deps/Qt/6.8.0`）；确定统一方向——A. 编译侧升到
   OSGeo4W 同源 Qt（若 prefix/deps 里有完整开发面：头文件
   + cmake 配置）；B. 依赖侧重取与 Qt 6.8 匹配的 QGIS 包
   （OSGeo4W 历史版本或重装）；判据：哪条路能让
   「configure 找到的 Qt == 运行时加载的 Qt」。决策与
   弃案理由记 ledger。
2. **接线统一**：按定案更新 CMAKE_PREFIX_PATH/QGIS_PREFIX/
   PATH 的权威组合；`paleo-dev.ps1`（bootstrap/环境脚本）
   同步——若脚本已含环境管理则改脚本，勿手口头约定。
3. **验证矩阵**：混链特征测试（方向 51 的 5 个 0xc0000139
   伞式测试 + 方向 57 的红集合样本 20 个）在统一后逐个
   复跑——从红转绿的计数是本方向的核心证据。
4. **全量回归**：统一后全量 ctest——目标：0 环境红（或
   收敛到沙箱 QTemporaryDir 已知面——那个是另一独立问题，
   如实区分记录）。
5. **文档回写**：BUILDING.md「独立 worktree 开发」+ Windows
   段的接线更新为统一后口径；旧 PATH 顺序技巧段改写（保留
   历史注记）；八、九批任务书（48-72）的环境接线段附勘误
   注（不改原文，README 加一行勘误指向）。
6. **防回归**：tools/ 或 paleo-dev.ps1 加环境自检
  （configure 使用的 Qt 版本 vs 运行时加载的 Qt 版本比对，
   不一致即警告）。

## 通用纪律（方向内全程有效）

- **边界**：本方向改的是**环境与脚本**（paleo-dev.ps1/
   BUILDING.md/工具），不因环境问题改 src/ 业务代码——若
   统一过程中暴露真实代码 bug，单独记档移交（不扩大本
   方向范围）。
- **诚实面**：无法本机完成的步骤如实记录；沙箱 QTemporaryDir
   问题与混链问题分开记账不混淆。
- **资源**：`-j8`；ctest 串行；重装/重下载依赖的磁盘与
   时间开销记 ledger。
- **无人值守**：统一方向决策自行定案记 ledger（判据齐全，
   不等人工）。
- **ledger**：`.goal-loop-ledger-win-qt-unify.md`。
- **多轮 review（硬要求）**：每批 → 验证矩阵绿 → diff
  自审（环境自检有效/文档与实际一致/脚本幂等/无越权改
  src/勘误准确 五维）→ 修复 → 再 review，至少两轮零
  High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 版本统一：configure 使用的 Qt 版本 == 运行时加载的
   Qt 版本（自检脚本输出或 dumpbin/进程模块清单证据）。
2. 混链特征红清零：方向 51 的 5 个 0xc0000139 测试 + 方向
   57 红集合样本 20 个，统一后逐个通过（计数证据）。
3. 全量回归：ctest 全绿，或红集合收敛到沙箱 QTemporaryDir
   已知面且逐条列名（与混链红明确区分）。
4. 防回归：环境自检对故意错配的 PATH/前缀能告警
  （mutation 验证）。
5. 文档：BUILDING.md 与 paleo-dev.ps1 为统一后口径；README
   勘误行存在。
6. src/ 零改动（git diff 证据——本方向不碰业务代码）。
