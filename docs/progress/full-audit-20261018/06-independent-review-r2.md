# Independent Review R2（full-audit-20261018 · Fix 1 复审）

- 复审员：Independent Reviewer 2（未参与任何修复编写，挑战姿态）
- 复审对象：`git diff 366aef97...HEAD`（55 文件，+3537/-58），11 个修复 commit
- 基线：BASE_SHA=366aef97；Finding 登记：04-finding-register.md（FC-001…FE-005）
- 方法：逐 commit 读 diff + 逐跳读修复落点周边源码（含未改动消费方）；对 FB-001 与
  GeologicalSmoothing 做逐情形语义对照；对 FB-002 做 per-worker 工作集逐数组盘点；
  对 FE-003 的钉版 sha256 做上游实下载独立核验；运行 `tools/check_layering.py --strict`
  （纯静态检查，非编译）。
- 限制：按铁律未编译、未跑 ctest（主 Agent 持编译锁）——「测试通过」结论均以代码
  逻辑推演 + commit 自述红绿记录为准，本报告单独标注无法验证项。

## 结论速览

| 分类 | 数量 | 编号 |
|---|---|---|
| MUST_FIX | 1 | R2-MF-01 |
| SHOULD_FIX | 5 | R2-SF-01…R2-SF-05 |
| DOCUMENT_ONLY | 12 | 见各条 |
| FALSE_POSITIVE | 1 | 复审中自查撤销（见 FC-003 附注） |
| EXTERNAL_OWNED | 0 | |

Fix 1 整体质量：**高**。17 项簇中 16 项修复治根因、口径与仓内既有范式一致；测试以
真实行为断言为主（worker 线程全链、截断注入、种子扫描），无发现新引入的功能性缺陷。
唯一 MUST_FIX 是 FA-001 在 xlsx 路径上的预算作用域与自述语义不符（按表重置而非按簿），
修复成本一行。

---

## FC-001 预测 INPUT 冻结 URI 快照（aca2aa61，P1）

**结论：修复正确，范式与仓内既有 runFactorAlgorithm 一致。**

逐点核查（对应主 Agent 自查面）：

1. `publishPredictionJob` 不再读 `params["INPUT"]`——通读 predictionworkflow.cpp:430-502，
   publish 只消费 job 字段（staging/parentVersionIds/layerId 等），无 INPUT 引用。✓
2. ONNX 分支不受影响——prepare 在 predictionworkflow.cpp:120-209 对 `onnx:` 前缀
   提前 return，INPUT 冻结段（:264-275）在其后，ONNX 路径本来就不产 INPUT。✓
3. 带 provider 参数的 uri 回放——`input->source()` 原样存快照，compute 以
   `QgsRasterLayer(uri, name, provider)` 整串回放（:398-404）。pipe 后缀选项是
   provider 自身解析的，与 constraintfactorjobs_factor.cpp:1200-1210 的既有冻结
   范式完全同款。✓
4. CRS 往返——`crs().toWkt()`（WKT2_Preferred）→ `fromWkt`，与 runFactorAlgorithm
   的 `layer->crs().toWkt()` 冻结同口径（constraintfactorjobs_factor.cpp:1194）。✓
5. 测试真实：tst_workflows.cpp:355-410 在真 worker 线程跑 compute 并断言全链成功
   + `threadViolationCount` 零新增；红绿路径可信。✓

DOCUMENT_ONLY（3 条，均不阻断）：

- **FC-001-D1**：冻结只覆盖 `"INPUT"` 单键；runFactorAlgorithm 对 params 里**所有**
  layer 值参数统一冻结（constraintfactorjobs_factor.cpp:1188-1200）。当前预测页只收
  标量，安全；未来若给预测加 MASK/参考图层参数，会静默重蹈跨线程活层违约。建议
  下次触碰该文件时改成全键冻结或加注释钉死「仅 INPUT 可携带层」。
- **FC-001-D2**：非 raster/vector 的 QgsMapLayer（mesh/pointcloud）会落进
  QgsVectorLayer 分支 → isValid() false → 「无法读取预测输入图层」。旧行为是把层
  透传给算法由其报错。实际不可达（自动解析只挑 raster 声明，调用方只传
  raster/vector），记录即可。
- **FC-001-D3**：GUI 层 CRS 无效（toWkt()==""）时 worker 层回落到文件内嵌 CRS，
  与旧行为（算法消费无效 CRS）有语义差。病态边界，且新行为更合理，记录。

## FB-001 转面多数滤波并列裁决（84622320，P2）

**结论：修复与 GeologicalSmoothing 逐情形语义一致（已做完整 case 分析）。**

对照 paleoalgorithms.cpp:1134-1144（先取 count 降序/value 升序最优，center 计数
并列则 center 胜）与 faciespolygonize.cpp:168-181 新实现：对「center 在/不在计票
表」「挑战者先后访问序」「三并列」「挑战者反超后 center 条目重访」等全部情形推演，
两实现产出恒等（新实现里 center 条目重访只会把 best 拉回 center，恰与「center
并列胜」殊途同归）。smoothMajority 的 center 必在 counts 内（nodata 早退 +
邻域含自身），无 0 票边界。`qSetGlobalQHashSeed` 在本机 Qt6 头文件存在且未废弃。

**R2-SF-01（SHOULD_FIX）——测试未钉并列裁决方向**（tests/tst_algorithms.cpp:888）：
`QVERIFY(referenceCodes.contains(3))` 近乎恒真——输入本身含 3，任何确定化方向都过。
若未来把「挑战者取小码」改成「取大码」，本测试仍绿。手工推演该 3×3 夹具 SMOOTH=1
后栅格为 `3 3 3 / 3 3 7 / 7 7 7`（3 面 5 格、7 面 4 格）；若裁决取大码则中心归 7
（3 面 4 格、7 面 5 格）——面积集不同。建议把断言换成 golden 面积集（如
`QCOMPARE(referenceCodes, (QVector<int>{3,7}))` + 面积 `{5*cellArea, 4*cellArea}`），
方向与确定性一次钉死。跨种子逐位一致的主断言本身有效。

## FA-001 工作簿列填充总量闸（fb6f7b7c，P2）

**R2-MF-01（MUST_FIX）——xlsx 路径预算按「表」重置，与自述「整簿拒收」语义不符**
（src/io/outsourceworkbook.cpp:500，声明位于 :481 的 `for (sheetTargets)` 循环体内）：

- readSpreadsheetMl 的 `cellBudget` 在函数作用域（:102，多 ss:Worksheet 共用一本账）；
  wellcompositexml 两个读口（:93、:934）也是全文件一本账；唯独最常用格式 xlsx 的
  预算每个 sheet 重开 4M。commit 自述与登记表均称「超限**整簿**拒收」。
- 放大向量仍在：xlsx 单格 `r="XFD204"`（~35B）经 padTo(:652) 换一行 16384 个
  QString 槽位；每 sheet 244 行即 4M 格（~32MB 驻留），一个 <1MB 的 zip 放 100 张
  sheet 就能堆 ~3.2GB——正是原 Finding「行数×列数总量无预算」在同一路径的残留
  （zip 层只限包体/解压总量，不限格数）。
- 附带格式不对称：同一份 6M 格的合法簿，存 .xml 会被（按簿口径）拒收，存 .xlsx
  放行。
- 修法：把 :500 的声明上移到 sheetTargets 循环之前（一行），错误文案改回
  「工作簿累计…」口径；多 sheet 合法大簿若需放行，应显式调大常量而不是按表分账。

其余核查（通过）：表头行**已**计账——两条 finishRow 均对 headers 分支减
`sheet.headers.size()`、数据行减 `row.size()`（存什么算什么，口径自洽）；预算检查
在 `while (!atEnd())` 循环顶，末行触负后下一次迭代即捕获（EndDocument 前必经一次
判负）；rowHasError 行被丢弃不计（驻留为零，正确）；4M 格 × ~8B 槽位的量级与
512MB 文件闸相称。

## FA-002 .xml 直读大小闸（fb6f7b7c，P3）

**结论：正确且闭环。** 三处 readAll 前置闸（outsourceworkbook.cpp:752-757、
wellcompositexml.cpp:69-75 与 919-925）齐备；「xlsx 由 zip 层把关」的说法经核验
成立——ziparchive.cpp 有包体 512MB（:31,:135）、**中央目录声明的解压总量** 512MB
（:293-303，防多 entry 叠加）与单条目 512MB（:388）三重闸。测试用稀疏 resize
不实写 512MB，闸在 readAll 之前生效，夹具合理。

DOCUMENT_ONLY：错误文案匹配「512MB」/「预算」属同仓消息自匹配，改文案需同步测试，
可接受。

## FB-002 SGS 并行工作集预算（fe6ef110，P2）

**结论：估算经逐数组盘点核实，无漏项。**

- 2D（sgs.cpp:207-215）：path `vector<size_t>` 8B/格 + gaussianField
  `vector<double>` 8B/格 + SimulatedLattice::m_values `vector<double>` 8B/格
  = 3·cells·8B ✓。KrigingWorkspace（a 为 n² double，maxPoints 钳 ≤64 → ≤32KB）、
  各邻域 vector ≤64 元素——相对 cells 可忽略，不计合理。
- 3D（sgs3.cpp:508-514 + SimulatedLattice3:215-243）：path/gaussianField/m_values
  24B/目标 + unordered_map 查找表 ~56B/项。libstdc++ 上
  `unordered_map<uint64,uint32>` 节点实占 ≈32B（next 8 + pair 16→24 usable、
  glibc chunk 32）+ 桶数组 ~8B ≈ 40B/项，56B 为保守上估 ✓；m_lookup 跨实现复用
  （「只清值不重建索引」）意味着每 worker 常驻一份，与「每 worker 一份」记账吻合。
- outputBytes = R×cells×8 与 `result.realizations.assign(R, vector<double>(cells))`
  （sgs.cpp:190、sgs3.cpp:490）严格一致。
- workersForPeakBudget 语义：预算只收并行档 worker 数（资源面），串行底 1 路允许
  超 6GiB——已在 sgs_internal.h:33-38 注释如实声明为策略（串行=并行化前行为），
  非隐瞒。{4,1e8}→1、{4,5.2e7}→3 等直测值经手算复核无误。

DOCUMENT_ONLY：预算函数是纯函数直测，sgs/sgs3 内的接线（实参传法）无专测——但
parallelRealizationsMatchSequential 保证收缩不改成果，接线肉眼三行，风险低。

## FB-003 fillSgs 聚合输出闸（fe6ef110，P3）

**结论：正确。** 闸位于 `out->assign(R)`/`values.assign(cellCount)`（sgsfill.cpp:157-165）
之前（:123-139），先拒后分配；`static_cast<int64>(R) * cellCount64 *
static_cast<int64>(sizeof(float))` 全 int64，`ni` 先转型再连乘，无 32 位溢出面；
闸值与实际驻留 `R × grid.cellCount() × 4B` 严格相等（cellCount()==ni·nj·nk）。
测试夹具 1024×1024×2047≈2.147e9 < INT_MAX（#478 体素闸内），ZoneGrid 只持逐列
数组（topZ/botZ/live ≈ 8MB），测试自身不重。kMaxAxis=1<<20 (:34) 保证轴界。

## FD-001 演化动览帧表重灌（350761fe，P2）

**结论：主链正确**——reloadFrames + onProjectOpened 与 chip 条同点位
（paleomainwindow.cpp:492-495）；测试经 AreaRules::setProjectDir 换装第二套词表后
断言 frameOrder 跟随、步进只在新集合内，路径真实。

**R2-SF-02（SHOULD_FIX）——集合外层位重灌后帧位/标签悬空**
（evolutionplayerpanel.cpp:106-113 + applyFrame :115-125）：activeHorizon 不在新
词表时 applyFrame 早退，`m_index` 原样保留——若旧词表较长（如 5 帧、m_index=4）
新词表较短（2 帧），m_index=4≥size：帧标签仍显示旧词表「H5 · 5/5」（撒谎），
且 stepBy ±1 双向越界全锁死，只能等外部 chip 切换救回。`else if (m_index >=
m_frames.size()) m_index = -1` 的复位只覆盖无 selection 分支，生产路径
（selection 恒在）够不着。建议 applyFrame 早退分支在 reloadFrames 场景把
m_index 钳到 [-1, size-1]（或重灌时统一 m_index=-1、标签清空）。

DOCUMENT_ONLY：reload 只挂 onProjectOpened 与 chip 条完全对称（仓内本就无
onProjectClosed 重灌面），无反向不对称。

## FA-003 并行扫描读错误回落（a70ddfea，P3）

**结论：正确，两条主 Agent 自查点均实证通过。**

- 残留清理：scanParallel 合并循环（segyreader.cpp:1094-1127）在 io-error 返回前确有
  分片数据 append 进 m_index/m_badTraceOffsets；openCached 的回落分支
  （:1436-1438）`resetState()` + `file.seek(0)` 后走顺序 `open()`，resetState
  （:895-911）清 m_index/m_byInline/m_byXline/m_badTraceOffsets 及全部标量——
  残留确实清干净，随后 open() 自身 :179 再 reset 一次，双保险。✓
- 测试竞态：opts 无 cancel 回调 → shard 内 `cancelled` 原子永不为真，首个失败分片
  必为 io-error；progress 在每 shard 首道即触发（(i-from)%128==0 于 i=from 成立），
  截断确定落在扫描窗内；`truncated` CAS 保证单次 resize；QFile::resize 对他线程
  只读句柄在 Linux 下安全。测试确定性成立。✓
- 生产侧混合场景（用户取消 + 某分片 io-error 并存）：按**文件序首个失败分片**
  分类——取消在先则 checkpoint 前缀正确，io 在先则全量顺序重扫（顺序路径自身
  继续尊重 cancel）。无误分类面。

## FA-004 links.ord 巨值闸（8e03ed9e，P3）

**结论：正确。** 正常写的 ord 恒连续有机制保障：链接删行触发整表重写
（datacatalog.cpp:647-657 注释 + catalogstore_crud.cpp:207-214 replaceAllLinks），
+4096 松量对合法库安全；闸后 resize 上界 ≤ size+4097，int 无溢出（ord=INT_MAX
必被闸拦）。坏行跳过+保留在库与 unsafeVersion 先例同口径。测试先正常建链再 SQL
直插 ord=2e9，断言开库成功且好行完好——有效。

## FC-002 同步 save retain 失败语义（d0a23078，P3）

**结论：正确。** 全仓检索：MapVersionController::saveVersion 的消费方仅测试
（tst_versions/tst_factorworkflow/tst_mapsnapshot_scale），生产走异步任务路径；
「成功返回值+非空 error」的自相矛盾态消除后无真实调用方受损。retain 失败改
qWarning（mapversioncontroller.cpp:336-346）与异步路径 #480（snapshotFinished
捎带 retainError）口径方向一致。

DOCUMENT_ONLY：同步路径 retain 失败现在对上层完全不可见（仅日志）——同步路径
无消费方，属可接受契约；若未来有人重新启用同步入口，需补一个披露通道。

## FC-003 legacy 发布 stale 守卫（d0a23078，P3）

**结论：守卫本体正确；有两处收尾不对称。**

主 Agent 自查点核验：

- **误伤并发 save 的 watches：不会。** startPublish 入口（:406）与 startSave（:169）
  同受 `snapshotBusy()`（preparation || runner.busy()，:154）闸——startPublish 里
  `disconnectSourceWatches()`（:426）执行时必无在飞快照任务，不存在拆掉别人
  watches 的窗口。✓
- **生命周期：安全。** 守卫 connect 以 `this`（控制器）为 receiver，控制器析构
  自动断连；lambda 只捕 `stale` shared_ptr（不捕 this），连接存续期间标记写入的
  是 job 自有标志，job 由 runner 持有。layer 先亡则 destroyed 信号既标记又自清
  连接。✓（此项最初疑「控制器亡后 lambda 悬空」，复查为 FALSE_POSITIVE：接收者
  this 亡则连接先断，lambda 无机会执行。）
- legacy 判定与 layer_id 取值（mapversionstore.cpp:1030）对得上；声明未实例化的
  legacy 层查不到实例会静默不挂守卫——该形态下 in-app 无编辑面，可接受。
- 测试用 worker-gate（QSemaphore 持住唯一线程）注入确定性编辑窗，断言发布失败、
  文案点名、isPublished false——真实有效。

**R2-SF-03（SHOULD_FIX）——发布完成/启动失败不摘守卫**（mapversioncontroller_async.cpp:480-491）：
save 路径的 jobCompleted（:382）与 `!task` 分支（:392）都调
`m_async->disconnectSourceWatches()`；publish 路径的 jobCompleted（:483-488）与
`!task` 分支（:491）均**不调**。后果：发布结束后十余条 legacy 层信号连接存活到
下一次快照启动，持续向已完结 job 的 stale 标志空写（无功能危害，但连接与
shared_ptr 悬挂一代）。补两行对齐即可。

DOCUMENT_ONLY：`editingStopped` 对「开-停但零修改」的空会话也置 stale → 偶发
假阳性失败需重试；与 save 路径同款保守口径，可接受。

## FD-002 量测 deactivate 清理（c56a70e2，P3）

**结论：正确。** deactivate 隐藏 band、保留 m_points、调基类
（previewmaptools.cpp:123-129），与 identify/profile 清理契约一致；测试经真实
QGIS 切换链（activate(pan)）触发 deactivate，断言 band 隐藏 + 点列保留 + 重激活
续测三点齐。无问题。

## FD-003 setLayers 记账摘除（c56a70e2，P3）

**结论：正确。** 主 Agent 自查点核验：

- 复杂度：`std::as_const(m_layers)` 遍历 + `layers.contains(old)` 为 O(n·m)，
  层数几十以内，可忽略；遍历中只改三个 QHash、不动被遍历的 m_layers（循环后才
  整表赋值），无迭代器失效。✓
- destroy 钩子误摘回表层：不会——(a) `paleoPreviewWatched` 属性挂在 QObject 上，
  地址复用的新层不带旧属性，watchLayer 正常挂新钩子；(b) 被置换层析构时
  forgetLayer（previewmapcanvas.cpp:196-207）经 `!m_layers.contains(dead)` 早退，
  且 QObject::destroyed 在内存释放前同步发出，新层不可能在旧层 destroyed 分发前
  复用同地址——无「新层被旧钩子摘除」的时序窗。✓
- 仍在表的层经 `!contains` 守卫保留用户显式设置（重排不清），语义如 commit 自述。✓
- 测试覆盖「置换出→回表取默认」与「同表重排保持」两翼，断言到 isLayerVisible/
  layerOpacity 真值。✓

## FD-004 属性表 ask seam（c56a70e2，P3）

**结论：seam 设计与分层正确；可达性排序削弱了可测性。**

- 分层：qgis 封装层只加 `std::function` + 本层枚举（nativeattributetablehost.h:41-48），
  不 include ui/；壳侧 attributetablepanel.cpp:48-63 接 PaleoNotify——方向符合
  UI_LAYER_PLAN。layering --strict 通过。
- **R2-SF-04（SHOULD_FIX）——seam 排在 offscreen 早退之后，CI 永不可达**
  （nativeattributetablehost.cpp:96-98 vs :100-102）：`offscreen || !m_parent`
  分支先行 return saveEdits，注入的 seam 在无头环境永远不会执行——这正是
  commit message 自己承认的「seam 分支无自动化覆盖」。而 seam 本身恰恰是平台无关
  注入点：把 seam 判断提到 offscreen 早退之前（或仅当 seam 未注入时才走 offscreen
  直存），headless 下即可用注入 lambda 断言 Save/Discard/Cancel 三路，且生产行为
  不变（生产从不 offscreen、seam 恒注入）。现状等于核心新分支零测试。
- DOCUMENT_ONLY（FD-004-D1）：`PaleoAttributeTableHost::SaveChoice` 复制
  `PaleoNotify::SaveChoice` 有漂移面——但映射 switch 全枚举 case 无 default，
  任一侧加值即 -Wswitch 报警，缓解充分；分层所限这是合理代价。
- DOCUMENT_ONLY（FD-004-D2）：allowCancel=false 保持原生两键路径（不走 seam），
  commit 已声明为防语义漂移的取舍，一致。

## FE-001 永真断言改真值（b19ceefe）

**结论：有效。** 六个枚举成员（litholexicon.h:14-22）与期望字面量一一对应无遗漏；
往返断言成立——parseGroup("未知") 经 fall-through 返回 Unknown（litholexicon.cpp:75），
六值全可回环。方向+往返双钉。

## FE-002 tst_perf 绑生产真值（b19ceefe）

**结论：有效，链接面无风险。**

- 生产值核实：`m_pool->setMaxThreadCount(4)`（paleotaskservice.cpp:220，D4.5）
  与 `workerPool()` 存取（paleotaskservice.h:182）均在；PaleoAlgoGuards::
  gridDimsForExtent 属 paleo_algorithms。断言从自证公式改为绑这些真值，变异即红。
- LIBS 增链（CMakeLists.txt:1116）：paleo_services/paleo_algorithms 目标存在
  （CMakeLists.txt:422/558）；PaleoTaskService 构造不触 QSettings；且 add_paleo_test
  每测试独立 XDG/HOME/**TMPDIR** 沙箱（CMakeLists.txt:884），无设置面互扰。
  tst_perf 本就 RUN_SERIAL 且已初始化 QgsApplication（夹具用 QgsVectorLayer），
  新增构造无前置条件缺口。

## FE-003 AppImage runtime 钉版（cc6ed122）

**结论：正确，且本复审做了独立外部核验**：实际下载
`.../type2-runtime/releases/download/20251108/runtime-x86_64`（944,632 字节）计算
sha256 = `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`，
与 make_appimage.sh:34 钉值**逐位一致**；校验逻辑（`sha256sum -c` 失败即退出，
因 `set -e` 类语义由 `|| exit`/脚本错误码承担——脚本头原有 fail-fast）成立。

DOCUMENT_ONLY：钉版后 runtime 升级需手工双改 URL+摘要，注释已声明。

## FE-004 LO profile 随机清理（cc6ed122）

**结论：正确，互扰风险不成立。**

- 主 Agent 自查点：ctest 下每测试独享 `TMPDIR=${_home}/tmp`（CMakeLists.txt:884），
  `QDir::temp()` 指向私有沙箱——`after.size()==0` 断言看不到其他测试进程的
  paleo-lo-profile-*，-j4/-j8 并行无互扰面。✓（裸跑单测二进制时共享系统 /tmp
  属开发者手工场景，不在 CI 口径内。）
- 行为改进顺带成立：同进程两个 DataImportService 实例现在各持独立 profile 目录
  （旧固定 pid 名会撞 LO 单实例锁）。
- 测试断言「不含 pid 可预测名」+「析构后零残留」，与修复点一一对应。

DOCUMENT_ONLY：`QTemporaryDir` 未查 `isValid()`（dataimport_document.cpp:301-304），
/tmp 不可写时会向 LO 传模板字面量路径——极端环境下的失败形态不优雅，可后续补
isValid 判定并如实报错。

## FE-005 死 qrc 删除（cc6ed122 / 物理删除落在 aca2aa61）

**结论：安全。** 基线上 git grep 确认 translations.qrc 未被任何 CMake/源码引用
（真嵌入点 extra-ux.cmake qt6_add_resources，main.cpp Q_INIT_RESOURCE 对应后者）；
删除无构建影响，唯一嵌入点已加防再犯注释（extra-ux.cmake:64-66）。

DOCUMENT_ONLY（横切）：qrc 文件的物理删除出现在 FC-001 的 commit（aca2aa61）里，
FE-005 的 commit 只补注释——登记表与 commit 归属有轻微错位，考古时以本条为准。

## 横切检查

- **分层**：`tools/check_layering.py --strict` 实跑通过（「检查通过：无层违规」）。
  predictionworkflow.cpp（功能层）新增 qgsrasterlayer/qgsvectorlayer include 与
  constraintfactorjobs_factor.cpp 既有先例一致，检查器口径（禁 QtWidgets/禁 ui/）
  不受影响。
- **测试真实性**：本轮新增测试整体质量高——worker 线程全链（tst_workflows）、
  worker-gate 确定性注入（tst_versions）、截断注入（tst_segy）、稀疏大文件
  （tst_io_outsource/tst_wellfacies）、种子扫描（tst_algorithms）、真实 QGIS 切换链
  （tst_previewmap_tools）。未发现永真/mock 绕过；文案匹配（预算/512MB/编辑）均为
  同仓消息自匹配，属可维护性取舍。
- **未验证项（如实声明）**：本复审按铁律未编译、未运行测试；上述「测试有效」均指
  断言逻辑与夹具设计层面的判定，执行绿灯以主 Agent 红绿记录为准。FB-001 的
  qSetGlobalQHashSeed 线程语义（Qt6 下影响当前线程新建 QHash）与夹具两键表跨
  6 种子的实际迭代序差异，依赖其运行记录，逻辑上可信。

---

## 总结论与清单

Fix 1 的 17 项簇（FC-001…FE-005，其中 FC-004/FD-005 RECORD-ONLY 无代码）中，
16 项修复判定正确、治根因、与仓内范式自洽；测试整体从「断言存在」提升到
「断言行为」。发现 1 项修复不完整（FA-001 xlsx 作用域）与 5 项应修改进点，
无任何修复引入新的功能性/并发缺陷。

| 编号 | 级别 | 位置 | 一句话 |
|---|---|---|---|
| R2-MF-01 | MUST_FIX | outsourceworkbook.cpp:500 | xlsx 格数预算声明在 sheet 循环内按表重置，与「整簿拒收」自述不符，多 sheet 恶意簿仍可 ~N×4M 格放大——上移一行到 :481 循环外并改文案口径 |
| R2-SF-01 | SHOULD_FIX | tests/tst_algorithms.cpp:888 | `contains(3)` 恒真，并列裁决方向未钉——换 golden 面积集（3 面 5 格/7 面 4 格） |
| R2-SF-02 | SHOULD_FIX | evolutionplayerpanel.cpp:106-125 | reloadFrames 遇集合外层位时 m_index 可越新表界：标签撒谎+步进双向锁死——早退分支钳位/复位 |
| R2-SF-03 | SHOULD_FIX | mapversioncontroller_async.cpp:483-491 | publish 的 jobCompleted 与 !task 分支缺 disconnectSourceWatches（save 路径 :382/:392 有）——补两行对称收尾 |
| R2-SF-04 | SHOULD_FIX | nativeattributetablehost.cpp:96-102 | seam 排在 offscreen 早退后，CI 永不可达、零覆盖——seam 提前判定使注入路径可测 |
| R2-SF-05 | SHOULD_FIX | dataimport_document.cpp:301-304 | QTemporaryDir 未查 isValid()，/tmp 失败时向 LO 传模板字面量路径——补判定并如实报错 |

（R2-SF-05 由 FE-004 的 DOCUMENT_ONLY 升格为 SHOULD_FIX 入表，便于排期追踪；
其余 DOCUMENT_ONLY 共 12 条散见各节，不另行编号。）
