# H6｜模型接入诊断与兼容探测——交付记录（2026-11-22/23）

- 分支：`codex/h6-agent-model-diagnostics`（独立 worktree `../paleo-h6-agent-model-diagnostics`）
- base：`origin/master = 8eca4644db31b4dcfd5ff11bebf39f0bbcfaf3a5`（G1–G5 与 #1068 整合复审合入后主干；开工时核实 0 个开放 PR）
- head：实现 `79c1f3c6` + 独立复审修正 `64d75fd9`
- 平台：Linux（CachyOS，gcc/C++20，Qt 6.10.2 + QGIS 4.2 来自项目受控 deb 闭包前缀 `vendor/prefix/usr`）
- 专属 fragment：`cmake/extra-h6-agent-model-diagnostics.cmake`（根 CMakeLists manifest 追加 1 行，不动并行条目）

## 一、交付范围（八个工作包全部落地）

| 工作包 | 生产入口 | 状态 |
|---|---|---|
| W1 候选合同 | `src/workflow/llmdiagnosticcontract.h`（候选/观测/报告值类型、端点身份与凭据来源判定助手）+ `llmconfigdialog.cpp` 的 `formConfig`（totalTimeoutMs 原样流转）与 `accept`（maxTokens 域守卫） | 完成 |
| W2 隔离生命周期 | `src/workflow/llmconnectiondiagnostics.{h,cpp}`（显式启动、单飞、取消单终态、迟到结果不覆盖新候选、专属网络对象随对话框析构终止） | 完成 |
| W3 有界目录发现 | `src/workflow/llmmodelinventory.{h,cpp}`（GET `/models`：单次、双时限、2 MiB 字节预算、条目/长度钳制、精确去重、凭据回显剔除、404/405/空/未知形状/无权限/限流分项） | 完成 |
| W4 文本/流式探测 | 同协调器（复用生产 LlmClient；流式动作恒 `stream=true`；探测输出上限 ≤ 候选值并记录实际值） | 完成 |
| W5 工具往返 | 同协调器（诊断专用 `paleo.diagnostic_echo`、严格帧匹配、原样帧回放、恰好 ≤2 请求、零第三轮、不经 runner/Host/Python/Pi） | 完成 |
| W6 报告 | `src/workflow/llmdiagnosticreport.{h,cpp}`（分项独立、真实 SSE 恒标未独立确认、usage 服务端报告口径、证据支持的建议、有界截断） | 完成 |
| W7 视图与装配 | `src/ui/ai/llmdiagnosticspanel.{h,cpp}`（纯文本报告/候选列表/导出摘要）+ `llmconfigdialog.{h,cpp}` 薄缝（诊断意图、候选装配、只填草稿）+ `aiconfigurationwiring.cpp`（装配诊断对象，不触 controller 编排） | 完成 |
| W8 真实本地验证 | `tests/tst_h6_modeldiagnostics.cpp`（27 例，QTcpServer 对账真实 method/path/Authorization/body）+ `tests/tst_h6_llmconfiguration.cpp`（9 例，真实 LlmConfigDialog 与 openAiConfiguration 闭环） | 完成 |

## 二、本地验证结果（真实运行，非源码推断）

构建：Ninja + `-j4`（全局互斥锁纪律）；配置走 deb 闭包路线
（`QGIS_PREFIX=vendor/prefix/usr` + toolchain 提供 `CMAKE_LIBRARY_ARCHITECTURE`
——Arch 宿主无 multiarch 自动探测；receipt 校验照常执行，未绕过任何锁）。

| 测试 | 结果 |
|---|---|
| `tst_h6_modeldiagnostics` | **27 passed, 0 failed**（1.0s） |
| `tst_h6_llmconfiguration` | **9 passed, 0 failed**（0.2s，真实模态 `openAiConfiguration` 闭环含目录缺席→手填模型→文本探测→仅原 apply 生效） |
| `tst_llmconfigdialog` | 9 passed（表单/busy/密钥掩码契约不回归） |
| `tst_g1_modeltransport` | 65 passed（G1 状态机未触碰） |
| `tst_g1_configcommit` | 19 passed（apply 提交语义保持） |
| `tst_aichatcontroller` | 24 passed |
| `tst_aiassistdock` | 11 passed |
| `tst_g1_credentialboundary` | 11 passed（凭据防回显不回归） |

Source gates（本地实跑）：
`check_layering.py`（普通+`--strict`+`--qgs-audit` 134/134 零新增+
`--transitive`（含受控 `QGIS_PREFIX` 的 func/ui 口径））、`check_file_budget.py
--strict`（新增 0）、`check_ui_size.py`、`check_ui_tokens.py`（violations 0）、
`check_i18n.py`、`check_docs_drift.py`、`cmake/verify-cpp-mainline.cmake` 全绿。

### 覆盖的验收矩阵场景（节选，全部断言于上述两测试）

- 只打开设置/编辑/关闭：零 HTTP、零配置保存（监听中服务器计数 0 + 配置文件字节对比）。
- 未改 totalTimeoutMs 只改模型名 → apply：非默认总时限保持，其余字段无回退。
- maxTokens 超持久化域：打开保留显示 + 打开即提示 + 应用可见拒绝 + 域内值恢复可应用。
- 草稿换 endpoint/base path 且未给目标凭据：本地拒绝零网络（两个端点的请求/Authorization 计数）。
- 目录 404/405/空/未知形状/条目全剔除：分项如实、文本能力不受污染。
- 目录列出模型但补全 401：GET/POST 分别记录，按实际阶段分类。
- stream=true 返回普通 JSON：客户端兼容成功 + 真实 SSE 恒标未独立确认（双向钉住）。
- 工具帧矩阵：未调用（Indeterminate）/错 nonce/未知名/多帧/严格匹配（原样帧回放 + call ID 配对 + 恰 2 请求）/后续再请求工具（零第三轮）。
- 取消/销毁：首请求在途取消零后续、owner 析构安全终止零续发。
- 重定向 307：不跟随、第二端点零请求零凭据。
- 非 2xx 伪成功体：HTTP 分类优先，不冒充通过。
- 单飞/迟到：在途再启动本地拒绝零额外网络；旧候选挂起取消后新候选报告隔离。
- 无工程/Host/历史：真实 `openAiConfiguration` + 裸 `AiChatController` 完成同一诊断闭环。

## 三、独立复审

由未参与实现的独立 reviewer 审查真实 diff（`8eca4644..79c1f3c6`）与调用链，
逐项核查只读边界/分层/凭据流/生命周期/工具协议/诚实口径/测试语义/CMake，
结论「可合入，无 P0/P1」。8 项 P2 建议中 6 项已落地为 `64d75fd9`（空可用条目
Empty 语义、缺 id 计入形状非法、计数先于 send、填入草稿初始禁用、工具终局
注明实际 stream 模式、GET 去除冗余头）+ 新增销毁期与全剔除目录两用例；
其余 2 项（目录也要求凭据、显式默认端口判不等）为已记录的保守设计决策。

## 四、边界与未证明事项

- 真实 SSE 是否发生不可由公开信号证明——报告恒标「未独立确认」（不改 G1
  状态机换取过强结论；G1 的非 SSE fallback 原样保留）。
- usage 为服务端报告值；缺省显示「未报告」，不显示精确零 token/费用。
- 目录也要求凭据（统一「本地拒绝而非静默无凭据探测」口径，杜绝旧 key 搬运
  风险）；公开免鉴权 `/models` 的服务在无 key 时不可目录发现——诚实提示，
  可手填模型继续。
- 工具往返只证明协议帧兼容（echo 内存往返），不冒充业务工具执行/发布验证。
- 环境备注：本机（Arch）无 superbuild 收据且 deb 闭包为唯一受控前缀路线；
  测试进程经 fragment 的 `ENVIRONMENT_MODIFICATION`（`PALEO_DEB_PREFIX` 守卫）
  获得前缀库/插件路径，ctest 本体不带该环境（父进程 expat 冲突规避）；
  superbuild/Windows 路线不受影响（守卫跳过）。

## 五、遗留

- 无新增免测豁免；未触碰 `tools/*-baseline.txt`（只缩不涨纪律未被扰动）。
- 协调记录见 `.git/paleo-parallel-20261122-agent/h6-agent-model-diagnostics.md`
  （worktree 私有，不入库）。
