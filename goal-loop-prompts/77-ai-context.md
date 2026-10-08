# Goal-Loop 方向 77：AI 工程上下文注入——query_project 工具 + 目录/血缘摘要进对话

## 背景（实测事实，勿再勘察；行号为 2026-10-08 master `e3c8d31d` 口径）

方向 61/62 交付工具闭环与会话/配置/markdown 后，AI 助手
「能聊天但不知道工程里有什么」——**零工程上下文注入**是当前
AI 面最实质缺口：

- **system prompt 只有工具表 + 地质红线**（`src/workflow/
  aichatcontroller.h:47` 注释自证），不含工程目录/catalog/
  血缘任何信息。rg 证据：`src/workflow` 全目录
  `DataCatalog|catalog` 在 aichatcontroller.cpp 与
  aichattoolrunner.cpp **零命中**。
- **上下文绑定只绑了两样**：`src/app/aiwiring.cpp:70-73` 只绑
  `context.horizon`（AreaRules 目标层位）与 `gridFetch`；
  **traceFetch / faciesInput 明确未绑**（递延，aiwiring.cpp:72
  注释「执行器按调用如实报错」）。
- **对照组（先例已在）**：`AiAssistWorkflow` 有完整 catalog
  接入（`aiassistworkflow.cpp:176` setCatalog、`:234`
  DerivedAssetRegistrar）；血缘只读服务
  `paleo::derivation::Service::build/selectionClosure`
  （`src/services/derivationgraph.h:42-46`）；血缘反链消费
  先例 `constraintfactorjobs_factor.cpp:244`。
- **runner 现状（闭环已通）**：三工具全部 `Routed`（`src/ai/
  chat/domaintools.cpp:201/:214/:226`），执行经 `aichattoolrunner.h:16-27`
  映射到 AiAssistWorkflow/WellFaciesService，串行 FIFO +
  世代号取消守卫；预算常量 `kMaxHistoryTurns=12 /
  kHistoryTokenBudget=6000 / kToolResultCharLimit=4000 /
  kMaxToolRounds=5`（aichatcontroller.h:82-85）。

本方向补上「第四工具」：让助手能回答「工程里有哪些井/层位/
资产、这个图件怎么来的」类问题。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\ai-context -b goal/ai-context-20261009 origin/master
cd .worktrees\ai-context
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。LLM 测试用假端点
（先例：方向 61 的 tst_aichattoolloop）。

## 目标形态（建议按序）

1. **query_project 工具**：`src/ai/chat/domaintools` 增第四
  （或并行分组）工具——只读工程问答：实体/资产/角色/版本
   摘要（井名清单、层位清单、某类型资产计数、某井有什么
   曲线）。catalog 只读面复用 `AiAssistWorkflow` 的接入模式。
2. **血缘查询**：`selectionClosure` 接为工具出参（某资产的
   派生链可视化数据源）——回答「这个相栅格是从哪些井点+
   约束线来的」。只读，零登记副作用。
3. **绑定接线**：`bindChatToolRunner`（`src/app/aiwiring.cpp`）
   增 catalog/derivation 注入（appcontext 已装配两者——
   装配根接线）；顺手收编两个递延绑定点：traceFetch（地震体
   道窗服务）、faciesInput（井缓存曲线组装）——若跨域服务
   依赖过重则如实递延（TODOS:20-27 已挂），不强做。
4. **上下文预算协同**：query 结果超 `kToolResultCharLimit=4000`
   时截断（既有纪律沿用）；工程摘要段（井数/层位数/资产
   计数一行式）作为 system prompt 常驻轻注入（不超过 2-3
   行，尊重 6000 token 预算）。
5. **只读红线**：新工具零 catalog 写副作用（不在 DerivedAsset
   通道登记）；回答引用数据带版本/来源（可追溯口径）。
6. **测试**：query 工具假 catalog 单测（实体枚举/角色过滤/
   截断）；血缘 closure 出参正确性；绑定/换工程重绑
   （appcontext 工程切换信号）；端到端假端点全链路。

## 通用纪律（方向内全程有效）

- **分层**：工具 schema 归 ai/chat（数据层），执行编排归
  workflow（aichattoolrunner 扩展），绑定归 app/aiwiring；
  `check_layering.py --strict` 绿。
- **诚实面**：无工程打开时工具如实 Disabled；数据不足直说
  （红线沿用）；版本/来源引用不编造。
- **安全**：只读工具无写副作用（测试断言 catalog 快照前后
  一致）；密钥/端点不入上下文。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行。
- **无人值守**：query 出参 schema 自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-ai-context.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/只读红线/schema 正确性/预算协同/i18n 五维）→ 修复
  → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. query 工具：假 catalog 下实体枚举/角色过滤/截断单测全绿；
   出参 JSON schema 校验。
2. 血缘：合成派生链的 selectionClosure 出参与直算一致
  （对拍断言）；引用带版本路径。
3. 绑定：appcontext 换工程 → 工具上下文重绑（重绑信号测试）；
   traceFetch/faciesInput 要么绑定要么如实递延（ledger 定案）。
4. 只读：工具执行前后 catalog 快照逐字节一致（测试）。
5. 预算：工程摘要常驻段 + 12 轮历史 + 4000 字符工具截断下
   请求体规模断言不超预算。
6. 端到端：假端点全链路（问「工程里有哪些井」→工具调用→
   结果回灌→含井名的终答）绿。
7. 回归：ai/chat 全族既有测试绿；全量 ctest 对照 R0 红集合
   diff 为空。
