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
