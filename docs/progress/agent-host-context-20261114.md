# Agent 可信宿主与统一上下文（W3）

本批叠加 [W1 #936](https://github.com/WindWang2/paleo_workstation/pull/936) 和
[W2 #937](https://github.com/WindWang2/paleo_workstation/pull/937)，对应
[实施计划](../planning/AGENT_CALL_READINESS_PLAN.md) W3。

`CapabilityHost` 在应用装配根捕获实际 DocumentSession、Catalog、工程写锁和选择上下文。
聊天、CommandDispatcher 的能力调用及文档命令写门共用宿主策略；模型给出的工程路径、
caller、current head 或可变 prepared 对象均不能授予权限。UI 上下文使用实际 documentId。

prepare 返回宿主保留的限时 opaque ticket，绑定原意图、命令、文档 epoch、效果及执行前
检查。参数篡改、换工程、失效/禁用/解绑命令、缺持久身份、只读、无写锁或恢复隔离均明确
拒绝；消费 ticket 后释放容量。重开同文档仍会使旧 epoch ticket 失效。

输入首批接受 pinned-value；未实现 follow 解析的入口返回 binding_resolution_required。
真实地震对象引用由 DocumentSession 冻结，传给 worker 前移除已消费的 JSON 输入；混合
输入拒绝，实际采样间隔由 SDK 校验并补入，prepare 不对模型展示虚构的采样值。
W5 owner 提交路径到位前，本批产品宿主明确拒绝 publish，既有旧同步执行不是发布捷径。
文档来源重定位只允许与已持久 intent 精确匹配的恢复操作穿过对应恢复门。

验证使用 GCC 16.2.1、Qt 6.11.2、锁版 QGIS 4.2.3 和 GDAL 3.12.2；Office 组件就绪。
实际应用 `paleo`、生产装配 `tst_commandshell` 及相关 target 均通过完整 CMake 编译，
构建上限 -j2。首轮新增地震 fixture 把契约枚举 rms 误写为 RMS，生产校验返回
out_of_range；修正 fixture 后五套集成回归通过。

叠加 W1/W2 后重新 configure 和编译实际应用及全部受影响测试，串行 CTest 13/13
通过（44.43 秒）：tst_capability_host、tst_commanddispatcher、tst_document_sources、
tst_aichatcapability、tst_commandshell、tst_chatresults、tst_aichatcontroller、tst_aichat、
tst_aichatprojectquery、tst_aichattoolloop、tst_capability_gateway、tst_capability_plan、
tst_capability_round2。QtTest 共 237 项含 fixture，零失败/跳过。

覆盖实际 AppContext/主窗口装配、UI/batch/chat 同数值核、真实 SDK 道窗口、只读查询、
跨 epoch 拒绝、票据篡改及 130 次连续消费、精确来源恢复。所有旧 UI 业务入口的逐项
能力迁移仍按迁移清单推进，未将“可路由”当成全面迁移完成。

fetch/rebase origin/master 后重新运行 CI Source gates 的完整命令集及附加自检，32/32
通过，传递门使用锁版 QGIS 头，所有基线未增长；git diff --check 通过。
