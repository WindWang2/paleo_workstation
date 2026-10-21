# 全仓深度审计基线（full-audit-20261020，第四轮）

## 基本事实

- 审计日期：2026-10-20
- BASE_SHA：`ee8518df605752e87e804a14296f78c85ea1590c`（= origin/master = 本地 master）
- 工作分支：`codex/full-review-fix-review-fix-20261020`
- 工作树：`/home/kevin/projects/paleo_workstation-full-review-20261020`（独立 worktree，自 origin/master 新建）
- 平台：Linux x86_64（Arch，kernel 6.18.55-2-cachyos-lts）
- 构建配置：Ninja + RelWithDebInfo + `QGIS_PREFIX=主仓 vendor/superbuild/prefix`（只读复用，见 BUILDING.md worktree 约定；当前 prefix 为 QGIS 4.2.2）
- 并行度约束：构建/测试均 ≤ -j4，全局编译锁 `flock /tmp/paleo_workstation-build.lock`
- 历史轮次：full-audit-20261017（PR #508）、full-audit-20261018（PR #524）已合并；
  full-audit-20261019（**PR #580，OPEN 未合并**）——本轮与其并行，见排除矩阵。

## 历史轮移交（本轮输入）

上轮（#580）终审移交下轮的资产：
1. **登记未修 P3 清单**（04-finding-register.md 状态 OPEN，无 issue/PR 占有，属本轮修复池候选）：
   B-03（sidecar 词表分叉）、B-04（SATR 取消谓词反转）、B-05（交会采样预算闸）、
   D-01 残留（constraintpage.cpp:233 无版本换勾等值距建议）、D-02（死参/空态）、
   A-04（负 ord 告警）、E-02（CI triage 对 lint 黑盒）、E-05（ctest 注册漂移）、E-07（口径张力）。
2. **未核查区域**（02-coverage.md 如实声明，本轮优先深审）：
   - D 域：src/ui 存量 correlation/seismic3d/layout 族大面（上轮仅接口级）
   - B 域：algorithms/singlefactor 12 文件、faultsurface 深网格段（上轮仅深扫部分）
   - A 域：io/segy 大函数主体（#554 已接手拆分，仅审集成边界）
3. **新合入代码**：上轮 rebase 点 816f9b40..BASE（即 PR #579，14 文件 +413/-28：
   paleomap 三级纹理整理/删除 + datalist 操作行 + assetops）——本轮全量逐行。

## 主仓 worktree 状态（不得触碰）

- 主 checkout（master @ ee8518df）clean（除未跟踪 .zcode/）。
- 其他 worktree：`paleo_workstation-full-review-20261019`（#580 分支，不触碰）。
- 远程分支仅 origin/master + origin/codex/full-review-fix-review-fix-20261019。

## 基线构建/测试

- 日志：/tmp/paleo-audit-r4/baseline-{configure,build,tests}.log
- 结果回填见 06-baseline-quality.md（Phase 2 完成时）。

## 审计范围

全部自有生产源码（src/** 12 模块）、tests/、tools/、cmake/、resources/、
构建脚本与关键文档。Vendor 第三方只审集成边界与调用契约（照任务书）。
