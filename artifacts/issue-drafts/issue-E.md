## 问题

**绿灯通胀**：48 条环境债豁免 + 「非崩溃失败串行重跑通过即放行」两套机制都只有增加通道、没有减少通道，真实缺陷会被系统性吞掉。

## 证据

- 豁免名单：`r0-red-set.txt` 26 条 + `r1-red-set.txt` 22 条 = 48 条。相对 win 388 / linux 373 的测试总量约 12%，即这部分红不影响结论。
- `tools/ci_ctest_gate.sh`：并行轮失败后，只要不匹配崩溃特征（`***Exception|Subprocess aborted|Child aborted|Subprocess killed|Illegal|SegFault`），就串行重跑失败项一次，**重跑通过即放行**，仅留 warning。
- 实证（run 38835970978，linux 车道）：373 项 2 红 `tst_wellsiting_kernel`、`tst_evidencee2e` → 串行重跑通过 → job 判 `success`。

## 影响

豁免与 flaky 放行本身是必要机制（#81 已经把崩溃从放行名单里摘出去，是对的），但当前缺少到期复审：

- 超时类失败可能掩盖真实性能退化（TODOS.md 已记录 golden 17/48 超容差、tst_cache_las、tst_perf_catalog、tst_singlefactor_perf 等文档化红项，同族）。
- 偶发竞态被吞掉后，直到某天在用户机器上复现才被发现。

## 建议

- 给 r0/r1 名单加「到期复审」字段与季度重估，目标**只减不增**；`tools/check_env_redset.py` 对「名单内项连续 N 次绿」提示出名单。
- flaky 重跑放行时写入固定台账（仓库内文件或 `flaky` 标签 issue），按周复审；同一测试命中 N 次升级为硬红。
- 把串行重跑放行的 warning 提升为 PR 可见的 annotation（`::warning::` 已在，但需在 PR 评审面可见）。

## 验证

出现至少一条「出名单」记录，且 flaky 台账可查。

## 关联

父项：本批 A；相关：#475（文档化红项 CI 不可见）
