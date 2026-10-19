# Finding Register（full-audit-20261020）

状态值：OPEN / IN-PROGRESS / FIXED <sha> / RECORD-ONLY / EXCLUDED_EXISTING_ISSUE / EXCLUDED_ACTIVE_WORK / FALSE_POSITIVE / BLOCKED_BY_EXTERNAL_WORK
来源：A/B/C/D/E = 本轮 5 个只读 subagent（全文见过程产物 findings-agent-*.md）；MA = 主 Agent 复核。
上轮移交项保留原 ID 并标注本轮复核结论。

## 修复池（按优先级排序）

| ID | Sev | Conf | 位置 | 一句话 | 状态 |
|---|---|---|---|---|---|
| A-10 | P2 | VERIFIED | src/io/ziparchive.cpp:204-221 | ZIP64 中央目录界检查用可回绕 u64 加法（#381 同族漏修点）→ crafted 包负下标 `at()` release 越界读（导入面：.sfpkg/.xlsx） | OPEN |
| D-05 | P2 | HIGH | src/ui/seismic3d/seismic3dviewpanel.cpp setVolume | 换体不清面板侧堆叠层/任意剖面缓存+不复位 stackExtracting_ → 旧体内容贴新体几何 + 新体堆叠请求永久跳过 + TF 重喂旧剖面 | OPEN |
| E-10 | P2 | VERIFIED | tools/ci_triage_reds.py + ci.yml（上轮 E-02 升级） | triage 对 lint 是 job 级黑盒；job-scope 条目无 review_due 强制、STALE 永不判、timed_out/缺位静默跳过——任何 lint 红被无差别降级 | OPEN |
| C-10 | P2 | VERIFIED | src/workflow/paleomaplibrary.cpp:44-47/:20-33 + paleomapimport.cpp:46-57 | #579 回收防线只盖 assetIds 路径：文件重导入命中软删资产复用 assetId 发布（「成功但不可见」），declareMaps 不过滤回收清单（画布与数据树可见性矛盾）；回收站永久删除可连带清掉重导入版本 | OPEN |
| A-14（=B-03） | P3 | VERIFIED | src/services/storagegovernance.cpp:32-37,97-119 + src/services/paleomapfiles.cpp:40-46 + src/io/ingestplan.cpp:192-197 | sidecar 词表三处分叉；主害=bundle 版本以 bundle_members.isEmpty() 为门跳过族扫描 → QGIS 导入后生成的 .aux.xml/.qix/.qml 全误报孤儿，用户确认 purge 即删 | OPEN |
| B-04 | P3 | VERIFIED | src/io/sattrio.cpp:212 + src/services/crossplotsources.cpp:86-88 | SATR 取消谓词语义反转（靠唯一调用方双负续命；生产调用方仅 1 处，测试不传谓词——爆炸半径零） | OPEN |
| A-15（=B-05） | P3 | VERIFIED | src/services/crossplotsamples.cpp:218-252 | 交会栅格采样无字节预算闸（9B/px/源+全程驻留），大网格/VRT 可 OOM 整进程；对照 #468 4GiB「拒绝而非钳制」口径 | OPEN |
| D-13（=C-13） | P3 | HIGH | src/ui/pages/constraintpage.cpp:229 + constraintpage_wellfactors.cpp:293-342（D-01 残留） | 无版本换勾等值距建议读旧因素值域（1/6 极差喂错参数进生成）；已定位上轮两次回退根因（tst_mappingpages:1303-1307 钉死「清表前读」）；最小方案=页侧 wellRowsKey 属性守卫 | OPEN |
| A-13（=A-04） | P3 | VERIFIED | src/catalog/catalogstore_json.cpp:277-294 | 负 ord 链接行静默丢弃+重复 ord 静默覆盖（与同函数 unsafeVersion/巨值告警口径不对称）；仅外部损坏可达 | OPEN |
| A-11 | P3 | HIGH | src/io/segyreader.cpp:1429-1441 eligibility + scanParallel | 并行 eligibility 仅抽查前 8 道判固定道长；变道长文件整除构造下产错位索引并 store.save 固化缓存 | OPEN |
| D-12（=D-02） | P3 | VERIFIED | src/ui/pages/mappingreferencepanel.cpp:193-197,256-289 | 关最后视图无空态引导 + setCanRemove 死参（#547 裁决后残留 API） | OPEN |
| C-11 | P3 | VERIFIED | src/qgis/paleomapsource.cpp:38 + workflow/projectareasummary.cpp:46-47 | surveyArea/井包络无条件 /1e6 当 km²，无 CRS 单位守卫（CRS_ASSUMPTION_AUDIT §2.3 建议未落地；当前米制脊线不触发） | OPEN |
| D-06 | P3L | HIGH | src/ui/correlation/correlationpanel.cpp:118-152 | 列头拖拽 m_dragColumn 裸指针跨事件回合（rebuildScene 删项后 UAF）；面板当前常驻隐藏=latent | OPEN |
| D-07 | P3L | HIGH | seismic3dviewpanel.cpp buildRenderControls 884-899 | 斜剖面/栅栏按钮无体早退不弹回，恒亮卡死（同族 btnSweepPlay_ 有先例） | OPEN |
| D-09 | P3L | HIGH | src/ui/layout/layoutexportactions.cpp:397-401 vs 286-299 | 任务启动失败双重上报（两条不同文案连闪） | OPEN |
| D-10 | P3L | HIGH | seismic3dviewpanel.cpp setVolume 445-449 | 六控件禁用无 reason tooltip（§35；wellsection 有同仓标杆） | OPEN |
| D-11 | P3L | HIGH | src/ui/pages/datalist_batch.cpp buildSearchActions 220-241 | #579 新操作行「删除…/三级相…」禁用态无禁用原因 tooltip（§35） | OPEN |
| E-11 | P3 | HIGH | tools/ci_ctest_gate.sh + check_flaky_ledger.py + ci.yml | flaky 台账「累计 ≥3 次升级」在 CI 恒不触发（gitignored 本地文件、每 run 清零、单 run ≤2 条） | OPEN |
| E-12（=E-05） | P3 | VERIFIED | CMakeLists.txt:1036-1042 vs 1123-1135 vs AGENTS.md | qgis_spread 族带 LABELS layering + 缺 WORKING_DIRECTORY，与其余 33 项源码闸门注册约定漂移 | OPEN |
| E-13 | P3 | HIGH | CMakeLists.txt:996-998 | find_program(PALEO_PYTHON3) 无 else——无 python 环境下 ~36 闸门 ctest 静默消失 | OPEN |
| E-14 | P3 | HIGH | tools/check_i18n.py:28-64 | 入口词表盲区（addItem/insertItem/setTabText/QMessageBox 标题位等未列入=不扫） | OPEN |
| C-14 | P3L | VERIFIED | src/services/paleomapfiles.cpp:64-78 | copyDataset 失败不自清（当前调用方已清理；接口契约隐患） | OPEN |
| C-15 | P3L | SUSPECTED | src/ai/remotepredictrouter.cpp:404-419 | fallback worker lambda 裸 this invokeMethod 关停期竞态（一行 QPointer 前置守卫） | OPEN |
| B-11 | P3L | VERIFIED | src/algorithms/singlefactor/support.cpp:259-288 + localdirectionalgorithm.cpp:714-721 | O(n²) meanNearestSpacing 先于样本预算闸执行（1e6 闸数值与注释假设差三个数量级） | OPEN（低） |
| B-10 | P3 | SUSPECTED | src/algorithms/faultsurface/faultsurface.cpp:238-278 | chainSegments 单链贪心，多链交线静默丢链（U 形断面）；measureSlip 区间失真 | OPEN（需夹具证实） |

## RECORD-ONLY（本轮记录不修）

| ID | 位置 | 摘要 |
|---|---|---|
| A-12 | src/metadata/mapversionstore.cpp:324-390 | 冻结目录在事务外，硬崩溃留孤儿快照目录（无恢复面）；磁盘泄漏无数据丢失——journal 化修复需独立批次 |
| A-16 | src/catalog/frameworkstore.cpp:259-265 | flush 失败不清孤儿字节（#155 兜底在位） |
| C-12 | src/qgis/paleomapsource.cpp/paleomapfacies.cpp | worker 内建 QgsLayer（QGIS Processing 同构可辩护；契约注释建议） |
| D-08 | seismic3dviewportwidget.cpp:788-797 等 | pickSliceAt 死代码+拾取/渲染投影 faultFit 不一致（生产未接线）；含 record-only 成分，死代码删除可随 D-05 批 |
| D-14 | 杂项 5 条 | petrophys outputUnit 恒 v/v / 轨道 caption 叠压 / horizonmarkers 空名防御 / mapbook 双份校验日志 / 深度道 first() 兜底 |
| E-07 | 数据层 17 文件直连 qgs | by design 口径张力，持平无恶化，RECORD-ONLY 维持 |
| B-10 若夹具证伪 | — | 见修复池（先证后修） |

## 正面确认（非 finding，引用自 subagent 报告）

- B 域数值面：IDW/OK/OCK/SGS/FFT-Hilbert/semblance/速度模型逐项手算核对无 bug；任务系统取消/迟到/析构纪律全部成立；无 worker 线程 GUI 活对象操作。
- C 域：#579 五重发布守卫（staging+journal+mutationSeq+recycleHash+task session）自洽；workflow 主干三段式纪律无中高危缺陷。
- D 域：correlation/seismic3d/layout/wellsection 四族 token/QPointer/generation 纪律良好；#579 UI 面合格。
- E 域：tests/ 注册面干净（396 add_paleo_test 一一对应）；src/app/selfcheck 合规；PR #579 测试真实（零 mock 零永真）。
