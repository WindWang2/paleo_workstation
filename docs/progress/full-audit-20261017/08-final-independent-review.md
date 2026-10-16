# Final Review（终审）— full-audit-20261017 Fix 1 + Fix 2 批次

- 终审者：FR（独立终审；未参与修复与前两轮审查）
- 对象：`git diff c720aef5...HEAD`（19 提交 = 15 修复 + 1f1b1d15 R2 收口 + 2 docs），仓库
  `/home/kevin/projects/paleo_workstation-full-review-20261017`
- 方法：全量 diff 逐文件审阅 + 关键修复独立推导/逐参数对拍 + 全调用点 grep + 只读脚本
  （`tools/check_layering.py --strict` 实跑）+ `git fetch origin` 后对照远端 + 极端输入静态推演。
  未构建、未运行动态测试（任务书约束）。

## 总体结论：PASS_WITH_NOTES

19 个提交全部审阅。15 项修复（F1–F17）与前两轮（主审 + R2）结论**未被推翻**：
终审独立复核了 F2 雅可比（重新推导 ∂T/∂k = −2000/k²·ln(1+kz/v0) + 2000z/(k·v)，与代码一致）、
F1 pin 覆盖完整性（穷举 attributionLayer 全部 7 个调用点，均在 pin 作用域内）、
F4 checkpoint/还原次序/连接关闭全 return 路径、F5 参数映射（对照 constraintbarriers.cpp
实现守卫 `pixelHeight<0` 与 paleoalgorithms.cpp:565 引擎调用逐参数一致）、
F17 单扫描语义恒等。**未发现 P0/P1/P2 级新问题**。新发现 7 项全部 P3：
2 项 SHOULD_FIX（一处一次性 GDAL feature 泄漏、一处格式残疵），5 项 DOCUMENT_ONLY/流程注记。
另 1 项挑战被证伪（deleteLater 质疑不成立）。

## 新发现问题列表

| ID | Sev | 分类 | File:Line | 问题与证据 |
|---|---|---|---|---|
| FR-01 | P3 | SHOULD_FIX | `src/io/constraintstore.cpp:721-722` | F4 新增 checkpoint 块中 `OGR_L_GetNextFeature(rows)` 返回的 feature 只读未 `OGR_F_Destroy` —— 每次 migrateLegacyCrs 尝试泄漏一个 OGRFeature。同文件既有惯用法（:781/:794/:853 的 PRAGMA 结果）一律 `Feature f(raw, OGR_F_Destroy)` RAII 包裹，本处违例。新测试 `tst_singlefactor_store.cpp` 的 `mode` 结果集 feature 同病。一次性小泄漏，不阻塞。 |
| FR-02 | P3 | SHOULD_FIX | `src/workflow/mappingevidencereview.cpp:274` | Fix 2 移动代码时把函数签名与首语句挤到一行：`...attributionLayer(const QString &versionId) const {  if (!m_catalog || versionId.isEmpty())`。合法 C++、CI 无 clang-format 门禁（lint=clang-tidy）不会红，但属明显的编辑残留，破坏文件既有排版，一行可修。 |
| FR-03 | P3 | DOCUMENT_ONLY | `src/io/constraintstore.cpp:703-705`（注释） | checkpoint 关闭与 Transaction 打开之间存在窄窗口：外部进程写者此刻提交 → 帧落 WAL → 事务内字节级备份缺这些帧 → 校验失败还原删 -wal 即丢弃该外部提交。V3 注释「还原不会覆盖并发写者的合法提交」在新实现下对该窗口不再严格成立（迁移是用户触发、checkpoint 时刻已断言独占，概率极低）。注释措辞应弱化或窗口应说明。（`wal_checkpoint(TRUNCATE)` 可阻塞至 busy 超时已在 R2-03 记录，不重复计。） |
| FR-04 | P3 | DOCUMENT_ONLY | `tests/tst_batchjobpanel.cpp:341`（taskProgressClampsOverflow） | `etaText()=="--"` 断言空转：单次 reportBytes → m_etaSamples 仅 1 样本 → m_rateBytesPerSec 恒 0 → etaText 经首个 rate 守卫返回 "--"，新加的 `remaining<=0` 分支未被钉住。需两次间隔 reportBytes 才能测到新分支。`percent()==100` 断言是真钉（旧代码返回 120）。 |
| FR-05 | P3 | DOCUMENT_ONLY | `tests/tst_composeattribution.cpp:335-340` | 用例头注释描述「用 updateVersionExtra 改写 ordered_inputs 为 10 输入」，实际实现走真实 10 输入 compose（更好、更真路径）——注释是早期方案的残文，与实现不符。 |
| FR-06 | P3 | DOCUMENT_ONLY | `tests/tst_singlefactor_store.cpp:296` | migrationAbortsWhenWalCheckpointBusy 在无 sqlite3 CLI 的环境 QSKIP——busy-拒绝路径在该类主机零覆盖（CI ubuntu 镜像自带 sqlite3，故 CI 覆盖；本地无 CLI 时静默跳过）。 |
| FR-07 | P3 | DOCUMENT_ONLY（流程） | 分支协调 | PR #507（codex/fix-open-issues-20261016，2026-10-16 21:09 开 PR，晚于审计文档快照）与本分支同文件 8 个：CMakeLists.txt、sgs.cpp、sgs3.cpp、datacatalog.h、datacatalog_versions.cpp、tst_catalog.cpp、tst_geostat_sgs.cpp、tst_mappingworkbench.cpp。逐 hunk 对照：**无直接文本碰撞**（F8 比较器 ~:253 在 PR507 的 129/179 与 292+ hunk 之间；F3 addVersion ~:106 vs PR507 ~:52；头文件 :435 vs :78），语义可共存；但两个分支都大改 tst_geostat_sgs.cpp（本分支 506 行 golden 再生），合并次序必有一边 rebase。origin/master 未前进（merge-base=c720aef5=origin/master tip），已声明的排除状态仍然准确；冲突矩阵 B 节已含该分支，属已知协调面而非新冲突，建议并入 R2-04 同款 PR 注记。 |
| FR-08 | — | FALSE_POSITIVE（挑战被证伪） | `src/workflow/mappingevidencereview.cpp:265-272` | 「trim 用 delete 而非 deleteLater 会否风暴/不安全」：不成立。图层 parentless（make_unique 无 QObject parent），直接 delete 时 QPointer 立即置空、缓存态即时一致；会话同步无事件循环重入，deleteLater 反而留下「缓存返回已排删对象」窗口。裁剪风暴 = 会话结束 N−8 次 GDAL 数据集 close，GUI 线程、按输入数有界，可接受。 |

## 十项清单逐项结论

1. **整体 diff 复核** — 52 文件全量过目：全部改动均可映射到 F1–F17/R2 处置/测试/docs，
   无无关改动、无调试残留（tst_geostat_sgs 新增 `PALEO_DUMP_SGS_GOLDEN` 环境变量再生机
   属有意工具面、默认空转，附口径注释）。新问题仅 FR-01/FR-02。
2. **已修复 Finding 逐项抽验（挑战导向）** —
   - F1：穷举 `attributionLayer` 调用点（impact:262/306/439/572/573/633 + review:354/440），
     全部位于持 AttributionPin 的函数（analyzeImpact/recomputePlan/executeRecompute/
     compareVersions/composeAttribution/composeAttributionAt），:633 的 lambda 在
     compareVersions pin 内。嵌套 pin 计数正确（analyzeImpact 内再调 compose 系不会提前
     trim）；声明序保证 inputs 先于 pin 析构；异常路径栈展开 pin 析构照常 trim。trim 时机
     （归零裁剪回 8）与 R2-06 处置一致。**挑战未推翻。**
   - F4：checkpoint 在事务外、busy==0 才放行且 fail-closed（rows==null 亦拒）；
     commit 后 `GDALClose(transaction.ds)+置null` 在两条还原路径（SRS 未生效 / 逐行校验
     失败）之前——后者 `ds.reset()` 后才 restore；还原次序 -wal/-shm→主文件（R2-03 已修）；
     备份先验证后动现场。FR-01 泄漏 + FR-03 注释措辞为残余。
   - F2：独立推导第二项 = 2000z/(k·v) ✓；k→0 极限分支未动 ✓。
   - F5：`supercoverPolylineCells(vertices, originX, originY, dx, −dy, cols, rows)` 对照
     实现守卫（要求 pixelHeight<0；GridGeometry 注释 originY=左上、dy>0 行向−y → −dy<0 ✓）
     与引擎调用（paleoalgorithms.cpp:565: originYTop=yMax、−cellSize、visit(c,r)）逐参数
     一致；visit(col,row) 下标 `r*cols+c` 与旧 mask 索引同构。**成立。**
   - 其余 F3/F6–F17 复核同 R2 结论，未发现反例。
3. **文件生命周期** — 图层所有权：cache 持 parentless QgsMapLayer，delete/QPointer 置空/
   bindCatalog 先 delete 后 clear（mappingworkbench.cpp:215）一致。信号连接接收者未变。
   `m_attributionPinned` mutable int 非原子：**证据表明全部归因会话在 GUI 线程**——
   两文件无 QtConcurrent/QThread/processEvents；生产调用方仅 paleomainwindow_workbench.cpp
   处理器（同步）+ 测试（单线程）。bindCatalog 中途打断会话不可达（无重入点）；可加
   `Q_ASSERT(m_attributionPinned==0)` 防御（建议级，不入 MUST）。
4. **跨模块调用** — F5：workflow→algorithms 新 include `../algorithms/constraintbarriers.h`
   与同文件 rasteralgebra/surfacevolumes 同款，功能层依赖数据层合法。F6：
   `setImpactAvailable` 在 busy 下为纯状态位 + updateState，busy 经 predictionBusyChanged
   → updateState(:201) 实时刷新门控；点击→处理器同步派发，无 busy 竞态窗口。
   F17：单扫描对外部并发写者反而给出一致快照（旧实现多次扫描可能撕裂）。
5. **性能热点对照** — F17：无新增要素时不扫（两版一致）；有新增要素时 1 次全表扫描
   替代旧 O(K) 次——纯改善。F1 trim：见 FR-08（deleteLater 不更好；N−8 次 close 有界）。
6. **架构分层** — `python3 tools/check_layering.py --strict` 实跑：**检查通过：无层违规**。
   新 include 方向全部合法（见第 4 项）。
7. **测试有效性** — 抽验 8 个新用例全部真钉（修复前红/空转项已单独标注）：
   velocitymodel 6b（oracle 网格 + 八方向局部极小，带噪必需——无噪测不出雅可比错）✓；
   composeattribution manyInput（真实 10 输入 compose + 会话内 ≥10/释放后 ≤8 双断言，
   修复前确定性红）✓（注释残文 FR-05）；singlefactor_store 两用例（busy 拒绝 + WAL 三件套
   还原，静态推演均能区分新旧实现；CLI 依赖 QSKIP 见 FR-06）；surfacegridding_thread 贴边
   （x==xMax 旧 floor 恒落格外，barrierCells>0 修复前红）✓；mappingworkbench 门控（状态机
   三档推进 + tooltip reason）✓；seismichorizoncluster nodata（1e30f 旧口径恒不等 → 16≠10
   修复前红）✓；batchjobpanel 钳制（percent 真钉，eta 断言空转 FR-04）；
   import_geoaffine 超限（稀疏 500MB+1 读盘前拒、两入口、不落输出）✓。
   无永真断言；夹具走真实生产路径（真实 compose/generateFactor/真实 GPKG/真实 GDAL 栅格）。
8. **PR/Issue 冲突复查** — `git fetch` 后 origin/master 停在 c720aef5（=本分支 base），
   未前进。PR#506（ci-green）重叠仅 CMakeLists.txt/tst_mappingworkbench.cpp（矩阵已声明）。
   codex/audit-remediation 推送态 = 769b8e3d 单提交领先 master，与本分支 9 文件重叠均在
   矩阵 B 节声明内（F7/B-04 已注明）。新增协调面 = PR#507（FR-07），矩阵以分支名声明过、
   但 PR 开启晚于文档快照，hunk 无碰撞，建议 PR 描述补一句。33 个 open issues 无新重叠。
9. **构建产物与仓库卫生** — `git status` 干净（零未预期文件）；无日志/临时文件入库；
   .gitignore 未动；docs 两提交纯文档；09-ledger 已补 D-02 异步化 Remaining（R2-07③ 兑现）。
10. **挑战尝试（静态推演）** — ①10 输入 + 中途 bindCatalog：不可达（会话同步无重入；
   见第 3 项）。②迁移还原二次失败：-wal 删除失败 → 主文件仍在盘、返回如实错误，库处
   已提交一致态（无嵌合——嵌合需旧主文件+遗留 WAL 组合，此处主文件未被换）；
   copy 失败（盘满）→ 主文件缺、备份在、错误串指位，与旧行为同窗、诚实。③SGS 并列 +
   maxPoints 边界：比较器严格全序（无 NaN 入比较器），唯一残余等价类（静态样本与已模拟
   格逐位同坐标）在 partial_sort 下选中集合不变，良性；golden 2048 项经 QCOMPARE 钉数。
   ④GeoJSON 恰 500MB：`>` 判定放行（与计划「等于放行」一致），测试钉 >500MB 拒。
   ⑤门控×busy 竞态：busy 翻转即 updateState，同步派发无窗口——闭合。

## 处置建议

- FR-01/FR-02：随下一提交顺手修（各一行/两行）；不阻塞本批合入。
- FR-03/FR-05：注释措辞修正，随手。
- FR-04：补第二次 reportBytes 使 eta 分支被钉（可选）。
- FR-07：PR 描述按 R2-04 同款补 PR#507 文件协调声明。
