# 方向67：单因素域补完（协克里金 / 隔断变差 / SFPKG 写出 / 外委统计 / 策略 UI）

## 范围

- 分支 `goal/sf-completion-20261007`，worktree `.worktrees/sf-completion`，基线
  `origin/master` `33f496a`。
- 方案/账本：`.goal-loop-ledger-sf-completion.md`（口径定案与本机环境注记）。
- 交付（TODOS P2「单因素原生算法后续」收口）：
  1. `algorithms/geostat/cokriging.{h,cpp}`：普通协克里金核（MM1 交叉结构）
     + 带约束普通克里金（组权重上限软罚主动集）；
  2. `algorithms/geostat/variogram.{h,cpp}`：实验变差函数隔断感知档
     （测地滞后距 + 跨隔断对不累积，双轨开关，关轨与既有口径逐位一致）；
  3. `io/ziparchive.{h,cpp}` 写面 + ZIP64（读写两向）与 `io/sfpkgwriter.{h,cpp}`
     （manifest/surface.npz/checksum 镜像读面契约）；
  4. `io/outsourceworkbook.{h,cpp}` 曲线统计（mean/median/min/max + 深度区间）
     与因素候选发现（Pearson 相关对清单，候选不自动进图）；
  5. `domain/singlefactorstrategy` 词表接单因素页方法下拉（标签/参数预览单一
     真源）+ `strategy_id` 进血缘（分派入口校验，词表外拒绝不回退）。

## 口径（关键取舍）

### 协克里金（MM1 简化，注释钉死）

- 交叉半方差 `γ12(h) = ρ·γ1(h)`（ρ=配置点相关系数，|ρ|≤1）——Markov MM1
  近似，不是严格 LMC：γ1/γ2 结构失配 + |ρ|→1 时方程组可轻病态，显著负方差
  判失败（与 OK 同口径），不静默输出。
- 方程组：主/协双 Lagrange 行（Σλ=1、Σν=0），加边 LU 复用 `linsolve`；
  协变量邻域为空时省 μ2 行（全零行会奇异），方程组退化为 OK 同构。
- 退化一致性（测试钉死）：ρ=0 → 与 `ordinaryKrigingAt` 逐点 1e-8 一致；
  井点精确通过（γ(0)=0 口径，估值=井值、方差=0）；协变量加密重建误差单调降；
  ρ=0.72 协克里金 RMS < 0.9× 纯 OK RMS（固定系数正弦合成场，无 RNG）。

### 带约束 OK（软罚，注释钉死）

- 方向线/软边界 = 「样本组权重上限」Σ_{i∈组} λᵢ ≤ cap（跨线组 cap=0；
  界外组 cap）。不等式硬约束（KKT/等式行）破坏加边矩阵条件正定且需 QP——
  这里单边二次软罚 + 主动集迭代（**单调只增 ≤4 轮**，回退会振荡）；软罚把
  组权重拉向 cap 不保证严格 ≤ cap（如实报告权重与 penaltyPasses）。
- 方差公式沿用 σ² = Σλγᵢ₀ + μ（近似口径：惩罚拉偏权重后不再保证最小方差）。
- 约束输入下标经 `dedupeSamples` 新增的 `inputToDeduped` 出参映射过重合合并
  （默认参数，kriging/sgs 既有调用零改动）。

### 隔断感知变差（双轨）

- `VariogramBarriers{enabled, polygons, gridResolution}`；每样本一个
  `faultPathMetric` 距离场（8 邻接 Dijkstra，chamfer 偏差上界同 distancetransform
  先例 ~8%），对滞后距取测地值；不可达（NaN=跨隔断，不同连通域）不进累积并计
  `unreachablePairs`。方向过滤仍按欧氏位移与欧氏距。
- 预算闸：n ≤ 512（超限 InvalidInput）；格网沿最长边默认 256 格（[32,1024]），
  总单元 ≤ 4×10⁶。
- 关轨/空 polygons 与既有代码路径逐位一致（测试严格 ==，不走 qFuzzyCompare）。
- 血缘：`ExperimentalVariogram.barrierAware/unreachablePairs`（method 细分）。

### SFPKG 写出 + ZIP64

- `zipBuildArchive/zipWriteArchive`：stored 起步（deflate 可选，raw deflate，
  输出缓冲走 `deflateBound`）；条目 >4 GiB / >65535 / 偏移溢出自动 ZIP64
  （本地 extra 16 字节、中央 extra 24 字节、EOCD64+locator）；`forceZip64`
  供结构性 round-trip。重名条目写面直接拒绝（读面拒重名，写面不产重名）。
- 读面同步扩 ZIP64：EOCD 哨兵 → locator → EOCD64；条目哨兵 → extra 0x0001
  按序还原；条目数恰 65535 且无 locator 按普通包（哨兵歧义以 locator 裁决）。
  512 MiB 单包上限与 zip-bomb 预算闸不变。重名检查 O(n²) → 哈希集
  （7 万条目包从 ~200 s 到亚秒）。
- `writeSfPackage`：manifest 关键字段（factor_name/horizon/method）必填、
  `grid_shape` 与 grid_z 不符拒写、has_pickle 拒写、整型数组非整数/越界拒写；
  grid_x/grid_y/valid_mask 缺失进 issues 照写（镜像读面 issue 语义）。
  checksum.json 恒写（对写出精确字节 SHA256）。NPY v1.0 头 64 字节对齐。

### 外委统计与因素候选

- 深度列 = 表头含「深度」首列；曲线列 = ≥3 个有限数值的非深度列（空单元格
  不算坏数据，非数值记 badCells）。
- 候选阈值：|Pearson r| ≥ 0.7 且共同样本 ≥ 8（默认，参数可调）；排序 |r|
  降序、并列列名字典序（同输入同输出）。**候选不是结论**，不自动进图。
  常数列相关无定义 → degenerate 计数，不虚报 r=0。

### 策略包 UI + 血缘

- `factorMethodCombo` 由 `surfaceMethodPacks()` 驱动（标签=词表、参数预览=
  geologicalNote 联动 QLabel + item tooltip）；词表 "idw" ↔ 页面 "legacy"
  是唯一映射例外（旧约束 IDW 不写 method 参数）；SGS 是实现族不在曲面词表，
  作页面专属项且**不带 strategy_id**（不冒充）。
- `strategy_id` 血缘：`generateFactor` 分派入口先校验（词表外 id 拒绝，
  不回退不冒名）；四个 publish 站点（本地方向含 surfer 委托 / geostat /
  structural / legacy）落 `extra.strategy_id`。

## 测试

- `tst_geostat_cokriging`（10 槽）：解析解夹具（固定系数合成场）——井点精确
  通过、协克里金优于 OK、加密单调、ρ=0=OK、远协变量=OK、约束权重响应、
  dedupe 下标映射契约。
- `tst_geostat_variogram_barrier`（6 槽）：封死墙跨侧对计数（6/9）、关轨
  逐位对拍、空 polygons 回落、绕行测地滞后、fitVariogram 消费、n>512 拒收。
- `tst_io_sfpkg_write`（11 槽）：stored/deflate round-trip、强制 ZIP64 结构
  （偏移经 extra 还原逐字节断言）、7 万条目 ZIP64、重名拒绝、sfpkg 全字段
  round-trip（NaN/dtype/边车/checksum）、坏 checksum 拒收、写面诚实拒收档。
- `tst_io_outsource_stats`（7 槽）：统计与直算一致、稀疏列如实 issue、候选
  发现/确定性/阈值闸、深度列不进候选、真实夹具（2 行表）诚实降级。
- `tst_mappingpages` 增 1 槽（词表驱动下拉 + 预览 + strategy_id 载荷）；
  `tst_geostat_workflow` 增 2 槽（strategy_id 进血缘 / 未知 id 分派前拒绝）。

## 本机环境注记（Windows 宿主）

- vendored QGIS 链 conda Qt 6.11.2（`paleo-qgis-deps/Library/bin`），应用按
  Qt 6.8 构建——测试运行 PATH 需 deps 在前统一 Qt6 运行时（Qt6 主版本内向
  后兼容）；`Qt6Pdf*` 等仅 6.8 有的模块由 6.8 目录兜底。
- 该宿主上 paleo_workflow/paleo_ui 系测试在 ctest 下既有 0xc0000135（R0
  同样红，含 `tst_geostat_workflow`/`tst_mappingpages` 全类），且部分直跑
  用例的 QTemporaryDir 环境性失败（tst_release 等）——均为存量环境红，
  R0↔R1 同环境对拍消化；`tst_mappingpages::strategyPackDrivesMethodCombo`
  直跑验证通过，`tst_geostat_workflow` 两个新槽随 CI（Linux）执行。
