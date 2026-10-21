# refactor: comprehensive review-fix-review-fix quality closure (round 5)

## Summary

对 Paleo Workstation（BASE `e6837e14`，#598 之后最新 master）执行第五轮
全仓 Review–Fix–Review–Fix 质量整顿：5 个只读深审代理（数据/IO、算法/服务、
工作流/AI、QGIS/UI、构建/测试/架构）+ 主代理交叉复核，聚合去重并对 10 个
开放 PR 与全部开放 Issues 做 hunk 级排除后，立案 23 项新发现（P2×8、P3×15）
+ 3 项上轮登记未修遗留，完成两轮修复与两轮独立复审。

## Baseline

- BASE_SHA `e6837e14`（提交时实时对账，master 未前进）
- linux x64（cachyos-lts），Ninja/RelWithDebInfo，Qt 6.12.0 + vendored QGIS 4.2.2
- 基线 ctest（`-LE 'core|perf' -j4`，LD_LIBRARY_PATH 含 vendor prefix——
  与上轮本地口径一致）：**96% 通过、14 红全为在册已知红**（11 个源码闸红
  = issue #586/#591 族在修面、tst_assetpaging SEGFAULT = #585、2 个因磁盘
  约束未链目标）。终局 **97%、12 红**（较基线净改善：2 个未链目标补链通过）。

## Review Coverage

- 5 份代理深审报告（docs/progress/full-audit-20261021/03-review-{A..E}.md）：
  约 120 文件逐行级 + 全仓接口级覆盖，覆盖清单与未覆盖区域如实登记。
- 排除库：review-from-zero 147 项（#602 修复中）、上两轮已修清单、
  10 个开放 PR 文件并集（441 文件）hunk 级冲突核对、Issues #556/#558/#559/#561。
- 主代理交叉复核 #598 新代码面（唯一未经审计的新增面）。

## Findings（新立案 23 + 遗留复核 3）

- P2×8：ShaCache 全表写放大（A-01）、Ricker 子波系数 2π→π² 主频 -20%（B-1）、
  任意线剖面线号冒充米（B-2）、AI 重试轮丢工具（C-1）、一致性报告跨文件系统
  rename 恒 EXDEV（C-2）、六链导出 mutationSeq 误判废（C-3）、反演暂存体
  永留 /tmp（C-4）、qgs 闸扫描面盲区（E-2）
- P3×15 + 遗留 K×3（详见 04-finding-register.md）
- 交叉排除 5 项（E-1→#580 已修、E-3→#581 已修、E-4→#501 后续、A-03→#603
  BLOCKED、E-5→#604 交互风险 SKIP）；B-3（相干 IO 放大 ~300×）登记不修
  （vendor 无窗读 API，集成边界外，读点注释留档）。

## Fix 1（23 commits）

每簇独立 commit + 红绿回归（详见 05 计划与 09 台账）。代表性：
- C-3：摘除六处「开始时 mutationSeq」发布闸 + publishMapExportJournal 增
  撞号 id 重映射（#480 P3-3 同口径），回归构造「导出中无关写占号」最坏形状。
- A-01：磁盘层双阈值节流（1s/64 条），回归钉死「8 次 miss 只落 1 条」。
- B-1：π² 系数修正，回归用零点半宽 9.0ms vs 病态 11.3ms 判别。
- E-2：qgs-audit/qgis-spread 扫描面扩至全部非视图模块 + 21/23 文件 #539
  初冻口径入册（闸后状态与 master 同面，identifytool +2 维持 #604 册外）。

## Review 2（独立复审 06）

1 MUST_FIX + 4 SHOULD_FIX + 2 FALSE_POSITIVE/EXTERNAL_OWNED：
- MF-1：C-3 重映射不避让同批 journal 自有 id（反例：多资产 journal + 噪声
  只消耗首号 → 重放 duplicate 整批失败）——Fix 2 改 live/journal 双避让。
- SF-1..4：厚度链声明源随映射改写 / 线号距离两支不外送 / 逐道族进度终填 /
  重复 include。
- B-1 注释数值质疑经独立复算证伪（FALSE_POSITIVE，2π vs 2π² 误读）。

## Fix 2（2 commits + 终审结转 1 commit）

MF-1/SF-1..4 全处置（07 对账表）；终审再抓 SF-2 被点名分支未清（F-1），
一行了结。

## Final Review（08）

**PASS-WITH-NOTES（零新增 P0/P1/P2）**：22 项修复逐项抽验无翻案；MF-1
双避让附前进性证明；测试无假回归（MF-1 重写版经旧算法路径推演确证改前红）；
质量闸实跑零新增红；8 支开放 PR 全量 hunk 复查分离。DOCUMENT_ONLY 结转项
（A-01 无 shutdown flush、8cba9e94 提交内容跨带 B-2 改动等）见 08 文档。

## Testing

- 全量：`ctest -LE 'core|perf' -j4` → **97%（384/396），12 红全为基线在册
  已知红**（闸门红 11 = #586/#591/#604 在修面 + tst_assetpaging = #585），
  零新增回归。
- 新增/扩展回归测试 11 个（Ricker 形状 / 重试轮 tools / EXDEV 跨文件系统红
  形态 / 导出无关写不判废 + MF-1 双避让 / 反演暂存回收 / ShaCache 节流 /
  map 重载 no-op / 剖面米弧长 / chip reload 即时性 / qgis_versions 反例
  selftest / ui_invariants 全参扫描）。
- 闸门：layering --strict 绿；file_budget 零新增（冻结值两处随缩下调）；
  ui_invariants/i18n 绿；docs_drift/ui_size/ui_tokens/qgs 族红全为 master
  同面（cat-file 实证）。
- `git diff --check` 干净。

## Performance

- A-01：修复前每 miss 全表落盘（表 3 万条 ≈5MB/次）——万级文件导入 ≈50GB
  同步写；修复后 1s/64 条节流（测试断言写盘次数）。
- B-4：进度账本对齐实际工作量（4 线程封顶 62% → 终值可达 100%）。
- B-3 未修（vendor 边界），已登记。

## Conflict Avoidance

未触碰 10 个开放 PR 的修复面（hunk 级核对，见 01-conflict-matrix）；唯二
邻接：file-budget-baseline.txt（4 PR 邻行，易解）与 inversionworkflow.cpp
include 块（#601，易解）。排除项：#556/#558/#559/#561/#582-#597/#501 后续/
#604 交互面（E-5 仅记录）。

## Known Limitations

- B-3（相干时间切片 IO 放大 ~300×）根治需 vendor 窗读 API——读点注释留档。
- B-2 的 BuildLineSection 回落路径无 per-column 线号，无法换算——清空交
  消费端自述兜底口径。
- A-01 无生产 shutdown flush（正常退出丢 ≤1 节流窗口，自愈）。
- 8cba9e94 提交内容与 message 有 B-2 改动跨带（历史提交，squash 后归并）。
