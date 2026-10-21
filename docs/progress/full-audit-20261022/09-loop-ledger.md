# Full Audit 2026-10-22 — Goal-Loop 台账

格式：每轮 Inspect → Hypothesis → Change → Verify → Independent Challenge → Record → Next。

## Round 0 — 环境与基线（2026-10-22）

- Inspect：origin/master=b301f2e8（本地 master 落后 20 提交，已记录）；1 开放 PR（#627）、
  16 开放 Issues（#556–#597 外部所有）、遗留分支 fix/master-green-20261021（陈旧，只读）。
- Change：建 worktree codex/full-review-fix-review-fix-20261022；00-baseline.md、
  01-conflict-matrix.md；基线构建启动（RelWithDebInfo/Ninja/-j4/flock）。
- Verify：configure 双跑成功（已知首跑漏 include 坑）；构建后台进行。
- Next：5 只读深审 subagents（A–E）返回后聚合并建 04-finding-register。

## Round 1 — Deep Review 1 聚合（2026-10-22）

- Inspect：5 份报告（03-review-{A..E}.md）：新立案 23 → 去重（B-05≡D-03）22 项，
  全 P3、零 P0-P2；排除登记 D-04..D-08（#590/#592 族）等。
- 主代理交叉复核：importrecovery/welllogread 调用链、#617 手势、#625 底图——
  证实 B-06 为 FALSE_POSITIVE（neighbourhood 严格比较使平坦面双标志同 false，
  候选为空；既有测试 tst_previewmap_assets:1164-1168 已钉住该行为）。
- Change：04-finding-register.md、05-priority-and-fix-plan.md（7 簇修复 + 4 结转）。
- 环境事故与修复补记：基线首链失败 = 共享 vendor prefix 的 libarrow/parquet
  5 个悬空符号链接（2026-10-20 某会话指向主仓已删除的 appimage 暂存目录）。
  从 pacman 缓存 arrow-25.0.1-7 提取真实库替换后恢复（ENVIRONMENT_FAILURE，
  非本仓代码问题；恢复到可用状态而非重建）。

## Round 2 — Fix 1（进行中）

- 簇 1 深度单位诚实性：C-7 + B-02 + B-03 + C-9 三套新测。
- 簇 2 文本曲线生态：A-02（GB18030 解码接入）+ A-03（井深词表/分号分隔统一）。
- 簇 3 批量让位：B-01（JobRunner priority 参数 + BatchQueue Low）。
- 簇 4 失败通道：B-05≡D-03（四 request* 失败回调 + 四面板失败态）；B-06 证伪撤销。
- 簇 5 导入 IO：A-01 文案 / A-06 列数早退 / A-08 行序。
- 簇 6 道身份与文档：D-01 FormationSource 身份路由 + E-01/E-02/E-03。
- 簇 7 双嗅探：C-8（parse 带 format 重载，六调用点 + 缓存路径消重）。
- Verify：基线构建成功（BUILD_EXIT=0）；纯净基线 ctest 与修后定向测试进行中。

## Round 3 — Independent Review 2 与 Fix 2（2026-10-22）

- Inspect：R2 独立复审（06 文档）：MUST_FIX 1 / SHOULD_FIX 4 / DOCUMENT_ONLY 10 /
  FALSE_POSITIVE 0；B-06 维持证伪；无需回滚。
- Change：R2-M1（空路径守卫回修 + mock 深度回归用例）、R2-S2（剖面空回调
  不留图例 + 失败测试扩四路）、R2-S3（readCurveTvd 词表对齐）、R2-S4
  （spread 基线头注）、D1 注释、S1 改判结转（登记册）→ commit 75f3a54e。
- Verify：构建 EXIT=0；定向 7 套件绿（唯一红 = tst_datapreview 基线在册红，
  与纯净基线同签名）；07-fix-r2-results.md 落档。

## Round 4 — 独立终审 + N1 收口 + 终局验收（2026-10-22）

- Inspect：终审（08 文档，PASS-WITH-NOTES）：17+4 项修复全部确认、六条构造性
  挑战逐一推演无伤、PR/Issue 零重叠、仓库卫生干净；新发现 N1（P1，
  file_budget 回升 2 项——B-05 失败态落地面 +12 行/函数）、N2（P3 结转）、
  N3/N4（P4）。
- Change：N1 收口（setStatLabel/markStatsUnavailable 共享助手 + 两 build 函数
  删本地 lambda，代码收缩非补登基线；mapproduct 373→372 基线随缩下调）→
  commit 485045a5；N4 措辞改写；N3 留档。
- Verify（Phase 7 终局）：
  - 终局全量 `ctest -j4 -LE 'core|perf'`（N1 收口后，带 prefix
    LD_LIBRARY_PATH）：**407/410（99%）**，3 红 = 纯净基线同集合
    （tst_onlinebasemap[PR#627 面]/tst_datapreview/tst_assetpaging）——
    **零新增回归**。
  - 源码闸门 13 项复跑全绿：layering(_strict)/qgs_audit(_strict)/
    qgis_spread(_strict)/file_budget(_strict)/ui_invariants_strict/i18n/
    ui_tokens/qgis_versions/docs_drift。
  - `git diff --check b301f2e8..HEAD` 干净。
  - 性能口径：C-8 为 IO 次数级收敛（每读取点 2-3 次文件 open + 64KB 头读
    减半；wellpredictionservice 由 per-ref 降为 per-path），按纪律不做无实测
    的墙钟宣称；B-01/A-06 为设计语义恢复与预算早退，同口径不宣称数字。
- 交付：9 commit（8 修复 + 1 文档待补）→ PR。

## Oracle 对账

- O1 基线可信：BASE_SHA/独立 worktree/PR-Issue 排除矩阵/基线构建测试记录 ✓
- O2 审查完整：5 报告覆盖清单 + 未核查区如实申报 + B-06 证伪留痕 ✓
- O3 Fix 1：17 项 Finding 修 16 + 1 结转对（C-9 部分覆盖注明）+ 全部定向验证 ✓
- O4 Review 2 独立：非作者复审，MUST_FIX 全进 Fix 2 ✓
- O5 Fix 2：R2-M1/S2/S3/S4 全收口 ✓
- O6 终验：终审完成（N1 收口后条件满足）；构建绿、全量 99%、零新增回归、
  闸门全绿 ✓
- O7 交付：分支推送 + PR（进行中）
