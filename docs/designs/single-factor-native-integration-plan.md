# 单因素图算法原生集成开发方案

> 交付用途：直接交给执行 agent 开发、验收和提交 PR。  
> 日期：2026-10-02；版本：1.0；状态：方案已定，开发与验收尚未执行。  
> 目标工程：`WindWang2/paleo_workstation`，Qt6 Widgets + vendored QGIS C++。  
> 参考工程：`WWX9/haiyou-visualization`。  
> 技术决策：**正式运行链采用 C++；Python 仅作为冻结的参考实现、对拍和实验工具。**

## 1. 目标与完成定义

将参考工程的局部方向插值、解释性软边界、井群局部权重和受约束等值线能力，接入现有“单因素图 → 综合编图 → 验证”工作流。

用户应能在现有单因素页选择井点数值字段、成图范围和约束，异步生成具有明确数值语义、可追溯且可复算的连续栅格；再从指定数值来源生成等值线。结果进入既有 catalog、图层树、样式和编图消费链。

完整交付包含两个阶段：

- **P0：可信分析场。** 原生局部方向 IDW、硬屏障兼容、软解释边界、可选井群权重、数据抽取、参数持久化、任务管理、DERIVED 回写、真实数值等值线和编图接入。
- **P1：显式制图派生。** 局部解释绕行工作场、等值线来源标识、工作场独立登记及禁止误用于定量分析的消费守卫。

允许按 P0、P1 分别开 PR，但不得把只完成 P0 报告成全文完成。每阶段都必须有独立可用的端到端路径、测试和进度文档。

以下事项属于 P2，写入 `TODOS.md`，不计入本次完成要求：克里金体系深化、完整 SFPKG 导入、外委 XML/XLSX 批量读取、对方全部历史制图策略、时深域转换、监督分类、打印排版和完整 Python GUI 嵌入。

## 2. 冻结基线与事实边界

### 2.1 源码快照

| 对象 | 本次审查基线 | 执行规则 |
|---|---|---|
| 参考工程 | `27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f`，2026-10-02 | 对拍固定此 SHA，禁止自动跟随 main |
| 目标工程 | `eaaf46def5ce20ed064e73c80853e67667b9407e`，本方案成文时 master | 执行时从最新 `origin/master` 建 worktree，记录实际基线与接缝变化 |
| 设计系统 | 目标工程 `DESIGN.md` | 开始 UI 修改前重新读取 |
| 架构 | `docs/PALEO_QGIS_PLAN.md`、`docs/UI_LAYER_PLAN.md` | 实现必须满足现有层契约 |

参考仓库可能需要 GitHub 登录访问。优先使用已有 `gh` 凭据，鉴权失败如实报告，不把浏览器 404 推断为仓库不存在。

本方案已进行源码、调用路径、数据契约和本机 vendored 头文件审查；**尚未执行参考工程全量测试、跨语言对拍或性能基准**。其文档中的历史测试数量和耗时只能作为定位线索，不能记成本任务通过证据。

### 2.2 已确认的实现差异

1. 参考工程单因素目录及其子模块约 1.8 万行 Python，其中 `constrained_engine.py` 8372 行、`workflow.py` 4056 行，包含历史兼容与界面数据组装，不能按目录整体翻译。
2. 当前 `structural_idw` 采用连续局部曲线核；默认产品路径已经加入解释性软边界。`build_structural_surface()` 并不按旧文档所述把硬隔断两侧自动分成独立数值场。
3. 当前工作流保存 `contour_partition.version=17`、`geometry_policy=local_interpretive_detour`。部分绕行线来自独立工作场，局部与分析栅格不满足严格逐点等值关系。
4. 目标工程已有 IDW、硬屏障格网连通域、全局各向异性、最小曲率、GDAL 等值线、任务池和 DERIVED 登记。新增局部方向核是增量，不能替换所有既有引擎。
5. 参考工程顶层 `drawing/__init__.py` 会导入 PyQt6、画布及 UI；其工作流依赖 `DrawLayer`、`DrawFeature`。正常 import 整包不是无界面算法接口。
6. 本机 vendored GEOS 头文件为 3.15.0，具有 union、polygonize、约束 Delaunay C API。执行时仍需核对目标平台闭包，不假设所有平台具有本机版本。

### 2.3 源码依据

所有上游链接固定到上述 SHA：

| 文件 | 阅读重点 |
|---|---|
| [structural_idw.py][up-structural] | 连续局部 IDW、查询分块、数值场入口、整域与软边界行为 |
| [continuous_metric.py][up-metric] | 紧支撑曲线核、切向张量、有限断层路径度量 |
| [interpretive_boundary.py][up-soft] | 有符号侧向权重与跨线降权 |
| [well_clusters.py][up-clusters] | 井位置连通分组、凸包支持、非零背景权重 |
| [workflow.py][up-workflow] | 当前产品实际方法分派、版本17参数、字段/层位/成果组装 |
| [field_contours.py][up-contours] | 保存栅格采样、网格三角化、等值线及策略分派 |
| [contour_work_field.py][up-workfield] | 工作场值选择、平滑过渡、仅穿线处局部绕行 |
| [methods/gridded.py][up-gridded] | 克里金/IDW 分支与退化条件，禁止只看 UI 名称 |
| [当前策略说明][up-policy] | 多代语义变化，顶部版本17优先于旧段落 |
| [test_structural_idw.py][up-tests] | 局部性、旋转、当前屏障行为和保存回读断言 |

注意：`experiments/core` 是独立论文实验引擎，不能冒充 `Drawing/` 当前产品算法。不得用其结果替代本方案指定的参考入口。

## 3. 技术路线与范围控制

### 3.1 C++ 产品链

```text
现有单因素页的输入/参数意图
    → 不可变输入快照、CRS与单位校验
    → C++局部方向IDW + 明确类型的约束
    → 分析栅格 + QC + provenance
    → DERIVED登记 + 现有factor图层声明
    → 真实数值等值线 / 显式制图派生
    → 既有编图、验证与图层树
```

NumPy/SciPy 本身包含原生计算，不能在未测量前宣称 C++ 必然获得某个加速倍数。本路线的主要收益是统一任务、部署、数据、几何与产品语义。

### 3.2 Python 的限定角色

- 固定参考源码、入口、依赖锁和适配脚本，离线生成数值对照夹具。
- 对拍工具只输出数据，不创建参考工程窗口或图层。
- 产品构建、常规 ctest 和正式运行不依赖 Python 科学计算环境；提交小型合成 golden fixtures，使 C++ 测试可独立运行。
- 对拍环境使用独立 venv，禁止修改系统 Python 或项目其它工具的 venv。
- 不把当前宽松 `requirements.txt` 当依赖锁；R0 固化实际可运行版本、来源与包 hash，保存环境清单。
- 若未来另有“先完全复现 Python 成图”的明确需求，采用独立进程和文件协议；它是另一条交付路线，本计划不以 Python bridge 代替 C++ 完成条件。

### 3.3 保留与不引入

- 保留现有 IDW、等厚、距井距离、最小曲率和旧工程语义；新增方法使用新 id。
- 不引入对方 PyQt6 界面、OpenGL 画布、符号库、项目模型或打包器。
- 不重做 catalog、mapbook/mapbookqueue、综合编图内部或数据管理导入全链。
- 不引入通用 ML 库；优先使用现有 vendored QGIS/GDAL/GEOS。
- P0/P1 不移植克里金。上游部分各向异性路径在井数超过 80 等条件下会执行 IDW，不能把该路径的 UI 标签当作真实求解方法。

## 4. 必须冻结的地质与制图语义

### 4.1 约束类型

| 内部语义 | 对分析值的作用 | 对等值线的作用 | 兼容策略 |
|---|---|---|---|
| `HardBarrier` | 禁止使用不可达区井点；自由端是否可绕达按明确模型决定 | 从分析场提线，屏障处允许终止 | 旧 `break_line` 保持硬屏障，不自动变软 |
| `DirectionGuide` | 有限范围内改变距离权重，范围外恢复基准 | 随分析场变化 | 对应方向线；旧算法仍使用原全局语义 |
| `InterpretiveBoundary` | 有限范围内平滑降低跨线井点权重 | 随分析场变化，不保证等值线不穿线 | 新类型，默认强度仅对新软边界生效 |
| `ContourStop` | 不改变分析场 | 对真实等值线裁切/停线 | 独立显示语义，不伪装断层 |
| `CartographicDetour` | 不改变已保存分析场；生成另一工作场 | 从工作场提取解释性绕行线 | P1显式选择；不能当作分析约束类型存回原线 |

所有类型可以同时存在，但每个要素的角色必须明确。硬屏障优先决定可用样本集合，方向和软边界只在该集合内改变正权重。硬屏障与软边界重合时不允许软权重重新引入被排除的井。

旧工程缺少新参数时走 legacy 方法，不凭名称、颜色、线型或 `weight` 猜测新语义。原约束存储中其它合法业务类型可不参与本引擎，但须列入“未参与约束及原因”；未知的新版语义值必须报错。

### 4.2 分析场与制图工作场

- `analysis`：井点与显式约束计算所得，是连续属性分析、取样、统计、相重分类和融合的数值来源。
- `cartographic_work`：为特定等值级别与绕行策略生成的解释性派生场，父版本必须指向分析场；不是新观测数据。
- `analysis_contour`：从分析场提取并满足其数值误差门的线。
- `cartographic_contour`：从制图工作场提取的线；其标签值对应工作场。

两个场分别登记、分别标识。制图工作场与制图线不得出现在连续因素融合、相分类、厚度统计的可选输入中；数据/功能层也必须验证，不能只在 UI 隐藏。不得沿用上游单个字典内多个 `grid_z` 副本而让调用者自行猜测来源。

改变等值级别可以改变工作场，但不得改变分析资产的字节、SHA、版本和当前关联。已人工修编的相图不得随重算被覆盖。

### 4.3 外推、空值与井控

- 内部缺失用 NaN；文件编码使用显式 nodata，0 始终可为有效数值。
- 新任务默认 `coverageMode=well_supported`，只在可用井的明确支持域中计算。
- 用户选择 `domain_extrapolation` 时允许在成图域内外推，并输出外推标记和覆盖统计；不得用全局均值补齐无井硬隔断区。
- 硬隔断形成无井闭合区：保留 nodata，返回区 id、面积和原因；`requireFullCoverage=true` 时整个任务失败。
- 相同 XY 的重复观测：参考兼容模式保留输入行，精确命中返回同坐标观测均值；不得先去重再改变其它位置的权重。提供重复/冲突计数。
- 厚度、砂地比、孔隙度等各有单位和合理范围；禁止统一截到 `[0,1]`。百分数转比例必须显式配置并记录。
- 有限值过滤要报告原始数、有效数、缺失数、越界数、重复数；不能静默丢样本后仍报原数量。

## 5. 接入点和分层设计

### 5.1 已存在的接缝

| 位置 | 用法 |
|---|---|
| `src/services/singlefactordef.*` | 保持因素身份与方法选择分离；厚度等因素不等于某一插值算法 |
| `src/workflow/workflows.*` / `ConstraintWorkflow` | 复用因素生成入口、声明和现有消费链；新增任务编排可拆文件 |
| `src/algorithms/paleoalgorithms.*` | 现有 `paleo:paleo_constraint_idw` 保留；注册新增局部方法 |
| `src/io/constraintstore.*` | 加法式保存新约束参数，保留旧 API 与旧工程读取 |
| `src/qgis/factorcontour.*` | 真实数值等值线基础路径，底层 GDAL C API |
| `src/workflow/derivedassets.*` | 受管路径、父版本、哈希和 DERIVED 登记 |
| `src/services/paleotaskservice.*` | 后台任务、进度、取消、关停 |
| `src/metadata/paleoprojectstore.*` | 项目单写队列、只读与工程生命周期 |
| `src/ui/pages/constraintpage.*` | 现有参数区、生成与等值线意图；不新增独立应用窗口 |
| `src/app` | UI、工作流、地图和数据服务的装配 |

QGIS C++ 嵌入环境不能假定 Python Processing provider 可用。不要用 `gdal:contour`、Python QGIS 插值 provider 字符串替代已经验证的 C API/原生引擎。

### 5.2 建议文件布局

以下是拟新增接口，**不是声称仓库已存在的 API**。允许 R0 小范围调整命名，但必须先写 ledger，保持层职责。

```text
src/domain/singlefactorrequest.h              # UI/功能层可消费的值型请求与结果摘要
src/algorithms/singlefactor/types.h           # STL数值DTO、网格、控制回调
src/algorithms/singlefactor/curvekernel.*      # 连续曲线核、张量与权重
src/algorithms/singlefactor/localidw.*         # 分块查询、软边界、井群权重
src/algorithms/singlefactor/support.*          # 支持域/硬屏障标签与数值支持规则
src/services/singlefactorsamples.*            # 输入校验、单位、样本和参数快照
src/services/singlefactorgeometry.*           # GEOS/GDAL几何预处理，不持有UI对象
src/services/singlefactorartifacts.*          # 栅格/工作场/线成果编码与校验
src/qgis/localidwalgorithm.*                  # 原生Processing包装、反馈桥接
src/workflow/singlefactorgeneration.*         # 异步生成、发布、世代守卫
src/ui/pages/constraintpage.*                 # 复用现有页面
tests/tst_singlefactor_*.cpp
tools/reference/singlefactor/                # Python参考适配与环境锁
tests/fixtures/singlefactor/                 # 小型合成输入、golden与来源清单
```

- 数值核使用 STL 与中性 DTO，无 QtWidgets、QGIS layer、画布或文件发布逻辑。
- GEOS C API 使用每任务上下文和 RAII；所有 create/destroy、错误回调路径有配对测试。
- GIS 对象提取在其所属线程完成；worker 消费不可变坐标、字段和参数，禁止跨线程持有 `QgsVectorLayer`、FeatureIterator、renderer、canvas。
- UI 只显示 DTO 和发信号；投影、采样、插值、统计、文件写入均不在视图层。
- 每个 `src/` 文件头三行带层标记；新增顶层模块需先经 `scripts/new_module.sh` 登记。上述布局使用现有顶层模块。
- 新文件进对应分层库，测试用 `add_paleo_test(... LIBS ...)`，避免扩大伞式 relink 范围。

## 6. 输入、网格与参数契约

### 6.1 输入快照

一次任务至少冻结：

```text
requestId / projectGeneration / horizonId / factorId
inputVersionIds + contentHashes
井点：stableRowId, wellId, x, y, value, valueUnit
成图域：Polygon/MultiPolygon + holes + CRS
插值域：可选，和成图域求交
约束：stableId, semantic, geometry, enabled, parameters
网格：cols, rows, 六参数transform, CRS, gridConvention
显式参数 + 所有自动参数的解析后数值
algorithmId / algorithmVersion / schemaVersion
```

井曲线统计优先复用工程已有层段和 petrophys 数据服务。P0 必须支持现有井点图层任意数值字段；如果加井段均值/中位数统计，深度基准、层段端点、缺失与采样间隔权重必须单独冻结，禁止把字段均值悄悄当厚度。

### 6.2 坐标与栅格

1. 所有距离运算使用同一明确的平面坐标系和线性单位。经纬度输入先转换到明确选定的工程投影；不自动选一个未经记录的投影。
2. 无 CRS 的输入只允许显式 local engineering grid 模式；已知/未知 CRS 混用拒绝。用户确认的局部坐标假设写入 provenance。
3. 产品栅格采用 north-up、pixel-is-area：原点是左上像元边，采样位置是像元中心，行向南；支持域按中心点与多边形/孔洞判定。
4. 上游常用 `linspace` 节点数组、Y升序；不可直接当成本项目像元边界。对拍须使用同一查询坐标数组，或显式做 Y翻转与半像元原点转换。
5. 首版输出仅 north-up；输入旋转栅格若参与取数必须经完整仿射/CRS，不能只使用 `gt[1]/gt[5]`。
6. 几何容差以网格间距和坐标尺度确定并记录，不能用一个不带单位的全局 epsilon 处理所有工区。
7. 超大网格在分配前以 checked multiplication 与工作集估算拒绝；不得仅根据一个像元数上限判断安全。

### 6.3 默认参数和合法范围

| 参数 | 新任务默认 | 契约 |
|---|---|---|
| `method` | `local_direction_idw` | 旧工程缺省仍为legacy；不改变等厚/距离引擎 |
| `power` | 2 | 有限且 >0；产品范围建议0.5–8 |
| `directionRatio` | 8，仅新方向线 | `[1,100]`；1应精确退化为无方向效应 |
| `influenceRadius` | auto | 复刻冻结源函数；保存实际解析值，单位为工程距离 |
| `coreRadius` | 0.3×解析后的影响半径 | `0≤core<influence`；核内上限规则与参考一致 |
| `softBoundaryStrength` | 0.35，仅新软边界 | `[0,0.8]`；0退化为不启用 |
| `softBoundaryRadius` | auto | 产品中与显示缓冲分离；兼容预设先显式解析参考值 |
| `wellClusterLocality` | false | 显式可选；参考预设可开启，记录统计与参数 |
| `coverageMode` | `well_supported` | `domain_extrapolation`须显式选择并生成标记 |
| `searchRadius` | auto | 保存解析值；无可用距离尺度时报错 |
| `minPoints/maxPoints` | 3/12，支持域模式 | 参考模式保留上游平滑截断语义，不能改成随意硬KNN |
| `duplicatePolicy` | `preserve_rows_exact_mean` | 不改变同坐标之外的重复观测权重 |
| `contourMode` | `analysis` | P1可显式选择`cartographic_detour` |
| `barrierTipPolicy` | `finite` | 首版不自动无限延长原始硬屏障 |

“Haiyou v17 对照预设”只用于明确复现：把其整域覆盖、井群权重、软边界和制图参数全部展开为显式值；不通过隐藏开关覆盖用户逐项设置。

## 7. 数值算法实现要求

### 7.1 连续局部方向核

以 `continuous_metric.py::CurveKernel` 和 `structural_idw.py::LocalInterpolator` 为参考，不用旧 `direction_corridor.py` 中另一套最近线段距离替代当前算法。

- 沿线段积分/离散求积形成有限支撑核；保留源实现的采样、归一化、平滑门函数和双向切向张量语义。
- 比较的是 query–well 成对距离。多方向线重叠按参考归一化混合，不能顺序覆盖或无限累乘。
- 线反向不改变结果；旋转/平移输入后结果满足相应不变性。
- 影响范围外与基准 IDW 一致；ratio=1、空方向线和关闭方向均应退化。
- 使用 double 计算，文件输出 Float32/Float64 必须明确。对溢出、无有效权重、退化线段和非有限参数返回结构化状态。
- 分块查询；禁止分配整个 `像元数×井数` 或 `像元数×方向离散点数` 的矩阵。

### 7.2 软边界与井群权重

- 软边界按局部有符号侧向关系降权；多条软边界重叠采用参考中的最大抑制量，不累乘成硬屏障。
- 保持正权重；关闭数值后处理时，有限估值应在实际参与样本的最小/最大值内。
- 井群按坐标邻接形成，表示采样分布而非地质相。复刻上游支持权重时保留非零背景下限，不将凸包用作 nodata 掩码。
- 距离统计必须读取源实现，而不是凭 `estimate_mean_well_spacing` 等函数名称猜测均值/中位数口径。
- 将显示缓冲、软边界半径、方向影响半径、制图工作场过渡宽度拆为独立参数。

### 7.3 硬屏障兼容

P0 复用现有栅格化屏障与连通域模型，记录栅格化规则及分辨率。有限线端部存在通路时同连通区可以绕达；闭合/贯穿成图域的屏障形成独立区。

它不等同于上游可见性图的最短路径加权。新结果 metadata 明确写 `hardBarrierModel=grid_connectivity_v1`；不要宣称已复刻 `FaultPathMetric` 的绕行距离。

屏障格可为 nodata，但不能把可视缓冲宽度扩成数值缺失带。落在屏障格/分区边界的井不得静默分配给最近一侧：列出歧义并要求明确归属或调整几何；邻接区共用井须显式指定。

有限断层路径距离可以后续独立增强。它不是本次硬屏障隔离验收的前置，不能借此推迟 P0。

### 7.4 自动参数与可复算

自动半径等依赖井集、范围和网格；每次运行保存输入值与解析值。换网格、井集或成图域后重算新版本，不能在相同参数 hash 下产生不同解析参数。

算法版本改变必须使结果可区分。对于相同版本、输入和解析参数：栅格数值、mask 与统计确定；文件容器时间戳不要求字节确定，但必须记录实际文件 SHA。

## 8. C++ 服务接口草案

以下签名用于冻结职责，R0 将字段落实为可编译头文件。视图使用 `domain` 请求 DTO，不 include `algorithms`。

```cpp
// algorithms/singlefactor：纯数值面
struct Control {
  std::function<bool()> cancelled;
  std::function<void(double)> progress; // [0,1]，单调
};

enum class Status { Ok, Cancelled, InvalidInput, BudgetExceeded, NumericalFailure };

SurfaceResult evaluateLocalIdw(const PreparedInput& input,
                              const GridSpec& grid,
                              const ResolvedParameters& parameters,
                              const Control& control);

QueryResult evaluateAt(const PreparedInput& input,
                       std::span<const Point2> queryPoints,
                       const ResolvedParameters& parameters,
                       const Control& control);

// services：DTO转换、输入验证、文件编码；无UI
SampleSnapshotResult prepareSingleFactorSamples(const SourceSnapshot& sources,
                                                const RequestParameters& request);
ArtifactWriteResult writeSingleFactorArtifacts(const SurfaceResult& result,
                                              const ArtifactPaths& paths,
                                              const Provenance& provenance);

// workflow：提交任务、单写队列发布；返回的任务归既有TaskService所有
PaleoTask* startSingleFactorGeneration(const SingleFactorRequest& request);
PaleoTask* startFactorContourGeneration(const FactorContourRequest& request);
```

`PreparedInput` 不含活跃数据库连接、QtWidgets/QGIS GUI 对象；输入仅供只读。`SurfaceResult` 至少包含 status、数值、有效域、支持/外推标记、计数、解析参数与错误详情。Cancelled/Failed 结果不能作为可发布成果。

Processing id 建议 `paleo:paleo_local_direction_idw`。方法白名单与因素适配在服务层注册；UI 不接受任意字符串直接调用任意 Processing 算法。

## 9. 参数保存、成果与血缘

### 9.1 约束参数持久化

当前 `ConstraintStore::append` 只有基础类型、相代码和 weight 等有限字段，不能承载逐线方向/软边界参数。采用加法式扩展：

- 增加带版本的参数存储（例如 `params_json` 字段及 `schema_version`），保留旧 append 重载。
- 每条要素保存 stable id、semantic、启用状态、参数和单位；不得借用 faciesCode 或 weight 存软边界强度。
- 更新 schema 和要素都经过项目写队列；迁移可重复执行、可恢复，失败不留下半迁移。
- 只读工程不执行建字段/DDL，不产生新数据库文件。
- 若约束仅是当前未登记图层，先创建受管输入快照并登记来源，不伪造 parentVersionId。
- 参考格式里的“缺省/未知就是硬隔断”只用于明确识别的 legacy 格式适配，新格式未知值拒绝。

### 9.2 产品类型

| 产品 | 格式 | catalog用途/消费边界 |
|---|---|---|
| 分析场 | GeoTIFF，Float32或显式Float64 | `single_factor_raster`，现有连续因素消费 |
| 质量/支持标记 | 伴生Byte GeoTIFF | nodata、井控支持、外推；枚举值需有图例和schema |
| 真实等值线 | GPKG LineString | `value_source=analysis`，包含 level、父版本 |
| 制图工作场 | 独立GeoTIFF | `single_factor_cartographic_work`，定量消费禁止 |
| 制图等值线 | 独立GPKG | `value_source=cartographic_work`，工作场/分析场双血缘 |
| QC报告 | JSON，必要时附CSV | 井点残差、覆盖、约束、耗时和复算信息 |

分析场继续使用 `factor.<horizon>.<factorId>` 的现有声明/当前版本机制。制图产品使用独立 layer id，不能覆盖 factor 分析层的源路径。

### 9.3 必填 provenance

```json
{
  "schemaVersion": 1,
  "algorithmId": "paleo:paleo_local_direction_idw",
  "algorithmVersion": "1.0.0",
  "semanticProfile": "paleo_local_idw_v1",
  "referenceRevision": "27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f",
  "valueSource": "analysis",
  "factorId": "sandthick",
  "valueUnit": "m",
  "parentVersionIds": [],
  "inputContentHashes": {},
  "parameters": {},
  "resolvedParameters": {},
  "grid": {},
  "constraints": [],
  "parameterHash": "computed-sha256",
  "qc": {}
}
```

示例空字段必须由运行时填实，不能照抄发布。parameterHash 使用规范化编码：对象键排序、稳定数值格式、UTF-8、拒绝 NaN/Inf、明确数组顺序。Hash 包含算法/语义版本、网格/CRS、单位、输入版本/hash、约束几何及解析参数；不含运行耗时、临时路径和创建时间。

制图成果额外包含 analysis/work 版本、`levels`、绕行策略版本、核心工作值、过渡宽度、实际改动像元数。`levels` 变化必须改变其参数 hash。

## 10. 异步、进度、取消和发布一致性

1. 主线程校验项目可写并冻结输入快照；所有长数值/几何步骤进入 `PaleoTaskService`。
2. 分阶段进度建议为 prepare 0–10%、geometry 10–25%、interpolate 25–80%、encode 80–95%、publish 95–100%。总进度必须单调，任务服务上报节流不超过20Hz；无法量化的阶段用不定进度，不编造百分比。
3. 查询分块内检查取消；GEOS复杂操作按独立区域/批次组织，在调用间检查取消，限制单次不可中断几何操作规模。
4. 带 projectGeneration/requestId；换工程、重新运行或销毁面板后，旧结果不能回填新上下文。
5. worker 不写共享 catalog/manifest。文件在任务私有 staging 中生成并验证后，由所属线程/项目单写队列统一发布。
6. 发布前复验工程代次、只读状态、输入身份和取消状态。不得把队列中尚未执行的任务误报成成功空成果。
7. 发布是明确的短临界区：进入前可取消；临界区内完成一致提交，不在多个关联产品中间响应取消。状态应如实显示“正在保存”。
8. 复用项目已有事务/版本提交协调能力；先审查能否覆盖文件、catalog和manifest。若不能，添加本特性局部的提交记录与恢复逻辑，不假装多文件原子事务已经存在。
9. 故障注入覆盖文件写失败、GDAL flush失败、catalog提交失败、manifest声明失败、进程终止恢复。旧当前版本继续可用；孤立staging可识别并回收，不展示部分成功产品。
10. 关停遵守 TaskService 排空纪律，不让 worker 在 catalog、GEOS上下文或store销毁后继续访问。

## 11. 等值线实现边界

### 11.1 P0：真实数值等值线

优先复用 `FactorContourService` / GDAL C API，增加显式级别、来源与必要的任务反馈适配。提线只读指定分析版本，不能为“更好看”重估井点、填 nodata、改分析值或连接不同级别的线。

若平滑导致数值误差超门，局部回退原线并记 QC；不得悄悄放宽误差门。边界、孔洞、nodata和硬屏障处允许开放线；不得把所有开放端点判为坏线或强制闭合。

### 11.2 P1：局部解释绕行

按固定版本的 `local_detour_surface` 行为建立独立策略：

1. 先从分析场提取指定级别的原线。
2. 只选择实际与原线相交且启用的制图约束。
3. 从分析场副本创建工作场，记录核心工作值和过渡规则；不修改原资产。
4. 在相同网格、有效域和单位上重新提线。
5. 校验工作场数值误差、几何相交、域/孔洞、穿线数量及保存重提一致性。
6. 线图层名称/属性明确“解释性等值线”；状态说明其值来自工作场。仅此信息影响用户判断，不在界面暴露内部模块名。

不能保证所有任意几何输入都绕行成功。未解决穿线或拓扑冲突必须列为 unresolved，严格模式拒绝发布；宽松预览也必须显示数量，不能强封线或制造假成功。

上游曾有多代闭环、补接、近线形变和允许穿行策略。本次只实现已选的原始数值提线和版本17局部工作场策略；不要把旧分支全部搬入新引擎。

## 12. UI 与既有编图接入

- 扩展现有单因素页的参数区：方法、数值字段、网格、覆盖方式、约束清单；高级区提供 power、方向/软边界参数和井群权重。
- 因素定义与求解方法分开。地层等厚、距井距离等专属算法不显示不适用的插值控件。
- 逐线参数编辑必须可保存和重开。禁止仅通过一次性 QVariantMap 临时传递而工程重开后丢失。
- 提供“真实数值等值线”和“解释性绕行”等明确选项；后者显示来源说明及独立图层。
- 运行中显示阶段、可取消、输入快照对应的层位/因素；关键输入变化后当前显示成果标记为旧输入结果，不偷偷重新解释它。
- 数据/功能层计算覆盖、井点残差和约束统计；UI只展示。
- 遵循 `DESIGN.md`：Qt原生控件、PaleoTheme、Noto Sans SC、数值mono字体、4/8/16px等间距、`tr()`、双主题和禁用原因。
- 在常用窄dock和高DPI条件下检查控件可达、滚动、完整标签；不改变外层dock布局策略。
- 分析结果出现在既有单因素/融合入口；工作场只在制图结果组显示。所有消费入口验证 `valueSource`，不能只以资产文件扩展名判断用途。

## 13. Python 参考工具与对拍夹具

### 13.1 参考适配器

`tools/reference/singlefactor/` 至少包括：

```text
README.md                 # 获取冻结源码、建环境、生成夹具的完整命令
requirements.lock         # 精确版本/来源/hash；执行时实测生成
reference_manifest.json   # 源SHA、源文件hash、适配器版本和环境
generate_reference.py     # 仅数据输入/输出，无GUI
compare_results.py        # 数值、mask、QC比较，不覆写golden
```

允许将经过审查的数值文件提取到最小包，移除上层UI import。提取器必须保存文件来源和差异；不得用大范围 `sys.modules` 假对象把真实依赖错误伪装成成功，也不得修改公式来迎合 C++ 结果。

最小运行依赖按实际导入分析确定；NumPy/SciPy/Shapely按版本锁定。针对 SciPy graph/KD-tree 等依赖做明确替代时，替代与源结果另有对拍，不能无声降级。

### 13.2 对拍域

- 在完全相同的坐标、单位、井行顺序、重复策略、网格和解析参数下比较。
- 对连续局部核、软边界、井群权重做 Python/C++ 数值对拍。
- 产品新增的硬屏障组合按目标工程语义独立验收，不能拿上游默认软边界结果强行对齐。
- 新任务的保守默认值不等于上游预设。对拍使用展开的显式参数，分别记录“参考兼容”和“产品默认”两组。
- 顶点序号、GEOS部件顺序和GPKG字节顺序可不同；几何比较用规范化与距离/拓扑指标，不只比较文件字节。

### 13.3 误差口径

令 `S=max(1,max(abs(finite_reference_values)))`，所有比较先要求 finite/nodata mask一致：

- double点查询与分析网格：`maxAbsError ≤ 1e-9×S`，`RMSE ≤ 1e-10×S`。
- Float32落盘回读：与期望Float32编码对比，误差不超过4 ULP或 `2e-6×S`；不得借输出容差掩盖核误差。
- 几何边界恰好落点的夹具单列规则，不把其mask差异排除后声称全域一致。
- 出现平台差异时先给出具体样本、误差分布和原因；变更容差须评审并记账，不能运行失败后自动放宽。

## 14. Oracle 验收矩阵

以下各项均必须记录命令、输入/环境、断言结果和输出摘要。所有“通过”由本次实际运行产生。

| ID | 验收内容 | 关键断言 | 建议测试 |
|---|---|---|---|
| O1 | 无约束数值核 | 常量场恒定、解析IDW点值、井点精确命中、重复XY均值、范围有界、空/单点/坏参数如实处理 | `tst_singlefactor_kernel` |
| O2 | 局部方向核 | Python对拍；ratio=1退化；范围外无影响；线反向/平移/旋转不变；折线转角无跳变；多线重叠有界 | `tst_singlefactor_kernel` |
| O3 | 软边界/井群 | 强度0退化、强度上限、重叠不累乘成硬墙、范围外无影响、正权重/精确命中；井群不是mask | `tst_singlefactor_constraints` |
| O4 | 硬屏障 | 改右区井值左区不变；有限端绕达与闭合屏障；无井区nodata；边界井歧义；显示缓冲不扩数值缺失 | `tst_singlefactor_constraints` |
| O5 | 数据与网格 | CRS/单位、Y翻转/半像元、孔洞/多部件、NaN与0、重复数、非法几何、极端坐标、预算拒绝 | `tst_singlefactor_samples` |
| O6 | 持久化兼容 | 旧工程方法/硬屏障语义不变；逐线参数重开一致；只读无DDL；迁移幂等、失败恢复 | `tst_singlefactor_store` |
| O7 | 任务与发布 | 协作/排队取消、世代守卫、关停、单调进度、I/O/登记失败无部分成果、旧版本仍可用 | `tst_singlefactor_workflow` |
| O8 | DERIVED与编图 | 文件像元/有效域、hash/父版本、重开、声明、既有融合读取analysis；定量入口拒绝work场 | `tst_singlefactor_workflow` |
| O9 | 真数值等值线 | 常量场无虚假环；斜平面/圆锥；正确level；孔洞/nodata不跨；允许自然开放端点；数值误差有门 | `tst_singlefactor_contours` |
| O10 | P1制图工作场 | 原analysis SHA不变；levels变更改变work hash；无穿线时不改场；只改声明区域；线来源/双血缘正确 | `tst_singlefactor_cartography` |
| O11 | UI | 真实点击选源/切方法/逐线参数/运行/取消/重开/上图；两主题/窄dock/高DPI；禁用原因；无同步阻塞 | `tst_singlefactor_panel` |
| O12 | 真工区 | 真实井值计数与独立统计一致；两种可用因素；四组约束对照；成果重开和编图消费 | `tst_singlefactor_realarea` |
| O13 | 性能 | 固定负载、缩放比率、峰值内存、取消延迟与GUI响应 | `tst_singlefactor_perf` |

### 14.1 等值线误差与拓扑

- 数值等值线对其声明来源场密集采样，采样间距不大于 `cellSize/4`，局部极值/转角加密。
- 对非恒定且等距的常规场：值残差目标 `≤ max(1e-5×S,0.02×interval)`。平滑前后分别测；任意级别列表用最小正级差。
- 鞍点、plateau、边界重合线用解析夹具单列，避免通用“禁止相交”检查误拒合法退化情形。
- 非退化合成场中：同级线自交0、不同级别线相交0、穿孔洞/nodata次数0；参考几何对拍的Hausdorff目标 `≤0.25×cellSize`，不要求部件顺序一致。
- 制图线只对工作场应用上述数值门，另报告相对分析场的误差分布，不宣称二者一致。
- P1指定合成绕行夹具必须达到穿约束0、无新增内部断口；任意真实输入的剩余问题计数必须可见并参与严格发布门。

### 14.2 真工区纪律

使用 `PALEO_REAL_PROJECT_AREA`，数据只读，生成结果落临时工程或明确的新输出工程。源数据不存在/缺字段是未完成验收，不能以合成数据冒充。

R0 从实际 catalog/井点字段选择两种具有明确单位的因素，并记录资产/版本/字段/有效井数；若只能取得一种，完成其余独立工作后报告缺项。可使用工程已有厚度计算链派生第二因素，但必须保存父版本和公式，不能造井值。

每种因素运行：无约束、仅方向、仅软边界、组合四组；另用真实井值配合明确标注为测试构造的贯穿硬屏障检查两区隔离。真实观测与构造约束的来源分开记账。

记录井点查询残差、栅格回采残差、有效/外推/缺失像元、参数、总耗时、峰值内存和成果截图。井点函数精确命中不代表像元回采也为0，两类残差不得混报。

LOO交叉验证至少在可承受井数的真实样本上比较普通IDW和局部方向方法。结果可以改善也可以变差，必须如实报告；“符合解释方向”不等于“预测精度提高”。

## 15. 性能与资源门

以下为**拟定验收目标，尚非已测结果**。R0冻结负载生成器、seed、CPU/内存、编译配置、线程数和计时边界；实现不得通过减少有效像元、改参数、隐藏回退或切换更易负载满足门槛。

| 场景 | 固定负载 | 初始目标 |
|---|---|---|
| S | 100井，512²像元，4方向线/合计64线段，2软边界 | 分析计算中位数≤5s |
| L | 100井，1024²像元，8方向线/合计128线段，4软边界 | 分析计算中位数≤15s |
| 数量缩放 | 同井/约束，512²→1024² | 耗时比≤6，捕获明显超线性退化 |
| 井数缩放 | 同网格/约束，100→200井 | 耗时比≤3.5 |
| 工作集 | L，查询分块 | 算法阶段RSS增量≤512MiB；另报GUI总RSS |
| 取消 | S/L插值阶段中途取消 | 中位响应≤250ms，最大≤1s |
| GUI | S/L后台运行 | 事件循环延迟p95≤100ms，无秒级假死 |
| 等值线/P1 | 512²、10级、8条简单约束 | 总提线/绕行目标≤20s；同时报告几何复杂度 |

基准使用 Release/RelWithDebInfo，无调试器；预热1次、测5次，记中位数与最大值，隔离其它重型任务。比率门在同机同构建执行；跨机器绝对时间用于验收记录，不把本机毫秒常量直接当全平台CI阈值。

若冻结硬件明显不适合上述绝对目标，R0必须记录理由并请求明确调整；不得事后静默改门。内存复杂度与取消约束必须仍满足。复杂几何还需限制顶点/网格/三角数与工作集，GEOS耗时不能被插值计时隐藏。

## 16. 逐轮实施与原子提交

| 轮次 | 工作 | 交付/退出条件 |
|---|---|---|
| R0 | worktree、源码/依赖冻结、现有接口复核、真实字段盘点、DTO/schema/语义定案、性能负载冻结 | ledger写明基线、接缝、夹具、未决事项；无虚报通过 |
| R1 | Python无界面参考工具、小型golden、普通/局部方向C++核 | O1/O2对拍全绿，未接UI也可独立调用 |
| R2 | 软边界、井群、硬屏障兼容、样本/CRS/网格/预算 | O3/O4/O5全绿，所有自动参数可追溯 |
| R3 | 参数存储、任务池、发布一致性、DERIVED与既有编图接入 | O6/O7/O8全绿，故障注入闭环 |
| R4 | 现有单因素页、真实数值等值线、真实壳端到端 | O9/O11全绿，P0功能闭环 |
| R5 | P0真工区、性能、全量review、文档 | O12/O13对应P0通过，P0 PR可提交 |
| R6 | P1工作场、来源守卫、解释性等值线、保存重提 | O10与相关O7/O8/O9/O11通过 |
| R7 | P1真工区/性能、全量review、最终文档与PR | 全矩阵对账，无未披露必做项 |

每轮一个或少量内聚提交，ledger同步更新。不能将未完成/未验收的全部工作压成单个“完成”提交。

若分P0/P1两个PR：优先等P0进入目标基线再建P1；若采用堆叠PR，P1明确以P0分支为base，记录依赖，P0合并后重定向并检查实际差异。

## 17. 构建、门禁和发布协议

### 17.1 独立工作目录

执行代理必须在新worktree开发。下例日期是方案日期；执行时可改为当天日期并避免重名。遇到同名worktree先检查归属，不删除或重置其它任务。

```bash
git fetch origin master
git worktree add .worktrees/single-factor-native \
  -b goal/single-factor-native-20261002 origin/master
cd .worktrees/single-factor-native
git rev-parse HEAD
```

参考仓库单独只读检出固定SHA。大体积真实资料、用户工程、依赖venv和生成的可执行文件不得进入提交。

若本文尚未进入 `origin/master`，新worktree不会自动包含它。执行代理须从任务附件或原工作目录复制本文到新worktree的同一路径，并纳入R0提交；不能因此丢失执行要求。

### 17.2 构建与测试命令

先读 `BUILDING.md`，以实际vendored prefix配置；下例为本机现有路径，不要求其它机器有同一路径：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j4
ctest --test-dir build -R '^tst_singlefactor_' --output-on-failure
python3 tools/check_layering.py --strict
python3 tools/check_i18n.py
python3 tools/check_ui_invariants.py --strict
python3 tools/check_tidy.py --build-dir build
git diff --check
```

真实数据与最终全量门禁：

```bash
PALEO_REAL_PROJECT_AREA=/实际/工区路径 \
  ctest --test-dir build -R '^tst_singlefactor_(realarea|perf)$' -V
PALEO_REAL_PROJECT_AREA=/实际/工区路径 \
  ctest --test-dir build --output-on-failure
git diff origin/master...HEAD
```

- 性能测试 `RUN_SERIAL`；禁止同时启动多个构建或让其它重型测试污染性能采样。
- Python夹具生成和复验命令由R1脚本实际提供，文档写可运行参数，不能只留省略号。
- 全目标vendor构建无新增项目警告；第三方既有警告单列来源，不笼统声称日志零警告。
- 全量失败先定位新问题；疑似既有失败必须在相同环境/基线对照。不能把所有失败归为环境，也不能静默降低断言/阈值。
- 修改范围外产品代码确为门禁所需时做最小修复；若触及明确禁区先列出具体修复请求，不能借历史其它任务授权扩大范围。

### 17.3 仔细review后开PR

必须完成全量diff自审：职责分层、线程、资源释放、边界值、临时文件、CRS、单位、nodata、旧工程迁移、来源守卫、取消和失败路径。发现问题先修再验。

交付内容至少包括：

```text
.goal-loop-ledger-single-factor-native.md
docs/progress/single-factor-native.md
tests/fixtures/singlefactor/manifest.json
tools/reference/singlefactor/README.md
相关C++实现/测试/构建注册
小型合成截图或可重现截图命令
```

门禁通过后push开发分支并创建PR，不等待CI完成，不合并master。PR描述包含真实完成范围、必做项对应证据、语义差异、性能、兼容性与递延。使用Codex时创建后附加PR artifact。

## 18. Ledger 模板与禁止的“通过”口径

每轮最少记录：

```markdown
## Rn — 标题
- 目标基线/参考SHA/本轮提交：
- 变更与接口：
- 输入fixture/真实资产版本：
- 构建环境/依赖锁/线程：
- 命令：
- 退出码与断言摘要：
- 耗时/峰值内存/数值误差：
- 对应Oracle：
- 失败、原因、修复及复验：
- 未完成项与下一轮：
```

以下均不能当作完成证据：仅语法检查、仅导入成功、仅有截图、仅文件存在、历史README说通过、环境门控未设置而跳过、把Python输出复制成C++输出、只比较色带看起来相似、把制图工作场当分析场。

## 19. 风险与实施决策

| 风险 | 处理方式 |
|---|---|
| 上游文档与实际算法多次变更 | 固定SHA、以调用路径和测试为准；兼容预设注明版本 |
| 软边界被误当硬断层 | 不同semantic；旧break_line不变；隔离/降权分开验收 |
| 漂亮等值线与底图数值不一致 | 独立工作场、来源标识、禁用定量消费、分别测残差 |
| 像元/节点半格错位 | 同查询坐标对拍；明确north-up转换及像元中心契约 |
| 依赖/geometry版本造成差异 | vendor复用、参考环境锁、几何而非字节比较 |
| 大网格或复杂几何OOM/卡取消 | checked预算、分块、空间索引、几何批次、RSS/取消门 |
| 部分发布或关停UAF | 不可变快照、世代守卫、写队列、提交恢复与故障注入 |
| 数值模式静默退化 | 方法实际执行id入provenance；不自动冒充克里金等方法 |
| 稀疏区被均值填满 | 显式外推标记；无井硬区nodata；不以颜色完整性掩盖缺证据 |

## 20. 可直接发送给执行 Agent 的任务说明

```text
请执行 docs/designs/single-factor-native-integration-plan.md。

目标：将 haiyou-visualization 固定提交
27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f 的局部方向单因素算法，
按文档语义移植到 Paleo C++，接入现有单因素/DERIVED/编图流程。
Python只作冻结参考与对拍，不能代替正式C++交付。

先读AGENTS.md、DESIGN.md、架构/分层计划和BUILDING.md，
从最新origin/master建立独立worktree，禁止在主checkout写实现代码。
按R0–R7推进，每轮原子提交并更新ledger。
优先完成P0可信分析场，再完成P1显式制图工作场；允许分PR，
但必须说明阶段范围，不能把P0报告为全文完成。

硬屏障、方向引导、解释性软边界、纯停线必须有独立语义；
旧break_line不能自动变软。制图工作场不可进入定量分析/相分类。
所有新输出需要参数hash、父版本、来源和QC。

完成数值对拍、真实壳端到端、真工区和性能Oracle，
再做全量diff审查、vendor构建、ctest及严格分层等门禁。
失败要定位修复；未执行和门控跳过不能记为通过。
门禁通过后push并创建PR，不等待CI完成，不擅自合并master。
最终报告PR、各Oracle证据、真实性能、兼容性、剩余项及文档路径。
```

[up-structural]: https://github.com/WWX9/haiyou-visualization/blob/27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f/Drawing/drawing/single_factor/structural_idw.py
[up-metric]: https://github.com/WWX9/haiyou-visualization/blob/27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f/Drawing/drawing/single_factor/continuous_metric.py
[up-soft]: https://github.com/WWX9/haiyou-visualization/blob/27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f/Drawing/drawing/single_factor/interpretive_boundary.py
[up-clusters]: https://github.com/WWX9/haiyou-visualization/blob/27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f/Drawing/drawing/single_factor/well_clusters.py
[up-workflow]: https://github.com/WWX9/haiyou-visualization/blob/27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f/Drawing/drawing/single_factor/workflow.py
[up-contours]: https://github.com/WWX9/haiyou-visualization/blob/27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f/Drawing/drawing/single_factor/field_contours.py
[up-workfield]: https://github.com/WWX9/haiyou-visualization/blob/27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f/Drawing/drawing/single_factor/contour_work_field.py
[up-gridded]: https://github.com/WWX9/haiyou-visualization/blob/27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f/Drawing/drawing/single_factor/methods/gridded.py
[up-policy]: https://github.com/WWX9/haiyou-visualization/blob/27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f/Drawing/docs/TREND_CONTOUR_PARTITIONS_20260913.md
[up-tests]: https://github.com/WWX9/haiyou-visualization/blob/27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f/Drawing/tests/test_structural_idw.py
