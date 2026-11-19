# P3/factor-publication — 单因素与约束插值的单元身份和证据发布（2026-11-19）。
# 源与测试集中在此；公共清单只登记本文件。
#  - workflow/constraintanalysisidentity：引擎准备入口的稳定单元身份裁决
#   （DS 实体核验、成果键/约束界面键双语义、调用方父版本核验、Host constraint.idw
#   受信冻结上下文）。
#  - workflow/factorevidenceconfirmation：发布尾部「受管提交 vs 权威证据确认」
#   完成状态收口（复用幂等投影器；evidence_pending 持久化 + 定向重试）。
#  - 测试：tst_p3_factor_identity（单元身份/血缘矩阵）、tst_p3_factor_evidence
#   （证据确认/失败注入/幂等恢复）。

target_sources(paleo_workflow PRIVATE
  src/workflow/constraintanalysisidentity.cpp
  src/workflow/factorevidenceconfirmation.cpp)

add_paleo_test(tst_p3_factor_identity LIBS paleo_workflow paleo_qgis Paleo::paleo_data_service)
add_paleo_test(tst_p3_factor_evidence LIBS paleo_workflow Paleo::paleo_data_service)
