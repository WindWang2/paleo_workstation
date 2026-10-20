# Agent B 深度审查报告（src/algorithms + src/services）

- 审查人：Agent B（只读深度代码审查）
- 基线：worktree `/home/kevin/projects/paleo_workstation-full-review-20261021`（BASE e6837e14）
- 日期：2026-10-21
- 范围：`src/algorithms/**`（全部）、`src/services/**`（全部）
- 方法：头文件契约 vs 实现逐条对照、调用链追踪（跨 ui/workflow/vendor 边界仅追到消费点）、
  数值推演（python3 辅助算术验证）、vendored 代码只看集成边界。

## 发现总览

| ID | Severity | Confidence | Module | 一句话 |
|----|----------|------------|--------|--------|
| B-1 | P2 | VERIFIED | seismictaskservice_slice | 合成记录 Ricker 子波系数写错（2π 代替 π²），实际主频 ≈ 0.798×请求值 |
| B-2 | P2 | HIGH | seismictaskservice_slice / .h 契约 | 任意线剖面 columnDistances 全链是 il/xl 线号单位，服务头注承诺「真实道距、绝不合成」，消费端按米用 |
| B-3 | P3 | VERIFIED | seismictaskservice_attr | 相干时间切片扫描每线整剖面读取（nS 样）只用 ±tH 小窗，IO 放大 ~nS/(2tH+1) 倍 |
| B-4 | P3 | VERIFIED | seismictaskservice_attr / _slice | 属性任务进度账本与实际工作量脱节：相干扫描恒停在 50% 后跳 100%；剖面属性 compute 段只计 0 号分片列 |

（本轮对其余所审面位未发现可达、可论证的新缺陷；宁缺毋滥，见文末覆盖清单与「审过未立案」段。）

---

## B-1 合成记录 Ricker 子波频率参数化错误（2π 写成 π²）

- **Severity**: P2　**Confidence**: VERIFIED（数学推演 + 仓内权威公式对照）
- **File:line**: `src/services/seismictaskservice_slice.cpp:1084-1093`（`syntheticCore`，匿名命名空间）
- **函数**: `syntheticCore()`（`computeSyntheticSeismogram` 两个公开重载的共用核）

### 根因

```cpp
const double pi2 = 2.0 * std::acos(-1.0);              // = 2π
const double f2 = rickerHz * rickerHz;
...
const double arg = pi2 * f2 * (tau / 1000.0) * (tau / 1000.0);   // = 2π·f²·t²
amp += double(rc[k]) * (1.0 - 2.0 * arg) * std::exp(-arg);
```

Ricker 子波的正确形式是 `w(t) = (1 − 2π²f²t²)·e^(−π²f²t²)`。指数与多项式里的系数
应为 **π² ≈ 9.8696**，代码用了 **2π ≈ 6.2832**。本仓权威定义就在
`src/algorithms/inversion/wavelet.h:44`（注释明写公式）与 `wavelet.cpp:154`（`makeRicker`
实现正确）——同一仓库内两套实现不一致，反演管线对、井震合成记录错。

### 数值后果（python3 复算）

- 得到的仍是 Ricker 形状，但等效主频 `f' = f·√(2/π) ≈ 0.798·f`：
  请求 25 Hz 实际产出 ≈ **19.95 Hz**；
- 零点间距变宽 `√(π/2) ≈ 1.253×`（周期长 25%）；
- 副瓣时序同比例错位。

### 触发条件 / 影响面（真实调用链）

- 生产消费点：`src/ui/seismicsection/seismicsectiondock_wells.cpp:248` ——
  地震剖面 dock 的「井震综合」叠加，对每口有 AC+DEN+时深的井调
  `computeSyntheticSeismogram(..., 25.0)` 画合成记录道。合成道带宽比同名义频率的
  反演子波（`makeRicker`，正确）低 ~20%，用户做井震标定时同相轴视觉宽度/分辨率
  与反演成果系统性不一致，且无任何提示（`rickerHz` 是硬编码 25.0，非用户可调，
  用户无途径发现「25 Hz」名不副实）。
- 测试缺口：`tests/tst_seismic_welltie.cpp:128-139` 只断言合成道**峰值位置**
  （`twtMs[peakIdx] ≈ 1560`）——峰值时刻在频率缩放下不变，该测试对系数错误不敏感；
  `syntheticSeismogramOverloadsAgree`（:217）只锁两个重载互一致，锁不住公式。

### 排除比对

- 147 项已知清单：无任何条目涉及 syntheticCore/Ricker/合成记录（B.7 是 _interp.cpp
  的 CSV 写入；L1.x 是 i18n）。
- `02-excluded-files.txt`：`seismictaskservice_slice.cpp` 不在清单内（只有 .h/_interp/_viz）。
- 上两轮已修/登记项：无此项。→ **新发现，非 EXCLUDED_ACTIVE_WORK**。

### 建议修复 / 回归测试

- 修复：`arg = π²·f²·t_s²`（用 `std::numbers::pi` 的平方；顺手把 `pi2` 改名 `piSq`）。
- 回归：单反射系数尖峰 → 对合成道做过零间距或 FFT 主频断言（期望 ≈ rickerHz ± 容差）；
  与 `inversion::makeRicker(f)` 的互相关峰值应 ≥ 0.99。

---

## B-2 任意线剖面 columnDistances：单位与「绝不合成近似值」契约双重失守

- **Severity**: P2　**Confidence**: HIGH（单位失实面 VERIFIED；合成分支可达性 SUSPECTED）
- **File:line**:
  - 合成分支：`src/services/seismictaskservice_slice.cpp:199-215`（`startSectionExtraction` 缓存命中路径）
  - 失实的契约：`src/services/seismictaskservice.h:768-770`
    「命中时 stats 一并从缓存条目取回（**真实道距；绝不合成近似值**）」
  - 单位源头（集成边界）：同文件 `:266` `outStats->columnDistances = std::move(slice.distances)`
    （引擎路径）与 `:289` `BuildLineSection`（回落路径）

### 根因（两个 facet）

**Facet 1 — 单位不是「道距」而是 il/xl 线号差。** 两条产出路径的距离都在
**inline/xline 线号空间**累计：

- 引擎路径：`vendor/sbm/src/Engine/PlannedSection.cpp:64-71`
  `di = pathPoints[i].x − pathPoints[i-1].x`（`glm::ivec2`，即线号差），
  `length = sqrt(di²+dx²)`，逐列累计为 `columnDistances`；
- 回落路径：`vendor/sbm/src/Data/Sgy/SgySectionBuilder.cpp:56-63` 同式。

而消费链把它**当米**用：`src/ui/seismicsection/seismicsectiondockwidget.cpp:597`
把 `stats.columnDistances` 传给 dock → `seismicsectioncanvas.h:84`
形参名即 **`columnDistancesM`** → `seismicsectioncanvas.cpp:104-107` 无距离时的
兜底是 `i * 25.0f`「Default 25m trace spacing」→ `seismicsectioncanvas_nav.cpp:169-171`
用 `m_columnDistances.back()` 当剖面总长，`_paint.cpp:493-501` 用它定位沿剖面对象。
il/xl 步长 × 线距 ≠ 1 米的测网（如 12.5 m 面元、或线号步进 2 的稀疏测网）上，
任意线剖面的横向比例尺与沿剖面对象定位按「线号差/米」的比值系统性错位。
（vendor 内部实现不改——这是服务层把 vendored 单位原样外送并配了失实文档的
集成边界问题。）

**Facet 2 — 缓存命中路径会合成距离，直接违反头注「绝不合成近似值」。**
`slice.cpp:200-215`：命中条目 `columnDistances` 为空且 `width>0` 时，按 pathPoints
线号长度**均匀重采样**出一套距离冒充。当前两条产出路径（vendor 两处）都恒定填满
该数组（`PlannedSection.cpp:171`、`SgySectionBuilder.cpp:74`），故该分支大概率
防御性死代码——但它一旦被走到（引擎侧契约未对该字段立书面保证），输出是
「均匀分布 + 线号单位」的双重近似，且与头注承诺直接矛盾。

### Observed vs Expected

- Observed：`columnDistances[i]` = 沿折线的**线号弧长**；缓存缺距离时被均匀合成。
- Expected（按头注与消费端命名）：真实物理道距（米），且缓存命中不合成。

### 排除比对

- 147 项清单：无 columnDistances/任意线距离相关条目（C.11 是 paleomapsource km²）。
- excluded-files：`seismictaskservice_slice.cpp`、`seismictaskservice.h` 均不在清单。
- 上两轮：B-04 是 sattrio 取消谓词，无关。→ **新发现**。

### 建议修复 / 回归

- 服务层把距离换算到米（引擎给逐列 inlineNos/xlineNos，可用
  `SgyCoordinateMapper` 仿射把线号位置映射到 XY 后按米累计），或至少改头注
  如实写「线号单位」并在消费端换算；
- 删掉均匀合成分支或将其改为显式失败（与头注一致）；
- 回归：过井任意线（井在路径 30% 处）断言画布上井标记的列偏移 ≈ 30%×总长（米口径）。

---

## B-3 相干时间切片扫描：整剖面读取只用小窗，IO 放大两个数量级

- **Severity**: P3（性能；交互路径）　**Confidence**: VERIFIED（读码）
- **File:line**: `src/services/seismictaskservice_attr.cpp:399-454`
  （`startTimeSliceAttribute` 的 Coherence 分支）

### 根因 / 触发条件

相干时间切片只需要目标采样位 ±`coherenceTimeHalf` 的 slab（头注 `seismictaskservice.h:576-579`
明写「相干：[s−timeHalf, s+timeHalf] 采样 slab」），但实现逐 inline 调
`volume->ExtractSlice(SgySliceType::Inline, ...)` 取**整条剖面（height == nS 全样）**，
几何校验也钉死 `slice.height != nS` 即失败（`:437`），随后只拷
`(nS−1−w)*nXl+xl, w∈[s0,s1]` 的小窗进 slab（`:447-450`）。

放大系数 = `nS / (2·coherenceTimeHalf+1)`：典型体 nS≈1500、默认 timeHalf=2 时
≈ **300×** 的解码/IO 浪费；这是「拖采样位滑杆 → 逐采样重扫」的交互路径
（`sampleIndex` 每变一次整趟重来，缓存键含 sampleIndex）。

对照：同文件 `startAttributeVolume`（`:891-953`）读整剖面是**合理**的（体扫描
要全样）；错只错在时间切片入口复用了整剖面取数。

### Observed vs Expected

- Observed：每线读 nS 样、用 ≤2tH+1 样；扫描耗时被读段支配。
- Expected：只取 slab 窗（SgyVolume 无窗口读时至少应把此放大倍数写进 QC/文档，
  或走 `ReadVoxelWindow` 类窗口通道）。

### 排除比对

- 147 项：无 seismictaskservice_attr.cpp 条目（H6/E.x 是门禁类）。
- excluded-files：`seismictaskservice_attr.cpp` 不在清单。→ **新发现**。

### 建议 / 回归

- 用体窗/ROI 读通道取 [s0,s1]；或按 `paramHash` 把 coherence 的 sampleIndex 维度
  从缓存键中摘除（同 slab 邻近采样复用同一 slab 集）。
- 回归：断言相干 ts 扫描触发的解码样点数 ≤ nIl·nXl·(2tH+1)·常数。

---

## B-4 属性任务进度账本失真：相干扫描恒 50% 后跳 100%；剖面属性 compute 段只计 0 号分片

- **Severity**: P3（进度/最终状态一致性，任务页与 ETA 面失真）　**Confidence**: VERIFIED（读码）
- **File:line**:
  - `src/services/seismictaskservice_attr.cpp:396`（`totalUnits = 2·nIl·nXl`）
    vs `:451`（读循环每线 `done += nXl` → 终值仅 `nIl·nXl`）与 `:479`
    （`reportBytes(totalUnits, totalUnits)` 强行跳满）；
  - `src/services/seismictaskservice_slice.cpp:749-758`（`startAttributeSlice` 逐道族
    并行分片，`report=true` 只有 0 号分片：`totalUnits − sliceUnits + (x−x0+1)·height`，
    x0=0、xEnd=chunk，其余 nThreads−1 个分片的列从不入账）。

### 根因 / 后果

- 相干 ts 扫描：读段结束 `done` 到 50%，随后整块 semblance 计算（该分支真正的
  计算热点，`:457-469`）期间进度恒示 50%，结束时跳 100%——「进度与最终状态
  一致性」破缺，ETA 按假速率线性外推也失真。
- 剖面属性（Envelope/InstFreq/…）：compute 段进度上限 ≈
  `sliceUnits·(sliceCount + 1/nThreads)`，4 线程时封顶 ~62%，随后任务直接成功。

### 排除比对

147 项清单与上两轮登记均无这两个进度账本条目（D-xx 为 UI 项）；
`seismictaskservice_attr.cpp` / `_slice.cpp` 均不在 excluded-files。→ **新发现**。

### 建议 / 回归

- 相干分支把 totalUnits 改为 `nIl·nXl·(1 + slabN/nS)` 口径或读完后按 compute 段
  单独推进；
- 剖面属性 compute 用共享原子计数器聚合全部分片列。
- 回归：跑相干 ts 与 Envelope 剖面属性，轮询 `task->percent()` 断言单调且终值前
  一帧 > 90%。

---

## 审过未立案（防重复劳动，附结论）

以下疑点经深查后**不成立或不达立案门槛**，记录备查：

1. `variogram.cpp` 隔断档预算收缩分支（`:135-142`）缩 cols/rows 不缩 cellSize——
   推演后该分支**不可达**（resolution 钳 [32,1024] → rows·cols ≤ (1024+2)² ≈ 1.05M
   < 4M 预算），死防御代码。
2. `neighborhood.h queryNearest` 环形扩张早退——(ring−1)·cellSize 下界论证成立，
   保守正确。
3. `sgs.cpp` 并行档 `threads.reserve(workers−1)`——`workersForPeakBudget` 保证 ≥1，
   无下溢。
4. `petrophyscomputeservice` GR 自动基线退化（hi==lo）检查在计算之后——dst 为
   Inf/NaN 但随即整体拒绝，无错值外泄。
5. `faciesclassificationservice` Hull/Box 继承 previous 无样本身份比对——
   `workflow/faciesclassify.cpp:30-37 setSamples→clear()` 保证换样本集即清分类，
   行数闸 + 流程闸闭合，不成立。
6. `jobrunner` prepare 失败路径不发 `jobCompleted`——start() 同步返回 nullptr，
   调用方有同步信号，契约自洽。
7. `lascache` 跨线程——头注明确并发安全 + InflightCoalescer，petrophys worker 调用合法。
8. `analyticSignal`/windowed 族只滤 NaN 不滤 Inf——Inf 输入导致诚实 NaN/Inf 不产生
   静默错值，且转码通道已 sanitize，暂不立案（可作为加固建议）。
9. `localidw` IDW `maxPoints` 锥形渐变（非硬 KNN）、方向引导开启时改用度量距 +
   各向同性 γ——均为有注释的设计口径，非缺陷。
10. `faciessliver`/`partition`/`velocitymodel`/`wellsiting` 的 int 维度乘法——均先有
    尺寸一致性闸或 qsizetype 比较，现实规模不可达溢出。
11. `petrophyscomputeservice::startBatch` 全井失败仍任务 Succeeded——代码注释明示
    「分井处置」口径，任务详情行逐井如实，属设计而非矛盾（如产品要改口径另议）。

---

## 覆盖清单

**逐行精读（实现+头契约对照）**：

- geostat：`kriging.{h,cpp}`、`linsolve.{h,cpp}`、`neighborhood.h`、`types.h`、
  `variogram.cpp`（+`variogram.h` 字段）、`sgs.{h,cpp}`、`sgs_internal.h`、
  `cokriging.cpp`、`crossvalidation.cpp`、`faultpath.{h,cpp}`
- singlefactor：`localidw.cpp`（全）、`krigingsurface.cpp`（全）、`partition.cpp`（全）、
  `curvekernel.cpp`（全）、`structural.cpp`（主要路径）、`buffertransition.cpp`（全）
- algorithms 其他：`petrophys.cpp`（全）、`seismicattr.cpp`（全）、`dsp/fft.cpp`（全）、
  `cluster/som.cpp`（全）、`velocitymodel.cpp`（全）、`mincurvature.cpp`（全）、
  `wellsiting/wellsiting.cpp`（全）、`faciessliver.cpp`（全）、`ensemblestats.cpp`（全）、
  `inversion/lowfreq.cpp`（全）、`inversion/volume.cpp`（全）、
  `stratgrid/sgsfill.cpp`（全）、`stratgrid/objectmodel.cpp`（主路径）、
  `faciesmapping/evidencesynthesis.cpp`（全）、`welldist.cpp`（全）、
  `distancetransform.cpp`（全）、`surfacevolumes.cpp`（全）、`curveexpr.cpp`（词法/骨架）
- services：`paleotaskservice.{h,cpp}`（全）、`jobrunner.{h,cpp}`（全）、
  `seismictaskservice.cpp`（全）、`seismictaskservice.h`（全）、
  `seismictaskservice_slice.cpp`（全）、`seismictaskservice_attr.cpp`（全）、
  `seismictaskservice_viz.cpp`（主路径）、`seismictaskservice_interp.cpp`（前 330 行
  + 追踪登记点）、`petrophyscomputeservice.cpp`（全）、`errorhub.cpp`（全）、
  `derivationgraph.cpp`（全）、`scriptrunner.cpp`（全）、`imagelod.cpp`（全）、
  `faciesclassificationservice.cpp`（全）

**接口级 / 定向检查（grep + 关键段精读）**：

- algorithms：`cluster/cluster.*`、`supervised.*`、`inversion/{bandlimit,sparse,wavelet}`
  （wavelet.h 公式对照）、`stratgrid/{stratgrid,propfill,upscale,faultoffset}`、
  `evolution/compare.*`（对照已知 C.7 边界）、`faultsurface.*`（对照已知 B-10）、
  `gridsolver.cpp`（对照已知 L3.6/L3.8）、`horizontrack.cpp`（对照已知 L3.7）、
  `faciesmapping/{dominantfacies,formationdominance,candidateboundaries}`（front() 守卫
  专项）、`faciespolygonize`、`frameworksuggester`、`rasteralgebra`、
  `constraintbarriers`、`paleoalgorithms`、`singlefactor/{fieldcontours,corridor,
  cartographicsmooth,wellacquisition,constraintparse,support,samples,types,
  localdirectionalgorithm,contourlevels,contouravoidance,regionalcontours,
  structuralalgorithm,geosutil,faultpath}`（多为已知项所在文件，按已知特征核对）
- services：`crossplotsamples/crossplotsources`（已知 A-15/C.11 核对）、`crashreport`
  （已知 C.12 外扫描）、`storagegovernance`、`pythonenv`（已知 L4.4 外）、`welllogset`
  （timeIndexToDepth 契约核对）、`cataloghealth`、`dataexport`、`paleomapfiles`、
  `datagraph/evidenceindex/previewdoc/projectdata/faciestraining/frameworkservice/
  seismichorizoncluster/seismicclusterlabels/singlefactordef/shortcutregistry/
  startuptrace/toolavailability`（风险模式扫描：空容器访问/递归/无界缓存/裸回调）

**跨层追踪（只为立案取证，不审 ui 层本身）**：
`seismicsectiondock_wells.cpp`（B-1 调用点）、`seismicsectioncanvas*`（B-2 消费链）、
`vendor/sbm` 的 `PlannedSection.cpp`、`SgySectionBuilder.cpp`、`Types.h`（B-2 单位源，
仅集成边界）。

**未覆盖 / 覆盖薄弱**：

- `seismictaskservice_transcode.cpp`（948 行）只做了结构级浏览（转码主链依托
  vendored TranscodeJob，已知 B.10 族之外未逐行）；
- `evidenceindex.cpp`（872 行）、`previewdoc.cpp`（1117 行）、`datagraph.cpp`（1060 行）
  为风险模式定向扫描而非逐行；
- `paleoalgorithms.cpp`（ConstraintIDW 主体）与 `singlefactor/support.cpp` 的参数解析
  长函数只读了与本案相关的段（两文件均在 excluded 清单，已知项密度高）；
- `pythonrepl/scriptprotocol/algoparamschema/jsonsidecar/fspathutils/futuretask` 仅
  头文件契约级过目。
