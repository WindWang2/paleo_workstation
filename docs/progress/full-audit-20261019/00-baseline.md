# 全仓深度审计基线（full-audit-20261019，第三轮）

## 基本事实

- 审计日期：2026-10-19
- BASE_SHA：`f6963e0b0562ec4c39417c791f09eb99e2a76123`（= origin/master = 本地 master）
- 工作分支：`codex/full-review-fix-review-fix-20261019`
- 工作树：`/home/kevin/projects/paleo_workstation-full-review-20261019`（独立 worktree，自 origin/master 新建）
- 平台：Linux x86_64（Arch，kernel 6.18.55-2-cachyos-lts）
- 构建配置：Ninja + RelWithDebInfo + `QGIS_PREFIX=主仓 vendor/superbuild/prefix`（只读复用，见 BUILDING.md worktree 约定）
- 并行度约束：构建/测试均 ≤ -j4，全局编译锁 `flock /tmp/paleo_workstation-build.lock`
- 历史轮次：full-audit-20261017（PR #508）、full-audit-20261018（PR #524）均已合并；
  其后续 swarm 评审 issues #531–#541 已由 #564 处理合并。

## 主仓 worktree 状态（不得触碰）

主 checkout（master）clean。其他 worktree（全部在 master SHA 或自有分支，均不触碰）：
- `.worktrees/issue-55{3..9}-*`、`.worktrees/issue-56{0,1}-*`：9 个 issue 分支
  （#553 GDAL RAII / #554 巨型函数 / #555 soffice 超时 / #556 ErrorHub / #557 渲染热点 /
  #558 头扇出 / #559 测试薄弱 / #560 folderimport 三胞胎 / #561 低危批）。
- `~/.codex/worktrees/paleomap-facies-integration`：PR #568。

## 开放外部工作（排除矩阵输入）

- PR #568 `codex/paleomap-facies-integration`：古地理图微相导入补齐三级相 + 系统纹理（data 域）。
- 开放 issues #553–#561 共 9 个（见 01-conflict-matrix.md 逐条特征与排除面）。

## 基线构建/测试

- 日志：/tmp/paleo-audit-r3/baseline-{configure,build,tests}.log，结果回填
  06-baseline-quality.md 与 05-priority-and-fix-plan.md。

## 审计范围

全部自有生产源码（src/** 12 模块 ~307K 行）、tests/（401 cpp）、tools/、cmake/、
resources/、构建脚本与关键文档。Vendor 第三方只审集成边界与调用契约。
