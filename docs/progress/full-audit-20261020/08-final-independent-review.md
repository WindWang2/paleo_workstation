# 独立终审（full-audit-20261020）

终审者：第三位独立 subagent（未参与修复与前两轮复审）。全文 /tmp/paleo-audit-r4/review-final.md。

## 挑战与结论（摘要）

1. **极端输入推演（5/5 站住）**：A-10 crafted ZIP64 多变体被减法判界+512MiB 总量闸
   封死；B-05 溢出被 goodGrid INT_MAX 前置封死、多源记账保守正确；C-10 恢复/
   永久删除/TOCTOU 排列由 commit 侧 mutationSeq+recycleHash 双指纹兜住；D-13
   handler 连接序殊途同归；E-10 对真实 gh api jobs.json 形态兼容成立。
2. **层归属疑云不成立**：dataopsSidecarPath 实体在 src/services/jsonsidecar.h
   （数据层），workflow→services 合法；check_layering 实跑绿。
3. **TU 拆分合法**：clearStackLayers 跨 TU 单一定义、ODR/moc 无碍。
4. **基线零违约**：六个基线文件 diff 为零；本树 file_budget/ui_size/ui_tokens/
   docs_drift 红+#533 transitive 红全部逐字对照确认 master 既有。
5. **仓库卫生**：vendor 零改动；主仓工作树未被污染（先在 .zcode/ 除外）。

## 终审裁定：初判 FAIL → MUST_FIX 闭合后 **PASS-WITH-NOTES**

- **FAIL 项**：tst_wellattributes 的 `#include <QDoubleSpinBox>` 修复停留在未提交
  工作树（HEAD 不编译该测试）——已补提交（d45f74d7），提交树重编译 + 该套件
  复跑绿，阻断解除。
- **NOTES（遗留，均已登记）**：
  1. SF-2 recycle schema 解析三份拷贝下沉（P3 OPEN）；
  2. SF-4 B-11 大输入拒绝的行为面（DOCUMENT_ONLY，文案已给出路）；
  3. A-10 stored int 截断理论洞（不可达，DOCUMENT_ONLY）；
  4. master 既有 G1 簇闸门红 10 项等 PR #580 合并自愈；
  5. tst_catalog sqliteOpenCostAttribution 裸墙钟抖动（#559 家族）。

## Oracle 对账（任务书 §11）

- O1 基线可信 ✓（BASE_SHA/排除矩阵/环境事件分账 06）
- O2 审查完整 ✓（上轮移交缺口补齐 + 新码全量 + 如实未核查面 02）
- O3 Fix 1 ✓（十簇 24 findings 修复，5 簇红绿双证）
- O4 独立复审 ✓（R2 双盲式挑战，2 MUST_FIX + 5 SHOULD_FIX）
- O5 Fix 2 ✓（R2 全部 MUST_FIX 闭合 + SHOULD_FIX 4/5 落地）
- O6 终局验收 ✓（独立终审 PASS-WITH-NOTES；426/440 零新增回归；基线零违约）
- O7 交付：分支推送 + PR（见 99-pr-body.md）
