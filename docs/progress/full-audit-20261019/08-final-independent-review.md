# 独立终审（full-audit-20261019）

> 审查者：全新 subagent（未参与审查/修复/R2 任何环节）；从 HEAD（24a8bf4a）出发
> 全量 diff 复核 + 对抗挑战。审查者全文原本在 /tmp/paleo-audit-r3/final-review.md
>（2026-10-19 会话期间 /tmp 目录被系统清理，原文佚失；本文件按审查者完整结论
> 摘要落账，处置已全部执行并在 04/07 号文档回填）。

## 结论：**PASS-WITH-NOTES**

代码面干净：22 个修复目标全部在位且经从零独立推演成立，无回归；分层/闸门本地
实跑全绿（layering --strict、qgs-audit 116==116、qgis-spread 149==149、
ui_tokens/file_budget/ui_size 0 新增 0 回升）；无孤儿文件、无卫生问题、vendored
零触碰。瑕疵集中在审计闭环的**声明诚实性**。

## 分级：MUST_FIX×1 / SHOULD_FIX×2 / NOTE×6

### FR-1（MUST_FIX）D-01「收口」声明不实 —— 已处置（540f10ca）

- 升级版 factorRowSwitch 测试仍无红/绿区分力：B handler 链尾
  refreshWellFactorFields→restore→refresh→applySuggested 在有保存版本时自愈，
  Fix-1 前后终态逐项相同。
- Fix-1 的 A-restore 在换勾时刻惰性（口径组合框仍是上一因素的对齐值，
  extractionParams 匹配必失配）；愈合全部来自预存链。
- 台账「残留场景由既有 invalidate 链覆盖」与控制流相反：B 清表后失败路径
  无重算者。
- **处置**：04/07 号台账如实更正（PARTIAL/残留 OPEN）；测试保留为终态钉子
  但降级声明；PR Known Limitations 在案。

### FR-2（SHOULD_FIX）无版本换勾路径等值距建议残留 —— 登记 OPEN

constraintpage.cpp:233 于清表前读旧值域 + wellfactors 失败路径无重算者；
风险窗=换勾未提取因素后直接生成。重构需以全套件为护栏（07 号试错记录），
移交后续批次。

### FR-3（SHOULD_FIX）transitive 基线与 #578 对撞 —— 已处置（rebase）

#578 削扇出根后 master heavy_tus 87→50 < 本分支 re-pin 88；rebase 取 50、
本轮 re-pin 作废留痕（tools/transitive-baseline.txt 注）。rebase 后实测
50==50 绿。

### NOTE×6（择要，均已清理或注明）

mapexport 重复注释块（已删）、known-reds 保留条目滞留引用（已更正）、
tst_wellattributes 无用 QDebug include（已删）、04 号登记册状态列未回填
（已全量回填）、HEAD 与台账 SHA 差（docs commit，无碍）、known-reds 重排版
diff 噪声（接受）。

## 对抗挑战（§9.1）全部未推翻修复

| 挑战 | 结果 |
|---|---|
| B-01 极端输入：gridCols=1 | parseHorizonHeader 的 2×2 下限在除零前拒绝 |
| B-01 spacing=1e-9 | 浮点单调退化不翻 bin |
| dedup 多候选/非 OUTPUT | 首中即断、四重过滤拒之 |
| C-03 多文件命名唯一性 | 序号名与字节无关仍唯一 |
| 快速连续勾选（重入） | blockSignals 域内 emit 的是页面自身信号，无重入 |
| 空数据（空/BOM geojson） | 空.legend 无崩溃 |
| B-01 数学独立重推 | 与修复者一致 |
| MA-F01 回退正确性 | vendored qgsmapsettings.h:243「index 0 on top」证实 |
| 性能 | 每次导出至多 1 次整文件哈希，无新 O(N²) |

## 终局回归（rebase 至 816f9b40 后）

见 99-pr-body.md Testing 节与 /tmp 重跑日志（final-tests2）：基线 17 红 →
终局 3 红（全部基线既有/已登记：layering_transitive #533 挂起、tst_ui
legendTitle 基线红、tst_assetpaging 基线 SEGFAULT）。
