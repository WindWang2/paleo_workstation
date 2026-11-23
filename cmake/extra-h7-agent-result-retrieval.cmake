# H7（agent-result-retrieval，2026-11-22）：临时工具结果在最终应答配额下的
# 连续检索与完整性。生产改动在既有 chatresultstore* / aichattoolrunner* /
# aichatcontroller_toolloop / toolresultprotocol 源内，此处只挂本任务测试：
#   · tst_h7_resultbudget    纯值/存储：最终 envelope 计价、连续分页、TTL。
#   · tst_h7_resultwire      真实 controller/runner/loopback（原生 + 受控 Pi）。
add_paleo_test(tst_h7_resultbudget LIBS paleo_workflow_results)
add_paleo_test(tst_h7_resultwire LIBS paleo_workflow paleo_ai)
