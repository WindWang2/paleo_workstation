# 方向67 ledger：单因素域补完（goal/sf-completion-20261007）

## 范围

TODOS P2「单因素原生算法后续」四件：协克里金核、隔断感知变差函数、
SFPKG 写出、外委统计/因素候选 + 策略包 UI 接线。

## 口径定案（无人值守决策，宁保守不过声称）

### 批次1 协克里金（src/algorithms/geostat/cokriging.{h,cpp}）

- **交叉结构**：Markov MM1 简化——γ12(h) = ρ·γ1(h)，ρ 为配置点相关系数
  （|ρ| ≤ 1）。这是近似不是严格 LMC：结构失配 + |ρ|→1 时方程组可能病态，
  显著负方差判失败（与 OK 同口径）。注释钉死在头文件。
- **退化一致性**：ρ=0 时交叉项消失，方程组严格退化为 OK（ν=0、μ2=0），
  测试以 1e-8 容差对拍 ordinaryKrigingAt（布局不同 LU 主元序可差末几位）。
- **n2=0（协变量全在邻域外）**：省去 μ2 行（全零行会让加边矩阵奇异），
  方程组与 OK 同构，测试近逐位对拍（1e-9）。
- **带约束 OK**：组权重上限不等式 Σ_{i∈组} λᵢ ≤ cap（方向线=跨线组
  cap=0；软边界=界外组 cap）。单边二次软罚 + 主动集迭代（**单调只增，≤4 轮**，
  不回退——回退会在「罚下满足/释放后违约」间振荡；未收敛用末轮解并如实记
  penaltyPasses）。软罚把组权重拉向 cap 不保证严格 ≤ cap（测试断言 <1e-3）。
  硬约束（KKT/等式行）会破坏加边矩阵条件正定且需 QP——注释钉死。
- **下标映射**：dedupeSamples 加可选 `inputToDeduped` 出参（默认参数，
  kriging/sgs 既有调用零改动）——约束的输入下标经重合合并映射，不重造簇逻辑。
- **未做**：全场扫描入口（ordinaryKriging 对应物）——本方向无消费方，
  YAGNI；krigingsurface 接协克里金不在本目标形态内。

### 批次2 隔断感知变差函数（variogram.{h,cpp}）

- **双轨**：`VariogramBarriers`（enabled + polygons + gridResolution）；
  默认关。关轨/空 polygons 走既有欧氏距代码路径，**逐位一致**（对拍测试用
  严格 == 而非 qFuzzyCompare）。
- **测地语义**：每样本一个 faultPathMetric 距离场（共享 faultpath 核，
  8 邻接 Dijkstra 含 ~8% chamfer 偏差——与 distancetransform 先例同口径，
  如实注释）；对的滞后距=场值；**NaN（不可达=跨隔断）不进累积**并计
  unreachablePairs。方向过滤仍按欧氏位移与欧氏距（方位角语义与测地无关）。
- **预算闸**：n ≤ 512（超限 InvalidInput 如实报因）；格网沿最长边默认 256
  格（[32,1024] 钳制），总单元 ≤ 4×10⁶。
- **血缘**：ExperimentalVariogram 加 barrierAware/unreachablePairs 字段
  （method 细分）；krigingsurface 消费该字段属后续方向（不在本批扩散面）。

### 批次3 SFPKG 写出（src/io/sfpkgwriter.{h,cpp} + ziparchive 写面）

- **ZIP 写面**：zipBuildArchive/zipWriteArchive（stored 起步，deflate 可选
  ——raw deflate windowBits=-MAX_WBITS）；条目 >4GiB / >65535 / 偏移溢出
  自动 ZIP64（本地 extra=16 字节 usize+csize，中央 extra=24 字节
  +offset，EOCD64+locator）；forceZip64 强制小包走 ZIP64 结构（round-trip
  测试用）。DOS 时间固定 1980-01-01（无假钟）。
- **读面同步扩**：EOCD 哨兵 → locator → EOCD64 恢复 64 位真值；条目哨兵
  → extra 0x0001 按序还原（usize,csize,offset 各 ≥8/16/24 字节时取）。
  条目数恰 65535 且无 locator 时按普通包读（哨兵歧义按 locator 存在性
  裁决）；偏移/大小真哨兵而 locator 缺失才报错。512MiB 读上限与
  zip-bomb 预算闸不变（>4GiB 条目解不出——注释如实）。
- **写面诚实分档**：致命（缺 grid_z / factor_name|horizon|method 缺失 /
  grid_shape 与 grid_z 不符 / has_pickle / dtype 不支持 / 整型数组非整数
  或越界 / 条目重名）→ 拒写不落盘；非致命（grid_x/grid_y/valid_mask
  缺失——读面会逐条 issue）→ issues 记录照写。format/version 缺省补写，
  grid_shape 缺省按 grid_z 补写。
- **f4 精度损失**：调用方选 dtype 的固有口径，不另发 issue（整型越界才拒）。
- **checksum.json 恒写**（对写出精确字节取 SHA256）；npz 内层恒 stored
  （numpy 习惯），外层压缩由参数选。
- **NPY v1.0 头**：64 字节对齐 + \n 结尾；一维 shape 写 (n,)。

### 批次4 外委统计（outsourceworkbook.{h,cpp}）

- **深度列口径**：表头含「深度」的首列（顶深/底深/深度都算），不进曲线。
- **曲线列**：≥ minCurveRows（默认 3）个有限数值的非深度列；空单元格
  不算坏数据（上游表格常见空洞），非数值记 badCells。
- **因素候选阈值**：|Pearson r| ≥ 0.7（常规经验「强相关」下沿）且共同
  有限样本 ≥ 8（默认）。排序 |r| 降序、并列按列名字典序——同输入同输出。
  **候选不是结论**，不自动进图（API 只产清单）。
- **常数列**：相关无定义 → pairsSkippedDegenerate 计数，不虚报 r=0。

### 批次5 策略包 UI（constraintpage + constraintfactorjobs_*）

- **下拉单一真源**：factorMethodCombo 由 surfaceMethodPacks() 驱动
  （标签=词表 label、参数预览=geologicalNote 联动 QLabel + item tooltip）。
  词表 id→页面 method 映射唯一例外：词表 "idw" ↔ 页面 "legacy"（不写
  method 参数走旧约束 IDW）；SGS 是实现族不在曲面词表，作页面专属项
  （无 strategy_id、无预览——不冒充）。
- **血缘**：params["strategy_id"]（词表 id 原样）；generateFactor 分派入口
  先校验——词表外 id 拒绝（**不回退**），不冒名落血缘。落 extra 的路径：
  本地方向（含 local_direction_idw/kriging + surfer 委托）、geostat
  （kriging）、structural、legacy（"idw"）四 publish 站点。
- **默认选择**：local_direction_idw（既有测试契约不变）。

## 进度

- [x] 环境：worktree + cmake 配置（MSVC/Ninja，Qt 6.8 + QGIS prefix）
- [x] 批次1 代码 + tst_geostat_cokriging（10 槽全绿）
- [x] 批次2 代码 + tst_geostat_variogram_barrier（8 槽全绿）
- [x] 批次3 代码 + tst_io_sfpkg_write（11 槽全绿；7 万条目重名检查 O(n²)→哈希集，
      ~200s→0.28s）
- [x] 批次4 代码 + tst_io_outsource_stats（7 槽全绿）
- [x] 批次5 代码 + tst_mappingpages 增 1 槽（直跑绿）+ tst_geostat_workflow 增 2 槽
- [x] R0 基线红集合（238 环境红，两次全量运行逐名一致 → 环境确定性）
- [x] R1 全量对拍：红集 diff = 0（329 项 = 325 + 4 新测试全绿）
- [x] 静态门：layering / layering --strict / i18n / ui_invariants / ui_tokens 全绿
- [x] 两轮自审（见复盘）；修复清单全部落实

## 本机环境注记（Windows 宿主，R0↔R1 对拍消化的存量红）

- **三套 Qt 运行时混战**：应用按 Qt 6.8（C:/deps/Qt）构建；vendored QGIS 链
  conda Qt 6.11.2（paleo-qgis-deps/Library/bin）；anaconda3 有 Qt 6.9.2。
  直跑 exe 解析到 6.11.2（deps 前置）；ctest 子进程解析到 6.9.2（机制未定论，
  两次实测一致）→ qgis 系测试在 ctest 下 0xc0000135（6.9.2 Qt6Core 缺 6.11
  符号），纯 Qt 系测试正常。
- **conda Qt 6.11.2 的 QTemporaryDir 在本机失败**（errorString 非空，TEMP/
  TMP/TMPDIR 全试过）：tst_release / tst_geostat_workflow / tst_mappingpages
  部分 slot 直跑因夹具临时目录失败——与 ctest 0xc0000135 无关的另一层环境红。
- 以上红项在 R0（改动前）与 R1（改动后）完全一致 → 红集 diff = 0。
  方向67 新增测试中受影响的只有 tst_geostat_workflow 的 2 个血缘槽（该类
  在本机 R0 即全红）；其同义 UI 载荷槽（tst_mappingpages::
  strategyPackDrivesMethodCombo）已直跑验证通过。CI（Linux）将完整执行。
- 测试运行 PATH 方子见 worktree 的 ctest.bat / ctestp.bat（未提交，env 专用）：
  deps(6.11.2) → prefix → Qt6.8 → qscintilla 前置，anaconda 殿后。

## 复盘（两轮自审记录）

**第一轮（写完即审）**：
- [High→已修] zipBuildArchive 中央目录 zip64 分支把偏移哨兵写错位置（应在外部
  属性之后）——重排后修复。
- [High→已修] 主动集迭代「回退式」会在罚下满足/释放后违约间振荡——改单调只增。
- [Medium→已修] 约束下标映射重造 dedupeSamples 簇逻辑（链式重合口径会漂移）——
  dedupeSamples 加 inputToDeduped 出参，映射在合并遍历内原生产出。
- [Medium→已修] deflateRaw 输出缓冲手算（不可压数据会溢出）——改 deflateBound。
- [Medium→已修] sfpkgwriter manifest.format 非字符串会被静默覆盖——改拒写。
- [Low→已修] outsourceworkbook columnValues 未用（-Wunused 风险）删除；
  cokriging.h 补 <cmath>。
- [Medium→已修] 隔断档方向过滤余弦分母误用测地距——改回欧氏距（hEuclid）。

**第二轮（全量 diff 五维走查 + 运行验证）**：
- 分层：check_layering --strict 绿；cokriging 纯数值无 Qt；sfpkgwriter 仅
  QtCore；constraintpage include domain 合法。
- 数值：全部解析解夹具绿（含 ρ=0=OK 对拍、关轨逐位对拍、round-trip 逐位一致）。
- 契约：sfpkg 写→读全字段 round-trip；ZIP64 双向；checksum 拒收。
- 诚实面：MM1/软罚/候选非结论/未收敛记 penaltyPasses 注释与文档一致。
- i18n：门禁绿；词表标签来自 domain 数据非 ui 字面量。
- 测试期修复（夹具标定，语义不变）：QTEST_MAIN 补齐、远协变量半径 12→20
  （12 恰裁一口井）、约束夹具界外井 8.0/8.6→6.2/6.8 与罚权 100→1000、
  mask[0] 断言随夹具口径、zip64 偏移 84→81（数据 18 字节非 21）。
- 零 High/Medium 遗留。Low：tst_geostat_workflow 2 槽本机 env-blocked（上述）。
