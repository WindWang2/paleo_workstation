# Review 2 与 Fix 2 结果（full-audit-20261019）

## R2 复审（两位独立复审者，均未参与修复编写；全文 /tmp/paleo-audit-r3/review-r2-{1,2}.md）

- R2-1（正确性/并发）：MUST_FIX×1 / SHOULD_FIX×3 / DOCUMENT_ONLY×9 / FALSE_POSITIVE×2
- R2-2（算法/测试/门禁）：MUST_FIX×1 / SHOULD_FIX×1 / DOCUMENT_ONLY×9 / FALSE_POSITIVE×0
- B-01 数值修复经两位复审者**独立推演双重证实**；E2 三项测试期望值更新全部裁定
  「契约有意变更的合法夹具跟进」；E3 基线纪律裁定合规（无补登转绿）。

## MUST_FIX 处置

| 项 | 复审者 | 处置 | Commit |
|---|---|---|---|
| MA-F01 层序反转前提错误（canvas layers() 实为 index 0=顶层；假阳性发现被 Fix-1 修成回归） | R2-1 | **回退反转**：删两处 std::reverse、测试输入序还原、契约注释钉死（qgsmapsettings.h「index 0 on top」+ PreviewTocPanel 自证） | eb39c31f |
| D-01 测试不具红/绿区分力（单因素无版本路径终态与修复前一致） | R2-2 | **测试升级**：双因素各存提取，断言表内容+等值距随新因素值域；证实 Fix-1 的 restore 有效（夹具须按勾选时页面自动对齐的口径提取——sandratio→direct+sand_ratio） | 24a8bf4a |

## SHOULD_FIX 处置

| 项 | 复审者 | 处置 | Commit |
|---|---|---|---|
| mapexport dedup 丢在盘复核（陈旧 OUTPUT 复用返回悬空路径） | R2-1 | 扫描内补 exists+sha256 复核 | c632b50f |
| B-06 warnings 零消费 | R2-1 | 进 predictionParameters（cluster_warnings）+ qWarning | c632b50f |
| D-01 无版本路径建议残留 | 双方 | **终审 FR-1/FR-2 更正**：此前「已闭合」声明不实——有保存版本路径由既有字段组合框链自愈（与 Fix-1 无关），无版本换勾路径的残留（constraintpage.cpp:233 清表前读旧值域 + 失败路径无重算者）**仍在，登记 OPEN**；重构两案破坏既有套件已回退；升级测试保留为终态钉子（无红/绿区分力，如实降级声明） | 残留 OPEN（移交后续批次，PR Known Limitations 在案） |

## 试错记录（诚实留痕）

D-01 重构两案（wireWellFactorTriggers 接线前置；itemChanged 单一所有者重整）
分别破坏 tst_mappingpages 既有语义（无工作流种子表建议）与 wellFactorExtractionPayload
等 3 用例——**按实证全部回退到 Fix-1 形态**，仅保留测试升级。教训：双 handler
耦合的 UI 路径改动须以既有全套件为护栏，勿凭调用序推断。

## DOCUMENT_ONLY（择要）

半格配准二义性注释、files>1 无专属回归（diag 套件覆盖主路径）、重复注释、
mapexport 线性扫描性能（O(版本数)/次可接受）等 9+9 条，见复审全文；
均登记不另改码。

## Fix 2 后状态

定向套件全绿（previewmap_identify 15/15、wellattributes 17/17、
exportpipeline/mapping/mappingworkbench/horizonbatchexport×2/factorworkflow/
mappingpages/import/ingestplan/onnx* 全绿）；tst_ui 仅剩基线既有 legendTitle 红。
