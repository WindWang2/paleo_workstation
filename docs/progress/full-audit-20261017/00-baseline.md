# Full Audit 2026-10-17 — 00 基线

## 环境

- BASE_SHA: `c720aef58c00b8e53bd3004598b17ae752496be9` (origin/master, 2026-10-17 fetch)
- Worktree: `/home/kevin/projects/paleo_workstation-full-review-20261017`
- Branch: `codex/full-review-fix-review-fix-20261017`（自 origin/master 新建）
- 平台: CachyOS Linux x64 (kernel 6.18.55-2-cachyos-lts), zsh
- 编译配置: Ninja / RelWithDebInfo / QGIS_PREFIX=共享 vendored superbuild prefix（只读复用）
- 资源纪律: 全局编译锁 `/tmp/paleo_workstation-build.lock`，`--parallel 4`，ctest `-j4`
- 基线构建日志: `/tmp/baseline-build.log`（进行中，完成后记录结论）
- 基线测试: 构建完成后 `ctest --test-dir build -j4 --output-on-failure` → `baseline-tests.log`

## 仓库态势（启动时点）

- Open PR: 仅 #506（codex/ci-green-20261016，CI 真红根因修复批次 #497–#504）
- Open Issues: 33 个（#460–#504，全部视为外部持有，见 01-conflict-matrix.md）
- 活跃并行工作树:
  - 主 checkout `codex/audit-remediation`（已推送 commit 769b8e3d "full repository forensic audit and P0/P1 remediation" + 19 文件未提交改动：mapping/preview/constraint 页面簇）
  - `codex/fix-open-issues-20261016`（修 #478/#463 等既有 issue，领先 master 6）
  - `codex/mapping-evidence-deepening`（= origin/master，#494 已合并，视为完成）
  - `codex/well-attribute-tables`（领先 4 落后 3，mapping tops set 方向）
- 前轮审计文档（在 codex/audit-remediation 分支）:
  - `docs/ledger-archive/reviews/COMPREHENSIVE_AUDIT_2026-10-16.md`（57 findings: P0×6 P1×23 P2×20 P3×8，基线 dbb03269）
  - `docs/ledger-archive/reviews/SWARM_REVIEW_2026-10-15.md`（16×P1 + 9 域低危 → issues #460-#485）

## 本轮独特切入点

audit-remediation 基线是 `ddd1325e`（#494 之后）；其后 master 又合入：

- `5103cb3f` #495 clang-tidy copy-init 修复（小）
- `a1efbd74` #496 singlefactor PR457 二轮收口（24 文件 +911 行：constraintworkflow /
  surfacegridding / constraintuseplan / mappingworkbench_edit / 单因素回执与 CRS）
- `c720aef5` #505 CI asan 磁盘（非代码）

即 #496 的代码未被任何前轮审计覆盖；opt-01..05（#489–#493）与 #494 虽被
audit-remediation 覆盖，但该轮修复本身未经独立复审。本轮 Deep Review 以
「新合入代码缺陷 + 前轮修复的正确性复核 + 前轮未覆盖的跨模块链路」为主攻方向。
