# Agent 调用可靠性实施记录

按 [W0–W6 计划](../planning/AGENT_CALL_READINESS_PLAN.md) 分批推进。
基线为 master `dd7baa1f`，与独立结构整理 PR #935 分离；不自行合并 PR。
共享依赖可复用，源码、工作分支与 build 分开。构建总并行不超过 8，Windows 测试串行。

| 批次 | 状态 | PR / 证据 |
|---|---|---|
| W0 入口基线 | 清单已写；随 W1 验证交付 | [迁移清单](../architecture/AGENT_CAPABILITY_MIGRATION.md) |
| W1 结果协议 | 已提 [PR #936](https://github.com/WindWang2/paleo_workstation/pull/936) | [72 业务/82 含 fixture，0 失败/跳过](agent-results-20261114.md) |
| W2 DAG 预检 | 已提 [PR #937](https://github.com/WindWang2/paleo_workstation/pull/937)；叠加复验通过 | [81 项/32 闸](agent-dag-preflight-20261113.md) |
| W3 可信宿主 | 已提 [PR #938](https://github.com/WindWang2/paleo_workstation/pull/938)；实际应用、13 套及 32 闸通过 | [237 含 fixture，0 失败/跳过](agent-host-context-20261114.md) |
| W4a/W4b 真分页 | 已提 [PR #939](https://github.com/WindWang2/paleo_workstation/pull/939)；实际应用、14 套及 32 闸通过 | [223 含 fixture，0 失败/跳过](agent-query-pages-20261114.md) |
| W5a/W5b 凭证与恢复 | 已提 [PR #940](https://github.com/WindWang2/paleo_workstation/pull/940)；实际应用、23 套及 32 闸通过 | [382 含 fixture，0 失败/跳过](agent-operation-recovery-20261114.md) |
| W6 效果及 provider | 已提 [PR #941](https://github.com/WindWang2/paleo_workstation/pull/941)；默认/无 ORT 实际应用与 53/41 套通过，32 闸通过 | [821/637 含 fixture，均 0 失败/跳过](agent-providers-20261114.md) |

后续 PR 采用有依赖的堆叠分支，基础分支指向前一批，PR 描述列明依赖；各批验证当前
累计代码并在开 PR 前 fetch/rebase origin/master 复跑门禁。没有获用户合并指令时不合并。
未运行的检查不记为通过；环境修复及剩余验收如实记录。

W4/W5 在共同 W3 上并行交付；W6 审阅基底为只组合这两批的内部
`codex/agent-integration-base-20261114@18098285`，避免把已独立验收的基础改动重新混进 W6 diff。
SDK 配套 [#18](https://github.com/changyanyanchang/Data_service/pull/18) →
[#19](https://github.com/changyanyanchang/Data_service/pull/19) →
[#20](https://github.com/changyanyanchang/Data_service/pull/20)，最终 pin `8c0a329`。
