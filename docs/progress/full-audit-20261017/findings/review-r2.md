# Review 2（独立复审）— full-audit-20261017 Fix 1 批次

- 复审者：R2（未参与修复，只读，未构建）
- 对象：`git diff c720aef5...HEAD`，仓库 `/home/kevin/projects/paleo_workstation-full-review-20261017`
- 提交数：实际 **17 个**（任务书写 15，实际 F1–F17 各一提交，含 F3 排序在 F1 前）；已逐一全文审阅
- 方法：独立数学推导、与权威实现逐参数对拍、全调用点 grep 核对、静态核验黄金锚数组完整性、运行只读 `tools/check_layering.py --strict`（通过：无层违规）

## 结论总览

17 个修复中 13 个根因修复正确且质量高（F1/F2/F3/F4/F5/F7/F8/F11/F12/F13/F14/F17 数学与语义经独立验证成立，F6 门控逻辑正确）。发现 **0 个 MUST_FIX**、**4 个 SHOULD_FIX**（1 个为修复覆盖缺口、1 个为测试缺失、1 个为还原次序缺陷、1 个为流程/合并协调）、其余为 DOCUMENT_ONLY 级记录。总体判断：**可进入 Fix 2/PR 阶段，但 SHOULD_FIX 项建议随 Fix 2 一并处理并在 PR 描述注明协调事项**。

---

## 逐条意见

### R2-01 | SHOULD_FIX | F1 (d455a9f8) | AttributionPin 覆盖缺口：compareVersions 持双裸指针未加 pin

- File:Line：`src/workflow/mappingworkbench_impact.cpp:562-570`（`MappingWorkbench::compareVersions`）
- 问题：`auto *before = qobject_cast<QgsRasterLayer *>(attributionLayer(oldVersionId)); auto *after = ... attributionLayer(newVersionId);`——两次调用之间/之后持有两个裸指针，函数内**无 AttributionPin**。缓存已有 ≥8 个不同版本时打开 `newVersionId`（第 9 个）会任意驱逐，命中 `before` 即 UAF——与 C-01/D-03 完全同构。
- 证据：全调用点 grep 核对结果——已 pin：composeAttribution(:319→:345)、composeAttributionAt(:409→:431)、analyzeImpact(:175→opener :262、:306)、recomputePlan(:382→:439)、executeRecompute(:513)；未 pin：compareVersions(:569/:570)。讽刺的是 `evidenceReverseQuery`（:652）加了 pin 但其函数体**根本不调用 attributionLayer**（只用 m_evidenceIndex），且其 pin 注释「before/after 两个栅格图层同时持有期间不得驱逐」描述的恰是 compareVersions 的处境——高度疑似作者想 pin 的是 compareVersions、pin 错了函数。
- 缓解：当前 `compareVersions` 唯一调用方是测试（`tests/tst_impactrecompute.cpp:195`，src/ui 无接线——UI 的 "compare" action 走另一 helper）。故为潜伏而非现行生产 UAF。
- 建议：compareVersions 函数头补 `const AttributionPin pin(this);` 一行；evidenceReverseQuery 的 pin 可留（无害）但注释应改。

### R2-02 | SHOULD_FIX | F9 (41b1fd6a) | 缺计划承诺的回归测试

- File:Line：`src/io/geojsonaffine.cpp:127-137, 212-221`
- 问题：闸本身正确（500MB 与 LAS 家族同口径、两入口齐、错误串与文件既有 QStringLiteral 风格一致、registration.cpp:107 的 fail(terr) 提前返回不落派生），但 05-priority-and-fix-plan.md 第 8 行明确承诺「tst_import_geoaffine：超限拒绝且不落派生」，该 commit 只改 geojsonaffine.cpp，零测试。闸与参数（500MB 边界、恰好等于时放行）无任何钉住。
- 建议：补一条用例（造 >500MB 稀疏文件断言拒绝 + 等于上限放行），随 Fix 2 落。

### R2-03 | SHOULD_FIX | F4 (e7dcf569) | restoreFileFromBackup 删除次序：主文件先删，-wal 失败时现场已失

- File:Line：`src/io/constraintstore.cpp`（`restoreFileFromBackup` 的 for 循环：`{QString(), "-wal", "-shm"}` 顺序）
- 问题：循环先删主文件再删 -wal/-shm。若主文件 remove 成功而 -wal remove 失败（被占用），函数返回 false，但**现网主文件已被删**，唯一副本剩备份——错误信息「无法移除待还原文件…（文件被占用？）」未反映主文件已不在。错误路径套错误路径的低概率场景，但次序反过来（先 -wal/-shm 后主文件）即可把「部分拆除」窗口消掉，零成本。
- 已核对其余部分正确：备份先验证后删除（缺失不动现场 ✓）；事务连接 commit 后 GDALClose+置 null（Transaction dtor 对 null 跳过、committed=true 无回滚，无双关 ✓）；两条 restoreBackup 调用路径（SRS 未生效 / 逐行校验失败）到达时连接均已关（ds.reset() 在前 ✓）；独占检查点 busy==0 判定读的是 PRAGMA 结果集首列（正确）且移到了事务外（旧代码在事务内跑 checkpoint 本就无效，顺手修正）。
- 次要：还原成功后未按 pre 快照复验（A-01 修复策略建议「还原后再验一遍」未做，仅尺寸比对）；`PRAGMA wal_checkpoint(TRUNCATE)` 理论上可阻塞至 busy 超时（GUI 线程可感，非正确性问题）。
- 建议：循环顺序改为 `-wal`、`-shm`、主文件；（可选）还原后 reopen 抽查首行。

### R2-04 | SHOULD_FIX（流程） | F6 (9d8a9a95) | 触碰主 checkout 未提交硬排除簇内文件，PR 未按 B-04 先例注明协调

- File:Line：`src/ui/paleomainwindow_workbench.cpp`、`tests/tst_mappingworkbench.cpp`
- 问题：01-conflict-matrix.md 把这两个文件列入「codex/audit-remediation 未提交改动（主 checkout）→ EXCLUDED_ACTIVE_WORK 硬排除」，本分支 F6 直接改了它们（根因所在，register 有意识收录，但台账/PR 注记未像 B-04/F7 那样声明）。实测主 checkout 未提交 hunk：`paleomainwindow_workbench.cpp` 仅 ~:307 一行、`tst_mappingworkbench.cpp` 在 ~:2673 文件尾追加 95 行；本分支 hunks 在 ：1-4/:784-880 与测试中部 ~:460——**当前不重叠，自动合并大概率成功**，但主 checkout 会话继续写入即可能撞。
- 建议：PR 描述注明这两个文件与主 checkout 未提交簇的 hunk 位置与合并顺序（与 F7/B-04 同款声明）；或 push 前与持有会话对齐。

### R2-05 | DOCUMENT_ONLY | F12 (57b67beb) | 均值分母正确，但 NaN 以 "nan" 字样进中文状态串

- File:Line：`src/workflow/inversionworkflow.cpp:613-628`；消费方 `src/ui/seismicsection/seismicsectiondock_inversion.cpp:268-269`
- 核验：done 每道恰 +1、failed ⊆ done（:585-586 同一迭代先 failed 后 done），succeeded = done−failed 严格等于成功道数——分母修正正确。`processedTraces` 字段本身不变，消费方（dock 显示、tst_inversion_* 断言）不受影响。
- 记录点①：全失败时均值置 NaN，dock 以 `'f',3` 格式化 → 用户看到「平均残差能量比 nan」——英文裸字样混入中文串（DESIGN.md §41.5 精神），建议消费侧判 NaN 换「无成功道」。
- 记录点②：无小回归测试（同 F13/F14，见 R2-08）。

### R2-06 | DOCUMENT_ONLY | F1 (d455a9f8) | pin 期间缓存上界=会话输入数，事后不回落

- File:Line：`src/workflow/mappingevidencereview.cpp:293`
- 评估：会话内缓存按 ordered_inputs 不同版本数无上限增长（每条 = 一个打开的 GDAL 数据集 + QgsMapLayer）；会话结束后不收缩——后续未 pin 的打开只「逐 1 换 1」，缓存长期停留在历史最大会话水位，直到 bindCatalog 重置。UI 输入数无上限（几十常见）。commit message 已自述「工作集按会话输入数有界」，属有意取舍。
- 建议（后续轮）：会话结束（pin 归零）时裁剪回 8，恢复常驻上界。

### R2-07 | DOCUMENT_ONLY | F6 (9d8a9a95) | 同步等待提示不会真正绘制；override cursor 无异常安全

- File:Line：`src/ui/paleomainwindow_workbench.cpp`（impactAnalysis/recompute handler）
- 记录①：`statusBar()->showMessage("正在分析影响…",0)` 后立即进入同步阻塞调用——无事件循环处理，状态栏文字大概率在阻塞结束被下一条 message 覆盖前从未重绘（WaitCursor 本身不走事件循环、确实生效）。提示诚实事倍功半，根因（D-02 异步化）已知递延。
- 记录②：`setOverrideCursor`/`restoreOverrideCursor` 非 RAII 配对，analyzeImpact 若抛异常（bad_alloc 族）光标卡死等待态。
- 记录③：异步化递延只在 commit message 里说「在 full-audit 台账记剩余工作」，但 09-loop-ledger.md 的 Remaining Work 只有 F18–F21 与流程步骤——D-02 异步任务化未入台账，应补记。

### R2-08 | DOCUMENT_ONLY | F13/F14/F12 | 计划承诺的「各自小回归」未落

- File:Line：计划表第 11 行「F11–F17 P3 快胜 … 各自小回归」。
- 实际：F15（测试自身）、F16（脚本）、F17（既有 redoSurvivesFidReuseByOtherWriter 照跑，语义已静态复核等价）、F10（ctest 清单即验证）、F11（纯死写删除，无需测试）达标；**F12/F13/F14 三个行为修复零新测试**（F13 的 nodata=1e30 用例、F14 的 reportBytes(120,100)→percent==100 用例都是几分钟能写的）。行为改动无钉住，防回升缺一格。

### R2-09 | DOCUMENT_ONLY | F5 (9ede75c1) | 旧域注释残留半开措辞

- File:Line：`src/workflow/surfacegridding.cpp:486-493`（context.domain 注释）
- 记录：注释仍写「x 左闭右开…与像元判定 cellOf 同口径」——cellOf 已被本提交删除，supercover 是闭域+clamp，注释与新实现相反（计划/引擎两侧现在一致，注释是唯一残留反话）。建议随 Fix 2 改三行注释。
- 其余全部核验通过：参数映射逐项与权威实现对拍（originYTop=geom.originY〔GridGeometry 注释：originY=左上角、dy>0 行向−y〕、pixelWidth=geom.dx>0、pixelHeight=−geom.dy 满足实现内 `!(pixelHeight<0)` 守卫、visit(col,row) 与 r*cols+c 及 paleoalgorithms.cpp:565 既有调用完全一致）；测试几何独立复算（planeScatter 散点 0..400 步长 100、cell 50 → 8×8 恰 [0,400]²，LINESTRING(400 0,400 400) 在 x==xMax：旧 floor 落格外面恒红、新 clamp 进末列而绿）——修复前真红。

### R2-10 | DOCUMENT_ONLY | F8 (ff9581e3) | dedupeSamples 次键注释与实际键不完全相符（无害）

- File:Line：`src/algorithms/geostat/neighborhood.h:59-61`
- 记录：注释说次键是「输入下标」，实际 `sorted` 携带的是 finite 过滤后数组的下标——因过滤保序，确定性等价，仅注释不精确。
- 独立核验通过项：① 严格全序成立——dedupeSamples 在排序前已滤非有限 x/y/value（NaN 不入比较器；sgs3.cpp:41-47 同），邻域中的已模拟点坐标来自有限网格坐标；唯一残余等价类是「静态样本与已模拟格坐标逐位相同」——此时两者值经 D5 特判相等，选取谁对克里金输入无差异，良性。② 黄金锚再生自洽：kGolden 数组 2048 项（QCOMPARE 钉 2048）、正则抽取无残留杂字符（逗号/结构完整）、min=272826356/max=1000000000 合理、±1 量子容差与量化口径未动、再生注释在位；邻域取值差异远大于 1 量子，锚能钉住跨实现漂移。③ 全 geostat grep 无第四处 d²-only 比较器（cokriging 排的是下标、queryNearest 堆是 (d²,index) pair 天然确定）。

### R2-11 | VERIFIED-OK | F2 (e467dfa9) | 雅可比独立推导成立

- 独立推导：T=(2000/k)ln(1+kz/v0)，∂T/∂k=−2000/k²·ln(1+kz/v0)+2000·(z/v0)/(k(1+kz/v0))=−2000/k²·ln(1+kz/v0)+**2000z/(k·v)**，v=v0+kz——与修复后代码一致；∂T/∂v0=−2000z/(v0·v) 与代码一致；k→0 泰勒极限 ∂T/∂k|₀=−1000z²/v0² 与守卫分支一致。
- 数值例独立复算：v0=2000,k=0.5,z=1000 → 真值 −1785.15+1600=−185.15（commit 称 ≈−185 ✓）；旧代码第二项 2000·1000/(2000·2500)=0.4 → ≈−1784.75（commit 称 ≈−1785 ✓）。
- 测试设计合理（oracle 网格 81×201 粗搜 + SSE≤oracle×1.02 + 八方向局部极小）；测试内 sse lambda 与模型 T 公式逐项一致、无噪真值构造（expm1 逆变换）正确；无噪不动点不受雅可比影响的论证成立 → 修复前该用例红。

### R2-12 | VERIFIED-OK | F17 (85b4263b) | 单次扫描与旧实现语义恒等

- 核验：storeFidIndex 的 maxFid 与 storeMaxFid 用同一迭代同式计算（含 layer 缺失返 0、ds null 返 −1 的边界都一致）——known/unknown 混合时旧实现各扫各的、新实现一次扫描复用，因 snapshot 循环内无 store 写入（GUI 线程单线程），两扫描的数据恒等，判定（m_nextFid 推进、occupied 让位）逐行比对不变。无新增要素时不扫（与旧一致）。storeMaxFid 非死代码（:229 仍在用）。并发窗口下新实现反而给出一致快照。

### R2-13 | VERIFIED-OK | F3 (f250782f) | excludeId 最小且正确

- 核验：markStaleDownstreamOf 全仓唯一生产调用方是 addVersion；闭包按 parentVersionIds 反查必然把新版本卷入（versionAdded 先注册边），排除 v.id 后，其余「以被取代旧版本为祖先」的 DERIVED 仍正确标 stale；空 excludeId 默认参数保持兼容；undo 栈不含被跳过者，save 失败回滚一致。测试夹具与 A-03 场景逐字段对应（同资产 RAW v1→DERIVED v2(parent=v1,number 2)），修复前必红。
- 遗留（记录）：计划中「assetops 回滚断言」未加（addVersion 级测试已覆盖根因）；存量工程里已被错误标 stale 的历史版本无清洗（可接受，等自然 supersede）。

### R2-14 | VERIFIED-OK | F6 (9d8a9a95) | 门控逻辑与既有用例无冲突

- 核验：reverseNeedsHorizon 豁免清单（search/references_source）与 evidenceindex 的谓词（pending_review/contradicted/stale_sources 按 horizon，references_source 按 feature uid，search 全文）及 m_reverseKind combo 的 addItem data 值（:443-447）逐项吻合；impactAnalysis 的 id.isEmpty() 早退与 gate 的 selected 条件同口径；recompute 的 m_impactReady 回喂双向（成功/失败）测到；`busy`/`selected`/`horizon` 均在 updateState 作用域。tst_mappingpages 无三按钮断言、tst_mappingworkbench 仅新用例引用（grep 证实）——无既有用例破坏。设计合规：禁用带 reason tooltip（§35）。

### R2-15 | VERIFIED-OK | F7 (98434a26) | JoinWorkers 覆盖全部异常/早退路径

- 核验：守卫在 workers 声明后、派发循环前——主线程 runRange 抛异常时栈展开触发析构 join；worker 内 runRange 逐列查 `!cancelled()`（:754），无死锁构型；正常路径显式 join 后 joinable()==false，析构 no-op；`return QString()` 取消早退发生在显式 join 之后。全仓 grep 确认仅剩第四处 `inversionworkflow.cpp:594`（EXTERNAL_OWNED，见 R2-17）。

### R2-16 | VERIFIED-OK | F10/F11/F13/F14/F15/F16 快胜复查

- F10（63cfa885）：入 `_paleo_perf_tests` 清单即同时获 LABELS perf + RUN_SERIAL，与 E-01 建议逐字一致，阈值未动。
- F11（33e005e1）：删除的确实是首写死代码（随后 insert 同键覆盖），manifest_layer_id 别名语义保留，零行为变化。
- F13（70378c2b）：`static_cast<float>(noData)` 与 #165 口径一致；nodata 为 DBL_MAX→float=inf 时 isfinite 已挡，安全。
- F14（7e02ede6）：qBound(0,…,100) 钳制正确（done*100 qint64 乘法无溢出）；etaText remaining≤0 → "--" 诚实。
- F15（49c2d244）：改名与断言一致；域注释 [-10,110] 与 paleoalgorithms.cpp:533-535 的 10% pad 复算吻合；LINESTRING(-10 0,-10 100) 恰在 x==xMin，colOf(-10)=floor(0)=0 落首列，sawLeft 入断言组——四边缘闭环。
- F16（a1198e76）：mktemp 替换正确，失败路径打印实际 log 路径。

### R2-17 | EXTERNAL_OWNED | inversionworkflow.cpp:594 裸线程池异常路径未 join

- 与 B-04 同根因的第四处，明示归 codex/audit-remediation（冲突矩阵 + 任务书已知背景），本分支 F12 与其不同 hunk 无冲突面（F12 commit 已注明）。仅记录，不计入本分支问题。

### 分层与 API 面（R2-E 汇总）

- `tools/check_layering.py --strict` 实跑通过（无违规、baseline 不变）。workflow→algorithms include（surfacegridding.cpp 新增 `../algorithms/constraintbarriers.h`）合法（功能层依赖数据层，同文件既有 rasteralgebra/surfacevolumes 同款相对路径写法）。
- 新公共 API 最小性：AttributionPin（私有嵌套）✓；`attributionLayerCacheCountForTesting()`（public、ForTesting 命名、内联一行）✓ 可接受；`restoreFileFromBackup`（public static，生产 restoreBackup lambda + 测试两用）✓；`setImpactAvailable`（生产回喂）✓；`excludeId` 默认参数 ✓。无过度暴露。

## 总体判断

修复批次整体质量高：两个 P1 数值/生命周期问题（F2/F1 主路径）修复经独立验证成立，测试真实（修复前红）。发现的缺口集中在「修复覆盖的最后一处」（compareVersions）、「承诺测试未落」（F9/F12/F13/F14）与「还原次序/流程协调」四个 SHOULD_FIX，均不阻塞 Fix 2 启动，建议随 Fix 2 收口并在 PR 描述写明 R2-04 的协调事项。
