# Review B — 算法数值正确性 / Services / 后台任务与性能（full-audit-20261022）

- 基线：b301f2e8（增量区间 093346e6..b301f2e8，收口 #607 之后 20 个 PR）
- Agent：B（只读深审；禁改源码、禁构建）
- 方法：Read 全文 + 调用链追踪为主，grep 仅定位；vendored QGIS 4.2.2 源码
  （/tmp/qgis-4.2.2、/usr/include/qgis）交叉验证 #609 谓词语义；开放 Issues
  （gh）核对排除项。

## 1) 覆盖清单

### 1.1 增量文件（git diff 093346e6..b301f2e8 -- src/algorithms src/services，11 文件，全文 Read + 调用链）

| 文件 | 增量来源 | 方式 | 风险级 | 测试对应 | 结论 |
|------|---------|------|--------|---------|------|
| src/algorithms/paleoalgorithms.{h,cpp} | #609 | 全文读 + PreparedHull 调用链 + QGIS 源对照 | 中 | tst_algorithms（+131 行逐点对照）、tst_idw_hullclip_perf（+83） | 通过；见 3.1 |
| src/algorithms/faciessliver.cpp | #610 | 函数级读（absorbSmallParts 溢出闸） | 低 | 已有 sliver 测试 | 通过（int 上溢防御正确；>INT_MAX 网格如实不合并） |
| src/services/crossplotsources.cpp | #610 | 全函数读 + WellLogRead/depthScaleToMetres/LasCache 链 | 中 | tst_crossplot_samples（+45） | 通过；口径不一致见 B-02 |
| src/services/petrophyscomputeservice.cpp | #610 | additionalLogGridIsMd/loadWellCurves 全读 | 中 | tst 侧（petrophys 面） | B-03（错误归因优先级） |
| src/services/welllogset.{h,cpp} | #610 | storedHeader/mdAxisInMetres/readCurveTvd 全读 + 消费方 datapreviewtabwelllog | 中 | tst_datapreview（+122） | B-02（legacy 单位双口径） |
| src/services/previewdoc.{h,cpp} | #610 | wellLogHeaderAt + DataCatalog::resolvedVersionPath/catalogPath 口径 | 低 | tst_datapreview | B-04（元数据优先无失效闸） |
| src/services/seismictaskservice.h | #611 | 注释契约 + SgySectionStats 字段 | 低 | — | 通过 |
| src/services/seismictaskservice_slice.cpp | #611 | startSectionExtraction 全读 + 引擎（vendor/sbm SgyVolumeSource/WorkspaceVolumeSource/PagedWorkspace distances 单位）+ 消费方 SectionGeometry::fromColumns / seismicsectiondockwidget | 高 | tst_seismic_index（:458-468 米口径断言） | 通过；见 3.2 |
| （#612 实载体）src/qgis/previewrasteranalysis.{h,cpp} | #612 | 全文读（robustResidualScale/gridExtrema/medianInPlace/readGrid） | 中 | tst_previewmap_assets（+123） | 通过；边缘态见 B-06 与 3.3 |
| （#618 实体面）src/ui/pages/constraintpage_generate.cpp + workflow/constraintfactorjobs_factor.cpp | #618 | 增量 diff + setRunBusy/runoutcome/factorJobFailed 发射序追踪 | 中 | tst_mappingpages（+152） | 通过；见 3.4（#618 对 algorithms/services 接口零改动，diff --name-only 证实） |

### 1.2 并发/任务系统（深读）

| 文件 | 方式 | 结论 |
|------|------|------|
| src/services/paleotaskservice.{h,cpp}（434 行全文） | 逐函数：TaskRunner markStarted/applyFinish、shutdown 补偿（#416）、beginNewSession、layerHolds 引用计数、retention/prune | 通过；shutdown 窗口竞态（clear 与 markStarted 间）经「先 requestCancel 全体」闭合，补偿 applyFinish 幂等闸防双到 |
| src/services/jobrunner.{h,cpp}（395 行全文） | claim/generation/commit 入口 owner 断言/#235 会话比对/失败态进 commit/failPublication | 通过；见 3.5 备注（stale 代 worker 返「已取消」串在析构 bump 下终态落 Failed 的化妆性错位，busy 闸下生产不可达） |
| src/workflow/batchjobqueue.cpp（1122 行全文） | 槽位/出队/让位/熔断/续跑/取消/持久化全链 | **B-01**（优先级反转）；其余（会话守卫、#415 回队泵、busy 重试、崩溃恢复对账、QSaveFile）通过 |
| src/services/futuretask.h（67 行全文） | track() 四连接生命周期、destroyed→cancel、completed 标志 | 通过 |
| src/services/imagelod.cpp（218 行全文） | requestDecode pending/epoch、decodeHub 线程闸 | 通过（GUI 线程约定自洽） |
| src/workflow/constraintfactorjobs_factor.cpp（runFactorWork :1129-1235） | 生产异步栈全读：generation/session/catalog/dir 四守卫、Pending cleanup 析构钩、watcher destroyed 兜底、#480 P3-3 输入冻结口径 | 通过；factorJobFailed 先于 factorJobStateChanged(false) 发射——#618 一键链时序正确 |

### 1.3 算法数值正确性（抽样深读 + 接口层）

| 模块 | 文件 | 方式 | 结论 |
|------|------|------|------|
| geostat | kriging.cpp（全）、linsolve.cpp（全）、variogram.cpp（模型/各向异性/分箱段） | OK 加边方程/方差符号/负方差闸/部分主元 LU/球-指-高斯模型/方位角旋转变换逐项推导核对 | 通过（协克里金总基台闸缺失属 #589 已排除项，未复核 cokriging.cpp 内部） |
| geostat | sgs.cpp :180-320 | per-realization 派生种子、Fisher–Yates、并列距离全序比较器、重合点特判 | 通过（同 seed 逐位可复现契约有真实机制） |
| petrophys | petrophys.cpp（252 行全） | Larionov 新/老、Clavier、Wyllie、Archie、Welford、resampleLinear 双指针逐式核对 | 全部公式与发表式一致；NaN/参数域防御到位 |
| velmodel | velocitymodel.cpp（427 行全） | v0kDepth/v0kTwt 闭合式（expm1/log1p）、Gauss-Newton 解析导数 dT/dv0、dT/dk 逐项求导复核、IDW 井间、二分插值 | 通过（导数推导与实现一致，指数定义域有阻尼回溯） |
| mincurvature/gridsolver | mincurvature.cpp（全）+ gridsolver.cpp :440-640 | Smith–Wessel 模板系数、屏障带调和算子切换（#293）、停滞/发散守卫、收敛阈 | 通过（模板系数代数核对一致：a0=6u²+6v²+8uv 与 GMT 一致） |
| seismicattr | seismicattr.cpp :1-260 | Hilbert reflect 填充、Barnes 差分瞬时频率、Q 原型、时窗族 NaN 语义 | 通过 |
| 单因素 IDW | paleoalgorithms.cpp :540-860（约束语义/屏障泛洪/各向异性/QC 元数据） | 全读 | 通过；各向异性 u²/r²+v²·r² 与 kriging「沿方位长变程」语义同向一致 |
| 合成记录 | seismictaskservice_slice.cpp :1069-1147 | Ricker π² 复核 | **B-1（20261021）已修复**：piSq=π²，系数正确 |

### 1.4 services 接口层抽样

scriptrunner.cpp（进程组回收/队列/取消，全读通过）、nativerunners.cpp（73 行全读，纯转发）、seismictaskservice_attr.cpp :380-560（**B-3 相干 IO 放大维持「登记不修」注释口径——已知项**；**B-4 进度账本已修复**：算段逐 inline 入账 + totalUnits 补齐）、imagelod（见上）、futuretask（见上）。

### 1.5 未核查区（如实声明）

- algorithms/：cluster/、dsp/、evolution/、faultsurface/、inversion/、stratgrid/、wellsiting/、horizontrack.cpp、ensemblestats、curveexpr、distancetransform、rasteralgebra、surfacevolumes、petrophys.h 之外的单因素大文件（localdirectionalgorithm.cpp 73KB、fieldcontours.cpp 47KB、localidw.cpp 41KB、structural*.cpp、support.cpp、partition.cpp、corridor.cpp、faciespolygonize.cpp、constraintbarriers.cpp 仅按 #589 已录条目覆盖，未独立复核）。
- services/：crossplotsamples、dataexport、datagraph*、derivationgraph、evidenceindex、pythonenv/pythonrepl/scriptprotocol、crashreport、cataloghealth、toolavailability、storagegovernance（仅抽样）、shortcutregistry、startuptrace、seismiccluster*/seismichorizoncluster/seismicmapping、faciesclassificationservice/faciesdisplaysettings/faciestraining、frameworkservice、paleomapfiles、projectdata、algoparamschema/algorithmspec/singlefactordef、seismictaskservice 其余分片（attr 之外仅接口层）。
- 排除项按任务书执行：#589/#594/#593/#561 各条、#627、20261021 已修清单（B-1/B-4 已验证修复；B-3 维持登记不修）。

## 2) Findings

### B-01（P3 / HIGH）批量作业以 High 优先级挤占共享 ≤4 线程池，「让位交互」闸在队列层失效

- Module/File：src/workflow/batchjobqueue.cpp:773-805（pump 让位判据）+ src/services/jobrunner.h:306（Priority::High 硬编码）
- Root Cause：JobRunner::start 对所有作业（含 BatchQueue 的批量项）统一
  `PaleoTask::Priority::High` 入 PaleoTaskService 专用池（4 线程）；交互任务
  （切片/剖面 startBounded、futuretask 面板作业）走 `start(title, work, layerId,
  quiet)` 默认优先级 0。QThreadPool 出队高优先级先出。pump 的让位判据
  `（服务 runningCount − 本批 running）>= poolCap` 只在**非批任务已占满池**时才
  停止出队——但排队中的 High 批任务永远先于排队中的交互任务被取走，非批任务
  达不到 4 个在跑，让位分支结构性不可达。
- Trigger：批量成图运行中（并发最多 8 项排队于 4 线程池），用户做切片/剖面/
  预取等交互取数。
- Observed/Expected：交互任务在池队列里排在 High 批任务之后，需等 1~2 个批量项
  （分钟级）完成才开跑；D4.6 明文「用户交互路径 High/Normal，预取与后台扫描
  Low」，批量属后台批处理却拿了最高优先级，与让位设计意图（ledger D4）正反
  两层自相矛盾。
- Reproduction：起一批多方法多井批量（concurrency≥4），批运行中拖动任意线
  提取剖面，观察任务开跑延迟（任务中心时间戳）。
- Impact：批量期间交互取数延迟可达分钟级（无错误结果，纯延迟）；quartz 式
  「提取中…」进度条长挂。
- IssueMatch/PRMatch：无冲突（20261021 register 无此项；开放 issues 无）。
- FixStrategy：JobRunner::start 增加优先级参数（默认 Normal），BatchQueue 传
  Low；或 startBounded 交互面提级 High。二者取一即可恢复 D4.6 语义。
- RegressionTest：tst 下起 4 个 sleep 批任务（Low）+ 1 个交互任务（startBounded
  同步等待），断言交互任务在首个批任务完成前已开跑（用 markStarted 时序）。

### B-02（P3 / VERIFIED）深度轴单位双口径：同一 LAS/DLIS 文件在综合井图静默按米直通、在 TVD 读取被拒；crossplot 与综合图对 DLIS/LIS 严格度相反

- Module/File：src/services/welllogset.cpp:283-307（mdAxisInMetres legacy 分支）
  vs :362-371（readCurveTvd 未知单位拒绝）；对照
  src/services/crossplotsources.cpp:176-186（DLIS/LIS 走严格 MD+单位闸）。
- Root Cause：mdAxisInMetres 把 las/dlis/lis 归为 legacy：`!legacy` 时才要求
  `depthScaleToMetres(unit)` 可解，legacy 文件单位既非 M/FT 词表（如 "DM"、
  "0.1FT"）也非空时，scale=nullopt 且不判错 → 原样数值当米轴返回 true；同文件
  走 readCurveTvd（goal/well-trajectory）时同一单位被「深度单位未知（%2），
  不猜折算」拒绝。crossplot 侧 detect()!=Las（含 DLIS/LIS）一律严格闸。
- Trigger：井 logset 内挂一条单位列为非 M/FT/空 的 LAS（或索引元数据未声明的
  DLIS），打开数据预览综合井图。
- Observed/Expected：综合井图按原始数值画 MD 轴（DM 井 10 倍错位）且无告警；
  期望与 readCurveTvd 同口径：未知非空单位如实拒绝并把 axisWarnings 上屏
  （datapreviewtabwelllog.cpp:340 已有告警通道，只差判定）。
- Reproduction：构造 LAS 深度道 unit=DM 的夹具，分别调
  WellLogSet::mdAxisInMetres 与 readCurveTvd：前者 true（原样轴），后者 false。
- Impact：显示层深度轴错标尺（不毒化数值计算链）；QC 口径分裂：同一数据在
  两个消费面给出相反结论。
- IssueMatch/PRMatch：#589 各条不含此项；#610 引入（mdAxisInMetres 为新增）。
- FixStrategy：mdAxisInMetres legacy 分支补 `scale == nullopt && unit 非空 →
  拒绝`，与 readCurveTvd 对齐；或在返回值带 unit 注记由消费方展示。
- RegressionTest：tst_datapreview 增夹具：unit=DM 的 LAS → mdAxisInMetres
  断言 false 且 error 含「单位未知」；unit=FT → 轴值 == 原值×0.3048（解析解）。

### B-03（P3 / VERIFIED）petrophys 合并的 MD 闸先于 parse 结果检查，解析失败被误报为「索引元数据未校正」

- Module/File：src/services/petrophyscomputeservice.cpp:550-556（driver 路径）与
  :539-547（单文件路径）。
- Root Cause：`additionalLogGridIsMd(plan.driverPath, plan.driverBasis, driver.curves,
  error)` 在 `!driver.ok || driver.curves.isEmpty()` 检查**之前**执行；driver 为
  text/XML 且解析失败时 curves 为空，MD 闸先落 `curves.isEmpty()` → 输出
  「测井计算需要明确且一致的 MD 基准和米/英尺单位」而非真实解析错误。同构问题
  在 :615 副文件路径（doc 解析失败时同样先吃 MD 闸文案）。
- Trigger：驱动井文件损坏/半写（parse 失败）或 basisByPath 缺失且
  WellLogRead::parseHeader 失败。
- Observed/Expected：用户看到「请先校正索引元数据」去找元数据，真因是文件解析
  失败；期望先判 ok/空并透传 parse error（additionalLogGridIsMd 内部对
  parseHeader 失败已如实返回，仅外层调用序把两类失败搅在一起）。
- Reproduction：把 text 驱动文件截断为非法表头，运行测井计算；错误文案断言。
- Impact：诊断误导（失败仍如实失败，无数值错误）；P3 低位。
- IssueMatch/PRMatch：无冲突。
- FixStrategy：把两处 additionalLogGridIsMd 移到 ok/isEmpty 检查之后（或给它加
  curves 空时的专门「解析失败」透传）。
- RegressionTest：tst 夹具：损坏 text 驱动 → 断言 error 含 parse 错误串而非
  「MD 基准」串。

### B-04（P3 / SUSPECTED）wellLogFiles/wellLogHeaderAt 优先信 catalog 元数据（storedHeader），无 mtime/指纹失效闸

- Module/File：src/services/welllogset.cpp:176-193（storedHeader 优先于
  WellLogRead::parseHeader）+ src/services/previewdoc.cpp:326-348
  （wellLogHeaderAt 全版本线性扫）。
- Root Cause：#610/#603 后目录头信息优先取受管版本 extra（logCurveNames/
  logIndexBasis/logWellName），文件本体被原地改动（用户直接编辑 artifacts 下
  受管文件、或 #603 前旧 extra 与后来替换的体不匹配）时无任何失效判定——
  旧行为每次实读文件头，无此窗口。附带：storedHeader 硬编码
  `sawAscii=true`；wellLogHeaderAt 对 `cat->versions()` 全表线性扫 + 双级
  cdUp 路径还原，每次预览表头请求 O(全部版本)。
- Trigger：受管 well_log 版本文件被外部修改（绕过 catalog 写口）。
- Observed/Expected：曲线并集/预览头沿用旧列名（后续按列取数才落到真文件，
  「曲线已不存在」类错在更深处爆）；期望 storedHeader 命中时校验
  version 的 mtime/size 指纹（catalog extra 已存 logCurveNames，同存指纹成本为零）。
- Reproduction：导入 text 井文件 → 手工改 artifacts 下文件列头 → 重开数据
  预览：曲线列表仍旧。
- Impact：低（受管文件按约定不可变，编辑属越轨操作）；错位时报错点远离真因。
- IssueMatch/PRMatch：#597（外链资产重定位后预览指旧路径）同族但不同根因
  （外链路径 vs 受管元数据陈旧），不重叠。
- FixStrategy：extra 增 size+mtime 指纹，storedHeader 校验不符即回落实读。
- RegressionTest：tst：导入后改文件 mtime → wellLogFiles 告警或按实读头出列。

### B-05（P3 / VERIFIED，存量非增量）requestSummary/requestHistogram/requestExtrema 失败/取消路径永不回调 ready

- Module/File：src/qgis/previewrasteranalysis.cpp:81-88 / :119-126 / :596-599。
- Root Cause：`connect(task, finished, receiver, [..]{ if (Succeeded && key 未变)
  ready(..); })`——Failed/Cancelled 分支无任何回调；调用方若以挂起态等
  ready（预览页统计/直方图/极值面板），任务失败后永久无响应（任务中心有行，
  但页面无完成事件）。receiver destroyed 时连接断开属正常。
- Trigger：预览栅格文件在请求后被删/读失败；或任务被工程切换取消。
- Observed/Expected：面板停「计算中」；期望失败也回调（带空载荷 + error）或
  至少提供失败信号。#612 改动了 requestExtrema 主体（阈值链），该缺口顺带
  复核，非本次引入。
- Impact：低-中（取决于调用方是否有超时兜底；预览页属交互面）。
- IssueMatch/PRMatch：无冲突（#590 是 UI 清单不含此条）。
- FixStrategy：失败分支 `ready(BandSummary{})` + 调用方按 valid=false 渲染错误。
- RegressionTest：tst：指向不存在路径的 uri 走 request*（tasks 服体）→ 断言
  ready 恰被调一次（valid=false）。

### B-06（P3 / SUSPECTED，边缘态）robustGridExtrema 在 MAD=0 的平坦/量化网格上阈值退化为 0，产出至多 maxCount 个零显著度「极值」

- Module/File：src/qgis/previewrasteranalysis.cpp:467-549（robustResidualScale
  返回 0 → gridExtrema(minProminence=0)）+ :374-399（neighbourhood 平坦格
  isMax=isMin=true，peak 恒 true）。
- Root Cause：完全平坦面 MAD=0 且 mean|r−med|=0 → scale=0；`prom < 0` 恒假 →
  全部内域格成为候选（prominence 0），stable_sort 后截 maxCount=100 个扫描序
  格子标 peak=true。头注自述「完全平坦/无残差 → 0（此时也不存在严格极值）」
  与实现不符：严格极值判定 `nv >= v` 在全等值面上恒真，不是「不存在」。
  旧口径（全局 σ×0.5）平坦面 σ=0 同样放行，行为等价——非回归，但 #612 的
  头注契约与实现矛盾。
- Trigger：常数值面/高度量化面（残差全零）上请求极值标注。
- Observed/Expected：平坦面标出 100 个「峰」；期望按头注返回空集（scale==0
  且 prom==0 双零应拒绝）。
- Reproduction：合成 32×32 全 1.0 的 ExtremaGrid → robustGridExtrema 断言当前
  返回 100 项、peak 全 true；修后断言空。
- Impact：纯标注噪声，无数值毒化。
- IssueMatch/PRMatch：无冲突。
- FixStrategy：`gridExtrema` 候选条件加 `prom > 0`（或在 robustGridExtrema 里
  scale==0 直接返回空）。
- RegressionTest：见 Reproduction（解析解夹具，确定性）。

### 不立案备注（复核后判定不构成缺陷/不可达）

1. JobRunner stale 代 worker 返回「已取消」串但任务落 Failed（applyFinish 按返回
   串判态）：仅析构 bump generation 路径可达，busy() 闸使生产不可达；丢弃逻辑
   （jobCompleted(false)）不受影响。化妆性，不立案。
2. previewrasteranalysis 的 static QCache 非线程安全：文档自限 owner-thread，
   现有调用面全在主线程；不立案，留意后续跨线程复用。
3. crossplotsources 对 LAS 硬编码 indexBasis="MD"：与 lasparser.cpp:220
   「LAS 不声明基准，行业惯例 MD」全仓口径一致，非缺陷。
4. PreparedHull（#609）：QgsGeometry::contains(x,y) 实现即临时 QgsGeos 未
   prepare 的 GEOSContains；prepared 只加索引不改谓词（GEOS 契约），hull 生存期
   覆盖 PreparedHull（同作用域后声明），prepareGeometry 失败时 QgsGeos 自动回
   落非 prepared 路径——逐格一致声明成立。
5. startSectionExtraction（#611）：引擎三源（SgyVolumeSource 计划/legacy、
   WorkspaceVolumeSource、PagedWorkspace）distances 均为线号弧长，与
   SectionGeometry::fromColumns 的 grid 轴契约、canvas 的 columnDistancesM
  （另算米）三面一致；缓存条目内含 columnDistancesM 一并命中；旧 B-2 的
   「均匀合成」分支维持删除。回归测试 tst_seismic_index:458-468 已断米口径
   解析值（9 步 ×25m×√2）。
6. faciessliver 溢出闸（#610）：qsizetype 乘法 + `total > INT_MAX` 早退，行为
   语义（不合并而非 UB）合理。

## 3) 模块健康结论

### 3.1 #609（IDW prepared GEOS）
正确。谓词等价经 QGIS 源码对照确认；裁剪掩码逐格一致的回归测试在
tst_algorithms/tst_idw_hullclip_perf 落地。IDW 核（power=2、各向异性
u²/r²+v²·r²、屏障同分量插值、非有限值落 nodata、凸包/屏障回执 QC 元数据）
本轮全文复核通过。

### 3.2 #611（剖面道距口径回归）
正确且三层（vendor 引擎→stats 双字段→SectionGeometry/canvas）自洽，
columnDistances（线号弧长）/columnDistancesM（米，不合成）契约在头注、实现、
测试三处一致。20261021 的 B-2 由本 PR 以更优口径终结。

### 3.3 #612（稳健极值阈值）
主体正确：medianInPlace 偶数下界中位数处理正确；MAD→1.4826、MAD=0→
1.2533·meanAbs 的回落链符合稳健统计惯例；阈值 k=3 离群口径有解析依据。
遗留边缘态 B-06（平坦面契约与实现矛盾）。

### 3.4 #618（一键成图）
algorithms/services 接口零改动（仅 UI 编排复用既有两按钮）。一键状态机的
「失败不假成功」链经信号发射序验证成立：factorJobFailed/factorJobCancelled 先
写 runoutcome、factorJobStateChanged(false) 后触发 singleShot(0) 的
continueOneClick，取消/失败均如实停链。

### 3.5 任务系统总评
PaleoTaskService/JobRunner/futuretask/runFactorWork 四件的生命周期、取消、
会话、代次守卫经全链审查无新缺陷：取消不假成功（applyFinish 按 cancel 位优
先）、失败不访问未产生 outputs（Pending 析构钩 + commit 失败 failPublication）、
generation/session 阻迟到结果（#235 比对点在框架内有真实消费）、关工程不发布
旧成果（beginNewSession → pump 单点守卫 + JobRunner 会话比对）、失败清理不破坏
历史版本（QSaveFile + 逐项终态回写）。唯一结构性问题是 B-01 的优先级反转
（设计意图 D4.6 与实现的矛盾，非数据正确性）。

### 3.6 数值正确性总评
抽查的 8 个数值核（OK 克里金、LU、变差函数、SGS、岩石物理五式、V0-k 速度模型、
最小曲率模板、复地震道族）公式与实现逐项推导一致，未见新的数值错误；边界防御
（零基台、非有限、负方差、发散/停滞拒发布）成熟。20261021 的 B-1（Ricker 2π）
已修复验证，B-4（进度账本）已修复验证，B-3（相干 IO 放大）维持代码内
「登记不修」口径（vendor 读 API 缺口，非本次范围）。

### 3.7 总计
新立案 6 项：P0×0、P1×0、P2×0、P3×6（B-01 HIGH，B-02/B-03/B-05 VERIFIED，
B-04/B-06 SUSPECTED）；其中 B-05 为存量非增量。无数据正确性级（P0-P2）新发现。
