# 方向 48 审计清账账本（goal/audit-closure-20261006）

基线：origin/master `3a4f8b5`（含审计口径 `adf2be7`）。Linux 本机 superbuild
prefix（`vendor/superbuild/prefix` symlink 自主仓，BUILDING.md「独立 worktree
开发」接线）。构建/测试 -j8 以内，ctest 串行。

## R0：44 项逐条重验（2026-10-05/06，rg+读码证据）

**复核口径教训落实**：审计快照（2026-10-01）与 10-05 三路复核都有漂移，
本表以 origin/master `3a4f8b5` 现码为准逐条重验。已修项尽力给出引入
commit（`git log -S` 反查；PR #193 = merge `0a02a0e`，WIP 提交 `57f0161`
在该 PR 分支上）。

### 仍存在——本方向修复（7 项）

| 项 | 现码证据 | 处置 |
|----|----------|------|
| TEST-07/ARCH-08 | `tests/tst_panels.cpp:4876` 仍 `QTest::keyClick(w, Qt::Key_Tab)` | 修：改 `nextInFocusChain()` 链断言 |
| RUNTIME-04 | `src/services/seismictaskservice.cpp:635` timeout 处理器内同步 `delete timer;` | 修：`deleteLater` |
| BIZ-09 | `src/ui/edittools/vertexeditortools.cpp:1122,1125` `1e-9` 硬编码（另 1177–1180 端点包络同值） | 修：具名常量+量纲注释 |
| ARCH-05 | `src/algorithms/paleoalgorithms.cpp:6,122,124` 仍 include/调用 `DataCatalog::localGridCrsWkt()` | 修：CRS WKT 参数化，算法核零 catalog include |
| TEST-04 | 四处恒真：`tst_decorations.cpp:59`、`tst_wellcomposite_shell.cpp:387`、`tst_layoutexport.cpp:56`、`tst_layoutshell.cpp:88` | 修：换真实不变量 + mutation 验证 |
| MEM-07 | `src/ui/edittools/editingtoolbar.h:150` `QgsMapCanvas *mCanvas` 裸指针 | 修：QPointer 化 |
| ARCH-06 | `src/workflow/mappingworkflow.cpp:466–479` + `src/ui/pages/constraintpage.cpp:985–987` `paleo.thickness.*` 动态属性暗道 | 修：typed 成员/访问器（MappingWorkflow 自有面已有 typed 访问器但回读属性；ConstraintWorkflow 镜像与 ConstraintPage 读取全部转 typed；测试走 `thicknessSample*()` typed 面零改动） |

### 仍存在——归方向 50（io-robustness，7 项，仅记档不动）

| 项 | 现码证据 |
|----|----------|
| BIZ-05 | `src/io/segyreader.cpp` decodeTrace 全文件无 `isfinite`（NaN/Inf 不清洗） |
| BIZ-06 | `src/io/timedeptool.cpp:8,27` 仅 `-99999.0` 哨兵；`src/io/wellfileparsers.cpp:119` 深度解析失败行仍 `tops.append(r)` |
| BIZ-07 | `src/workflow/registration.cpp:73–77` sx/sy/rotDeg 无有限性/零尺度校验 |
| BIZ-10 | `src/algorithms/paleoalgorithms.cpp:365–371` 构造有 try/catch 但 `g.transform(*xform)` 前无 `xform->isValid()` 前置 |
| BIZ-11 | `src/algorithms/welldist.cpp`/`distancetransform.cpp` 无 `GDALSetProjection`/wkt 命中 |
| BIZ-14 | `src/workflow/sectionworkbench.cpp:281–285` 仍只认 `M`/`FT`/`F`，METER/METRE 同义词拒绝 |
| RUNTIME-03 | `src/io/lascache.cpp:240–252` InflightCoalescer 合乘者 `issues` 不回填；`lasdoc.h:22–28` LasDoc 无 issues 字段 |

### 归方向 52（test-deepening，2 项）

| 项 | 现码证据 |
|----|----------|
| TEST-01 | 构建完成后 `ctest -N` 验证 7 套件注册（fresh configure 理论上已解决 build 目录漂移） |
| TEST-06 | 零测试头清单（方向 52 交叉域） |

### 已修（26 项，带证据/commit）

| 项 | 证据 | commit |
|----|------|--------|
| MEM-01 | `constraintstore.cpp:54–56` QPointer<PaleoProjectStore> safeStore | `57f0161` |
| MEM-03 | `paleomainwindow.cpp:363–366` m_horizonFlashTimer 成员化+守卫 | `6845d84`（再入泄漏）+`bcecd04` |
| MEM-04 | `qgislayerservice.h:66` `QHash<QString, QPointer<QgsMapLayer>>` | `0a02a0e` (PR #193) |
| MEM-05 | `paleotaskservice.cpp:180–190` 超时兜底+`delete m_pool` | `0a02a0e` (PR #193) |
| CONC-01 | `seismicsectiondockwidget.cpp:1225,1438,1614,1909` 四处 QPointer guard | `5e3aea1`/`0a02a0e` |
| CONC-02 | `seismictaskservice.cpp:5026–5030` SemaphoreGuard RAII | `0a02a0e` |
| CONC-03 | `cachebudget.cpp:32` `m_evictionZeroCond.wait` + :91–115 evictionScope RAII | `0a02a0e` |
| CONC-04 | `segyreader.cpp:843,849,896` progressMutex 串行化 | `0a02a0e` |
| CONC-06 | `dataimportservice.h:206–240` produce-then-commit 重设计，「没有任何 BlockingQueuedConnection 回 owner 的读写」 | `57f0161` |
| RUNTIME-01 | `paleomainwindow_attach.cpp:2530,2622,2990,3223,3280` + workbench `if (m_projectSvc)` 全守卫 | `aa2e2ba` 波次 + 后续 |
| RUNTIME-02 | `qgisprocessingservice.cpp:334–351` 超时不达即孤儿化（"Do NOT call delete m_feedback"），feedback 为 shared_ptr | `57f0161` |
| ARCH-01 | `CMakeLists.txt:480` paleo_io 链 `paleo_deps paleo_domain paleo_store`（无 paleo_qgis）；io/ 无 qgis include | `57f0161`（layerDeclared 信号） |
| ARCH-02 | `workflows.h:425` `std::unique_ptr<ConstraintStore> m_ownedConstraintStore` | `57f0161` |
| ARCH-03 | CMake 四层 tier 模型（`b451452` "4-tier dependency model (ARCH-03...)"；`paleo_qgis_core_deps` Tier 2） | `b451452` |
| ARCH-04 | `check_layering.py:187,189` `data-qgis-include`/`data-functional-include` 规则 | `57f0161` |
| ARCH-07 | `SeismicSectionTool` 已移至 `src/qgis/seismicsectiontool.*`（QGIS 封装层，QtWidgets 豁免合法） | `0a02a0e`（新增于该 merge） |
| BIZ-01 | `lasparser.cpp:620–707` 动态 nCurves 向量（`col < nCurves`） | `e69ae58`/`34df4ea` |
| BIZ-02 | `lasparser.cpp:482` `out.bomBytes` 扫描期落位，无 4MB peek | `e69ae58` |
| BIZ-03 | `segyreader.cpp:917` `m_index.size() + m_badTraceOffsets.size()` | `0a02a0e` |
| BIZ-04 | `segyreader.cpp:1140–1162` anyDifferent 逻辑 | `0a02a0e` |
| BIZ-08 | `vertexeditortools.cpp:660–685` defense-in-depth 逐环最小数校验 | `eff046e` |
| BIZ-12 | `timedepthmodel.cpp:61–76` empty 检查先行 | `9a9cec1` |
| BIZ-13 | `paleoalgorithms.cpp:650` `d2 < 1e-12` | `09c8db2` |
| TEST-02 | `tst_correlation_full.cpp:2503–2536` offscreen 感知 + 平台自适应预算（回归门语义） | `bc79b6a` |
| TEST-03 | `tst_wellcomposite_depth.cpp:204,214,393` `spy.count()==1`（:407 注释留档原恒真） | `0a02a0e` 系 |
| TEST-05 | `tst_cache_core.cpp:433–434` size≤cap + bytes 关系断言 | `7f7381f` |

### 不复现（2 项，带论证/复现尝试）

| 项 | 定性 |
|----|------|
| MEM-06 | `horizonmarkers.cpp:318–331,431` 重写为「现场查询 `mouseGrabberItem()` + reap 循环逐项豁免 grabber + 无跨帧存指针」（`235ca00` 引入 grabberReused 范式）。审计场景（m_chrome 删除致 grabber 悬垂后再 `grabber->scene()`）不可达：grabber 是本帧现场查询；QGraphicsItem 析构自动 ungrab（跨帧死亡 → mouseGrabberItem 返回 nullptr）；reap 只删带 HorizonMarker 角色的兄弟项（同为 host 子项，不级联）。复现尝试：读码推演 + tst_edittools 69 用例绿（MEM-02 反驳时同套证据）。 |
| CONC-05 | `lascache.h:91` Timings 仍非原子，但消费面全在 GUI 线程：`rg -l LasCache` → crossplotsources（仅 src/app/crossplotcontroller.cpp 装配根调用）/previewdoc/wellsectionworkflow/selfcheck，均无 QThreadPool/QtConcurrent/std::thread 命中。头注释明示「粗粒度诊断面；不做多线程记账」口径。审计前提（多线程并发 load）现码不存在。 |

### 与 10-05 三路复核的差异（漂移记录）

- 该复核口径「8 未修/15 待证」已过期：本 R0 实测 BIZ-12、BIZ-13 已修，
  ARCH-03/04/07 已修，MEM-06/CONC-05 不复现；BIZ-07/10、RUNTIME-03 确认仍存在
  （归方向 50）。以本表为准。

## R0 判定

- 本方向实际修复清单（7 项）：TEST-07、RUNTIME-04、BIZ-09、ARCH-05、
  TEST-04、MEM-07、ARCH-06。
- 44 项终态分布：已修 26 / 不复现 2 / 本方向修 7 / 方向 50 记档 7 /
  方向 52 记档 2（TEST-01 构建后补 ctest -N 证据）。

## 下一步

R1 起逐项修复（先红后绿）；TEST-07 待基线 ctest 确认 offscreen 红后修。

## R1：七项修复（每项：改动/验证/判定）

1. **RUNTIME-04**（`e308372`）：timeout 处理器 `delete timer`→`deleteLater`。
   验证：tst_seismic_welltie sectionCacheHit 绿 + 新增 findChildren\<QTimer*\>
   清净断言（QTRY 循环处理延迟删除）。红面：断言性论证——Qt 生命周期规则
   （发送者事件栈上自杀）+ 审计证据；本机默认派发器下旧行为不崩，无法本机打红。
2. **BIZ-09+MEM-07**（`c814130`）：1e-9 三处提取 kTopoCandidateEnvelope +
   量纲注释（R-tree 预筛包络 vs sameXy 精确门；**QGIS4 qgsDoubleNear 为绝对
   4·DBL_EPSILON≈8.9e-16——首版测试按 3.x 相对语义写，实测打红后修正**）；
   mCanvas→QPointer + destroyed 钩子去掉手工置空。
   验证：tst_edittools 75 用例绿（新增 vertexCoincidentEnvelopeBoundsNoise
   ——1e-8 偏移邻居不得并入；toolbarOutlivesCanvasDestruction——画布先亡
   拆卸顺序）。红面：BIZ-09 为常量提取（行为不变），包络上界由新用例直接
   钉住（放宽 sameXy/包络即红）；MEM-07 红面为断言性论证（QgsMapTool 以
   画布为 QObject 父级联析构，析构器 `if(mCanvas)` 守卫消除残余悬垂类）。
3. **ARCH-05**（`d7d650c`）：PaleoRasterOut create\*Raster 增可选
   canonicalCrsWkt；createGTiff 比对命中即按原串写；9 个算法读
   LOCAL_GRID_WKT（canonicalWktFromParameters）；constraintfactorjobs
   五个 runParams 站点 + 克里金/SGS 直写注入；cartographicworkfile 请求
   结构体穿线。**行为保真关键**：原全局特例只作用于局部网格 CRS；注入后
   全部既有端到端（tst_mappingworkbench EDATUM 往返判等）保持绿。
   Oracle4：`rg "catalog/" src/algorithms/` 零命中 ✓（ensemblestats.h 注释
   路径措辞一并修）。mincurvature 死 include 清除。
   验证：tst_algorithm_harness 26 用例绿（新增 distanceOutputsPreserve
   LocalGridDatum——**逐字保真钉法**：canonical 加尾随空格（WKT 解析等价、
   文本可区分），穿线拆除即红）。Mutation 演示：welldist 穿线拆为
   QString() → 红；恢复 → 26 绿。**实测发现**：本机 superbuild QGIS 的
   PreferredGdal 导出已保 EDATUM（旧前提为 QGIS 构建相关），特例在本机
   行为惰性——逐字保真是不依赖该环境差异的上位契约，注释如实记档。
4. **ARCH-06**（`6e7a1e2`）：MappingWorkflow typed 成员+访问器（不再回读
   自身属性）；ConstraintWorkflow::setThicknessSamples typed 镜像；
   ConstraintPage typed 直读；头/注释同步。
   验证：tst_mapping/tst_constraintediting/tst_composeworkflow 全绿
   （typed 访问器面零测试改动——行为保留证据）。
5. **TEST-04**（`bf93dc4`）：四处换真实不变量（各处注释写明原断言假绿面
   与新断言验证面）。Mutation 演示：paintDecorations 早退 → 红（回退绿）；
   layoutexport getter 每次 new → 红（回退绿）；wellcomposite 打印探测失真
   mutation 在本机不可见（本机有打印机，后端真值=true）——记档，CI
   offscreen 无打印面可辨。
6. **TEST-07/ARCH-08**（`ca18f88`）：keyClick(Tab)→nextInFocusChain 链步进
   + Tab 语义过滤（focusPolicy 含 TabFocus 位且 isVisibleTo(page)）。
   **修前红复现受限**：本机 Qt 6.11.2 offscreen 下原用例绿（审计 CI 红证据
   LastTest.log:151-153；平台焦点事件派发差异）——按断言性论证记档。
   Mutation 演示：折叠选项披露（不点开）→ 红；恢复 → 绿（新断言可咬人）。
7. **R1 附带**：tst_edittools 修一处 layer.rollBack 收尾。

### R1 工程事故记档（自伤，已恢复）

- 后台全量构建与定向构建并发 → libpaleo_services.a 写坏（file format not
  recognized）。教训落实：**此后单构建串行**。
- 链接器两次瞬时 SIGSEGV（collect2），重试即过（资源压力）。

## R2：门禁与文档

- layering 三档：soft ✓ strict ✓ selftest ✓（ctest 3/3）。
- TEST-01：fresh configure 后 7 套件（catalog_scale/correlation_async/
  importqueue_progress/pyramid_consume/sectionlifecycle/seismic_realarea/
  wellcomposite_shell）ctest -N 全注册 ✓ → 记「环境项已消解」。
- AUDIT_ISSUES.md：46 条目（44 活跃 + MEM-02 + ARCH-08 双记）逐条终态
  标记 + 顶部 2026-10-06 清账注记（分布：已修26/不复现2/本批7/方向50七项/
  方向52 一项/环境消解1）。
- TODOS.md：QPointer 收口条目补记 mCanvas（无其他对应递延条目）。
- 原子提交：e308372 / c814130 / d7d650c / 6e7a1e2 / bf93dc4 / ca18f88
  （+docs 提交）。

## R3：全量 ctest 两遍 + 环境红定性

- **run1**：301 套件，300 绿，唯一红 `tst_startup_trace`（perf·services）。
  隔离复跑 **绿**（9/9，26.9s）→ 负载环境红。
- **run2**：301 套件，299 绿，红 `tst_startup_trace` + `tst_singlefactor_perf`
  （均 perf 标签）。`tst_singlefactor_perf` 在 run1（同一二进制、同一代码）
  186.5s **通过**，run2 222s 失败、隔离复跑（恰逢兄弟会话并发跑同一测试）
  1200s 超时——交叉不一致 + 当时机load 27.4（`io-robustness` worktree 在跑
  同名测试、`seismic-svc-split`/`shell-split` 在并行编译，ps 证据）。
  ARCH-05 对该路径的改动=每次算法调用一次 QVariantMap 查键（纳秒级），
  不构成 6× 时长变化的物理可能。**定性：两红均为机器争用环境红，记档。**
- 触及面套件两遍全绿：tst_panels/tst_edittools/tst_decorations/
  tst_layoutexport/tst_layoutshell/tst_wellcomposite_shell/
  tst_algorithm_harness/tst_mapping/tst_mappingworkbench/
  tst_seismic_*/tst_realization*。
- 层门：layering/layering_strict/layering_selftest 三档绿。
- 工程备注：全量构建期两次链接器瞬时 SIGSEGV（并发资源压力）重试即过；
  一次自伤竞态（后台构建与定向构建并发写坏静态库）——已落实单构建纪律。

## 自审两轮结论

R1（提交前全 diff 五维）+ R2（提交后 hunks 复查）：分层（algorithms 零
catalog、layering 三档绿）、断言真实性（四处 mutation 红→绿示范）、行为
保留（ARCH-05 注入面全覆盖既有端到端；ARCH-06 typed 1:1；QPointer/deleteLater
等价强化）、文档同步（AUDIT_ISSUES/TODOS/注释）、i18n（无新用户面文案）。
零 High/Medium。Low 记 PR body：本地 clang-format 非仓库门禁（基线同样
不过，未重排）；perf 双门在共享机器高负载下本就偶发（两遍交叉证据）。

## 终局

分支 `goal/audit-closure-20261006`：e308372 / c814130 / d7d650c / 6e7a1e2 /
bf93dc4 / ca18f88 / 2e66a65(+本注记)。Oracle 六条对应：①46 条目终态标记
（零 open 无注记）②七修复各自红绿证据（上文）③TEST-04 四处清零+mutation
示范 ④rg "catalog/" src/algorithms/ 零命中+消费域全绿 ⑤layering 三档绿+
两遍全量（环境红记档）⑥AUDIT_ISSUES/TODOS 与 diff 一致。
