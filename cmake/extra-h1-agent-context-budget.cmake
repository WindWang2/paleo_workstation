# H1（agent-context-budget，2026-11-22）：长会话上下文投影与请求预算专属
# 接线。生产改动在既有 paleo_workflow 源内（aichatcontextprojection 新组件 +
# aichatcontroller 装配接缝），此处挂新源与测试。
target_sources(paleo_workflow PRIVATE
  src/workflow/aichatcontextprojection.cpp
)
add_paleo_test(tst_h1_contextprojection LIBS paleo_workflow paleo_ai)
add_paleo_test(tst_h1_requestassembly LIBS paleo_workflow paleo_ai)
