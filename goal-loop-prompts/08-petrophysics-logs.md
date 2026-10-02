# Goal Loop — goal/petrophysics-logs：测井岩石物理计算与曲线处理（新功能）

你接手一个自治迭代循环。方向：**测井解释计算面**——标准岩石物理公式
（Vsh/孔隙度/含水饱和度）、曲线计算器、多井批处理、结果曲线回写与 QC。
新功能，数据层可断言性强。

## 背景事实

- LAS 链路厚：`src/io/lasparser.*` + `lascache.*`（缓存/再开 ~10ms 量级实测）、
  `tests/tst_las.cpp`（刚入库 469 行）是测试风格参照；真机 20 井 LAS 可实测。
- wellcomposite 连井展示、`curvebrowser`/`correlationtrack` 曲线浏览基建在。
- catalog/`DataImportService` 产物登记先例；写队列 `paleoprojectstore`
  单写者语义——回写曲线须走队列。
- 算法层禁 QtWidgets；计算器表达式解析自写或查 vendor 现有物（无 eval 库
  先例时自写递归下降，别引新依赖）。

## 范围（建议子项，按需取舍/排序）

1. **标准公式库**（`src/algorithms/` 纯数据层）：
   - Vsh：GR 线性/Larionov/Clavier 指数任选其二以上，参数明确；
   - 孔隙度：密度/中子/声波三选二的常规换算；
   - Sw：Archie（a/m/n/rw 参数化）；
   - 每条公式代码注释给出名字与出处；输入输出 = 曲线数组 + 参数 + 深度对齐语义。
2. **曲线计算器**：表达式引擎（`DEN - 0.5*NEU` 类逐点运算 + 条件掩膜），
   缺失值/null 传播语义钉死。
3. **批处理编排**：`SeismicTaskService` 式异步任务——选井集 → 参数 → 结果曲线
   写回（LAS 侧产物文件 + catalog 登记），进度/取消语义复用。
4. **QC 面**：计算前后曲线统计摘要（样本数/null 率/min-max/均值）+ 
   异常区间标记（如 DEN<1.5 或 >3.0 计数），留痕可查。
5. **UI 挂点**：井面板/连井上的「计算」入口 → 参数表单 → 结果曲线进曲线集，
   视图只发信号。

## Oracle

1. ≥3 类公式可用：参数→逐点结果，合成井曲线解析断言（已知 GR 分布→已知 Vsh，
   容差写因由）。
2. 计算器：表达式解析 + null 传播断言 ≥6 用例（含非法表达式报错路径）。
3. 批处理：多井任务可取消、进度单调、结果曲线经 catalog 可查，写路径走队列。
4. QC 摘要数值断言 + 异常区间计数断言。
5. 分层绿（`--strict`）；ctest 全绿；ledger；push + `gh pr create`。
6. 真机 20 井批跑实测耗时入 docs/progress。

## 勘察指引

- `src/io/lasparser.*`/`lascache.*` + `tst_las.cpp`：曲线数据形状与测试夹具。
- `src/catalog/`+`DataImportService`：产物登记；`paleoprojectstore`：写队列。
- `curvebrowser`/`correlationtrack`：结果曲线如何进既有展示面。
- 深度对齐：各曲线深度采样不齐——先钉对齐策略（按参考曲线重采样/共深度点表），
  语义入 ledger。

## 禁区

- 不改 LAS 解析器契约形状（可只读消费）；不旁路写队列直写 LAS 文件。
- 公式参数无默认臆造——不确定的地质常数走显式参数或 ledger 记录所取值+出处。
- 不做机器学习岩性预测（AI 方向另属）。

## 迭代协议

- 轮0-1：勘察曲线/存储/任务接口 → 公式清单与对齐策略定型入 ledger。
- 中段：公式库→计算器→批处理→QC→UI 挂点，每件一轮。
- 完成定义 = Oracle 6 条全绿，ledger 轮次齐全。

## 交付协议（必守）

1. **新建 worktree 分支开发**：`git worktree add .worktrees/petrophysics -b goal/petrophysics-logs-<日期>`
   从最新 `origin/master` 起，全程不在主 checkout 写代码。
2. **完成后先仔细 review 再开 PR**——逐条执行，全部通过才准推送：
   - `git diff origin/master...HEAD` 自审全部改动：删掉调试残留、printf、注释掉的死代码；
     每个文件头三行层标记在；分层 `tools/check_layering.py --strict` 绿；
   - 全量构建无新警告；ctest 全绿（含新增测试）；
   - Oracle 每条在 ledger 里有对应验证证据（命令+输出摘要），不是「应该没问题」；
   - review 发现的问题先修再验，直到干净为止。
3. 推送 + `gh pr create`（标题 conventional，body 写 `## Summary`/`#### Test plan`/
   自审结论清单）。
