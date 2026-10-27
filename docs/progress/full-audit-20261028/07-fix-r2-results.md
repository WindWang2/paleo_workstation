# Review 2 结论与 Fix 2 结果（Phase 4/5）

## Review 2（独立 subagent 复审 0b9c0a7a..d70714dd）

**结论：可合并、无 MUST_FIX**。18 项修复全部命中根因；全部挑战尝试（双
finished 双摘、空 id ≥3 条、cancelled 重入/双重发射、QFileInfo 时序、
isActiveOnCanvas 等价性、软上限流式 IO 预算、两遍法 metric 逐位一致、
锁内 meta 一致性）均被推翻或降级为登记项。

## SHOULD_FIX 处置（全部采纳）

| 意见 | 处置 | Commit |
|------|------|--------|
| S-1 previewwellbases stat 复验死代码（QFileInfo 惰性缓存：preInfo 首取值在解析后，比较恒等） | 基线 size/mtime 前置到解析前取局部变量 | b5b61043 |
| S-2 onToolFinished removeAt 跳位漏摘（空 id ≥3 条时一次摘不全 → 取消窗口漏答） | 首条匹配摘除即 break | d59b9f95 |
| S-3 计划点名的 4 个回归测试未交付 | F-07（205 事件损坏行翻页）与 F-16（65 表软上限）补齐；F-04（widget 定时器）与 F-20（并发时序）无确定性红测构造面，以 R2+终审两级代码复核兜底并在此登记 | de2f355e / ecb6af2a |

## Fix 2 期间的新发现与修复

**F-23 [P1] #815 审计历史读取回归（constraintstore.cpp readHistory）**
- 现象：tst_constraintevidence 5 个存量用例在 master 0b9c0a7a 确定性红
  （主 worktree pre-#815 构建全绿对照坐实）；诊断线索为
  `Corrupted audit event row skipped: "1"`。
- 根因：#815 L-26 给 SELECT 加 fid 列并按位读（0=fid,1=event_json）。
  GDAL SQLite 方言对 GPKG 层的 fid 有特殊映射（ogrinfo + gdb 双实证）：
  裸列名与别名都被从结果字段表剥离、改承载在层 FID 上——按位读把 fid
  值当 event_json，**整表行被判损坏跳过**。影响面：history()/
  historyForHorizon() 恒空、copyAudit 零拷贝、journal() recent 读取为空
  （evidence_recent_events 丢失、修订链重启为 1）。
- 修复：SELECT 列表 fid AS audit_fid；读侧按名取 event_json，audit_fid
  在场按名读、缺席以 OGR_F_GetFID 兜底（两代方言行为均正确）。jsonIdx
  缺席的显式失败严格安全于旧静默错读。
- 验证：存量 5 红用例转绿（套件 8/8），新增
  copyAuditSurvivesCorruptedRowsMidPagination 双重锁定（F-07 判终 + 剥离
  回归）。commit de2f355e。

其余 Fix 2 commit：696a27f0（D-3 restore 判空硬化）、36044fdc（D-1
QHash include 清理）。

## 终审（Phase 6）遗留登记（DOCUMENT_ONLY）

- constraintstore copyAudit：整页 200 行全损坏且后页仍有有效行时游标无
  法越页（minFidOut 只记有效行）——阈值从 1 行提升到 200 连续损坏行，
  病态场景，彻底修复需 readHistory 暴露原始行最小 fid。
- paleotaskservice busy 键 "task-N" 未按服务实例隔离：当前唯一 store 绑定
  实例是 AppContext（撞号不可达）；未来开放第二注入点时需带实例前缀。
- mappingworkbench_polygonize QPointer 池线程访问的固有 TOCTOU 窗——
  PW-031 家族既定口径（#770 在册）。
- previewwellbases stat 门控存在「同尺寸+同毫秒原地改写」理论逃逸窗，
  窄于旧方案固有 TOCTOU，接受。
