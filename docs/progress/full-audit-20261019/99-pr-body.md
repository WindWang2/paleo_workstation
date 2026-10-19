# refactor: comprehensive review-fix-review-fix quality closure (round 3)

## Summary

第三轮全仓 Review–Fix–Review–Fix 质量整顿。5 只读审查 subagent（数据/IO、算法/服务、
workflow/AI、qgis/ui、构建/测试/架构）+ 主 Agent 交叉复核完成全仓深审，聚合修复池后经
Fix-1 → 独立 Review-2（双复审者）→ Fix-2 → 独立终审 → 本地全量回归。
核心成果：**收敛 2026-10-18 合入潮击穿的 10 项闸门红**（不等-CI 合并的债务），
外加 1 项数值正确性修复（SMI 投票半格偏移）、1 项数据完整性修复（导出 dedup 层位归属）
与 12 项健壮性/行为修复。

## Baseline

- BASE_SHA `f6963e0b`（origin/master @ 2026-10-19 02:29）；任务期间 master 前进多次
  （#568/#574-#578），分支已 rebase 至 `816f9b40`。
- Arch Linux x86_64 / Ninja / RelWithDebInfo / vendored QGIS prefix（只读复用）。
- 构建与测试全程 `-j4` + 全局 flock；基线全量构建成功、基线 ctest 421/438
  （17 红：10 闸门 + 7 二进制，全部如实分账见 docs/progress/full-audit-20261019/06）。

## Review Coverage

src 12 模块（~307K 行）+ tests/tools/cmake/ci/docs：新合入代码（219176bb..BASE，
180 文件 +8502 行）全量逐行；存量按风险分级（高险逐行/接口级/未核查区如实登记——
singlefactor 12 文件、faultsurface 深网格段、ui correlation/seismic3d/layout 大面
等在 02-coverage.md 声明）。5 份子报告 + 2 份复审 + 1 份终审全文在
docs/progress/full-audit-20261019/ 与审计过程目录。

## Findings

P1×1（闸门回红簇）、P2×3、P3×23 修复池 + RECORD-ONLY 若干；排除全部 9 个开放
issue（#553-#561，他属 worktree/PR）与活跃 PR 面。终审对抗挑战（单列网格/极小
spacing/dedup 多候选/重入勾选/空 BOM）全部未推翻修复。

## Fix 1（6 commit）

- `fix(gates)` G1 簇：identifytool 视图原生控件白名单裁决、ui-token token 化+stale
  清理、datalist_tree 提取收敛（1100→1049）、folderconfirm/updateEngineRows 拆分、
  attachConstraintPage 冻结值违约回撤（719→706）、heavy_tus 带归因 re-pin（后被
  #578 超越，rebase 取 50）、known-reds 5 条 STALE 清理
- `fix(workflow,services)`：C-01 dedup 层位域严格同域 + C-03/C-06 批量出图
  horizon 参/多文件命名 + **B-01 SMI 散点 extent 半格外扩**（投票 Voronoi 对齐，
  相图配准同源校正——两位复审者独立推演证实）
- `fix(ui,qgis)`：D-01 换行勾选 restore + MA-F01（后被证实假阳性）+ D-03/D-04
- `fix(io,ai,services)`：A-01/02/03 大文件闸与进度契约（tst_ingestplan 基线红转绿）、
  B-02/B-06/B-08、C-02 onnx 锁纪律
- `test(export)`：582 行零注册诊断套件接闸（含 moc 基名错误修复）

## Review 2（双独立复审者）与 Fix 2（3 commit）

- **R2-1 抓住 MA-F01 假阳性**：vendored QGIS qgsmapsettings.h 证实 canvas layers()
  index 0=顶层，Fix-1 的反转把正确行为改反——已回退并钉死契约注释（eb39c31f）
- R2-2 抓住 D-01 测试无区分力 → 升级为双因素夹具（24a8bf4a）
- SHOULD_FIX 落地：dedup 在盘复核 + 聚类 warnings 接线（c632b50f）
- 试错留痕：D-01 两案重构破坏既有套件，按实证回退（07 号文档）

## Final Review（独立终审：PASS-WITH-NOTES）

22 项修复在位、对抗挑战全过、分层/闸门绿、无孤儿/卫生问题。终审更正已采纳：
**D-01 声明诚实更正**（有版本路径由既有链自愈；无版本换勾的等值距建议残留
登记 OPEN，见 Known Limitations）。

## Testing

- 基线：421/438（17 红）→ rebase 前终局全量：436/439（3 红，全部基线既有/已登记）
- rebase 至 816f9b40 后共享 vendor prefix 被并行工作于 08:24 重建（QGIS 4.2.3→
  4.2.2，新库无 RUNPATH，传递依赖 libqgis_native 解析断——219 个用例加载失败，
  **外部环境事件非代码回归**；BUILDING.md「不要在共享 prefix 中清理或重建」被
  并行会话违反）。以 LD_LIBRARY_PATH 前置旁路完成终局全量回归（见下），并顺带
  收敛 master 自身的 docs_drift 红（主窗家族 TU 计数 26→27，#578 漏更）。
- 终局全量（LD_LIBRARY_PATH 旁路）：**437/440（99%），3 红全部基线既有/已登记**
  （layering_transitive #533 挂起、tst_ui legendTitle 基线红、tst_assetpaging 基线
  SEGFAULT）；docs_drift 经本 PR 主窗 TU 计数同步后绿
- 3 红分账：layering_transitive（ui_qtwidgets_notes 7>6，#533 裁决挂起，基线文件
  头注+known-reds 在案）、tst_ui legendTitle（stash 法证实基线即红）、
  tst_assetpaging SEGFAULT（基线在册）
- 门禁：layering(--strict)/qgs-audit/qgis-spread/ui_tokens(--strict)/
  file_budget(--strict)/ui_size(--strict)/ui_invariants(--strict)/i18n/docs_drift 全绿
- 新增回归 7 处 + 既有夹具跟进 3 处（终审裁定全部合法跟进非迎合）

## Performance

dedup 由索引查改线性扫描+在盘 sha256 复核：每次导出至多一次候选文件整读，
终审确认无新 O(N²)；无其他热点改动（本轮无性能声明项）。

## Conflict Avoidance

- 未触碰 #553-#561 的任何根因（其 PR #569-#578 已全部合并，本分支已 rebase 其上；
  唯一冲突 transitive-baseline 取 #578 的 50 并留痕作废注）
- 与 #568（paleomap 链）同域问题只登记未修改（EXCLUDED_ACTIVE_WORK）

## Known Limitations

1. D-01 无版本换勾路径的等值距建议残留（constraintpage.cpp:233 清表前读旧值域）——
   重构需以全套件为护栏，建议独立小批处理
2. E-02（CI triage 对 lint 的 job 级黑盒）、B-03（sidecar 词表分叉）、B-04（SATR
   取消谓词反转）、B-05（交会采样预算闸）、D-02（死参/空态）、A-04（负 ord 告警）、
   E-05（ctest 注册漂移）、E-07（口径张力）：登记未修（P3 低危）
3. B 域未核查面（singlefactor 12 文件等）如实移交下轮
4. 基线既有 3 红与 tst_cache_las/tst_singlefactor_perf（known-reds 在册）非本轮范围
