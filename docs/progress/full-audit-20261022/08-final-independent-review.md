# Full Audit 2026-10-22 — 08 Final Independent Review（终局独立终审）

- 终审人：独立终审 Agent（未参与任何修复编写；未采信前序报告结论，全部从
  HEAD=75f3a54e 重新审查）
- 对象：`git diff b301f2e8..75f3a54e`。**实际 9 个提交**（任务书写 8——R2 复审
  覆盖 b301f2e8..80bc61ac 共 8 个，Fix 2 的 75f3a54e 是第 9 个），38 文件
  +673/−74。
- 方法：源码只读静态复审 + 轻量源码闸门实跑（只读 Python 检查器，非构建、
  非测试）；调用面全量 grep；对 8 个新用例逐一做断言可达性推演。
- 结论速览：**PASS-WITH-NOTES（有条件）**——17 项 Finding 修复 + 4 项 R2
  收口全部确认修对、无新增运行时缺陷；但 **1 项新 P1（file_budget 闸门
  回升 2 条，CI lint 必红）使本轮 HEAD 不能原样合入**，需一处小型提取
  收敛（代码收缩，非补登基线）后方可合并。另 1 项 P3、2 项 P4 备忘。

---

## 1. 总判定

| 级别 | 数量 | 清单 |
|------|------|------|
| P0 | 0 | — |
| P1 | 1 | N1 file_budget 规模闸回升 2 项（buildHorizonContent 441>429、buildMapProductContent 385>373） |
| P2 | 0 | — |
| P3 | 1 | N2 services 侧 C-8 同型双嗅探残留（readCurveTvd / crossplotsources，原 finding 范围外存量） |
| P4 | 2 | N3 空白分词 Unicode 口径微扩；N4 「master 在册红」措辞与 known-reds.json 不符 |

按登记册自身的严重度阶梯（主线阻塞=P1 级），**N1 单项即判定本轮 HEAD
非 merge-ready**；因其修复是纯机械提取（两处重复的五行「不可用」标签块
合并为文件内 helper，预计各函数净 −8 行、回到冻结值内），无需回滚任何
整项修复，故总评给 PASS-WITH-NOTES（合并前置条件 = 先收 N1）。

---

## 2. 十项检查逐项结论

### 2.1 整体 diff 复核（9 commit 逐 hunk）

38 文件逐 hunk 走查：注释声称与代码行为逐一对拍成立；无夹带无关改动；
无注释与实现相悖处（R2-D1 指出的过时注释已在 75f3a54e 同步）。提交边界
干净（每 commit 单一主题）。`git diff --check` 干净（零空白错误）。
任务书「8 个提交」与实际 9 个的差异系 Fix 2 增量，非问题。

### 2.2 已修复 Finding 逐项验证（根因消除 / 最小修改 / 行为语义 三问）

| 项 | 根因消除？ | 最小修改？ | 行为语义（独立推演结论） |
|----|-----------|-----------|------------------------|
| C-7 predictionLog 米制归一 | ✓ 归一从仅 explicitAxis 扩到全部格式 | ✓ +15/−8 行，行为变化即修复目标 | LAS parse 恒置 indexBasis=MD（lasparser.cpp:220/381 独立核verified），LAS 不会被新闸误杀；`*scale!=1.0` 跳乘安全（词表只产 1.0/0.3048 精确值）；入库侧 normalizeDepthCurve 未知单位返 NaN 拒收（welllogfacies.cpp:583-590 独立核），对比页拒 DM 与入库同口径 |
| B-02 mdAxisInMetres | ✓ legacy 未知非空拒收 | ✓ 条件加一析 + unit 先 trim | 「0.1INCH」DLIS → nullopt → 拒；消费面 datapreviewtabwelllog.cpp:325-350 axisWarnings 经 hintFill setText+show 如实上屏（独立核）|
| B-03 MD 闸序 | ✓ ok/空检查前移，两路（driver+副文件） | ✓ | 副文件新检查顺带消灭修前「损坏副文件静默产全 NaN 列」更深问题；D8（CSV 报「LAS 无曲线」措辞）维持文档备忘级 |
| B-01 批量让位 | ✓ JobRunner 增 priority 参数（默认 High 保旧语义）+ BatchQueue 传 Low + 3/4 参 start 显式 Normal | ✓ | 见 2.3/挑战 3：无饥饿死锁；让位闸 `pool.running − batch.running ≥ cap` 差值=纯非批任务数，语义成立 |
| B-05≡D-03 回调诚实 | ✓ 四路 else ready(空载荷) | ✓ | 见 2.3：恰一次回调、无重复缓存写、无悬垂 |
| A-01 单模糊井名文案 | ✓ matches==1 且原名精确仍自动选中（前置 if 未动），仅规范化命中改新文案 | ✓ | tr 串为中文源文，l10n 闸不涉及 |
| A-02 GB18030 | ✓ decodeText 与井表同口径 | ✓ | Latin1 兜底永不失败的接受面扩大由结构闸兜底（sniffText 需索引名列+≥2 列；readTable toDouble 逐格严格）——独立复核成立 |
| A-03 词表三面一致 | ✓ isIndex ≡ 分类器正则(:158/161) ≡ 报错文案，分隔符 tab→逗号→分号 ≡ `[,;\t]` | ✓ | 全部独立 grep 对拍一致 |
| A-06 逐 token 早退 | ✓ 空白分支获 kMaxColumns 预算 | ✓ | 失败点与分隔符分支同在 257th token；>256 列空白表头在 sniffText 也早退（.txt/.csv/.tsv 有扩展名兜底仍判 Text），错误文案从「列数不一致」变「列数超过上限」，均诚实 |
| A-08 行序 | ✓ lateRows 暂存缀尾 | ✓ | 契约 dataimportservice.h:90-93 落地；`lateCount>0`→`!lateRows.isEmpty()` 等价 |
| C-8 消双嗅探 | ✓ parse(format) 重载；六处全部改造（5 处转新重载 + predictionLog 撤外层 detect） | ✓ | 见 2.4 盘点与 2.5 推演 |
| D-01 身份路由 | ✓ source() 枚举替代标题判据；switch 三支与旧标题判据逐支对拍等价（Sequence 无条件覆盖/SandGroup 不同步/Layers 带 non-empty 守卫） | ✓ | defaultlayout 三处 setSource、duplicateTrack 保源、captureSpec/restore 全保存面覆盖；「formationSource」camelCase 与既有 params 键（penWidth/grainQuantum）风格一致 |
| E-01 P14 | ✓ PATCHES.md 与 SgySectionBuilder.h:29-33 实体对账一致（never-synthesized 契约、消费面、编译失败信号） | ✓ 文档 | — |
| E-02 计数对齐 | ✓ 实跑 check_qgis_spread：172；qgs-audit：137——与 AGENTS.md 新文案一致 | ✓ 文档 | — |
| E-03 缩进复位 | ✓ CMake 忽略缩进，块归属恢复 | ✓ | — |
| R2-M1 空路径守卫 | ✓ `!path.isEmpty() && format != Las` 恢复 + parse 仍带 format（守卫与性能兼得） | ✓ 一行 | 回归测试 predictionWellWithoutLogFallsBackToMockDepth 在位（断言 errors==0，修前 preparationError 必红） |
| R2-S2 空剖面不留图例 | ✓ 两消费方 `samples.isEmpty()` 早退 | ✓（**但引入 N1**） | 五标签「不可用」块即 N1 的 +12 行来源 |
| R2-S3 词表收敛 | ✓ readCurveTvd legacy 链改 depthScaleToMetres | ✓ | 旧接受集 {FT,F,M,空} ⊂ 新词表（严格超集，加 METERS/FOOT/FEET）；大小写不敏感保持（两实现都 toUpper） |
| R2-S4 头注对齐 | ✓ 「初冻 149…现值 172」 | ✓ | — |

B-06 维持证伪：neighbourhood 极性（nv≥v 清 isMax / nv≤v 清 isMin）使平坦面
内域格双标志皆假、进不了候选；既有 robustExtremaScaleOnSyntheticGrids
第 1) 例钉住。本轮只加注释，处置正确（独立读 previewrasteranalysis.cpp
邻域比较核过）。

### 2.3 文件生命周期 / 悬垂

- **previewrasteranalysis 四路回调**：lambda 按值捕 task/result(shared_ptr)/
  ready(std::function 拷贝)；连接以 receiver 为上下文——receiver 销毁则断连
  零回调（destroyed→requestCancel 的任务终态后无 listener）；applyFinish 有
  `m_state != Running` 运行态闸（paleotaskservice.cpp:193-196），finished 恰
  发一次；失败分支不写 cache（重选重试语义保留）。**无新悬垂**。
- **FormationSource**：纯值枚举 setter，无异步无生命周期面。
- **jobrunner priority**：值参追加第 6 位，既有调用方（derivationworkflow
  5 参 quiet 位、datagraphworkflow 3 参）源级兼容零变更；Priority 为
  enum class（bool 不可隐转），4 参 start 传 `true` 无重载歧义。
- **排队期取消**（挑战 3 佐证）：TaskRunner::run 见 `t->cancelRequested()`
  即不跑 work 落 Cancelled——会话切换后排队的 Low 批任务不烧算力。

### 2.4 跨模块调用面盘点（grep 全量）

- WellLogRead::parse/detect 全 src 调用点盘点：六处 workflow 调用面全部
  改造（propertymodelworkflow:1161-1163 / sectionworkbench:414-415 /
  wellsectionworkflow:377-382 / wellpredictionservice:273-277+284 /
  mappingworkbench_predict:518-523 / wellpredictionreview:35 撤外层 detect）。
  4 参旧签名仅作委托（welllogread.cpp:154-158），行为与旧版逐路径等价
  （Unknown → parseAdditional「无法识别」，与旧版内嵌 detect 同果）。
- welllogset 两函数（mdAxisInMetres/readCurveTvd）调用面：
  mdAxisInMetres 唯一消费方 datapreviewtabwelllog（两分支均走
  axisWarnings 通道）；readCurveTvd 消费面（tst_import_ux/performance/
  welllog_multiformat + 图件服务）词表扩为严格超集，无回归。
- paleotaskservice 3/4 参 start 提级：受影响调用方（seismictaskservice/
  mapbookqueue/horizonbatchexport/previewdoc 预取等）相对序不变（同
  Normal 档 FIFO），相对显式 Low 从并列变让位、相对 High 不变——即
  D4.6 本意。**遗漏面：零**。

### 2.5 性能热点对照

- **C-8 open 次数**：detect = Dlis::sniff + Lis::sniff + 64KB 读 ≈ 3 open。
  修前六调用面每文件每轮 = 外层 detect(3) + parse 内 detect(3) + 实际解析；
  修后 = detect(3) + 实际解析——**每文件每轮省 3 次 open + 64KB 读**；
  wellpredictionservice 更从「每 ref 一趟外层 detect」收敛为「每 path 一次
  缓存」（formats hash）。
- **A-06**：超宽单行在 257th token 即失败，不再物化整行 QStringList；O(n)
  无正则。sniffText 同步受益（>256 列空白表头在嗅探层即出）。
- **D-01**：switch 三分支 O(1)，无成本。

### 2.6 架构分层 / 序列化键

- 新增 include 仅 `io/welllogtext.cpp → io/encodingdetect.h`（数据层同层）
  与测试壳两处（豁免）。实跑 `check_layering --strict`：零违规；
  `--qgs-audit --strict` 137=137 绿；`check_qgis_spread --strict` 172=172 绿；
  ui_size/ui_invariants/ui_tokens/i18n/docs_drift/qgis_versions/l10n 全绿。
- formationSource 键风格与 TrackSpec 既有 params 键一致（camelCase 短键）；
  Layers 缺省不写（旧会话文件兼容）。

### 2.7 旧测试回归面

- predictionLog 空 curves / LAS 新闸：LAS 恒 MD 基准故不误杀；既有
  predictionLogAcceptsUnifiedMdAndRejectsOtherAxes 的文本 TIME/TVD/DEPTH/
  MD[league] 拒收断言语义不变；tst_mappingworkbench:2947 setLog(…)
  无内容断言。旧「未知非空 LAS 直通」无在册测试依赖（grep 无 UTF-8 拒收
  文案断言、无未知单位 LAS 断言）。
- mdAxis 拒收：tst_datapreview 无直接 LAS 单位夹具；tst_import_ux LAS 夹具
  全部 DEPT.M（仍收）。readCurveTvd 接受集严格扩，无旧用例受挤。
- pickDelimiter 分号：仅无 tab/逗号时生效，既有 CSV（逗号）用例零影响。
- 已知红对照：tst_datapreview::wellLogPreviewShowsCuttingsLithologyTrack
  在纯净基线 b301f2e8 复现（06-baseline-quality #2，EXISTING_BASELINE_-
  FAILURE），与本轮改动面（requestSummary 消费端）不同用例——**非本轮
  引入**（但见 N4 措辞备忘）。

### 2.8 新测试有效性（8/14 抽查断言可达性，超过要求的 5）

| 用例 | 可达性判定 |
|------|-----------|
| predictionWellWithoutLogFallsBackToMockDepth | 修前（C-8 重构态）explicitAxis=true→preparationError→predict false→VIRIFY2 红 ✓ |
| FT/DM LAS（C-7 扩展） | 304.8/unit=="m" 与 DM 拒收：修前 LAS 直通 ok=true、值=1000 → 双断言红 ✓；夹具 ~C DEPT.FT 可被 lasparser 接受（与既有 ~Curve DEPT.M 夹具同构） |
| mdAxisRejectsUnknownLegacyUnitAndScalesFeet | DM 例修前返回 true → 红 ✓；FT/空单位例修前已绿（属锁行为而非红绿） |
| testMergeReportsDriverParseFailureAsParseError | 垃圾 CSV→detect Unknown→parseAdditional 错误含路径（「%1：%2」.arg(path)）→「bad.csv」断言可达；修前先落 MD 闸文案 → NOT-contains 红 ✓ |
| batchTasksYieldPoolToInteractivePriority | 单线程池 + Qt 文档化优先级出队，确定性无墙钟；修前 H2=High 必先出 → interactive<second 红 ✓ |
| rasterRequestFailureStillCallsReady | 修前零回调 30s 空转后 QCOMPARE 红（慢红可接受）；四路恰一次+空载荷断言在位 ✓ |
| renamedSequenceTrackKeepsSequenceData | 修前标题判据把改名道覆盖为 1000（≠1020）→ qFuzzyCompare 红 ✓（编译级：source() API 新增） |
| gb18030 / 井深+分号 / wide | 字节序列（C4B3/BEAE）、词表、256 预算路径均可达；修前分别编码拒/缺索引列/文案不含「列数超过上限」 ✓ |

无永真断言、无绕过真实路径。tst_import_ux 两处为既有用例内扩展断言
（行序指针序、文案 NOT-contains），同判可达。

### 2.9 PR/Issue 冲突复查

- **PR #627**（OPEN）文件面 {DESIGN.md, wells-during-basemap-refresh.md,
  onlinebasemap.{cpp,h}, previewmapcanvas.cpp, qgiscanvascontroller.cpp,
  tst_onlinebasemap.cpp} ∩ 本轮 38 文件 = **∅**；根因面（在线底图刷新时
  序）与本轮（深度单位/导入反馈/批优先级/预览栅格回调/道身份）零重叠。
- 开放 Issues #556-#597：本轮修复根因面与各 issue 描述零冲突；排除登记
  项（D-04~D-08→#590/#592、A/O-1→#587）未被本轮触碰；A-04/A-05/B-04/D-02
  结转面未触碰。**注意**：N1 属 issue #591（闸门红单登记与实测不符/基线
  超标未收敛）同类的新增实例——本轮自身制造了一条 #591 类问题。

### 2.10 仓库卫生

- `git status`：仅未跟踪 docs/progress/full-audit-20261022/（审计文档本身，
  预期）；无意外生成文件、无测试数据泄漏。
- vendor 仅 PATCHES.md +12 行；无 vendored 意外改动。
- `git diff --check` 干净。

---

## 3. 新发现问题明细

### N1（P1）file_budget 规模闸回升 2 项——CI lint 必红、本轮 HEAD 非 merge-ready

- **证据（本终审实跑）**：
  ```
  $ python3 tools/check_file_budget.py --strict   # 非 --strict 同样红
  FAIL src/ui/datapreview/datapreviewtabhorizon.cpp [fn:buildHorizonContent]
       441 行 > baseline 冻结 429——超标面回升，只缩不涨
  FAIL src/ui/datapreview/datapreviewtabmapproduct.cpp [fn:buildMapProductContent]
       385 行 > baseline 冻结 373——超标面回升，只缩不涨
  ```
  用检查器自身函数跨逻辑实测：b301f2e8 两函数恰为 429/373（== 冻结值，
  绿）；75f3a54e 为 441/385（各 +12，红）。**红是本轮 b29301ae + 75f3a54e
  的 B-05/R2-S2 消费端五行「不可用」标签块引入**，且 06-baseline-quality
  记录纯净基线 17 项检查器全绿——即门禁状态在修复过程中由绿翻红。
- **影响**：ctest `file_budget`（linux/windows 车道无标签项）与 CI lint 车道
  Source gates（ci.yml:131，required check）双红 → 按登记册严重度阶梯构成
  主线阻塞（P1）。违反 AGENTS.md「任何基线都只缩不涨」与本轮自己的计划
  承诺（05-priority-and-fix-plan.md:40「基线文件只缩不涨；闸门本地全跑」）
  ——Fix 2 只跑了定向 7 套件，未复跑源码闸门。
- **修法（代码收敛，禁止补登基线）**：两函数重复的「失败→五标签不可用」
  块提取为文件内共享 helper（两文件各净 −8 行左右，回到冻结值内），行为
  零变化；随附 file_budget 复跑绿证据。属机械收口，不影响任何已修复项
  的语义。

### N2（P3）services 侧 C-8 同型双嗅探残留（原范围外存量）

- welllogset.cpp readCurveTvd:337 `parse`（内部 detect 一次）+ :354 再
  `detect` = 每调用 2 趟嗅探；crossplotsources.cpp:169/:182 两次 detect +
  parse 内一次 = 3 趟。原 C-8 finding 明确限定 #603 的 workflow 六处接线面，
  故**不构成修复缺陷**；登记为「C 观察6 / C-8 后续收敛批次」的下一批
  候选（readCurveTvd 改 format-arg 重载即一行级）。

### N3（P4）A-06 空白分词 Unicode 口径微扩

- 旧 `\s+`（PCRE，ASCII 空白）→ 新 `QChar::isSpace()`（含 U+3000/U+00A0
  等 Unicode 空白）：全角空格分隔表头旧版单 token 失败、新版正确分列。
  接受面扩大方向与 GB18030 生态一致、无失败模式恶化；留档即可。

### N4（P4）「master 在册红」措辞与 CI 分诊册不符

- 07-fix-r2-results.md:21 称 tst_datapreview::wellLogPreviewShowsCuttings-
  LithologyTrack 为「master 在册红」；该红**未登记**于 docs/ci/known-reds.json
  （entries 仅 cache-las / singlefactor 两条测试级条目），「在册」实际指
  审计自身台账（06-baseline-quality EXISTING_BASELINE_FAILURE，纯净基线
  复现在案）。非缺陷；终局/PR 描述建议改措辞为「纯净基线复现在案」，
  或按 #497 纪律决定是否补分诊登记。

---

## 4. 构造性挑战逐条回应

1. **predictionLog DLIS（FT）**：DlisParser 曲线按主帧 fr.channels 序组装、
   行对齐即以 curves.first() 为索引道（dlisparser.cpp:1414-1416 独立核）；
   indexBasisFor：BOREHOLE-DEPTH→MD（换算 ×0.3048、unit→"m" ✓）、
   VERTICAL-DEPTH→TVD（修前修后同拒）、空→TIME 探测或空（同拒）。
   无回归；FT 归一断言经新 FT-LAS 用例锁住（DLIS 无夹具，见 §6 缺口）。
2. **mdAxisInMetres「0.1INCH」**：词表无此项→拒收；datapreviewtabwelllog
   325-350 失败走 axisWarnings→hintFill 显示（先 hide 后 show 的时序在该
   分支末尾统一处理）——如实上屏，不静默。
3. **BatchQueue Low + beginNewSession**：pump 会话守卫把 Queued/Pending 全
   置 Cancelled、清 FIFO、协作停在飞项后 finishIfDone——**续跑路径不存在，
   无饥饿窗口**；池内已排队的 Low 任务因 TaskRunner 排队期取消检查不烧
   算力。唯一饥饿面是持续 Normal 流压 Low 的 D5（无老化）——预取/可取消
   语义下已留档接受；且让位后 pumpLater 自排补位只在「交互真占满池」时
   以事件循环频率轻量自旋（旧「自计入自身」病态已修）。
4. **FormationSource 旧 sidecar**：恢复按「类型+标题」匹配——旧会话未改
   名道匹配成功且 live 面源已由 defaultlayout 显式设定（同步路由正确）；
   改名道标题不匹配被丢弃回默认布局，与修复前行为**完全等价**（既有
   D-02 族标题键限制，非本轮引入）。手工新建标题恰为「层序」的道现绑
   Layers——身份语义更正确（R2-D4 已留档）。
5. **#618 一键链**：本轮 38 文件不含 constraint/提取生成链任何文件——
   **确认未触碰**。
6. **R2-S3 strictAxis 解引用**：上闸（welllogset.cpp:356-361）已在
   `!depthScaleToMetres` 时 return false；:365 解引用仅在 scale 有值路径
   可达——**无空解引用**（独立核）。

---

## 5. Oracle O1-O7 口径下的缺口

以本仓 oracle 记账惯例（job-framework 账本口径）衡量本轮：

- **O1 行为保留**：静态推演全覆盖；DLIS/LIS 的 predictionLog 归一**无运行
  时证据**（新用例只锁 LAS+文本）——建议后续补 DLIS FT 夹具或至少在
  tst_welllog_multiformat 挂一条 predictionLog 断言。
- **O2 框架断言**：优先级语义仅单线程池用例；多线程池/多槽出队序未测
  （可接受，注明即可）。
- **O3 既有测试原样绿**：主 Agent 全量 ctest 在跑（本审不抢资源）；已知
  预存红 2 项（tst_datapreview / tst_assetpaging，纯净基线复现在案）+
  tst_onlinebasemap 属 PR #627 排除面。
- **O4 分发覆盖**：C-9 结转债在册——propertymodelworkflow / sectionworkbench
  两处 MD 拒收路径仍零覆盖（登记册已注明，独立测试批次）。
- **O5 探测面记账**：C-8 原 FixStrategy 建议的「open 次数计数钩子」未实现
  （本轮以调用面盘点代替）；N2 的 services 残留未探测。
- **O6 规模闸**：**N1 即本口径的具体缺口**——file_budget 回升 2 项红灯。
- **O7 全账收口**：本报告收口；Phase 7 全量闸门/测试记录尚待主 Agent
  落档（本审已代跑 12 项轻量检查器：11 绿 1 红=N1）。

---

## 6. 最终裁定

**PASS-WITH-NOTES（有条件放行）**

- 17 项 Finding 修复 + R2-M1/S2/S3/S4 收口全部通过独立验证：根因消除、
  改动最小、行为语义正确、无新增运行时缺陷（无悬垂/无生命周期回归/
  无分层违规/无 PR-Issue 冲突/仓库卫生干净）。
- **合并前置条件**：先修 N1（file_budget 回升 2 项；机械提取收敛，禁止
  补登基线）并复跑 `python3 tools/check_file_budget.py --strict` 转绿；
  同时建议随批处理 N4 措辞。N2 入 C-8 后续收敛批次；N3 留档。
- 若团队的裁决口径是「任何 P1 即 FAIL」，则在本报告口径下应读作：
  修复语义全部 PASS、流程闸门单项 FAIL——差异完全由 N1 一项机械收敛
  消除。

— 终审 Agent，2026-10-22。方法：HEAD 静态复审 + 只读闸门实跑 + 调用面
grep + 断言可达性推演；未修改任何源文件、未构建、未运行测试二进制。

---

## 附：主代理处置记录（终审后，2026-10-22）

- **N1 已收口**（485045a5）：datapreviewtabs_internal.h 增 setStatLabel/
  markStatsUnavailable 共享助手（层位/成图两统计页共用），两个 build 函数
  删本地 update lambda——file_budget/file_budget_strict 复跑绿
  （buildMapProductContent 373→372，基线随缩下调；buildHorizonContent 回
  冻结值内）。处置方式与终审建议一致（代码收缩，未补登基线）。
- **N4 已改写**（07 文档措辞对齐 known-reds.json 现实）。
- **N3 留档不改**（接受面微扩与 GB18030 生态同向）。
- **N2（P3）结转**：services 侧 readCurveTvd/crossplotsources 双嗅探为
  原范围外存量，登记下一收敛批次（与 C 观察 6 三体收敛合并处理）。
- 终审其余结论（17+4 项修复全部确认、PR/Issue 零重叠、分层绿）无异议。
- 终局全量 ctest（N1 收口后）与全部源码闸门复跑结果见 09 台账 Round 4。
