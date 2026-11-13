# Agent W1：合法工具结果与会话报告引用

对应[整改计划 W1](../planning/AGENT_CALL_READINESS_PLAN.md)。源码起点为 master
`dd7baa1f`；本批只调整结果传输、报告读取及工具轮预算，W4 查询源分页、W5 持久恢复和
W6 推理发布仍分批实施。

状态：生产 CMake 相关目标编译、5 套回归和 fetch/rebase 后 34 项源码门禁均通过，待提 PR。

## 实现

- 模型结果使用 `paleo.tool-result.v1` envelope，按实际 compact JSON 的 UTF-8 字节
  计预算，不再对 JSON `left(4000)`。原生 runner `toolFinished` 及原生结果 schema
  保持不变，`AiToolSpec::modelResultSchema()` 单独描述 envelope 的 `data`。
- transport/coverage 与业务结果分开。原 gateway 的 `execution`、`commitState`、
  `presentation`、`receipt`、`resultRef` 和 operationId 均按原事实保留；已提交但日志
  终结失败/缓存不足的回执不能改成“发布未发生”。无法装入的元数据明确省略并标覆盖缺项，
  不裁切身份。末端逐字段测量转义后的 JSON，保证最小预算下也不越界。
- 新增实际登记的只读 `paleo.read_result`。临时引用只保存不可变 JSON 值，支持数组/对象页、
  JSON Pointer 子树和 Unicode 码点字符串片段；签名游标绑定引用、selector、模式及位置。
  不生成“读取结果的结果引用”，不从文件路径读取，也不把 operation 凭证当聊天引用解析。
- 缓存按 compact JSON 字节计账：单结果 1 MiB、总量 8 MiB、128 句柄、单调时钟 TTL
  30 分钟。宿主只能收紧这些限额；容量不足不提前驱逐承诺期内的引用。聊天、文档、
  documentEpoch 或任务会话改变会失效旧引用；缓存不写工程，也不要求工程写锁。
- 同一用户请求跨最多五次工具往返累计预算，单模型应答最多 8 调用、整轮最多 16 调用，
  实参不超过 16 KiB。准入前预留最小合法应答；整帧拒绝时零执行，不留下未配对的调用。
  估算 token 继续使用既有字符近似，不宣称等价于服务端 tokenizer。
- 历史坏 JSON 只在再次发送时转换为 `legacy_result_unavailable`，保留磁盘原文和调用配对。
  UI 仍接收原生结果卡片/摘要。同步取消/切会话的回包重入先完成 pending 状态变更，
  防止已完成调用再补取消结果或取消后继续发送模型请求。
- 协议值类型和会话缓存分别编进 `paleo_services_toolresults` / `paleo_workflow_results`，
  新增 Qt Core-only 的 `tst_chatresults`；现有 controller/query/toolloop 继续覆盖真实路由。

## 当前验证证据

- Linux GCC 16.2.1 / Qt 6.11.2：Qt Core-only 原生壳编译生产协议与缓存实现，
  `tst_chatresults` **15 通过、0 失败、0 跳过**。覆盖大数组/嵌套对象、超过 100 KiB 的
  Unicode 字符串精确重组、伪造/跨 selector 游标、作用域/TTL、缓存满、真实 gateway
  已提交/回放形状以及控制字符导致的转义体积膨胀。
- CI Source gates 及附加 qgis-spread/分诊/门禁规则自检：**34 通过、0 失败**。
  传递检查使用锁版 QGIS 4.2.3 头；未增加或降低任何源码基线。
- 完整生产 CMake configure 及相关目标编译成功，Qt 6.11.2 / QGIS 4.2.3 / ORT 1.30.0；
  `office-editor` 真实接线，无缺失警告。5 套 CTest 全部通过：

  | 套件 | 业务/数据行用例 | 含 init/cleanup 的 QtTest 成功计数 |
  |---|---:|---:|
  | tst_chatresults | 13 | 15 |
  | tst_aichat | 13 | 15 |
  | tst_aichatprojectquery | 13 | 15 |
  | tst_aichatcontroller | 21 | 23 |
  | tst_aichattoolloop | 12 | 14 |
  | 合计 | **72** | **82** |

  以上各套均 0 失败、0 跳过。工具闭环使用真实 QGIS/ORT 夹具与本地 HTTP 端点，
  包含生产 runner 的 read_result 接线、跨聊天/文档世代失效、完整请求捕获及回包同步取消。

实际验证命令（build 独立于其他分支，构建 `-j2`、测试串行）：

```bash
git fetch origin && git rebase origin/master
source ../paleo-agent-plan-20261113/local/agent-build/common-env.sh
cmake --build /home/kevin/build-paleo-agent-results --target \
  tst_chatresults tst_aichatcontroller tst_aichat tst_aichatprojectquery tst_aichattoolloop -j2
ctest --test-dir /home/kevin/build-paleo-agent-results \
  -R '^(tst_chatresults|tst_aichatcontroller|tst_aichat|tst_aichatprojectquery|tst_aichattoolloop)$' \
  --output-on-failure -j1
QGIS_PREFIX="$QGIS_PREFIX_PATH" python local/w1-source-gates.py
```

配置使用 `configure-agent.sh <本 worktree> /home/kevin/build-paleo-agent-results`；环境脚本是
本机验证接线，产品依赖/测试布线仍来自仓内 CMake。门禁壳逐项执行 CI Source gates 的
28 条命令，另加 qgis-spread 自检、红项名单校验/自检、flaky 台账自检和门禁规则校验/自检。
原始日志：`/home/kevin/paleo-w1-build-final.log`、`/home/kevin/paleo-w1-tests-final.log`、
构建树 `Testing/qtest-first-run/` 和本 worktree 的 `local/w1-source-gates.json`。

验证边界：本批没有本机 Windows 编译/回归、全应用视觉检查或真实工区/远端数据验收。
查询源分页及外部操作恢复仍属于 W4/W5，不能以报告缓存分页代替。

首轮已修复证据：测试辅助代码误用了 QJsonArray 的不存在方法；独立复审复现实际
gateway 提交字段丢失，以及控制字符使 768 字节预算膨胀到 797 字节。两处均加入回归并修复。
一次附加验证壳写错了分诊脚本名，改为仓内真实 `ci_triage_reds.py` 后全闸通过；未改检查器。
生产回归首次编译另修复了新增 SSE 夹具的对象构造括号与会话保存错误参数；修复后重新
构建全部相关目标并跑完上述 5 套回归，不以仅编译新协议文件替代集成验证。
