# G2（agent-turn-execution，2026-11-21）：回合执行闭环专属测试接线。
# 原生/受控 Pi 两引擎的落盘边界与终态一致性；生产改动在既有
# paleo_workflow 源内（aichatcontroller* / pichatbridge*），此处只挂测试。
add_paleo_test(tst_g2_turncheckpoint LIBS paleo_workflow paleo_ai)
add_paleo_test(tst_g2_piterminal LIBS paleo_workflow)
