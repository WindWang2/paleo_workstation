# 任务 A（数据/资产/图谱深化 2026-10-13）：资料图谱契约/性能回归
# （tst_datagraph：#424 交付的 datagraph 家族首个直接测试）。
add_paleo_test(tst_datagraph LIBS paleo_workflow paleo_io)
add_paleo_test(tst_dataopssidecar LIBS paleo_store)  # sidecar 原子写/损坏告警
