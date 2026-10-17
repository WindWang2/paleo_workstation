# Agent D — QGIS 封装与 UI/UX 层深度审计报告（2026-10-17）

基线：`/home/kevin/projects/paleo_workstation-full-review-20261017` @ `c720aef5`（= origin/master）。
范围：`src/qgis/` 全部 + `src/ui/` 全部。只读审查，未修改任何文件。
主攻：#496 (a1efbd74)、#494 (ddd1325e)、opt-05 (#491)、opt-03 (#492)、opt-04 (#493)、opt-01 (#489)、opt-02 (#490)、#487/#488 合入的 UI 侧新代码。
排除矩阵已交叉：#463/#472/#473/#474/#481/#482/#483/#502 及 audit-remediation 已改/在改文件的方向性问题不重复报；无法完全隔离者标 `UNCERTAIN_OVERLAP`。

---

## Findings

### D-01 | P2 | VERIFIED | 编图页新证据命令按钮零门控（busy/层位/选择态全不判）

- **Module**: src/ui/pages（mappingworkbenchpage）+ paleomainwindow_workbench
- **File:Line / Function**:
  - `src/ui/pages/mappingworkbenchpage.cpp:280-281`（ctor，`impactAnalysis`/`recompute` 按钮创建）、`:454`（`reverseQuery` 按钮）
  - `src/ui/pages/mappingworkbenchpage.cpp:919-1010` `MappingWorkbenchPage::updateState()`（全量 gate 清单中无这三个 id）
  - `src/ui/paleomainwindow_workbench.cpp:776-845`（命令分发 handler）
- **Root Cause**: #494 给 compose 模式页新增「分析所选输入的版本变化影响」「按最近影响报告受控重算」「反查」三按钮，但 `updateState()` 的 `gate(...)` 体系没有为它们写任何规则——对比同页 `predict`/`compose`/`references` 均有 `horizon && !busy && ...` 门控与 reason tooltip。
- **Trigger**（多条独立路径，逐一核实）:
  1. 预测/编图任务在飞（`m_workbench->busy()`）时按钮仍可点：`impactAnalysis` 与后台 compose 并发跑（共享 `attributionLayer` 层缓存与 GDAL 打开同一 GPKG）；`recompute` 点击后由 `startCompose` 报 busy 错误，但用户先得到一个可点按钮再吃到失败。
  2. 未选层位 chip（`m_horizon` 空）时点「反查」选 `pending_review`/`contradicted`/`stale_sources`：`reverse_argument = m_horizon = ""`，`evidenceindex` 的谓词是 `horizon = ?`（`src/services/evidenceindex.cpp:107-113`）→ 空串恒不匹配 → 结果列表显示「无匹配结果（索引随成果保存增量更新）」——把「没选层位」伪装成「没数据」。
  3. 结果树无选中（`selectedLayer()` 空）时点 `impactAnalysis`：`after.assetId` 为空 → `before` 恒空 → 报错「该图件只有单一版本，没有可分析的版本变化」——没选东西被说成"只有单一版本"。
- **Observed vs Expected**: 预期（同页既有模式 + DESIGN.md「禁用控件必须带 reason tooltip」精神）：busy/无层位/无选中时按钮禁用并给 reason。实际：恒 enabled、无 tooltip、错误文案误导。
- **Impact**: 交互误导 + 与后台任务并发重 IO；错误路径文案直接说谎（单一版本/无匹配）。
- **Existing Match**: NEW（UNCERTAIN_OVERLAP：mappingworkbenchpage.cpp 在 codex/audit-remediation 已改文件清单内，但该分支改的是 QScrollArea 方向（UI-P0-02），与本条根因不同）。
- **Fix Strategy**: 在 `updateState()` 增加三条 gate：`impactAnalysis`（horizon && !busy && 有选中图件）、`recompute`（!m_lastImpact.isEmpty() 的状态需经信号回喂页面或按钮由窗口侧控制）、`reverseQuery`（horizon || kind==search）；reverseQuery 空层位直接给「请先选择层位」。
- **Regression Test**: tst_mappingpages：busy 期间三按钮 disabled；无层位点反查 → 页面提示选择层位而非"无匹配"；无选中点影响分析 → 错误文案区分「未选择」与「单一版本」。

### D-02 | P2 | HIGH | 影响分析/证据反查在 GUI 线程同步执行，无进度无取消（DESIGN.md Motion 违约）

- **Module**: src/ui（paleomainwindow_workbench 命令分发）× workflow（analyzeImpact/evidenceReverseQuery）
- **File:Line / Function**: `src/ui/paleomainwindow_workbench.cpp:776-845`（同步直调）；`src/workflow/mappingworkbench_impact.cpp:155-370` `analyzeImpact()`、`:636-689` `evidenceReverseQuery()`；`src/workflow/mappingworkbench_impact.cpp:33` `kImpactMaxGridCells = 4000000`、`:34` `kImpactMaxProducts = 64`；`src/services/evidenceindex.cpp:498+` `Index::synchronize()`
- **Root Cause**: 三个重操作以普通函数调用形态挂在按钮 handler 里：
  1. `analyzeImpact`：对下游闭包（≤64 个 composed 产品）逐个做两次 `composeattribution::replayGrid`（每次 ≤400 万像元；含相点输入时逐像元 `QgsSpatialIndex::nearestNeighbor`，`src/qgis/composeattribution.cpp:534-578`）+ 逐受影响图斑 `index.intersects` + `layer->getFeature`；此前每次 resolve 还要 GDAL 打开 ≤N 个冻结输入（`attributionLayer`）。
  2. `evidenceReverseQuery`：每次点击先 `m_evidenceIndex->synchronize(...)`——工程打开后首次点击对**所有** mapping_product 矢量版本做 SHA256 全文件读 + GPKG 全要素扫描（`evidenceindex.cpp:520-556`），调用方传空的 `cancelled`（`mappingworkbench_impact.cpp:654`），UI 无取消通道。
  3. 无任何 busy 指示、无任务中心登记、无进度条——`composeattribution.cpp:509-510` 注释自认「影响分析的大网格在 UI 线程同步执行」，但只做了「逐点 sample→整块 block」的降维，没改变同步事实。
- **Trigger**: 大型/多版本工程 → 编图页点「分析所选输入的版本变化影响」或「反查」→ 主窗口冻结数秒~分钟级；期间底栏任务中心不可用、无法取消。
- **Observed vs Expected**: DESIGN.md Motion 节：「任务进度走状态栏/底部任务面板，>1s 显示进度条，>10s 给预计时间」。实际完全同步、无任何反馈。
- **Impact**: 用户可感知的界面冻结；与 DESIGN.md 明文冲突；大工程下功能事实不可用。
- **Existing Match**: NEW（家族近似 #479 的「主线程同步 ORT 降级」，但根因/位置独立）。UNCERTAIN_OVERLAP：paleomainwindow_workbench.cpp 有另一会话未提交改动（编图工单方向），本条针对 #494 已合入代码。
- **Fix Strategy**: 最小改——把 `analyzeImpact`/首次 `synchronize` 包成 PaleoTask（既有 `constraintfactorjobs`/`startMapLayoutExport` 三段式范本），handler 只弹任务中心；`synchronize` 接取消回调。或先加 `QApplication::setOverrideCursor` + 分片进度。
- **Regression Test**: tst_mappingworkbench：`analyzeImpact` 经由任务服务（busy 期间 impactAnalysis 按钮 disabled，联动 D-01）；大网格场景断言 handler 立即返回。

### D-03 | P1 | VERIFIED | attributionLayer 8 条目缓存任意驱逐：使用中图层被 delete → 归因重放 use-after-free

- **Module**: src/workflow/mappingevidencereview（缓存本体）× src/qgis/composeattribution（消费契约）——由 UI 选择/按钮直接触发，跨层报告
- **File:Line / Function**:
  - `src/workflow/mappingevidencereview.cpp:264-302` `MappingWorkbench::attributionLayer()`——`:293-297`：`if (m_attributionLayers.size() >= 8) { delete first.value().data(); m_attributionLayers.erase(first); }`（`first` = QHash 任意序，非 LRU）
  - `src/workflow/mappingevidencereview.cpp:342` / `:426`（composeAttribution / composeAttributionAt：`input.layer = attributionLayer(...)` 裸指针存进 `QList<ResolvedInput>`，随后 `replay()`/`replayAt()` 逐个 `qobject_cast` 解引用）
  - `src/workflow/mappingworkbench_impact.cpp:51-106` `resolveFrozenCompose()`（同模式：`input.layer = opener(effective)`；`analyzeImpact` 对同一产品先 resolve base 后 resolve substituted——第 9 个不同版本入缓存即驱逐 base 侧仍在用的图层）
  - 契约声明：`src/qgis/composeattribution.h:24-26`「layer 为按冻结路径打开的独立图层」
- **Root Cause**: 缓存容量 8 + 「打开第 9 个前先 delete 任意一条」，而所有调用方都以裸 `QgsMapLayer*` 持有返回值并跨多次 `attributionLayer()` 调用继续使用。复合编图输入数 ≥9（单次 composeAttribution）或 ≥8（analyzeImpact 的 base+substituted 双解析：N 个共同输入 + 1 个替换版本 = N+1 > 8）即触发：被 delete 的正是仍在 `ResolvedInput::layer` 里的对象。
- **Trigger**: 用户在 compose 页勾选 ≥9 个输入（UI 无输入数上限，多单因素+相点+地震叠加常见可达）生成综合图 → 点选任一图斑（selectionChanged → `EvidenceInteraction::capture` → `featureEvidence` → `composeAttribution`）→ 第 9 个输入打开时随机驱逐第 1~8 个之一 → `replay()` 对已释放对象做 `qobject_cast`/`dataProvider()` → 崩溃或垃圾归因数据。analyzeImpact 同理（D-02 按钮）。
- **Observed vs Expected**: 期望：归因输入图层在重放期间存活（头文件契约）。实际：8 条目上限内的任意驱逐可发生在指针仍被持有期间。
- **Impact**: 内存安全（UAF）+ 归因/影响分析结果可能损坏；触发条件是正常的用户数据规模。
- **Existing Match**: NEW（不在任何 issue/分支清单）。
- **Fix Strategy**: 短期——驱逐只针对「当前无借用」的条目：给 `attributionLayer` 加借出登记（或返回 `std::shared_ptr<QgsMapLayer>`，QHash 存 shared_ptr、缓存只 Drop 引用不 delete）；长期——LRU + 明确所有权文档化。
- **Regression Test**: tst_mappingworkbench：构造 9 输入 compose → 断言 composeAttribution 全程无释放对象访问（ASAN 下跑）；或单测 mock 缓存容量为 2 验证借用保护。

### D-04 | P2 | VERIFIED | #496 新增三处裸 QMessageBox，违反 DESIGN.md Tier-2 收敛契约

- **Module**: src/ui/pages（constraintpage_constraints）
- **File:Line / Function**: `src/ui/pages/constraintpage_constraints.cpp:344-357`（`constraintCrsMigrateButton` clicked lambda：`QMessageBox::question` 确认 + `QMessageBox::information` 成功 + `QMessageBox::warning` 失败）
- **Root Cause**: a1efbd74（#496）为「约束库 CRS 迁移」新增的确认/结果呈现直接用了裸 `QMessageBox`。DESIGN.md《错误呈现》Tier-2：「全局统一走 ui/notifications/paleonotify.h 的 ask / askSaveDiscard 收口，消除散弹式裸 QMessageBox 调用」；warning/information 应走通知卡（Tier 1）/状态栏。该契约自方向 64（2026-10-07）起生效，#496 是其后合入的新代码，属回归性违约（仓内现存裸调用都是迁移前存量 + 白名单决策，见 `dataops*`/`attach_shell`——后者已在 #481 登记）。
- **Trigger**: 约束页 legacy CRS-less 库 → 点「为约束库登记本地网格坐标系…」→ 裸模态弹窗链（确认→结果两连弹）。
- **Observed vs Expected**: 期望 `PaleoNotify::ask(...)`（确认）+ `PaleoNotify::warning/information`（结果）。实际裸 QMessageBox 三连。
- **Impact**: 错误呈现体系旁路：成功/失败不入 ErrorHub 历史（Tier 3 审计缺失）、无 60s 去重保护、视觉脱离双主题通知卡规范。
- **Existing Match**: NEW（具体调用点未见于 #481 清单；与 #481 的 attach_shell 裸弹窗同族不同实例）。UNCERTAIN_OVERLAP：constraintpage* 全簇在主 checkout 有未提交活跃改动（约束页方向）。
- **Fix Strategy**: 确认改 `PaleoNotify::ask`（OkCancel/Reject 默认）；结果改 `PaleoNotify::information/warning` 或状态栏 + 页面提示行（同文件 `refreshConstraintList` 已有 hint 通道）。
- **Regression Test**: tst_constraintpage：迁移确认经 `setConfirmHookForTesting` 钩子应答；断言 ErrorHub 历史出现迁移结果记录。

### D-05 | P3 | VERIFIED | 约束编辑会话每条编辑命令 O(新增要素×全表行) 的 GDAL 全表重扫

- **Module**: src/qgis（qgisconstrainteditsession，#496 F4 新路径）
- **File:Line / Function**: `src/qgis/qgisconstrainteditsession.cpp:79-106` `storeFidIndex()` / `:60-77` `storeMaxFid()`（GDALOpenEx + 约束表逐行全扫）；`:146-176` `snapshot()`——每个 `added` 要素在 `m_newFids` 已映射时调用一次 `storeFidIndex(&owners)`；`:301-304` `editCommandEnded → synchronize → snapshot`（每条编辑命令必跑）
- **Root Cause**: #496 的「fid 映射逐次复核」对**每个**已映射新增要素独立做一次全表扫描（owners 表只在本要素内复用，循环不共享）。每条编辑命令（含单纯移动一个顶点）成本 = O(新增要素数 × 约束库总行数) 次 OGR feature 读取，全部在 GUI 线程。
- **Trigger**: 老工程约束库跨层位累积数万行 + 本次会话绘制/导入几十条新约束 → 之后每次顶点编辑/撤销都有可感知卡顿（估算 10 新增 × 5 万行 ≈ 50 万次行读/命令，秒级）。
- **Observed vs Expected**: 逐次复核的占用表每次 snapshot 只需扫一遍全表（一次 `storeFidIndex` 结果供本 snapshot 全部新增要素复用），现在是每要素一遍。
- **Impact**: 编辑手感劣化；无正确性问题（复核本身是 #496 修的正确性守卫）。
- **Existing Match**: NEW（文件不在活跃改动清单）。
- **Fix Strategy**: `snapshot()` 内先做一次 `storeFidIndex`（仅当存在任一 known 新增要素），循环内复用 owners/maxFid。
- **Regression Test**: tst_constraintediting：会话中新增 N 条后连续 M 次顶点编辑，用 GDAL 打开计数（或计时上限）断言全表扫描次数 ≤ O(1)/命令。

### D-06 | P3 | VERIFIED | factorcontour 用户可见错误串英文裸字面量、无 tr()（与 #496 中文新串混用）

- **Module**: src/qgis（factorcontour）
- **File:Line / Function**: `src/qgis/factorcontour.cpp:47,52,61,67,102,109,123,160,165,178,192,200,220,228,238,292,302` 等——`setError(error, QStringLiteral("contour levels are empty"))` 等 ~20 处英文裸串；对照 `:84`、`:297` 同文件新加的中文裸串。全文件 `tr(` 仅 1 处。
- **Root Cause**: 错误经 `QString *error` 出参 → workflow → `errorOccurred`/statusBar 直接上屏（如 `paleomainwindow_workbench.cpp:1106`），但 `tools/check_i18n.py` 只扫控件文案入口（setText/addAction 等），错误出参链路不在门禁内 → 中英混出且都不走翻译机制。DESIGN.md Typography：「全部用户可见字符串走 tr()/翻译机制（§41.5 i18n 规则）」。
- **Trigger**: 等值线任何失败（栅格缺失/层创建失败/GDAL rc≠0）→ 状态栏出现英文句子；同屏其他错误是中文。
- **Observed vs Expected**: 期望中文源串 + tr()。实际英文裸串（存量）与中文裸串（#496 新增 `:84/:297`）并存。
- **Impact**: i18n 违约 + 用户可见语言不一致；翻译抽漏。
- **Existing Match**: NEW（前轮审计未点名此文件错误串；门禁盲区属门禁设计选择「存量清算归属主方向」——但 #496 又在同一文件新增了两条中文裸串，扩大存量）。
- **Fix Strategy**: 全文件错误串改 `QObject::tr()`（中文源串，与 2026-09-29 口径一致）；check_i18n 增加 `setError(\s*\(\s*error` 首参裸字面量的扫描规则（可带 baseline）。
- **Regression Test**: check_i18n selftest 加 setError 模式正反夹具。

### D-07 | P3 | VERIFIED | 影响分析报告落盘失败静默 + 状态栏「（详见 ）」空参残句

- **Module**: src/ui（paleomainwindow_workbench）× workflow（mappingworkbench_impact）
- **File:Line / Function**: `src/workflow/mappingworkbench_impact.cpp:357-368`（`QDir().mkpath` 或 `file.open` 失败 → 无 report_path、无任何告警）；`src/ui/paleomainwindow_workbench.cpp:812-817`（`tr("...（详见 %4）").arg(..., m_lastImpact.value("report_path").toString())`）
- **Root Cause**: 报告 JSON 是「分析快照固定版本对」的留档承诺（文件头注释），写失败只表现为 out 里没有 `report_path`；UI 端无条件引用 → 用户看到「…可确定变化 N 像元（详见 ）」；且重算（G09）的 `plan["impact_report"]` 指向空值，审计链断链无声。
- **Trigger**: 工程目录只读/盘满/杀毒占用 → 点影响分析 → 状态栏残句 + 无报告，无 warning。
- **Observed vs Expected**: 期望落盘失败至少进 ErrorHub/消息日志；UI 端 report_path 为空时改写文案（「报告未能保存」）。
- **Impact**: 轻微（文案 + 审计链），但与「真实失败记账」的 opt-05 主旨相悖。
- **Existing Match**: NEW。UNCERTAIN_OVERLAP：paleomainwindow_workbench.cpp 有活跃未提交改动。
- **Fix Strategy**: analyzeImpact 落盘失败时 `out.insert("report_error", ...)`；UI 判空换文案。
- **Regression Test**: tst_mappingworkbench：只读目录下 analyzeImpact → status/message 含「报告未能保存」且 unresolved 记账。

### D-08 | P3 | HIGH | 自定义绘制序回填：悬垂指针集合跨事件循环 + flush 不复查 hasCustomLayerOrder

- **Module**: src/ui/layers（layertreepanel，opt-05 新增）
- **File:Line / Function**: `src/ui/layers/layertreepanel.cpp:465-510` `queueCustomOrderSync()/flushCustomOrderSync()`；`m_pendingOrderLayers`（`QSet<QgsMapLayer*>` 裸指针，头文件 `:129`）
- **Root Cause**: 两个小缺陷叠加：① 排队到 `QueuedConnection` 的 flush 之间集合持有裸 `QgsMapLayer*`——指针值比较本身不触垂（注释已自知），但地址复用（旧层销毁 + 新层同址分配）会把无关新层误判为 pending 并改写其 customLayerOrder 位置；② `flushCustomOrderSync` 不再检查 `root->hasCustomLayerOrder()`——排队后用户关闭自定义序，flush 仍改写 customLayerOrder 列表（当前画布忽略它，属潜伏态污染，重开自定义序时显现）。
- **Trigger**: 拖拽图层后瞬间删除该层再快速新增图层（低概率 ABA）；或拖拽后立刻在图层面板关掉自定义绘制序。
- **Observed vs Expected**: 期望 flush 前复查开关；pending 集合用 `QPointer` 值比对或随图层 destroyed 即时摘除。
- **Impact**: 低频、后果轻（图层绘制顺序意外变更，无崩溃）。
- **Existing Match**: NEW（opt-05 S5 复审未列）。
- **Fix Strategy**: flush 开头加 `if (!root->hasCustomLayerOrder()) { m_pendingOrderLayers.clear(); return; }`；pending 改 `QSet<QPointer<QgsMapLayer>>` 或 connect destroyed 摘除。
- **Regression Test**: tst_layertree（既有 opt-05 测试位）：排队后关自定义序 → customLayerOrder 不变。

### D-09 | P3 | VERIFIED | 受控重算「已启动 X/N」吞掉逐条失败原因

- **Module**: src/ui（paleomainwindow_workbench）
- **File:Line / Function**: `src/ui/paleomainwindow_workbench.cpp:823-845`（`recompute` handler：`executeRecompute` 返回 results 中 `ok=false` 条目的 `error` 字段只被计数，不上屏不入账）
- **Root Cause**: `executeRecompute`（`mappingworkbench_impact.cpp:498-553`）在任务模式只启动首条，其余条目 ok=false 携带「任务模式下逐条重算：请等待上一条完成后再次执行」或各条真实启动失败原因；UI 只显示「已启动 %1/%2 项」，逐条 error 既不进页面消息也不进 ErrorHub——「真实失败记账」缺口（同 PR #491 主旨）。
- **Trigger**: 多条目重算计划（≥2 个下游 composed 成果）→ 一条启动失败或被单飞规则拒绝 → 用户只见 X/N，不知哪条为何没跑。
- **Observed vs Expected**: 期望逐条失败原因进状态栏/页面提示/任务面板。
- **Impact**: 可诊断性；用户需盲试第二次。
- **Existing Match**: NEW。UNCERTAIN_OVERLAP（paleomainwindow_workbench 活跃改动文件）。
- **Fix Strategy**: handler 中收集失败条目 `{title: error}` 摘要附在消息尾或入 ErrorHub warning。
- **Regression Test**: tst_mappingworkbench：双条目计划 + 首条 busy → 消息包含第二条失败原因。

### D-10 | P3 | HIGH | 每次图斑选择重建相点空间索引（GUI 线程，逐点击重复付费）

- **Module**: src/qgis（composeattribution）× workflow（evidenceinteraction 触发链）
- **File:Line / Function**: `src/qgis/composeattribution.cpp:90-164` `decideAt()`——`QHash<int, PointIndex> pointIndexes` 是 `replay()`/`replayGrid()` 调用局部量，相点输入的 `QgsSpatialIndex` + 全要素 `getFeatures()` 遍历每次调用从零构建；触发链：画布 selectionChanged → `EvidenceInteraction::capture`（`evidenceinteraction.cpp:110,128-141`）→ `featureEvidence` → `composeAttribution` → `replay()`（maxSampledCells=4096 有界，但索引构建 O(全点数) 无界）
- **Root Cause**: 采样上限护住了评估循环，没护住「每次选择都全量重建输入索引」：井相点层数千~数万点时每次点选图斑都全表读 + 建索引（GUI 线程，几十~几百 ms/次）；与 D-03 同一 `attributionLayer` 邻域（图层缓存了，索引不缓存）。
- **Trigger**: 综合相图（含相点输入）上连续点选/框选图斑查看证据 → 每次选择卡顿一拍。
- **Observed vs Expected**: 期望索引按（版本 id）缓存随图层存活（与 attributionLayer 同生命周期），或选择路径降采样。
- **Impact**: 选择交互迟滞，数据规模越大越差；无正确性问题。
- **Existing Match**: NEW。
- **Fix Strategy**: PointIndex 挂到图层缓存侧（keyed by versionId，随 attributionLayer 驱逐一起失效）。
- **Regression Test**: tst_mappingworkbench：同层两次 composeAttribution 只建一次索引（计数器或耗时上限）。

---

## 已核验为既有持有/活跃工作（不重复报，仅记录状态）

| 项 | 状态 @ c720aef5 | 归属 |
|---|---|---|
| `layers.remove` Delete 键 WindowShortcut 遮蔽画布顶点删除（综合审计 UI-P0-01） | 仍未修：`shortcutcatalog.cpp:247` 默认 WindowShortcut；新增了「正在编辑拒绝删除」守卫（layertreepanel.cpp:217-229）但**按键仍被快捷键吞掉**，顶点编辑时 Delete 不达 `vertexeditortools.cpp:929` | 前轮综合审计（去重） |
| decorations `setDark` 零调用（暗色下画布装饰浅色 token） | 未修 | #481 |
| previewmapcanvas 裸 QgsMapLayer* 键 | 未修（另有活跃未提交改动） | #483 + EXCLUDED_ACTIVE_WORK |
| attach_shell 裸 QMessageBox | 未修 | #481 |
| Qgs* 越层 / attachWorkbench 巨函数 / layering 闸 / 视图规模 | 未变 | #472/#473/#474/#502 |
| #493 取消弹「未完成」告警、全 nodata 发布（opt-04 U4/U5） | 已由 66e79e61 修复（顺序摘除 m_request / definedCells==0），本审计复核改动未见新缺陷 | 已合并修复 |

## 审查中确认无问题的重点（负面结果摘要）

- `composeattribution` 重放核心与 `composeRaster` 语义对齐（网格规则/优先级/阈值/最近邻/拒收编码逐条一致）；`thresholdsValid` 防越界索引；`replayGrid` 有 4M 上限与 batch 块读。
- `TransientLayerStyleGuard`（#491）：快照/还原/QPointer 防垂/双失败路径（提前 return 与布局析构）均正确；`buildHorizonMapLayout` 调用方 `unique_ptr` 接管、导出走 XML 快照重建（无异步裸指针）。
- `layertreepanel moveCurrentNode` 树序→自定义序回填、`qgislayerprofile` 主题写回目标修正（不串页）、`qgislayerorganizer` declared() 快照共享：逻辑正确。
- #487 `wellFaciesRenderer` 层位分组 + 编码集限定 else 语义保持。
- opt-02 fencewidget 落库失败记账/提示行防增长、wellsectionpanel 地震缝保全、opt-03 词表/parseLevels 上限/字段缓存键（size+mtime）：未见缺陷。
- `qgisconstrainteditsession` 会话生命周期（writeMapLayer 改写 datasource、destroyed 断链、finish 双失败路径、G05 constraintsSaved 指纹对比语义字段）正确——除 D-05 的性能面。
- previewmaptools/previewrendercache/previewidentify：连接全部带接收者、LRU/磁盘上限齐备、`toolDisplayName` 无生产消费者（硬编码中文不上面）。
- 状态栏 mono 数字 token、factorstylewriter 双描边/等值线注记规格逐项符合 DESIGN.md《单因素地图符号》节。
- src/ui 全量 `connect` 扫描：无「捕获 this 无接收者」的悬垂 lambda；`QTimer::singleShot` 全部带 context。

## 覆盖清单

src/qgis（重点深读）：composeattribution.*、constraintcrs.*、qgisconstrainteditsession.*、qgiseditingservice.*、mappingartifactwriter.cpp(composeRaster)、layoutexport.cpp、qgislayerorganizer.cpp、qgislayerprofile.cpp、qgisstyleservice.cpp(#487)、factorcontour.cpp、factorstylewriter.cpp、previewmaptools.cpp、previewrendercache.cpp、layervocabulary.h
（对照审：previewmapcanvas/previewidentify/previewrasteranalysis/qgislayerservice——活跃改动文件，仅核对既有已知项）

src/ui（重点深读）：pages/mappingworkbenchpage.cpp、pages/constraintpage_{constraints,sections,wellfactors}.cpp + internal.h、pages/composepage.cpp、pages/datagraphpage.cpp(#489 diff)、paleomainwindow_workbench.cpp、paleomainwindow_attach.cpp、paleomainwindow_attach_shell.cpp、paleomainwindow_attach_compose.cpp、paleomainwindow_attach_mapping.cpp、paleomainwindow_pages.cpp、paleomainwindow_status.cpp、layers/layertreepanel.cpp、layers/layerpropertiesdialog.cpp、wellsection/{fencewidget,wellsectionpanel}.cpp(#490 diff)、sequenceframeworkpanel.cpp(#490 diff)、releasepanel.cpp、pages/validatepage.cpp、shortcuts/shortcutcatalog.cpp、edittools/vertexeditortools.cpp（Delete 键链路）
（扫描面：src/ui 全部裸 QMessageBox/QTimer::singleShot/connect 模式 grep）

触发链跨层核对：workflow/{mappingworkbench_impact,mappingevidencereview,evidenceinteraction,surfacegridding}.cpp、services/evidenceindex.cpp（只用于根因定位，根因归属见各条）
