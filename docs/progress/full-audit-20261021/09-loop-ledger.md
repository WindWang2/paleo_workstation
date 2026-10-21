# 循环台账（full-audit-20261021）

## Round 0 — 开工（2026-10-21）

- Finding IDs：无（环境阶段）
- Changed Files：docs/progress/full-audit-20261021/{00,01,02,02a,02-agent-briefing}（基线/排除矩阵/简报）
- Verification：BASE_SHA=e6837e14 建立独立 worktree；三支活跃分支并集 295 文件；
  review-from-zero 147 项特征库入位；基线构建后台进行中（RelWithDebInfo+ccache）
- Review Result：N/A
- Remaining Work：等 5 subagents 审查报告 + 基线构建/测试完成
- Next Action：聚合 Findings → 优先级分类 → Fix 1

## 主 Agent 交叉复核记录（Round 0 期间）

- #598（唯一未经审计的新代码面，91 文件）抽查：main.cpp 启动屏/回调、
  projectmapreference mbtiles URI、previewdoc requestHorizonPreview 生命周期、
  qgisprojectservice_map projectLocalBasemap、paleomapfacies/mappingartifactwriter
  字段保留、paleostartupscreen——未发现显性新缺陷（subagents 将全面覆盖）。
  备注：AppContext 回调契约洞（runtime 已初始化时 applicationCreated 被跳过，
  main() 单实例路径不触发）——仅记录不修。
- 20261020 遗留池复核：
  - D-06 correlationpanel.cpp:104/126 m_dragColumn 裸指针 + rebuildScene:849 删项 →
    UAF 成立（面板常驻隐藏=latent）。入池。
  - D-09 layoutexportactions.cpp:397-400 内部 reportExportOutcome + 调用方 291-297
    再上报 → 双重上报成立。入池。
  - A-16 frameworkstore.cpp:259-265 flush 失败唯一不清孤儿文件路径（对照 198/210/251）→ 成立。入池。
  - E-14 check_i18n 词表盲区 → 标 EXCLUDED_EXISTING_ISSUE（#561 文案 tr 化工程 +
    fix-review 分支 L1 系列同面；扩闸会引爆全仓清扫，冲突）。
  - D-14 杂项：petrophys outputUnit 恒 v/v（含 EXPR）确认（P3 边际）；
    深度道 first()/caption 叠压/mapbook 双日志不可定位或有守卫 → 放弃。
  - A-12 mapversionstore 冻结目录 journal 化 → 维持上轮裁定不接（独立批次）。

## Round 1 — Fix 1（2026-10-21）

- Finding IDs：B-1/C-1/C-2/C-3/C-4/A-01/C-5/A-02/A-04/A-05/D-01..04/K-1..3/C-6/E-6/7/8/E-2/B-2/B-4
- Changed Files：40+（23 commits）
- Verification：全部定向测试绿（含 5 个新红绿用例 + 3 个扩展用例）；闸门与 master 同面零新增；
  全量 ctest 97%、12 红全为基线已知（较基线 14 红少 2：缺失二进制已链上并通过）
- Review Result：R2 独立复审（06 文档）——1 MUST_FIX + 4 SHOULD_FIX + 2 FALSE_POSITIVE/EXTERNAL
- Remaining Work：Fix 2

## Round 2 — Fix 2（2026-10-21）

- Finding IDs：R2-MF-1/R2-SF-1..4 + 文档级两条
- Changed Files：mapexport/mappingworkflow/seismictaskservice_slice/shacache.h/tst_exportpipeline
- Verification：定向 6 套件全绿（含 MF-1 红形状扩展）；全量 ctest 复跑见终审
- Review Result：终审（08 文档）进行中
- Remaining Work：终审 → PR

## Round 3 — 终审与交付（2026-10-21）

- 终审：PASS-WITH-NOTES（零新增 P0/P1/P2）；唯一结转 F-1 一行了结（b0435e68）
- 最终全量：ctest -LE 'core|perf' 97%（384/396），12 红全为基线在册已知红
- 交付：29 commits → PR #607（分支已推 origin；master 与主 worktree 未动）
- Oracle 对账：O1-O7 全部满足（见 99-pr-body.md）
