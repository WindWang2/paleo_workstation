## 问题

CI 门禁已功能性失效：master 上最近 10 次 push 触发的 workflow run **全部 failure**，代码仍持续合入（#486–#494 均已进 master）。

## 证据

- `gh run list --event push --limit 10`（2026-10-15 ~ 10-16）：38756099683 … 38835970978 结论全为 `failure`。
- run 38835970978（#494 合入后，master @ ddd1325e）job 级结论：
  `windows ❌ / lint ❌ / linux-asan ❌ / linux ✅ / linux-perf ✅`。
- AGENTS.md「PR 合并」既定流程：`DELETE branches/master/protection/required_status_checks` → `gh pr merge --squash --admin` → 恢复保护。即红灯在流程上不构成合入阻碍。

## 影响

1. 绿灯不再携带「可合入」语义，回归只能靠事后人工分诊发现。
2. 与 #475（文档化红项 CI 结构性不可见）、#484（CI 布线清单）叠加后，CI 从「质量门」退化为「事后日志」。
3. 新引入缺陷（如本次 lint 红）平均要等到下一轮人工 review 才暴露。

## 建议

- 立一条 master 健康度底线：**windows + linux 两车道必须绿**才允许合入；asan / lint 若长期不可用，要么修复要么显式下线并在 PR 模板标注「未覆盖」。
- 三条车道恢复前，每个合并显式登记红灯分诊 owner 与预计修复时间。
- 逐条关闭本批子项（B / C / D）后重估是否恢复 strict 门禁。

## 验证

`gh run list --event push --limit 5` 连续两次全绿。

## 关联

- 子项：本批 B（asan 磁盘）、C（Windows 两真红）、D（lint）
- 相关：#475、#484
