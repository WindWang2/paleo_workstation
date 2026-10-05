# Goal-Loop 方向 47：多 realization / 不确定性——预测面集合与置信度派生

## 背景（实测事实，勿再勘察）

当前预测管线只产单个「预测置信度」图层（文档 §7）。多 realization
是研究级差异化能力（TODOS P2，CEO review D6）：

- **schema 已预留可空 `realization_id`**（PALEO_QGIS_PLAN.md
  NOT-in-scope 决议注记）——做时先定 realization 与 version 的
  正交关系：realization 是同一版本语义下的等概率成员，还是独立
  DERIVED 版本序列？勘察 catalog 版本契约后定口径，注释钉死。
- 触及面：数据模型（schema/catalog）、存储（多成员同网格）、预测
  管线（单次→集合）、版本（parentVersionIds 锚定）、UI（同画布
  切换成员 + 派生置信度面）。
- 置信度派生：N 成员 → 均值面/离散度面（逐像素方差或分位数）——
  井点稀疏区的不确定性可视化是核心价值。
- **生成侧现成生产者**：`geostat/sgs.h` 已产种子化多 realization
  场（nRealizations/seed 参数化、跨平台可复现）——集合契约的
  首个填充源优先接它，预测管线扰动种子跑 N 次为第二路径；
  勿再写新采样核。
- **本方向扩量约定**：不止「存储+统计+查看」三段——再加
  集合级对比（两集合均值面差值/成员叠加动画帧序列）、
  不确定性图签（统计口径+成员数随图面走）、集合版本族在
  catalog UI 中的分组呈现（父集合→成员树）。XL 量预期。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/realization -b goal/realization-20261004 origin/master
cd .worktrees/realization
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **realization 契约**：勘察 schema/catalog 版本表定正交关系——
   推荐「realization 集合 = 同集合 id 的 N 个 DERIVED 版本 +
   `extra["realizationIndex"]`」或启用预留列；契约进注释 + 测试。
2. **集合存储/读取**：集合枚举、成员寻址、集合级 provenance
   （成员数/种子序列/父版本）。
3. **派生统计**：N 成员 → 均值面 + 离散度面（方差或 P10/P90）——
   统计栅格各落独立 DERIVED 版本，parentVersionIds 锚全部成员。
4. **UI**：同画布成员切换（下拉/滑块）+ 派生面并排视图——
   沿用预测页/画布控件先例；成员缺失/集合不完整如实标。
   **集合对比**：两集合均值面差值视图 + 成员叠加动画帧序列；
   **不确定性图签**：图面随带统计口径+成员数（规范图签先例）；
   **catalog 分组**：集合在数据页呈父集合→成员树，不单列 N 行。
5. **生成侧最小闭环**：既有预测管线以扰动种子跑 N 次填充集合
   （参数面板显式「成员数」）；不新造采样理论。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；集合/统计归
  catalog/algorithms，编排归 workflow，控件归视图层。
- **诚实面**：集合不全/成员损坏如实列；离散度面标注统计口径
  （方差 vs 分位数不混称）；realization 不是概率校准——文案不
  越称「真实概率」。
- **资源**：构建/测试一律 `-j8`。
- **UI**：对照 `DESIGN.md`；i18n 两门。
- **性能断言**：禁绝对毫秒墙钟；N 成员存储/统计用比率门，
  内存按成员惰性加载不全体驻留。
- **schema 纪律**：catalog schema_version 迁移走既有机制
  （SCHEMA_MIGRATION.md）——`realization_id` 启用要写迁移测试。
- **ledger**：`.goal-loop-ledger-realization.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/schema 契约/
  统计口径/provenance/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 契约钉死：集合/成员寻址 round-trip 一致；schema 迁移测试过
   （旧工程打开不炸、`realization_id` 缺省 null 兼容）。
2. 派生正确：合成 N 成员（已知均值/方差场）→ 派生面逐像素
   收敛真值（容差断言）。
3. 切换查看：成员切换响应正确显示对应栅格，集合不完整如实标
   （缺成员号列得出）；catalog 分组树展开/收起与集合一致。
4. 集合对比：两集合差值面数值与逐像素直算一致；动画帧序列
   成员序正确。
5. provenance：统计面 parentVersionIds 覆盖全部成员；成员各带
   种子/参数；图签口径与统计面实际算法逐字一致。
6. 空态/退化：N=1 集合如实标「单成员无不确定性」；零成员集合
   不 crash 不显示假面。
