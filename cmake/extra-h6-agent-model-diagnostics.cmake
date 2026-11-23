# 并行方向挂点：H6 agent-model-diagnostics（2026-11-22 批次）
# 模型接入诊断与兼容探测：有界模型目录发现（GET /models，可缺席）、
# 文本/流式探测（复用生产 LlmClient，不改 G1 状态机）、工具往返内存协议
# 探测（诊断专用 echo，零业务副作用）、有界脱敏报告、原生配置流程装配。
# 约定见 CMakeLists.txt「并行开发挂点」：本任务新增源文件/测试在这里登记。
target_sources(paleo_workflow PRIVATE
  src/workflow/llmconnectiondiagnostics.cpp  # 功能层：诊断协调器（W2/W4/W5）
  src/workflow/llmmodelinventory.cpp         # 功能层：有界目录发现（W3）
  src/workflow/llmdiagnosticreport.cpp       # 功能层：报告渲染（W6）
)
target_sources(paleo_ui PRIVATE
  src/ui/ai/llmdiagnosticspanel.cpp          # 视图层：诊断面板（W7）
)

# 诊断服务回归（QTcpServer 假服务器：目录/文本/流式/工具往返/取消/迟到/
# 凭据防回显/重定向/预算；真实生产入口，无外部账号）。
add_paleo_test(tst_h6_modeldiagnostics LIBS paleo_workflow paleo_ai)
# 配置对话框诊断接缝与草稿字段一致性（真实 LlmConfigDialog +
# openAiConfiguration 闭环；totalTimeoutMs 保留、maxTokens 域守卫、
# 凭据来源判定、模型选择只填草稿）。
add_paleo_test(tst_h6_llmconfiguration LIBS paleo_ui paleo_workflow paleo_ai)

# deb 闭包路线（Linux）：传递依赖（QGIS/Qt 全链）按前缀序解析——只注入测试
# 进程（ENVIRONMENT_MODIFICATION），ctest 本体不带该 LD_LIBRARY_PATH（系统
# expat/ctest 与前缀 expat 不同源，父进程注入会符号冲突；同 ORT 测试先例）。
if(PALEO_DEB_PREFIX)
  set(_paleo_h6_deb_libs
    "${CMAKE_SOURCE_DIR}/vendor/prefix/usr/${PALEO_DEB_MULTIARCH}"
    "${CMAKE_SOURCE_DIR}/vendor/prefix/usr/lib")
  list(GET _paleo_h6_deb_libs 0 _paleo_h6_deb_libs_0)
  list(GET _paleo_h6_deb_libs 1 _paleo_h6_deb_libs_1)
  set(_paleo_h6_deb_plugins
    "${CMAKE_SOURCE_DIR}/vendor/prefix/usr/${PALEO_DEB_MULTIARCH}/qt6/plugins")
  set_tests_properties(tst_h6_modeldiagnostics tst_h6_llmconfiguration PROPERTIES
    ENVIRONMENT_MODIFICATION
    "LD_LIBRARY_PATH=path_list_prepend:${_paleo_h6_deb_libs_0};LD_LIBRARY_PATH=path_list_prepend:${_paleo_h6_deb_libs_1};QT_PLUGIN_PATH=path_list_prepend:${_paleo_h6_deb_plugins}")
  unset(_paleo_h6_deb_plugins)
  unset(_paleo_h6_deb_libs)
endif()
