# 外部工作排除矩阵（2026-10-28）

## 1. 开放 Issues（全部 EXCLUDED_EXISTING_ISSUE）

共 80 个（#712–#814），完整清单见同目录 `01-open-issues-exclusion.md`。
任何与之根因重合的发现标记 `EXCLUDED_EXISTING_ISSUE`，只登记不修复。

## 2. 主 worktree 进行中的修复（EXCLUDED_ACTIVE_WORK）

主 checkout `/home/kevin/projects/paleo_workstation` 的未提交改动是针对
#794–#814 的在途修复（Issue 正文自证「已在本地工作树修复」）。涉及文件：

- `DESIGN.md`、`cmake/paleo-modules-ui.cmake`
- `src/algorithms/geostat/sgs3.cpp`、`src/algorithms/stratgrid/propfill.cpp`（#794）
- `src/domain/wellsection.cpp`（#807）、`src/io/lasparser.cpp`（#806）
- `src/io/paleomapfiles.cpp`（#809）、`src/io/welltable.cpp`（#796）
- `src/qgis/seismicsectiontool.cpp`（#795/#813-1/#813-5）
- `src/ui/dialogs/projectsetupdialog.cpp`（#804）、`src/ui/pages/mappingeditbasis.cpp`
- `src/ui/pages/mappingworkbenchpage.{cpp,h}`（#812-1）、`src/ui/paleomainwindow_projectsetup.cpp`（#798）
- `src/ui/seismic3d/seismic3dviewportwidget.cpp`（#805）、`src/ui/wellcomposite/panelsetup.cpp`（#803）
- `src/workflow/batchjobqueue.cpp`（#808）、`src/workflow/projectsetup.cpp`（#801/#811-3）
- `tests/tst_mappingworkbench.cpp`、`tools/file-budget-baseline.txt`、`tools/ui-size-baseline.txt`
- 未跟踪新文件：`src/ui/pages/mappingtaskguide.{cpp,h}`、`docs/progress/review-from-zero-20261019.md`

规则：本轮**不改动上述文件**（除非发现与之完全无关函数内的独立 P0/P1，
且修复不产生文本冲突——此时先记录 BLOCKED_BY_EXTERNAL_WORK 再评估）。

## 3. 其他活跃 worktree

- `codex/perf-maintenance-20261027`（/home/kevin/projects/paleo_workstation-perf-maint，@ d7abab8d）
- `codex/perf-runtime-20261027`（/home/kevin/projects/paleo-perf-runtime，@ d7abab8d）
- 历史 detached worktree（20261019/20/21/23、fix-review）与 `.worktrees/open-issues-20261024`
  ——一律只读对待，不清理、不覆盖、不重置。

## 4. 既往评审已覆盖范围（避免重复劳动）

- AUDIT_ISSUES.md（2026-10-01 多代理审计，全部条目已带终态标记）
- docs/progress/swarm-review-20261027.md（#815 全量发现/遗留登记 L-01…L-21）
- docs/ledger-archive/*（方向 48/50/52/58 清账账本）
- 本轮只修：未被上述任何一项覆盖的**新发现**。
