# H2（agent-session-archive，2026-11-22）：私有会话档案有界读写、容量准入
target_sources(paleo_ai PRIVATE
  src/ai/chat/chatsessionarchive.cpp
  src/ai/chat/chatsessionindex.cpp)
target_sources(paleo_workflow PRIVATE
  src/workflow/aichatrecentsummaries.cpp)
target_sources(paleo_ui PRIVATE
  src/ui/ai/aiassistdock_sessions.cpp)
add_paleo_test(tst_h2_sessionarchive LIBS paleo_ai)
add_paleo_test(tst_h2_capacitygates LIBS paleo_workflow paleo_ai)
add_paleo_test(tst_h2_recentarchives LIBS paleo_workflow paleo_ai)
add_paleo_test(tst_h2_sessioncombo LIBS paleo_ui)
