# 已完成待办归档（2026-11-13）

来源：master `dd7baa1f` 的 TODOS.md；当前未完成事项仍以根 TODOS 为准。
以下保留原始状态和证据，不能把旧环境/测试数当作当前验证结果。

## 已完成的操作模型项

- **P1 / R04：已完成（2026-10-28，`docs/progress/impact-background-and-navtree-perf-20261028.md`）。** ImpactJob/CompareJob 三段式落地（worker 独立开层、commit 核对世代/修订/输入版本）；取消/切工程/修订前进不发布过期结果；GUI 响应性有确定性同步点证据（tst_impactrecompute 10/10）。

2026-11-13 核对 R14：命令实现已在功能层；此次删除两个视图转发头和旧 moc 接线，
真实命令与回执回归通过。原待办描述保留如下，现不再列为未完成：

- **P2 / R14：数据操作编排下沉。** “设主文件”已接既有共享撤销栈；将 SetPrimaryCmd 等数据操作从 ui/pages/dataops 迁入功能层，视图只发意图。

## P2 — 新 worktree 首 configure 漏 include 的根因修复（from #484 P2, 2026-10-17）✅ 已收口（goal/perf-maintenance-20261027）

**What:** 新 worktree 首次 configure 会漏 glm/saribbon/sbm/segyio 的 include 路径，再跑
  同一条 configure 即恢复（症状与「再 configure 一次」操作手册见 BUILDING.md
  「构建与测试」段）。#484 指出：坑已写成手册 12 天，根因修复始终没有独立
  TODOS 条目跟踪——本条补上。根因方向：这些 vendored include 路径靠
  configure 期探测产物（superbuild/prefix 或 cache 变量）在**第二次**
  configure 才命中——候选修法是把探测移到 bootstrap/fetch-only 产出后立即
  写入 cache，或在 configure 期对「目录存在但 cache 未命中」直接
  FATAL_ERROR 早失败（与 #498 磁盘水位闸同「早失败并报明确原因」口径），
  替代「红一轮再重跑」的隐式恢复。
- **Why:** 首 configure 红一轮对新手是断头路；「再跑一次就好」的隐式约定
  无法被 CI 捕获（CI 每次都是全新 worktree 形态）。
- **Context:** BUILDING.md「独立 worktree 开发」段；#484 P2-1。
- **Effort:** human: M / CC: M
- **Priority:** P2
- **收口（2026-10-27，Linux 验收）**：原「漏 4 条 vendor include、二跑恢复」
  症状在当前 master 三条路线（-D superbuild / env / 系统兜底）全新构建目录
  均不复现（compile_commands 四条 include 全量在位，CI 全新 checkout 亦绿；
  历史症状出自 2026-10-03 Windows 本机记录，本任务无法复验 Windows）。
  实测确认的同族真实缺陷：显式 QGIS_PREFIX 悬空时 find_* **静默回落系统栈**
  ——链接系统库同时把 PALEO_QGIS_PREFIX_DEFAULT 编译成悬空路径（链接/
  运行资源栈分裂）。已按本条候选修法落地早失败：悬空显式 prefix →
  configure 期 FATAL_ERROR（报文含 BUILDING.md/bootstrap 修复入口）；prefix
  存在但 QGIS 头命中在 prefix 外 → FATAL_ERROR；无显式 prefix → STATUS 明示
  命中路线。护栏 `ctest -R first_configure`（cmake/verify-first-configure.cmake，
  UNIX）覆盖依赖存在（stub prefix 首配 + 四条 vendor include 断言）/ 缺失
  （显式+env 两路）/ 幂等复用三情形，反向验证（还原修复后）9 断言红。
  详见 docs/progress/perf-maintenance-20261027.md。
