# 并行方向挂点：goal/ai-assist-20261006（方向51）
# Mock 路由接线 + LLM 地质对话助手骨架。
#
# 约定见 CMakeLists.txt「并行开发挂点」：新增源文件/测试一律在这里登记，
# 不直接改根 CMakeLists 的模块源列表（并行分支在此汇合零冲突）。
#
# 方向51 的四件事：
#   ① src/app/aiwiring.cpp —— 装配根：MappingWorkbench 的预测服务来自
#      RemotePredictionRouter（替身 Mock 已从生产装配路径退场）。
#   ② src/ai/remotepredictrouter.{h,cpp} —— 传输改为异步信号链（零 QEventLoop）。
#   ③ src/ai/chat/* —— 对话域模型 / 会话持久化 / LLM 客户端 / 领域工具表。
#   ④ src/ui/ai/* + src/workflow/aichatcontroller.* —— 助手 dock 与其编排。
target_sources(paleo_ai PRIVATE
  src/ai/remotepredictrouter.cpp
  src/ai/remotepredictconfig.cpp
  src/ai/chat/chatmessage.cpp
  src/ai/chat/chatsession.cpp
  src/ai/chat/llmclient.cpp
  src/ai/chat/llmkeystore.cpp
  src/ai/chat/domaintools.cpp
  src/ai/chat/markdown.cpp
)
# ORT 本地降级引擎：TU 里用到 PaleoOnnxService 的符号，只在 ORT 构建里编。
if(PALEO_HAVE_ORT)
  target_sources(paleo_ai PRIVATE src/ai/onnxlocalpredictor.cpp)
endif()
target_include_directories(paleo_ai PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(paleo_ai PUBLIC Qt6::Network)

# 编排面（功能层）：会话 + 客户端 + 工具分发的对外那一层。
target_sources(paleo_workflow PRIVATE src/workflow/aichatcontroller.cpp)

# 装配根：远端预测装配（唯一的装配入口，产品与测试同源）。
target_sources(paleo_app PRIVATE src/app/aiwiring.cpp)

# 视图层：助手 dock。
target_sources(paleo_ui PRIVATE src/ui/ai/aiassistdock.cpp)

# ---- 测试 ----
# 刻意不走 ORT 门：异步路由/降级语义用桩 LocalPredictor 即可覆盖，
# 有无 ONNX 运行库的构建都跑同一套断言。
add_paleo_test(tst_aichat LIBS paleo_ai)
# 装配断言：产品与测试同源（app/aiwiring.cpp）。用 paleo_app 最小链接集，
# 绕开伞式 paleo_core 在本机的 0xc0000139（见 ledger「宿主红项」）。
add_paleo_test(tst_aiwiring LIBS paleo_app)
add_paleo_test(tst_aichatcontroller LIBS paleo_workflow paleo_ai)
add_paleo_test(tst_aiassistdock LIBS paleo_ui)
# 方向62：markdown 转换器（纯逻辑，无 UI）。
add_paleo_test(tst_aimarkdown LIBS paleo_ai)
