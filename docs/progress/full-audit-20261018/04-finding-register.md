# Finding Register（full-audit-20261018）

新问题登记（全部经主 Agent 对源码交叉验证）。完整 Root Cause/Reproduction/Impact 见
reviews/agent-*.md；本表为权威汇总与状态跟踪。状态：OPEN→FIXED→REVIEWED。

## 新问题（修复池）

| ID | Sev | Conf | Module | 位置 | 一句话根因 | 外部重合 | 状态 |
|---|---|---|---|---|---|---|---|
| FC-001 | P1 | VERIFIED | workflow | predictionworkflow.cpp:229-262/374 | prepare 把主线程活 QgsMapLayer 塞进 job.params，worker 线程整段交 QgisProcessingService::run——违反服务成文契约，跨线程并发读 GDAL dataset UB/崩溃面 | 无 | FIXED(commit 待记) |
| FB-001 | P2 | HIGH | algorithms | faciespolygonize.cpp:168-177 | smoothMajority 并列计票由 QHash 迭代序裁决（Qt6.8 每进程随机种子，全仓未固定）→ 同输入两次转面图斑/PROVENANCE 漂移；同文件另两处已排序钉死此处漏网 | 无（#470 是 cluster 随机分布，不同机制） | FIXED |
| FA-001 | P2 | VERIFIED | io | outsourceworkbook.cpp:46-53,183-275,498-499,627-629 + wellcompositexml.cpp:533-537,556 | 工作簿「列填充」只有每格 16384 列上限，无行数×列数总量预算：~40B 的 ss:Index="16384" 单元格换 ~128KB 内存（~3000×放大），GUI 线程路径 OOM/假死 | 无 | FIXED |
| FA-002 | P3 | VERIFIED | io | outsourceworkbook.cpp:727-733 + wellcompositexml.cpp:63-69,895-902 | SpreadsheetML .xml 路径 readAll() 无文件大小闸（xlsx 512MB/LAS 500MB 均有，此路径裸奔） | 无 | FIXED |
| FB-002 | P2 | HIGH | algorithms | sgs.cpp:205-214,375-392 + sgs3.cpp:505-514,684-701 + sgs_internal.h:26-32 | #468 输出闸只管张量；并行档 4 worker 各持 path+gaussianField+lattice（3D 再加 lookup 表），合法参数（cells=1e8,R=4）峰值 ~12.8GB（3D 25GB+），注释承诺 ≤4.5GB 失实——#478 并行化新引入量 | 相邻 #468（不同机制：工作集×worker ≠ 输出张量；#518 的 #468 修复落 paleoalgorithms.cpp） | FIXED |
| FB-003 | P3 | MED-HIGH | algorithms | stratgrid/sgsfill.cpp:204-212 | fillSgs 聚合输出 R×cellCount×4B float 无组合闸（每带各自过 #468 闸而总和无界，最坏 128GB） | #468 同族第三入口（未被其修复覆盖） | FIXED |
| FD-001 | P2 | HIGH | ui | evolutionplayerpanel.cpp:19 + paleomainwindow.cpp:489-490 | 演化动览帧表构造时快照内置层位词表、无 reload 通路——自定义 sequence_boundaries 工程点「下一期」把 activeHorizon 切到本工程不存在的层位，chip 全灭、地图层被清、编图三页上下文污染 | 无（paleomainwindow.cpp 在 #518 清单但 hunks 在 934-1000 行，零重叠） | FIXED |
| FA-003 | P3 | HIGH | io | segyreader.cpp:1104-1118 vs 1419-1428 | scanParallel 取消与读错误同走 m_lastScanPartial=true → openCached 注释宣称的「非取消失败回退顺序扫描」分支恒不可达（死码），瞬时读错误被当不可续扫失败 | 无 | FIXED |
| FA-004 | P3 | VERIFIED* | catalog | catalogstore_json.cpp:277-285 | 损坏/手改 sqlite 的 links.ord 巨值 → QVector::resize(2^31) 天文分配，工程打开即崩（*机制已亲验；自然触发需坏库，SUSPECTED→VERIFIED 升级） | 无 | FIXED |
| FC-002 | P3 | VERIFIED | workflow | mapversioncontroller.cpp:336-343 | 同步 saveVersion 的 retainMapCitations 失败产生「成功返回值+非空 error」自相矛盾态（异步路径 #480 已修，同步残留） | 同族 #480（不同实例） | FIXED |
| FC-003 | P3 | VERIFIED | workflow | mapversioncontroller_async.cpp:401-450 | 异步 publish 快照对 legacy（非冻结）来源零 stale 守卫，与 save 路径十一路信号守卫不对称 | 无 | FIXED(最小指纹版) |
| FC-004 | P3 | VERIFIED | workflow/ui | compositionworkflow.cpp:156-279 + attach_compose:49-66 | fuseFactors 在 GUI 线程整段同步跑 QGIS Processing（冻结 UI、无取消），与后台化 startCompose 双栈漂移 | 修复落点 attach_compose 在主仓未提交簇 | RECORD-ONLY |
| FD-002 | P3 | HIGH | qgis | previewmaptools.cpp:102-121 | PreviewMeasureTool 无 deactivate() 清理（identify/profile 都有）——切工具后量测橡皮带永久残留画布 | 无 | FIXED |
| FD-003 | P3 | MED-HIGH | qgis | previewmapcanvas.cpp:106-122,178-191 | setLayers 整表置换只加不减 m_visible/m_opacity/m_blend 三层记账；forgetLayer 对已置换层早退——悬垂指针键无界累积，地址复用时新层继承陈旧可见性/透明度 | 相邻 #483 P3（裸键问题；本条是 setLayers 独立缺口） | FIXED |
| FD-004 | P3 | HIGH | qgis/ui | nativeattributetablehost.cpp:94-110 | 破坏性确认走裸 QMessageBox::question，绕过 DESIGN.md Tier-2「统一 paleonotify 收敛」契约（#481 只记了 attach_shell 一处，此为独立第二处） | 同族 #481 P2（不同实例） | FIXED(ask seam) |
| FD-005 | P3 | HIGH | ui | paleomainwindow.cpp:1013 | 多地震体装载完成回调不续扫（#481 P3-3 弱化变体） | #481 + paleomainwindow.cpp 在 #518 清单 | RECORD-ONLY |
| FE-001 | P3(Med) | HIGH | tests | tst_factorworkflow.cpp:429 | 永真断言 QCOMPARE(groupDisplayName(Sand), groupDisplayName(Sand))——词表显示名回归零保护 | 无 | FIXED |
| FE-002 | P3(Med) | HIGH | tests | tst_perf.cpp:82-90,130-145 | concurrencyCap/defaultCellSize 测试内自算公式后自证（生产无该公式，实际 setMaxThreadCount(4)）——数学上不可失败，占 CI perf+RUN_SERIAL 槽位 | 无 | FIXED |
| FE-003 | P3(Low) | HIGH | tools | make_appimage.sh:44-48 | AppImage runtime 从可变 tag `continuous` 下载且无 sha256 校验——供应链纪律缺口（产物是分发件） | 无 | FIXED |
| FE-004 | P3(Low) | HIGH | io | dataimport_document.cpp:300-301 | LibreOffice profile 固定可预测名 /tmp/paleo-lo-profile-<pid> 且全仓无清理（symlink 面弱化 + /tmp 永久累积） | 无 | FIXED |
| FE-005 | P3(Low) | HIGH | resources | resources/translations.qrc:4 | 死 qrc 引用不存在的 ../translations/paleo_zh_CN.qm（真嵌入点在 extra-ux.cmake）——双轨陷阱 | 无 | FIXED |

## SUSPECTED（记录不修，需后续验证）

| ID | 位置 | 摘要 |
|---|---|---|
| S-1 | paleoalgorithms.cpp:1186-1316 | Isopach 顶−底符号约定：深度域构造面全图负厚度→negativeToNodata 整图 nodata（响亮失败非静默）；约定/文档问题待域确认 |
| S-2 | lowfreq.cpp:52-74 | nearestCell O(井×格) 注释自证百万级但地震体几何可达 1e7×井 1e2；无实测证据不优化 |
| S-3 | sgsfill.cpp:333-338 | fillSgs 硬数据钉死均值以 float 存储（行业常规，记录） |

## 排除统计

- EXCLUDED_EXISTING_ISSUE：agents 各报告排除节合计 60+ 条观察（含本树已修复确认 20+ 项、
  残余归并 #466/#467/#477/#478 家族 4 项）。
- EXCLUDED_ACTIVE_WORK：PR #521（相面拓扑编辑领地）、PR #518（issue-sweep 触碰面）、
  主仓未提交导出簇（FC-004/FD-005 落点）。
- 修复池无一条与既有 issue 同根因（各 agent 逐条对照 44 issue 快照 + PR 文件清单核实）。
