# petrophysics-logs — 测井岩石物理计算面（goal/petrophysics-logs-20261002）

分支 `goal/petrophysics-logs-20261002`（自 master e6e95af 起）。迭代账本
`.goal-loop-ledger-petrophysics-logs.md`（轮次/Oracle 证据）；本文是交付记录。

## 交付一览（按提交）

| 提交 | 内容 |
|------|------|
| feat(algorithms) | 公式核库 `src/algorithms/petrophys.{h,cpp}`：Vsh 四变体（线性/Larionov 年轻·老/Clavier，共用 IGR 钳 [0,1]）+ 孔隙度三法（密度/中子 %→v/v/声波 Wyllie 含 Cp）+ Archie（a/m/n/Rw 显式，φ 曲线或密度内联）+ 深度线性重采样（不外推/缺失不跨接）+ QC（curveStats 样本标准差/anomalyIntervals NaN 断段）。逐条公式注释出处（Larionov 1969/Clavier 1971/Wyllie 1956/Archie 1942，AK04 转引）。数值单测 16 例（解析常数 python 预算，容差写因由） |
| feat(algorithms) | 曲线计算器 `src/algorithms/curveexpr.{h,cpp}`：递归下降自研（vendor/系统零表达式库）——四则/^ 右结合（-2^2=−4）/比较/逻辑/where·min·max·clamp·ln 等函数/科学计数；NaN 三值逻辑（比较遇 null → NaN 非 false）；除零 IEEE 如实。编译一次逐井复用。测试 10 例（null 传播 4 路 + 报错路径 9 断言含 offset） |
| feat(io) | LAS 写出器 `src/io/laswriter.{h,cpp}`：本仓首个 LAS 出向路径（~V/~W/~C/~A 完整 2.0，QSaveFile 原子落盘，NaN→NULL token 与读侧 NaN 语义闭环）；解析器契约零改动 |
| feat(services) | 批处理 `PetroPhysTaskService`（petrophyscomputeservice.{h,cpp}）：井集→LasCache 载入→逐井公式/表达式→QC→产物 LAS 经 `PaleoProjectStore::enqueueWrite` 单写者队列落盘→任务终态后服务线程 catalog DERIVED 登记（asset `petrophys_<公式>_<井id>`，parent=源 RAW 版本，extra 记参数/基线实取值/统计/QC）。井间协作取消（部分成果如实交付并登记）、井粒度单调进度、`resolveWellLas`（well 实体→well_log 主链接→currentVersion）。测试 8 例：解析断言批/产物读回闭环/catalog 四表断言/写队列信号证据/进度单调/40 井中途取消/表达式双井含未知曲线失败路径/QC 数值/Archie 内联/解析缺井 |
| feat(ui) | 参数面板 `src/ui/correlation/petrophyspanel.{h,cpp}`（表单→意图信号，文献预填值 tooltip 注出处；Rw 必填不臆造）+ `WellCorrelationPanel::mergeComputedCurves`（结果曲线进曲线集，上轨由勾选驱动）+ 壳层编排（paleomainwindow_attach：井集解析→批任务→DERIVED 登记→曲线并回）。offscreen 面板测试 6 例 |
| test(perf) | `tst_petrophysperf`：20 井 × 15,580 行（A1.Las 同形状）批跑实测 + A1 真文件冒烟（脱敏桩全空列 → 诚实全 NaN 传播断言） |

## 语义决策（合并评审重点）

1. **公式参数显式原则**：核函数零内置默认；未设（NaN）→ `validateRequest`
   拒绝。UI 预填 = 文献值可见形态（ρma 2.65 石英/ρf 1.0 淡水/Δtma 182·
   Δtf 620 µs/m/a=1·m=2·n=2，AK04 ch.3/6，tooltip 逐控件注明）；Rw 无
   文献通用值 → spinbox 0 显示「必填」，提交 NaN。Cp=1 显式语义「不校正」。
2. **IGR 先钳 [0,1] 再入非线性映射**：Larionov/Clavier 只对 [0,1] 内 IGR
   有定义（AK04 惯例），外推无物理意义；四变体端点 0→0、1→≈1。
3. **深度对齐（轮1 钉死）**：单 LAS 文档行对齐（~A 行共享，零成本零失真）；
   跨源线性重采样到参考 DEPT，出界 NaN 不外推、端点缺失不跨接、源深度
   非严格递增报错不静默。批处理只走行对齐路径；重采样核供混源合并用。
4. **NaN 三值逻辑**：计算器比较/逻辑遇 NaN → NaN（条件掩膜不得把「无数据」
   当「不满足」，区别于 IEEE 比较的 false）；where 条件 NaN→NaN、选中支
   语义；除零 ±∞/NaN 如实输出由 QC 标记。QC 异常区间 NaN 断段（无证据
   不连通）。
5. **产物写路径**：LAS 产物经 `enqueueWrite` 单写者队列（禁区红线）；catalog
   登记推迟到任务终态服务线程（catalog 非线程安全，worker 不碰）。取消时
   已完成井产物保留并登记（部分成果如实交付），batch 标 cancelled。
6. **登记形态**：外链产物（managed=false + sha256，seismic 派生同例）；
   asset `petrophys_<公式短名>_<井id>`（幂等复用）；type well_log/format
   las——既有井资产面（曲线浏览/连井重导入路径）天然可见；link role
   well_log 非主（不抢源 LAS 主链接）；version parent 回指源 RAW。
7. **Archie φ 来源双路**：显式曲线名（canonical 等价匹配）或 ρma/ρf 密度
   内联（φD 先算再入 Archie，两式出处同 AK04）；两者皆无 → 校验拒绝。
8. **A1.Las 真数据**：脱敏桩（数据段全部列恒 −99999）——管线对全空列的
   诚实行为是全 NaN 传播 + stats.valid=0（冒烟测试钉住此语义）。

## 20 井实测（tst_petrophysperf，本机 Arch/16 核，RelWithDebInfo）

合成井 = A1.Las 同形状（DEPT/GR/DT/RHOB/NPHI 五道，15,580 行 ≈
252.57–2200.07m @0.125m，每 97 行埋 NULL），Vsh 线性 + 井内极值基线 +
产物落盘：

| 指标 | 实测 |
|------|------|
| 20 井批墙钟（含产物写出） | 356–426ms |
| Σ解析（LasCache 冷） | 181–226ms（~9–11ms/井） |
| Σ计算（公式核 20×15,580 点） | **3.1–3.7ms** |
| 吞吐 | ~875 行/ms |
| A1 真文件（2,000 行桩）解析 | 2.8–4.9ms |

比率门（机器无关护栏）：Σcompute < Σparse——公式核毫秒级，解析主导；
真回归（引擎退化/核内多余拷贝）给 O(rows) 级反转。

## 遗留（TODOS 方向）

- 跨源深度对齐（重采样核已备）未接批处理——多趟合并场景实测驱动再上。
- 计算结果曲线进 wellcomposite 柱状图（现走 correlation 曲线集；复合图
  有独立 CurveData 通道）。
- Archie 的 φ>1 域外 NaN 与气洗带 Rt 异常的 QC 规则库（现 QC 门是通用
  [lo,hi] 区间）。
