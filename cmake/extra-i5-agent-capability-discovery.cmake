# 并行方向挂点：I5 agent-capability-discovery（2026-11-23 批次）
# Agent 使用的原子能力发现、精确契约解释与纯候选参数预检：
# paleo.capability_inspect（list/describe/check 封闭三模式）。
# services 层只读投影 + AlgorithmSpecRegistry 全版本枚举接缝；workflow 层
# 纯检查服务与 runner 分支；ai/chat 工具声明。零业务副作用：不 prepare/
# start、不占 Host 票据、不触发 provider/模型/文件读取/UI。
# 约定见 CMakeLists.txt「并行开发挂点」：新增源文件/测试一律在这里登记。
target_sources(paleo_services PRIVATE
  src/services/capabilitydescription.cpp       # 数据层：三注册表纯值投影 DTO
)
target_sources(paleo_ai PRIVATE
  src/ai/chat/capabilityinspectiontool.cpp     # 功能层：工具声明/schema/契约常量
)
target_sources(paleo_workflow PRIVATE
  src/workflow/agentcapabilityinspection.cpp   # 功能层：list/describe/check 纯检查服务
  src/workflow/aichattoolrunner_capabilities.cpp # 功能层：runner 只读分支（宿主快照注入）
)

# I5 严格边界单测（无 UI）：services 投影逐字段对照注册表、多版本无遗漏/
# 不合并；检查服务反例矩阵（contours/wellfacies/AiToolSpec-only/View 命令）、
# 伪造 authority 不授予任何权限、纯策略判定与 not_checked 语义。
add_paleo_test(tst_i5_capabilitydescription LIBS paleo_services)
add_paleo_test(tst_i5_capabilityinspection LIBS paleo_workflow paleo_ai paleo_services)
# I5 接线测试：runner 精确分支/readiness 纯路径/availableTools 登记/预算与
# 世代守卫；真实 controller + loopback 模型端点上的 list→describe→check→
# 显式原工具调用闭环（检查阶段零执行计数，最后真实执行走原链）。
add_paleo_test(tst_i5_capabilityinspection_wire LIBS paleo_workflow paleo_ai paleo_services paleo_store)
