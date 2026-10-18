# 全仓深度审计基线（full-audit-20261018）

## 基本事实

- 审计日期：2026-10-18
- BASE_SHA：`366aef975d21f6c937005fa08782476b0e6d432c`（= origin/master = 本地 master）
- 工作分支：`codex/full-review-fix-review-fix-20261018`
- 工作树：`/home/kevin/projects/paleo_workstation-full-review-20261018`（独立 worktree，自 origin/master 新建）
- 平台：Linux x86_64（Arch，kernel 6.18.55-2-cachyos-lts）
- 构建配置：Ninja + RelWithDebInfo + `QGIS_PREFIX=主仓 vendor/superbuild/prefix`（只读复用，见 BUILDING.md worktree 约定）；ccache 可用
- 并行度约束：构建/测试均 ≤ -j4，全局编译锁 `flock /tmp/paleo_workstation-build.lock`

## 主仓 worktree 状态（不得触碰）

- 主 checkout（master）有大量**未提交**改动：数据/图件导出功能簇
  （`src/domain/exportrequest.h`、`src/qgis/dataexportwriter.*`、`src/qgis/geologicalmaplayout.*`、
  `src/services/dataexport.*`、`src/ui/dialogs/dataexportdialog.*`、`src/ui/dialogs/geologicalexportdialog.*`、
  `src/workflow/dataexportworkflow.*`、`src/workflow/geologicalexport.*`、`tests/tst_dataexport.cpp`、
  `tests/tst_exportpipeline.cpp`、`docs/EXPORT_PLAN.md` 及 33 个 M 文件）。
  → 标记 EXCLUDED_ACTIVE_WORK（export 主线），本审计不修改这些文件在 master 分支上的基线版本以外的东西；
  审查可覆盖 origin/master 上的对应旧文件，但涉及 export 链路的修复一律降级为记录不修改。
- 其他 worktree：`codex/facies-topology-editing`（PR #521）、`goal/issue-sweep-20261017`（PR #518）。

## 开放外部工作

- PR #521 `codex/facies-topology-editing`：三级相面分割/覆盖绘制/拓扑安全合并（mapping 域）。
- PR #518 `goal/issue-sweep-20261017`：声称处理全部 27 个开放 issues（#460–#503）+ CI 门禁重建。
- 开放 issues：#460–#503 共 44 个（含各模块 P2/P3 清单 #476–#484、swarm P1 追踪 #485、
  CI/环境债 #497–#503、深化 backlog #489–#494）。
  全量正文快照见本目录 `99-external-issues-snapshot.txt`（按 issue 拆分件在 /tmp/paleo-audit/issue-*.txt）。

## 基线构建/测试

- baseline-configure.log / baseline-build.log 见 /tmp/paleo-audit/（结果回填见 05-priority-and-fix-plan.md）。
- ctest 基线在构建完成后执行，结果回填同上。

## 审计范围

全部自有生产源码（src/**）、tests/、tools/、cmake/、resources/、构建脚本与关键文档。
Vendor 第三方只审集成边界与调用契约。
