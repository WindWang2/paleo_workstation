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

# 编排面（功能层）：会话 + 客户端 + 工具分发/执行回路的对外那一层。
target_sources(paleo_workflow PRIVATE
  src/workflow/aichatcontroller.cpp
  src/workflow/aichattoolrunner.cpp
)

# 装配根：远端预测装配（唯一的装配入口，产品与测试同源）。
target_sources(paleo_app PRIVATE src/app/aiwiring.cpp)

# 视图层：助手 dock + 配置对话框（方向62）+ 工具结果结构化视图（方向93）。
target_sources(paleo_ui PRIVATE
  src/ui/ai/aiassistdock.cpp
  src/ui/ai/llmconfigdialog.cpp
  src/ui/ai/aitoolresultview.cpp
)

# ---- 测试 ----
# 刻意不走 ORT 门：异步路由/降级语义用桩 LocalPredictor 即可覆盖，
# 有无 ONNX 运行库的构建都跑同一套断言。
add_paleo_test(tst_aichat LIBS paleo_ai)
# 方向77：工程上下文只读工具（query_project / asset_lineage）——纯 catalog 面，
# 无 ORT 依赖，有无运行库的构建都跑同一套断言。
add_paleo_test(tst_aichatprojectquery LIBS paleo_workflow paleo_ai)
# 装配断言：产品与测试同源（app/aiwiring.cpp）。用 paleo_app 最小链接集，
# 绕开伞式 paleo_core 在本机的 0xc0000139（见 ledger「宿主红项」）。
add_paleo_test(tst_aiwiring LIBS paleo_app)
# 方向95：换工程重绑段吃 mkproject mini 夹具（QProcess 全链产物）——第二源
# + 生成器表达式对齐 tst_mkprojectfixture 口径；RUN_SERIAL 同先例（QProcess
# 子进程 + QGIS init 资源面防抖）。
target_sources(tst_aiwiring PRIVATE tests/fixtures/mkprojectfixture.cpp)
target_compile_definitions(tst_aiwiring PRIVATE
  MKPROJECT_BIN="$<TARGET_FILE:paleo_mkproject>"
  MKPROJECT_MINI_DIR="${CMAKE_SOURCE_DIR}/tools/reference/mkproject/mini")
add_dependencies(tst_aiwiring paleo_mkproject)
set_tests_properties(tst_aiwiring PROPERTIES RUN_SERIAL TRUE)
add_paleo_test(tst_aichatcontroller LIBS paleo_workflow paleo_ai)
add_paleo_test(tst_aiassistdock LIBS paleo_ui)
# 方向93：工具结果结构化视图（分派/表格/键值对/血缘小图/兜底折叠）。
add_paleo_test(tst_aitoolresultview LIBS paleo_ui)
# 方向62：markdown 转换器（纯逻辑，无 UI）。
add_paleo_test(tst_aimarkdown LIBS paleo_ai)
# 方向62：图形化配置对话框（表单 round-trip / 密钥掩码 / 无钥匙串禁用态）。
add_paleo_test(tst_llmconfigdialog LIBS paleo_ui)

# 方向61：工具调用闭环（tools 上送→tool_calls 执行→role=tool 回灌→终答）。
# 真 ORT 执行面（夹具小模型 + catalog 声明断言），只在 ORT 构建跑。
if(PALEO_HAVE_ORT)
  add_paleo_test(tst_aichattoolloop LIBS paleo_workflow paleo_ai)
  # POSIX 侧 ORT 运行库路径前置（同根 CMakeLists 的 ORT 测试口径；属性放在
  # 本文件是因为测试注册在这里——根文件的 set_tests_properties 先于本 include
  # 执行，放那边会因测试不存在而 configure 失败）。
  set_tests_properties(tst_aichattoolloop PROPERTIES ENVIRONMENT_MODIFICATION
    "LD_LIBRARY_PATH=path_list_prepend:${CMAKE_SOURCE_DIR}/vendor/onnxruntime/lib")
endif()
