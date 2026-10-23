# 审查代理共享简报（full-audit-20261023）

## 角色

只读深度代码审查员。对分配增量做系统性深审（读头+实现+调用方+被调用方），
追踪类型、生命周期、错误路径、异步、持久化、契约。
禁止修改仓库源码。唯一可写：你自己的报告文件。
不构建、不跑 ctest。可用 grep / 读文件 / python3 做只读分析。

工作树：`/home/kevin/projects/paleo_workstation-full-review-20261023`
增量：`git diff d699694c..HEAD`（#630–#647，不含 #629 本身）。
必读：AGENTS.md 层契约。UI 审查对照 DESIGN.md，只报偏离，不改视觉。

## 排除（同根因不得作为新发现）

开放 Issues：

- #645 linux 16 套件回归（症状已列在 00-baseline.md）。你可以深挖根因并在报告里
  写「归属 #645 / 测试过期 vs 产品缺陷」的证据，但不要把同一症状再编号成新 Finding。
- #646 windows Test 串行跑不完。
- #638 Windows 0xC0000409（tst_import / tst_folderimport / tst_sectionlifecycle 退出崩溃）。本机无 Windows，只记线索，不臆造复现。
- #639 tst_panels_dataops 5 失败。
- #585 导航树测试段错误。
- #556 吞错 / ErrorHub 低采用。
- #558 传递性分层债。
- #559 测试薄弱区。
- #561 低危清单。
- #587–#596 review-2026-10-19 中低危清单（catalog 开库先写后验、IO 整读、算法哨兵、UI 悬垂 this、i18n 闸门盲区、空棒 0.0、死代码、IO UB、UI 细节）。同根因标 EXCLUDED_EXISTING_ISSUE。

不报：纯风格、vendored 第三方内部、已知问题的换行号复述。
grep 命中不等于 Bug。每条必须有真实调用链。宁缺毋滥。不确定标 SUSPECTED。

## 审查重点

- 导入/井表/岩屑/分层：拒收与接收口径是否与调用方、文案、版本计数一致。
- 相分类补齐后：图例、compose 阈值个数（N−1）、单因素、导出模板是否仍按旧类别数假设。
- 工程引导 / 坐标 / native CRS / 离线底图：失败是否回滚、后台线程是否写活 catalog、CRS 是否在 worker 退出后仍有效。
- 证据链推迟（#644）：草稿/撤销/定位（#647）是否丢状态、双写、悬垂。
- 层违规、新 Qgs 直连文件、错误吞没、魔法常量、重复真源。
- 测试：永真断言、把产品回归改成放宽期望、mock 绕过。

## 输出格式

写入指定报告。每个 Finding：

- ID（前缀-序号）/ Severity(P0-P3) / Confidence(VERIFIED|HIGH|SUSPECTED) / Module
- File:line / 函数 / 根因 / 触发条件 / Observed vs Expected / 调用链
- 排除比对
- 修复策略 / 回归测试思路

末尾：覆盖清单（逐行 vs 接口级）+ 未核查区。
#645 归属分析单独一节，不占 Finding 编号。
