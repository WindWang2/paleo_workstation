# goal/gridding-surface-ops — 网格化算法与面运算（功能深化）

- 分支：`goal/gridding-surface-ops-20261002`（worktree `.worktrees/gridding-ops`）
- 基线：`e6e95af`（origin/master，PR #74 合并后）

## Oracle 对账

| # | Oracle | 状态 | 证据 |
|---|--------|------|------|
| 1 | 新算法 ≥1 落地：解析面采样→网格化 RMS 断言 + 留一法 CV 用例 | ✅ | `tst_gridsolver` 22 用例全绿：平面/二次面（最近节点量化语义，RMS<1%/1.5%·range）、T=0.5 张力有界偏差<5%、正弦面<5%、屏障两盘隔离、留一法 CV（平面<1.5%·range、正弦<25%、单折边界）；ctest `-R tst_gridsolver` 100% |
| 2 | 栅格代数 ≥5 用例（含 null 传播/零除）+ 等厚图端到端 | ✅ | `tst_rasteralgebra` 8 用例（加/减/乘/除/最值/常数/条件/原地别名；NaN 传播、零除→NaN）；`tst_surfacevolumes::isopachEndToEnd`（散点→双层面网格化→Subtract→体积积分 vs 解析 67.2e6，rel<1%） |
| 3 | 体积量算解析断言（楔形/锥形 ≥2 用例） | ✅ | `tst_surfacevolumes`：棱柱精确（1e-9 相对）、楔形精确（中点法则对线性面解析精确）、锥形 <3%（曲边界 O(h)）、基准面上方 140600（解析离散值精确 + 对真积分 0.018%）、符号体积/NaN 排除 |
| 4 | 网格规模上界护栏：超规模拒绝而非 OOM | ✅ | 核 `geometryForExtent`（1 亿像元预算，审计 #33 同口径）：`tst_gridsolver::guardRejectsOversizeGrid/…DegenerateInput/…AcceptsBoundaryBudget`（恰好 1e8 放行）；Processing 路径 `tst_algorithms::minCurvatureGuardAndProgress`（CELL_SIZE=1e-5 → run 失败 + 不落盘）；UI 对话框实时规模反馈 + 超预算禁 OK |
| 5 | 任务可取消、进度单调；分层 --strict；ctest 全绿；push + PR | ✅ | 取消：`tst_gridsolver::cancelStopsSolve`（协作取消→"canceled"）；进度单调：`progressMonotonic`（多级级联全局遍号单调）+ Processing `progressChanged` 序列非降；`tools/check_layering.py --strict` 检查通过；ctest 全量绿（本 PR 新增 4 测试 43 用例）；PR 见关联 |
| 6 | 真机 8 层位网格化实测耗时 | ✅ | `tst_gridding_realarea`（`PALEO_REAL_PROJECT_AREA` 门控）真机实测（Round 4 稳定版）：8 层位全过，单层 ~17.7-19.8s（263k 散点、640×821、8 级×500 遍），总 142.2s——表见 Round 3 |

## Round 0 — 勘察与选型（2026-10-02）

### 现状盘点

- 散点→栅格：`io/horizonbinner`（装箱，非插值——同像元多点只留计数）；
  算法层 `ConstraintIDWAlgorithm`（QGIS Processing，IDW p=2 + typed 约束线：
  break_line 硬屏障 BFS 连通域 / direction_line 各向异性）。
- 距离场基建：`paleo_welldist`（精确欧氏）+ `paleo_distance_transform`
  （绕障 Dijkstra，PR #64/`49f17d4`）。屏障栅格化（半格超采样）模式在
  `paleoalgorithms.cpp` 内联，未提炼——本方向作为消费方复用其模式，不改契约。
- 审计 #33（GitHub issue，已 CLOSED）：horizonbinner `kMaxCellCount=1e8`
  上限 + 旋转测网正交性拒绝已修（`0de3e45`）；算法侧同款守卫
  `PaleoAlgoGuards::gridDimsForExtent` 已在。**本方向新网格化路径自带同口径
  守卫**（核内 `geometryForExtent`），复核确认已闭环项无需返工。
- 栅格 nodata 双族：编图族 float `-9999`（GeoTIFF 落盘口径），地震族 NaN。
  **新核内部统一 NaN=缺失**（数学传播自然），GeoTIFF 边界换 `-9999`。
- 依赖面：**Eigen 不在依赖面**（全仓无 find_package/无 include/vendor 无）。

### 选型：最小曲率/连续曲率张力样条（弃克里金-lite）

Smith & Wessel (1990) 连续曲率样条（GMT `surface` 同族）：
`(1−T)·∇⁴z − T·γ·∇²z = 0`，γ=4/(dx·dy)，T∈[0,1)。T=0 → Briggs (1974)
最小曲率；T→1 → 膜方程（谐和插值，无过冲）。Gauss-Seidel + SOR 迭代。

理由：
1. 零新依赖（SOR 是纯循环；克里金要稠密 Cholesky + 变异函数拟合，禁区
   「不引重量级数值库」下要自证数值可信，面大收益小）；
2. 屏障语义天然：屏障格在松弛模板里以中心值镜像代入（等价 no-flux 内
   边界），断层两盘各自松弛、不跨界插值——与 ConstraintIDW 的 break_line
   语义同族；
3. 解析面可严格断言：平面/二次面满足 ∇⁴z=0（T=0 时为精确解），
   正弦面误差可用采样密度控制；
4. 张力参数直接对应地质编图「过冲抑制」的实务旋钮。

### 模块定型（数据层纯数值核，风格随 `seisattr`）

- `src/algorithms/gridsolver.{h,cpp}`：最小曲率核（散点+网格几何+屏障掩码
  → 栅格 + 质量标记：收敛/迭代数/最大残差）；`geometryForExtent` 守卫
  （1e8 像元预算）；`distanceToData` 距数据距离质量面；留一法 CV。
- `src/algorithms/rasteralgebra.{h,cpp}`：面-面/面-常数/条件选择核，
  NaN 传播 + 零除→NaN 语义钉死。
- `src/algorithms/surfacevolumes.{h,cpp}`：面间体积/基准面上方体积
  （像元中心矩形法则，与 GDAL pixel-is-area 同口径）。
- Processing 包装：`paleo_min_curvature`（注册进 PaleoProvider）。
- UI：层位右键「网格化…」→ 参数表 → PaleoTaskService 异步 → 受管派生
  资产 + 上图；面运算入口走图层树栅格右键。

### 真机数据

`/home/kevin/projects/paleo_project/data/project_area/层位/`：C3/C6/D53/
D61/D62/D63/D71/D72 共 8 文件（XYZ+inline/crossline 散点文本）。

## Round 1 — 网格化核（多级级联 + 解析面验证）

1. **多级粗→细级联**（多网格思想）：双调和 GS 低频误差刚性收敛（单级
   tol=1e-6 需 2.4 万遍）；2 倍逐级加密、上级解双线性上采样作下级初值，
   末级只修高频。合成用例几十遍收敛。
2. **数据约束**：GMT surface 默认语义——最近节点赋值 + 冻结 Dirichlet
   （节点=所属点均值），松弛只更新自由节点。亚格偏移按最近节点量化
   （半像元 × 局部梯度量级，与 GMT 默认同口径，解析面测试容差如实反映：
   平面/二次面 RMS < 1%/1.5% 极差）。
3. **边界**：越界邻居取线性外推幽灵格（保线性精确——平面恒等式在边界
   亦成立，诊断程序逐项验证 |gs−z|≈1e-14）。
4. **留一法 CV / 距数据距离质量面**：见核测试 21 用例。

## Round 4 — 数值稳定性战役（自审发现，三个连环 bug）

全量 ctest 中 `tst_algorithms` 挂死 → gdb 栈显示 PROJ WKT 导出中 abort +
QtTest 看门狗 fork 死锁。逐层剥开：

1. **屏障掩码多级粗化越界写**（堆损坏根源）：中间级掩码错误地从**最细级**
   一步 `/2` 折算——中间级缓冲更小，级数 ≥3 时越界写堆（640×821 真机网格
   8 级、42×42 测试 4 级全中招；40×16 旧用例 3 级侥幸不炸）。修复：掩码
   逐级自上一级粗化（`levelMasks[li]` 由 `levelMasks[li+1]` 2×2 足迹收缩）。
   教训：越界写的 detonation 点（GDAL/PROJ malloc）离案发现场极远，
   gdb 栈只给引爆点不给着火点。
2. **真机「未收敛」与测试振荡的同源问题**：数据点位残差反传的约束步
   （Briggs 式迭代）与松弛算子复合不收缩——真机 8 层位 `converged=0`
   （delta 卡平台）、屏障用例极限环后指数发散（ω=1.0/1.5、T=0~0.35 均
   散，与参数无关）。修复：删约束步，回冻结 Dirichlet（GMT 默认语义，
   无竞争投影恒稳）。
3. **屏障模板本身的正反馈**：冻结 Dirichlet 后屏障用例仍散。最小复现
   （6×6 单级 + 内部墙列）+ Python 实验室逐项对照定位：屏障邻居「取
   中心值」的镜像项 + 跨墙 ±2 项形成正自反馈，显式（放分子）与隐式
   （折对角）两种处理 GS 均散、Jacobi 也缓散——迭代矩阵谱半径 >1 与
   松弛参数无关。**修复：屏障项丢弃 + 保留项重归一**——`gs = Σcₙvₙ/Σcₙ`
   （同号负系数 → 正权重凸组合，构造性有界）；无屏障内点 Σcₙ=−a0 精确
   退化为原模板，既有精度口径不变。Python 原型验证收敛后移植 C++。
   （期间还修掉一个复刻程序自身的 bug：`round()` 银行家舍入 ≠ `lround`，
   一度误导定位。）
4. 顺带：CV `maxPoints=1` 除零边界；测试 `z[0]` 行号笔误（北向上 y=5 在
   末行）；沙箱目录被裸跑测试删除后 `file(MAKE_DIRECTORY)` 只在 configure
   期执行导致 GDAL 路径全断的假阳性全家桶——重新 configure 修复。

## Round 2 — 栅格代数 + 体积量算

- `rasteralgebra`：NaN 诚实传播、**零除→NaN**（不是 ±inf——inf 污染下游
  统计与渲染，null 更可诊断）、比较 NaN 恒假、where 按选中源传播。
- `surfacevolumes`：像元中心矩形法则（pixel-is-area 同口径），带符号体积
  （底高于顶→负，如实报告），`aboveDatum` 只累计正部分（挖方语义）。
  楔形/棱柱解析精确；锥形曲边界 O(h) 容差 3%；等厚端到端（两面网格化→
  Subtract→积分 vs 解析 67.2e6）rel<1%。
- Processing 包装 `paleo:paleo_min_curvature`：INPUT/FIELD/CONSTRAINTS
  （仅 break_line 生效，作输入消费不改引擎契约）/TENSION/MAX_SWEEPS/
  CELL_SIZE/OUTPUT；QC 元数据（PALEO_TENSION/SWEEPS/CONVERGED/
  FINAL_DELTA/DIST_TO_DATA_MAX/MEAN 等）。

## Round 3 — UI 链 + 真机实测

- 链路：层位资产右键「网格化…」（datalist `gridHorizonRequested` 意图
  信号）→ 参数表（`ui/dialogs/griddingdialog`：网格尺寸/张力/迭代上限/
  屏障源/LOO CV 折数 + 实时规模反馈，超 1e8 像元禁 OK）→
  `SurfaceGriddingWorkflow::gridHorizonText`（worker 线程：核求解 +
  `DerivedAssetRegistrar` 受管 GeoTIFF + DERIVED 版本登记）→
  `rasterReady` 排队回 GUI → manifest 声明 + 实例化上图（00_Data 组）。
- 面运算：图层树栅格右键「面运算（等厚/体积）…」（`surfaceOpsRequested`）
  → 顶/底选择 → `isopachBetweenRasters`（同网格校验 + Subtract + 体积
  报告 + 可选受管等厚栅格）→ 报告对话框（metrics + CSV 导出，体积单位
  如实标注 z 单位·m²，换算 m³ 需速度场）。
- io：`parseHorizonScatter`（x y z il xl 行文法，跳行计数 + 测网范围）。

### 真机 8 层位实测（PALEO_REAL_PROJECT_AREA，T=0.25，8 级×500 遍；Round 4 稳定版）

| 层位 | 散点 | 网格 | 耗时 ms | 遍数 | 收敛 | finalDelta | 过冲下/上 (ms) |
|------|------|------|---------|------|------|------------|----------------|
| C3 | 263451 | 640×821 | 19804 | 4000 | 0 | 142.3 | 64.1 / 60.2 |
| C6 | 262915 | 640×821 | 19625 | 4000 | 0 | 123.6 | 57.2 / 54.6 |
| D53 | 263451 | 640×821 | 19824 | 4000 | 0 | 64.2 | 29.0 / 37.3 |
| D61 | 263446 | 640×821 | 19702 | 4000 | 0 | 69.1 | 32.2 / 33.7 |
| D62 | 263451 | 640×821 | 19777 | 4000 | 0 | 72.6 | 32.6 / 34.2 |
| D63 | 263434 | 640×821 | 19363 | 4000 | 0 | 71.4 | 34.1 / 34.4 |
| D71 | 263443 | 640×821 | 19670 | 4000 | 0 | 71.2 | 34.9 / 34.3 |
| D72 | 263447 | 640×821 | 17675 | 4000 | 0 | 96.7 | 41.4 / 43.3 |
| **合计** | | | **142165** | | | | |

解读（如实）：全部层位在 8 级×500 遍预算内未达 1e-4 收敛阈（263k 点冻结
Dirichlet 的低频残差收敛慢、但**单调不发散**——Round 4 稳定化的直接验证），
`converged=0` 如实上报（任务 detail 同时提示）；过冲集中在数据域边缘的
样条外推带，是张力样条在无约束边缘的已知行为——增大张力或提高迭代上限
可压。单层 ~18-20s 在 UI 异步任务口径下可接受。

## 验证证据（命令 + 输出摘要）

```
$ cmake --build build -j16                      # 全量构建，新代码零警告
$ python3 tools/check_layering.py --strict      # 检查通过：无层违规。
$ ctest -R 'tst_gridsolver|tst_rasteralgebra|tst_surfacevolumes|tst_algorithms'
  # 100% tests passed out of 4（tst_gridsolver 22/22、tst_rasteralgebra 8/8、
  # tst_surfacevolumes 9/9、tst_algorithms 14/14）
$ ctest -j1                                     # 100% tests passed out of 146
$ PALEO_REAL_PROJECT_AREA=... ctest -R tst_gridding_realarea
  # 1/1 Passed（8 层位 BASELINE 见 Round 3 表）
```

注：`-j8` 并行全量偶发 `tst_startup_trace`（启动计时门）/`tst_panels`
（offscreen 对话框计时）负载闪红——两测试在本 worktree 与主 checkout
独立复跑均 100% 过（性能门测试的已知负载敏感特性，与本 diff 无关）；
全绿结论以 `-j1` 串行为准。

## 遗留移交

- 真机层位收敛预算：8 级×500 遍未到 1e-4 阈（如实报 converged=0）；迭代
  预算/阈值旋钮已在参数表面（MAX_SWEEPS），全量收敛优化（如级联级数
  自适应）留后续。
- 等厚链的父版本 provenance：`gridHorizonText` 只见文本不见源资产，父版本
  留空（sourceUri 带 surfacegridding/<层位>）；壳侧带 assetId 的重载可补。
- CV 全量扫描（fullScans=true）对 263k 点真机层位代价 O(n·solve)，UI 默认
  快档（折数 16、迭代减半）；自动折数选择器留后续。
