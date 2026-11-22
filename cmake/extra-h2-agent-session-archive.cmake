# H2（agent-session-archive，2026-11-22）：私有会话档案有界读写、容量准入
# 与轻量最近列表。生产源：ai/chat 的档案契约/索引组件 + workflow 的查询
# 协调器；控制器与 dock 的改动在既有 paleo_workflow / paleo_ui 源内。
target_sources(paleo_ai PRIVATE
  src/ai/chat/chatsessionarchive.cpp
  src/ai/chat/chatsessionindex.cpp)
target_sources(paleo_workflow PRIVATE
  src/workflow/aichatrecentsummaries.cpp)
add_paleo_test(tst_h2_sessionarchive LIBS paleo_ai)
add_paleo_test(tst_h2_capacitygates LIBS paleo_workflow paleo_ai)
add_paleo_test(tst_h2_recentarchives LIBS paleo_workflow paleo_ai)
add_paleo_test(tst_h2_sessioncombo LIBS paleo_ui)
