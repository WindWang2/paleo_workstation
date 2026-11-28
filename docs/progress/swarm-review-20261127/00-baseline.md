# Swarm review 2026-11-27 — baseline

- HEAD: `42296fd3` (master, 2026-11-27 19:03 +0800)
- Subject: 整合地震聚类与井赋相、Qt 工作区和 Pi Agent (#1118, #1119)
- Previous full swarm: `docs/progress/swarm-20261028.md` and `docs/progress/full-audit-20261028/` at `0b9c0a7a`
- Commits since 2026-10-28: 222
- Recent surface: Agent G1–I5 (#1063–#1081), document model (#1116), vector edit (#1113–#1115), LAS wellbore reassignment (#1117), seismic cluster + well facies (#1118/#1119)
- Dirty worktree (not part of this review): seismic cluster plan doc, prototype zip, two OFL font files
- Mode: read-only domain swarm. Findings only. No source edits in this pass.
- 登记：`04-finding-register.md`（P1 × 5，P2 × 28，P3 × 7，P4 × 2）。分片原文在 `findings/`。

## Module sizes (cpp+h)

| Module | Files |
|--------|------:|
| ui | 575 |
| workflow | 381 |
| services | 332 |
| qgis | 177 |
| io | 161 |
| algorithms | 154 |
| domain | 110 |
| ai | 68 |
| catalog | 54 |
| app | 43 |
| metadata | 38 |
| linkage | 10 |
| selfcheck | 3 |

## Slices

| ID | Scope |
|----|--------|
| A | `src/io` `src/metadata` |
| B | `src/domain` `src/catalog` |
| C | `src/algorithms` |
| D | `src/services` |
| E | `src/workflow` `src/linkage` job/import/export/mapping paths, excluding Agent tool hosts |
| F | `src/ai` plus Agent tool host, operation, plan, python sandbox, capability code under `src/workflow` |
| G | `src/qgis` |
| H | `src/ui` `src/app` behavior defects only |
