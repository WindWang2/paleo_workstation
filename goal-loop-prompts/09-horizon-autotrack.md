# Goal Loop — goal/horizon-autotrack：层位自动追踪（新功能）

你接手一个自治迭代循环。方向：**层位自动追踪器**——在 2D/3D 剖面/切片上给定
种子点后自动沿同相轴追踪层位，产出可编辑的层位面。解释效率核心功能，算法
可断言性强（合成数据有解析答案）。

## 背景事实

- 地震读路径实测：966MB IL 切片 ~64ms、时间片 30-59ms、体窗 ~46ms——追踪器
  逐点取数性能预算足够。
- 层位基建：`horizonbinner`（散点→栅格）、`horizonmarkers`、层位经
  `layermanifest` 上图；`seismicsectiontool`/`seismic3d` 拾取挂点在。
- 属性核 `src/algorithms/seismicattr.*` 刚入库（Hilbert/相干等）——追踪器可用
  相位/相干特征，复用而非重写。
- 撤销栈：`editingundostack`；联动：`selectioncontext`。

## 范围（建议子项，按需取舍/排序）

1. **追踪核**（`src/algorithms/` 纯数据层）：
   - 种子点 + 局部窗内同相轴匹配（波形相关/振幅极值跟随 + 倾角引导任选组合）；
   - 终止条件参数化（相关阈值/最大跳跃/相干门槛/边界）；
   - 追踪结果 = 拾取点集 + 逐点置信度——置信度语义钉死（什么信号降权）。
2. **2D 剖面追踪**：种子→沿剖面双向推进；多种子合并策略（重叠区取高置信）。
3. **3D 扩散**：切片辅助下的面扩散追踪（IL→XL 邻域外推，限步长）——范围大，
   可先 2D 闭环后 3D 最小可用。
4. **工作流挂接**：追踪结果 → 层位散点集 → `horizonbinner` 上图；手动修正
   走编辑撤销栈（追踪产物的删除/重选可 undo）。
5. **QC**：逐点置信度可视化（色带叠加）、追踪失败区如实留空（诚实失败——
   不外推填充），覆盖率统计入面板。

## Oracle

1. 合成断言：构造已知倾角/连续同相轴的合成体 → 追踪结果与真值逐点差 ≤
   容差（写明因由）；断层模型上追踪必须在断层处停止而非穿越。
2. 2D 闭环：剖面种子 → 双向追踪 → 拾取点集成层位 → 上图，offscreen 可测。
3. 置信度产出且单调性语义断言（低相关区置信度下降）。
4. 撤销栈语义 + 取消长任务可中断。
5. 分层绿（`--strict`）；ctest 全绿；ledger；push + `gh pr create`。
6. 966MB 实测追踪速率（拾取点数/秒）入 docs/progress。

## 勘察指引

- `src/algorithms/seismicattr.*`：体窗访问与属性核形状；`horizonbinner`：散点
  落栅格；`seismicsectiontool`：种子拾取挂点。
- 追踪算法选型：波形互相关窗为主干最稳妥；倾角引导可用局部斜率扫描
  （semblance 核已在仓内）。选型理由入 ledger。
- 断层停止判据：相干体核已存在——直接用相干值做终止门槛可复用。

## 禁区

- 不动读路径/索引层；不重写属性核（复用）。
- 追踪产物必须经既有层位面管线入库（horizonbinner/layermanifest），
  不开平行层位格式。
- 不做全三维体自动追踪大跃进——2D 剖面 + 有限面扩散为本方向边界。

## 迭代协议

- 轮0-1：勘察体窗接口 + 合成体夹具做法 → 算法选型入 ledger。
- 中段：追踪核→2D 闭环→置信度/QC→3D 扩散（若有余量）→工作流挂接。
- 完成定义 = Oracle 6 条全绿，ledger 轮次齐全。

## 交付协议（必守）

1. **新建 worktree 分支开发**：`git worktree add .worktrees/horizon-autotrack -b goal/horizon-autotrack-<日期>`
   从最新 `origin/master` 起，全程不在主 checkout 写代码。
2. **完成后先仔细 review 再开 PR**——逐条执行，全部通过才准推送：
   - `git diff origin/master...HEAD` 自审全部改动：删掉调试残留、printf、注释掉的死代码；
     每个文件头三行层标记在；分层 `tools/check_layering.py --strict` 绿；
   - 全量构建无新警告；ctest 全绿（含新增测试）；
   - Oracle 每条在 ledger 里有对应验证证据（命令+输出摘要），不是「应该没问题」；
   - review 发现的问题先修再验，直到干净为止。
3. 推送 + `gh pr create`（标题 conventional，body 写 `## Summary`/`#### Test plan`/
   自审结论清单）。
