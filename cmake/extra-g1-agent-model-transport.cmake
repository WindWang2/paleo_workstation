# 并行方向挂点：G1 agent-model-transport（2026-11-21）
# 模型通信：有界传输（attempt 冻结/双时限/字节预算）、SSE 流式协议
# （按字节边界分帧、工具调用完整性、统一终态裁决）、配置提交一致性
# 与对外内容凭据边界。
#
# 约定见 CMakeLists.txt「并行开发挂点」：本任务新增的源文件/测试在这里
# 登记；对已有模块（paleo_ai/paleo_workflow）只追加私有 TU，不动既有清单。
target_sources(paleo_ai PRIVATE
  src/ai/chat/llmstreamparser.cpp    # SSE 解析器实现（自 llmclient.cpp 拆出）
  src/ai/chat/llmcredentialguard.cpp # 结构化凭据防回显（llmclient/runner 共用）
  src/ai/chat/llmconfigcommit.cpp    # applyConfig 串行提交/补偿协议
)

# 直连 LlmClient 的传输回归（QTcpServer 假服务器 + QNetworkReply 替身）。
add_paleo_test(tst_g1_modeltransport LIBS paleo_ai)
# 配置提交协议（注入式存储钩子）与 applyConfig 接线。
add_paleo_test(tst_g1_configcommit LIBS paleo_workflow paleo_ai)
# 凭据边界：runFaciesModels 两出口 + LlmClient 诊断脱敏（真实 HTTP 夹具）。
add_paleo_test(tst_g1_credentialboundary LIBS paleo_app Paleo::paleo_data_service)
