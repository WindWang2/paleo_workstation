# Full Audit 2026-10-22 — 06 Independent Review R2（Fix 1 复审）

- 复审人：独立复审 Agent R2（未参与任何修复编写）
- 对象：`git diff b301f2e8..80bc61ac`（8 提交：fc7e2b08 / d7424bde / 9b231b0d /
  0235c343 / b29301ae / 49bd3723 / 3af0e299 / 80bc61ac）
- 方法：源码只读静态复审（逐文件精读 + 调用链追踪 + 与基线版对照）；未构建、
  未运行测试/闸门（任务书约束）。所有行号以 80bc61ac 工作树为准。
- 结论速览：**MUST_FIX 1 / SHOULD_FIX 4 / DOCUMENT_ONLY 10 / FALSE_POSITIVE 0
  （另：原 B-06 维持证伪） / EXTERNAL_OWNED 0**。除 R2-M1 外可进 Fix 2，
  无需回滚任何整项。

---

## 1. 逐项复审结论（按 Fix 1 的簇）

### 簇 1（C-7 / B-02 / B-03 / C-9）fc7e2b08

#### C-7 predictionLog 米制归一（src/workflow/wellpredictionreview.cpp:35-57）

修对了根因：归一化从「仅 explicitAxis」扩到全部格式，与入库侧
normalizeDepthCurve（src/domain/welllogfacies.cpp:576-591，空单位→1.0）同口径；
`*scale != 1.0` 跳过乘法是安全微优化（词表只产 1.0/0.3048 精确值）。
重点挑战逐条核过：

- **DLIS 情形（detect=Dlis）**：parse → DlisParser::parse；curves.first() 即
  深度道是该仓全链约定（dlisparser.cpp:1415-1416 行对齐即以 first 为索引道；
  readCurveTvd 同样用 curves.at(0)，src/services/welllogset.cpp:334）。DLIS
  indexBasis 由 indexBasisFor 决定（BOREHOLE-DEPTH→MD / VERTICAL-DEPTH→TVD /
  空不冒充，dlisparser.cpp:965-986），TVD/空基准在修前修后都被拒——无回归。
- **空 curves 的 tr("未知") 分支**：DLIS/LIS 名单为空时 parse 直接失败（ok=false），
  文本无索引列也失败；仅 LAS 理论上可 ok+空曲线，此时 scale=nullopt → 拒收，
  「未知」可达且是防御正确路径。
- **行为放宽点（有意）**：非 LAS 空单位从「拒收」翻为「按米」（修前
  depthScaleToMetres("")=nullopt → 拒）。与入库侧一致（welllogfacies.cpp:586
  空→1.0），注释声称的约定经核实成立。见 D2/D3 备忘。
- 红绿锁真实：FT-LAS 断言 304.8/unit=="m" 与 DM 拒收在修前均红（修前 LAS
  分支原样直通、ok=true）。tst_mappingworkbench.cpp:620-655 夹具合法
  （~C 表 DEPT.FT 可被 lasparser 接受）。

#### B-02 mdAxisInMetres（src/services/welllogset.cpp:287-310）

新增 `legacy && !unit.isEmpty() && !scale` 拒收正确：DM 拒、FT 换算（×0.3048，
乘法链为 #610 既有）、空单位按米、非 legacy 分支一字未动（挑战 2 的
「spreadsheetml 空单位全拒」是 #610 既有行为，非本轮引入，见 D3）。
消费面 datapreviewtabwelllog.cpp:325-346 失败走 axisWarnings 通道如实上屏，
显示面影响可接受。遗留口径缝隙见 R2-S3。

#### B-03 petrophys MD 闸序（src/services/petrophyscomputeservice.cpp:548-557, 615-620）

- driver 路径：ok/空检查提到 additionalLogGridIsMd 之前，真因透传 ✓。
- 副文件路径：新增同序检查——顺带消灭了修前「损坏副文件静默产出全 NaN 列」
  的更深问题（修前 additionalLogGridIsMd 对非 Text/XML 一律放行，空 doc 走
  空 srcDepth → NaN 列），语义从静默 NaN 升级为诚实失败，方向正确。
- 红绿锁真实：修前该夹具落「请先校正索引元数据」文案（curves 空 →
  additionalLogGridIsMd 的 `!curves.isEmpty()` 恒假），测试的
  NOT-contains("校正索引元数据") 先红。
- 小瑕疵：副文件是 CSV 时错误文案沿用「LAS 无曲线」（D8）。

#### C-9 拒收路径测试 —— **欠交付，见 R2-S1**

### 簇 2（A-02 / A-03 / A-06）d7424bde

- **A-02 GB18030**：readText/sniffText 换 EncodingDetect::decodeText 与井表生态
  同口径 ✓。测试字节序列核对无误（某=C4B3、井=BEAE，GBK/GB18030 兼容）；
  红绿真实（修前严格 UTF-8 拒收）。
- **Latin1 兜底接受面（挑战 3）**：decodeText 永不失败（Latin1 保字节），编码层
  拒绝消失——结构闸兜底成立：detect→sniffText 需首行含索引名列且 ≥2 列，
  readTable→toDouble 逐格严格。二进制垃圾无法伪装成曲线；Latin-1 西文井名
  曲线从拒收变可读是接受面扩大（与井表一致），见 D9。
- **A-03 词表三面一致**：分类器正则 `DEPTH|DEPT|MD|TVD|TVDSS|TIME|深度|井深`
  （projectclassifier.cpp:172-177）≡ 读取器 isIndex（welllogtext.cpp:24-27）≡
  报错文案（welllogtext_convert.cpp:108）✓；分隔符 tab→逗号→分号 与分类器
  [,;\t] 对齐 ✓。红绿真实（修前 井深 文件报「缺少明确索引列」、分号文件单列）。
- **挑战 5（混合分隔符）**：表头含分号、数据逗号 → 表头取 ';'、数据行单格 →
  「数据列数与表头不一致」诚实失败（无静默错切）；表头同时含逗号+分号时逗号
  胜出、残余分号并入表头 token——单分隔符解析器的固有边界，与分类器同判。
  见 D7。
- **A-06 逐 token 早退**：与旧 `\s+` split 的分词语义等价（trimmed+isSpace），
  256 列预算先检后 append vs 分隔符分支 append 后检——失败点同在 257th token，
  行为等价；O(n) 无正则。红绿真实（修前超宽表头整行物化 301 列、错误落第 2 行
  「列数不一致」，不含「列数超过上限」）。

### 簇 3（B-01）0235c343 + fc7e2b08 的 paleotaskservice/jobrunner 改动

- **JobRunner 优先级参数**：默认值保持 High=原硬编码语义，全部既有调用方
  （constraint/propertymodel/compose/polygonize/depth/realization/snapshot 等）
  行为零变更；仅 BatchQueue 显式传 Low ✓ 源级兼容（无调用方曾以第 5 参传
  非布尔）。
- **PaleoTaskService 3/4 参 start 提级 0→1**：把实现拉回头注已声明的契约
  （paleotaskservice.h:153「同签名 3 参版默认 Normal」）。受影响调用方盘点
  （startBounded/seismictaskservice.cpp:265、mapbookqueue.cpp:227、
  horizonbatchexport.cpp:434、各 export/faciesclassify/validation/previewdoc
  request* 等）：彼此相对序不变（同 FIFO 档），相对显式 Low（previewdoc.cpp:1048
  预取）从并列变让位、相对 High（JobRunner 族）不变——即 D4.6 的本意。
  交互任务不再与预取同档，是 B-01 诉求的正向收益。Low 可能被持续 Normal 流
  饿死（QThreadPool 无优先级老化）——预取语义可接受，见 D5。
- **BatchQueue 与 pump 让位闸交互（挑战 4）**：闸算式
  `m_tasks->runningCount() − BatchQueue::runningCount() ≥ poolCap` 中 running
  是队列自身的 runningCount（batchjobqueue.cpp:782/805），差值=纯非批任务数，
  「非本批占满池才停出队」语义成立；批任务自占并发额度不触发让位 ✓。Low 排队
  期间新开批被 busy 拒、会话切换走 requestCancel→onDropped→pump 全链既有覆盖。
- **测试确定性论证（挑战 4/R2-E）**：setMaxWorkerThreads(1) 后单线程池，
  H1 占线程、H2 池内排队（Low）、interactive 后到（Normal）——QThreadPool
  文档化优先级出队（同优先 FIFO、高优先先出），完成顺序确定性成立，无墙钟。
  红绿真实（修前 H2 为 High>Normal，interactive 必后跑）。两个备忘：300ms
  门超时窗内主线程被强占 >300ms 的理论 flake（D6）；测试注释已过时（D1）。

### 簇 4（B-05≡D-03 / B-06）b29301ae

- **四处回调发射语义**：各路径恰好一次——成功+key 未变：ready(结果)（唯一
  缓存写点）；失败/取消/key 变更：ready(空载荷) 恰一次（finished 单发射，
  applyFinish 幂等闸保证）；receiver 销毁：连接断开零回调（destroyed→
  requestCancel 的任务随后终态但无 listener）✓。取消时 receiver 活着：ready
  (default) 落地，消费方守卫核过——
  - previewtocpanel.cpp:499-517：currentLayer()!=selected 先行拒绝跨图层竞态
    （切层会另行发起新请求）；m_suppressSignals 保存/恢复对称，失败路径不破坏。
  - tabhorizon/tabmapproduct：!statsRaster 早退在 validity 判定内正确分层，
    失败落「不可用」五标签；receiver=page 的连接上下文防 UAF。
  - applyRasterStyle 直方图失败保留现渲染不套 0..1 假拉伸 ✓。
- **无重复缓存写（R2-D）**：else 分支不 insert cache，失败结果不缓存、重选
  重试 ✓。
- **遗留**：requestProfile 失败回调使消费方 addProfile(空序列)——图例多出
  空「剖面 N」条目（见 R2-S2）；requestHistogram/requestProfile 失败路径无
  测试（同 S2）。
- **B-06 维持证伪**：neighbourhood 极性复核（nv≥v 置 isMax=false、nv≤v 置
  isMin=false，previewrasteranalysis.cpp:395-400）——平坦面内域格双标志皆假，
  根本进不了候选，与既有测试 robustExtremaScaleOnSyntheticGrids 第 1) 例
  （平坦→空集，tst_previewmap_assets.cpp:1166-1171，非本轮新增）互证。
  Fix 1 只补注释、不动代码，处置正确。原 B-06 判 FALSE_POSITIVE（登记册口径）。
- **测试**：rasterRequestFailureStillCallsReady 红绿真实（修前 30s 空转后
  QCOMPARE 失败）；ctx 作 receiver 活到期末、每回调恰一次断言在位。

### 簇 5（A-01 / A-08）9b231b0d

- **A-01**：matches==1 且原名精确（CaseInsensitive）仍自动选中；==1 规范化
  命中给新文案；≥2 维持「多口井」✓ 根因修复。红绿真实
  （!contains("多口井") 修前红）。tr() 新串未入 paleo_zh_CN.ts——源文即中文、
  l10n 闸只查 ts 内未完成项，非缺陷（过程备注）。
- **A-08**：lateRows 暂存缀尾，well_head/执行行在前、迟到 Skipped 殿后，契约
  （dataimportservice.h:90-93）落地；plan.items=confirmed 门槛从 lateCount>0
  改 !lateRows.isEmpty() 等价；无其他行为变化。红绿真实（指针序断言修前红）。

### 簇 6（D-01 / E-01 / E-02 / E-03）49bd3723 + 80bc61ac

- **D-01 身份路由**：
  - syncSessionToTracks switch 语义与旧标题判据逐支对拍等价（Sequence 无条件
    覆盖、SandGroup 不同步、Layers 带 formationIntervals 非空守卫）✓。
  - defaultlayout 三处 setSource、duplicateTrack 保源、道配置对话框走同对象
    setTitle 不换源——改名/复制/配置三条改名路径全保身份 ✓。
  - **持久化路径（挑战 6）**：所有 FormationTrack 的 TrackSpec 化都经
    TrackRegistry::captureSpec（paneltracks.cpp:310/410/417），capture 侧
    formationSource 序列化覆盖全部保存面 ✓；旧会话文件无该参数 → createTrack
    缺省 Layers + defaultlayout 显式设源，兼容 ✓。**但**恢复侧
    applySpecsIncrementally 只按「类型+标题」匹配、formation spec 从不走
    createTrack——序列化出的 formationSource 目前在会话恢复路径无人消费；
    改名道（「层序 副本」）恢复时标题不匹配被丢弃，回默认布局（既有标题键
    限制，D-02 族，非本轮引入）。见 D4。
  - 红绿真实：改名层序道在修前被 formationIntervals 覆盖（topDepth 1000≠1020）。
- **E-01**：PATCHES.md P14 与 vendor/sbm/src/Data/Sgy/SgySectionBuilder.h:30-33
  实体对账一致（字段、never-synthesized 契约、消费面、编译失败信号）✓。
- **E-02**：AGENTS.md/cmake 注释现值 172/137 与两基线实测条目数（137/172）
  相符；但 qgis-spread-baseline.txt:22 仍有显式「条目数：149」旧值未随改
  （现值只能靠头注 +23 推算）——E-02 自己治理的漂移形态残留一处，见 R2-S4。
- **E-03**：缩进复位正确（CMake 忽略缩进、无功能影响），块归属恢复可读 ✓。

### 簇 7（C-8）3af0e299

- parse(format) 重载语义与旧 4 参版逐路径等价（Unknown → parseAdditional
  「无法识别」，与旧版内嵌 detect 结果一致）；旧签名委托 detect 保持兼容；
  无重载歧义（WellLogFormat 与 LasHeaderRef 不互转）。
- **嗅探盘点（R2-D）**：六点 2→1（wellpredictionservice 更是从每 ref 一次
  外层 detect 收敛为每 path 一次缓存）；predictionLog 由 2 次（外层+内层）收
  敛为 1 次（内层）。声明成立。
- **六调用点等价性逐一核过**：propertymodelworkflow/sectionworkbench/
  wellsectionworkflow/wellpredictionservice 全等价；**mappingworkbench_predict
  不等价——见 R2-M1**。
- 分层：新 include 仅 io/welllogtext.cpp → io/encodingdetect.h（同层）✓；
  测试壳 include 豁免 ✓。无新文件，规模闸面增量微小（未复跑闸门，任务书禁跑）。

---

## 2. 意见清单

### MUST_FIX（1）

#### R2-M1 mappingworkbench_predict 空路径 explicitAxis 翻转——无测井井整批预测被误杀（C-8 重构回归）

- 证据：src/workflow/mappingworkbench_predict.cpp:516-518
  ```cpp
  const auto format = path.isEmpty() ? WellLogFormat::Unknown : WellLogRead::detect(path);
  const bool explicitAxis = format != WellLogFormat::Las;
  ```
  基线版为 `!path.isEmpty() && detect(path) != Las`（空路径 explicitAxis=false）。
  空路径可达：inputs()（src/workflow/mappingworkbench.cpp:525-543）对有坐标无
  well_log 链接的井发 `log_version_id=""`，predict 侧 `logPaths << QString()`
  （mappingworkbench_predict.cpp:186-189）。
- 后果：修前该井走设计好的 depth_mock 回退（[0,120]，well.insert("depth_mock",
  !measured)，:491-497）；修后 explicitAxis=true 且 parsed=false → 进
  preparationError 分支（:521-524），**整批预测以空文件名的「预测需要可读取且
  声明米或英尺单位的 MD 测深」中止**。3 参版（C-8）是纯性能重构，不应携带
  行为翻转。
- 修法：恢复守卫——`const bool explicitAxis = !path.isEmpty() && format !=
  WellLogFormat::Las;`（一行）。建议随附回归测试：无 log_version_id 的井进入
  wells 预测走 mock 深度、不产 preparationError。

### SHOULD_FIX（4）

#### R2-S1 C-9 欠交付：五条拒收路径只覆盖一条

登记册/计划把 C-9（五处 workflow MD/单位硬拒绝零测试）排进 Fix1 簇1。交付的
测试锁住 wellpredictionreview（C-7 用例）与 welllogset/petrophysys（B-02/B-03
面）；wellsectionworkflow.cpp:383-387、mappingworkbench_predict.cpp:521-525、
propertymodelworkflow.cpp:1166-1181、sectionworkbench.cpp:417-421 四处仍零
覆盖。修法：Fix 2 补四例（fixture 可复用 io 层样例），或在登记册明确改判结转
——二选一，勿悬空。

#### R2-S2 requestProfile 失败回调产生空剖面图例项；Histogram/Profile 失败路径无测试

b29301ae 给 requestProfile 加 `ready({})`，消费方（datapreviewtabhorizon.cpp:
526-536、datapreviewtabmapproduct.cpp:253-263）无条件 addProfile——失败/取消
时图例多一条无样本的「剖面 N」（旧版静默、新版噪音且误导「有剖面」）。修法：
消费方对空样本跳过或以「剖面不可用」呈现；随 B-05 测试补 requestHistogram/
requestProfile 失败分支（现只锁 Summary/Extrema）。

#### R2-S3 mdAxisInMetres 与 readCurveTvd 的 legacy 接受面仍分裂（B-02 残留）

mdAxisInMetres 现走 wellnumeric 词表（M/METER/METERS/METRE/METRES、FT/F/FOOT/
FEET），readCurveTvd legacy 分支（welllogset.cpp:348-361）仍只认 FT/F/M/空——
单位 METERS/FOOT/FEET 的同一 LAS 在综合井图可用、TVD 读取仍报「深度单位未知，
不猜折算」。B-02「同口径」对拒收（DM）成立、对接受集不成立，QC 分裂只收窄未
消除。修法：readCurveTvd legacy 链改用 depthScaleToMetres（C 观察6 三体收敛的
最小切片）。

#### R2-S4 qgis-spread 基线头注「条目数：149」未随 E-02 对齐

tools/qgis-spread-baseline.txt:22 显式写「条目数：149（ui 100）（workflow 49）」
（E-2 扩面后实值 172，仅可由头注 +23 推算）。E-02 治的就是这类显式计数漂移，
且 AGENTS.md 新文案指路「见两基线头注」——qgs-audit 侧 137 可推算无显式旧值
残留，spread 侧这条应改为「初冻 149；E-2 扩面后 172」。

### DOCUMENT_ONLY（10）

| # | 事项 | 证据/说明 |
|---|------|----------|
| D1 | 优先级用例注释过时 | tst_batchjobqueue.cpp:865「start 的 3/4 参重载走池默认优先级 0（=Low）」——fc7e2b08 后 3/4 参已显式 Normal，注释与实现相悖（测试本身仍有效且等价于 3 参调用）。 |
| D2 | predictionLog 非 LAS 空单位从拒收翻为按米（有意） | wellpredictionreview.cpp:43-45；与入库侧 welllogfacies.cpp:586 空→1.0 对齐经核实。行为放宽、注释已声明。 |
| D3 | 空单位双口径并存：predictionLog 收（按米）vs mdAxisInMetres 非 legacy 拒 | 文本曲线「MD」列无 [unit] 时验证页可看、综合井图拒（带 warning）。两者各自诚实；属 C 观察6 深度函数三体并存族，收敛批次处理。 |
| D4 | FormationSource 持久化参数在恢复路径暂无消费者 | applySpecsIncrementally（paneltracks.cpp:318-347）仅按类型+标题匹配、formation spec 不走 createTrack；改名源道恢复即回默认布局（D-02 族既有标题键限制）。参数对 add-track/后续身份化恢复是正向储备。另一行为注记：用户手工新建标题恰为「层序」的 formation 道现绑 Layers（修前按标题误绑 Sequence）——身份语义更正确。 |
| D5 | Low 优先级无老化，持续 Normal 流可饿死预取 | QThreadPool 无 priority aging；预取/缓存预热语义可接受，注意不要把用户可见长任务放 Low。 |
| D6 | 优先级用例的理论 flake 窗 | waitOnGate 300ms 超时：主线程在 m_queue->start() 与 gateOpen.store(1) 之间被强占 >300ms 时 H2 先跑、断言误红。窗口为相邻数条语句、实际概率极低；如遇偶红优先查此处（可加大该用例门超时或改 per-item 门参数）。 |
| D7 | 混合分隔符文件诚实失败 | 表头含分号+数据逗号 → 「数据列数与表头不一致」；表头逗号+分号并存时逗号胜出、残余分号入表头 token。单分隔符解析器固有边界，与分类器同判，不立项。 |
| D8 | petrophys 副文件 CSV 解析空时报「LAS 无曲线」 | petrophyscomputeservice.cpp:618-620 沿用 driver 路径措辞；文案面小疵。 |
| D9 | Latin1 兜底使编码层拒绝消失，结构闸兜底已核 | sniffText 需索引名列+≥2 列，readTable toDouble 逐格严格；二进制垃圾无法成曲线。接受面扩大（Latin-1 西文井名）与井表生态一致。 |
| D10 | B-06 维持证伪、Fix 1 注释准确 | neighbourhood 极性（nv≥v 清 isMax）使平坦面内域格全出局；既有测试 tst_previewmap_assets.cpp:1166-1171 钉住平坦→空集。原 B-06 记 FALSE_POSITIVE。 |

### FALSE_POSITIVE / EXTERNAL_OWNED

- FALSE_POSITIVE：无（对 Fix 1 改动本身）；原审计 B-06 经独立复核维持证伪（D10）。
- EXTERNAL_OWNED：无。

---

## 3. R2-E 红绿判定汇总（新增测试逐个）

| 测试 | 修前红？ | 依据 |
|------|---------|------|
| tst_mappingworkbench predictionLog FT/DM（C-7） | 红 | 修前 LAS 直通：values≠304.8、DM ok=true |
| tst_welllogset mdAxisRejectsUnknownLegacyUnitAndScalesFeet（B-02） | 红（DM 例） | 修前 legacy 未知单位返回 true；FT 例修前已绿（#610 已乘） |
| tst_petrophysbatch testMergeReportsDriverParseFailureAsParseError（B-03） | 红 | 修前文案含「校正索引元数据」→ NOT-contains 断言失败 |
| tst_batchjobqueue batchTasksYieldPoolToInteractivePriority（B-01） | 红 | 修前批任务 High>Normal 必先出队；确定性见 §1 簇 3 |
| tst_previewmap_assets rasterRequestFailureStillCallsReady（B-05） | 红（慢红） | 修前失败零回调，30s 空转后 QCOMPARE 失败 |
| tst_wellcomposite_framework renamedSequenceTrackKeepsSequenceData（D-01） | 红（编译级+行为级） | source() API 新增；行为级：修前改名层序道被 1000/1050 分层覆盖 |
| tst_welllogtext gb18030/jingshen+semicolon/wide（A-02/A-03/A-06） | 红 | 修前分别：编码拒、缺索引列/单列、错误文案不含「列数超过上限」 |
| tst_import_ux 两处断言（A-01/A-08） | 红 | NOT-contains("多口井")；行序指针断言 |

分层：新增 include 全部合规（io→io；测试壳豁免）；无新 src 文件；基线文件
零改动（闸门未复跑——任务书禁构建/测试，靠 diff 面判断增量微小，建议 Fix 2
合入时全闸照跑）。

---

## 4. 总体结论

Fix 1 的七簇修复在根因定位、最小改动、红绿测试三条纪律上整体过硬：C-7/B-02/
B-03/A-01/A-02/A-03/A-06/A-08/D-01/E-0* 全部确认修对，B-05 的回调语义与
B-01 的优先级链经并发/生命周期推演无新缺陷，B-06 证伪处置正确。唯一必须回修
的是 **R2-M1（mappingworkbench_predict 空路径 explicitAxis 翻转，一行）**——
它是 C-8 性能重构顺手夹带的行为回归，破坏了「无测井井走 mock 深度」的设计
路径。

**裁定：可进 Fix 2，前置条件 = 先收 R2-M1（含回归测试）；SHOULD_FIX 4 项
（C-9 补测/结转二选一、profile 失败消费面、readCurveTvd 词表收敛、spread
基线头注一行）随 Fix 2 捎带；无需回滚 Fix 1 的任何整项。**

— R2，2026-10-22。
