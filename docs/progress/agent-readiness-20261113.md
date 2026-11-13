# Agent 调用可靠性实施记录

按 [W0–W6 计划](../planning/AGENT_CALL_READINESS_PLAN.md) 分批推进。
基线为 master `dd7baa1f`，与独立结构整理 PR #935 分离；不自行合并 PR。
共享依赖可复用，源码、工作分支与 build 分开。构建总并行不超过 8，Windows 测试串行。

| 批次 | 状态 | PR / 证据 |
|---|---|---|
| W0 入口基线 | 清单已写；随 W1 验证交付 | [迁移清单](../architecture/AGENT_CAPABILITY_MIGRATION.md) |
| W1 结果协议 | 编译、5 套回归及 rebase 后 34 项门禁通过；待 PR | [72 业务/82 含 fixture，0 失败/跳过](agent-results-20261114.md) |
| W2 DAG 预检 | 独立分支 81 项/32 闸绿；待叠 W1 复验及 PR | 未完成累计分支验收 |
| W3 可信宿主 | 实施中，独立分支 | 未完成批次验收 |
| W4a/W4b 真分页 | 实施中，独立分支 | 未完成批次验收 |
| W5a/W5b 凭证与恢复 | 实施中，独立分支 | 未完成批次验收 |
| W6 效果及 provider | 实施中，独立分支 | 未完成批次验收 |

后续 PR 采用有依赖的堆叠分支，基础分支指向前一批，PR 描述列明依赖；各批验证当前
累计代码并在开 PR 前 fetch/rebase origin/master 复跑门禁。没有获用户合并指令时不合并。
未运行的检查不记为通过；环境修复及剩余验收如实记录。
