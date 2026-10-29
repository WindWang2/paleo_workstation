# 全仓深度 Review–Fix–Review–Fix 基线（2026-10-28）

## 基本事实

- BASE_SHA：`0b9c0a7a0094c3f56c31910e5e375ffbfe35e90f`（origin/master，2026-10-28 fetch）
- 工作分支：`codex/full-review-fix-review-fix-20261028`
- Worktree：`/home/kevin/projects/paleo_workstation-full-review-20261028`
- 主 checkout（`/home/kevin/projects/paleo_workstation`，master @ 9fdc7bcf，落后 4）**不动**，
  其未提交改动是另一 Agent 针对 Issues #794–#814 的进行中修复（EXCLUDED_ACTIVE_WORK）。
- 平台：linux x64（CachyOS LTS 6.18.55-2），Qt6 + vendored QGIS superbuild prefix（只读共享）。
- 构建配置：Ninja + RelWithDebInfo，`QGIS_PREFIX=vendor/superbuild/prefix`（symlink 至主仓），
  命令来自 BUILDING.md「独立 worktree 开发」。编译并行度 4（本任务约束，低于文档的 -j8），
  全程 `flock /tmp/paleo_workstation-build.lock`。

## 仓库近况

- master 最近合入：#790（新建工程扫描）、#791（首页布局）、#792（账本归档）、
  #793（QGIS 原生坐标控件）、**#815（swarm 全仓深度审查：四轮修复 21 P1 / ~55 P2 / 50+ P3，
  100 文件 +1281/−278，含 docs/progress/swarm-review-20261027.md 遗留登记 L-01…L-21）**。
- 无开放 PR。
- 开放 Issues 共 80 个（#712–#814），全部为外部所有：
  - #794–#814：review-2026-10-27 批次（其修复正落在主 worktree 未提交改动里）；
  - #749–#779：更早批次（test/CI/生命周期/io 等）；
  - #712–#748：算法/导出/元数据批次。
- 本轮增量审查对象（未被任何 Issue 覆盖的最新代码）：
  1. `d7abab8d..0b9c0a7a`（#815 自身的四轮修复——修复引入回归的一手风险区）；
  2. `9fdc7bcf..d7abab8d`（#791/#792/#793 的 UI/坐标控件改动）；
  3. 全仓系统性扫描中未被既往评审与 Issue 覆盖的角落。

## 审查分工（Phase 1）

- Agent A：`src/domain` `src/catalog` `src/io` `src/metadata`（重点 #815 改动的 io 文件 + 非最近改动文件）
- Agent B：`src/algorithms` `src/services`（重点 #815 改动的算法/服务 + 数值正确性）
- Agent C：`src/workflow` `src/ai` `src/linkage`（重点 #815 改动的 workflow/ai + 任务生命周期）
- Agent D：`src/qgis` `src/ui`（重点 #815 改动的 qgis/ui + #791/#793 新 UI + DESIGN.md 符合度）
- Agent E：独立横切审计——`git diff d7abab8d..0b9c0a7a` 全量复核 #815 修复自身的正确性 + tests/tools/cmake

## 构建与测试基线

- baseline-build.log：worktree 根目录（后台执行，完成后归档结论于此）
- baseline-tests.log：ctest 全量结果（构建完成后运行 `-j4`）
- 已知环境注记：主仓 2026-10-27 评审记录称本机 ctest 夹具曾全红（stash 对照证实为既有
  环境问题），#774 记录 5 个测试在审查环境确定性失败——若复现需与 master 基线对照归因，
  不得误判为本轮回归。
