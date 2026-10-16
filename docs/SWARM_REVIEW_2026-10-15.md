# Swarm 深度 Review 报告（2026-10-15）

范围：全仓 ~29 万行 C++（src 13 个模块）+ 构建体系 + 架构契约。
方式：8 个并行审查 agent 分域深审（domain/catalog/metadata、io、algorithms、services/ai/linkage、workflow、ui 主壳、ui 专业页面、qgis/app + 构建/架构），全部静态审查、findings 均带 file:line 证据。

## 总体结论

**工程质量显著高于常见 Qt/QGIS 项目水准**：全仓分层契约机械执行零违规（13 模块所有文件头标注齐全、include 白名单无越界）、382 个测试全部真实注册且多为真断言、历次加固以 issue 号留痕可追溯、写路径普遍原子化。**未发现 P0（必然触发的正确性/数据损坏缺陷）**，但查出 **16 条 P1**，集中在四个系统性主题（见文末）。

**未提交改动专项**：工作区 seismic3d viewpanel 的 GL 看门狗有界重试改动（一次性 3s 判决 → 最多 4 次重试/总预算 ~15s）**判定正确、无回归**。论证链完整：超时回调先查 isGlReady 再查 isVisible，重试只发生在「可见但未就绪」歧义态；glInitialized_ 单向置位无误判路径；fallbackActive_ 无复位不会空转。唯一 nit：预算重置依赖「定时器失活」隐式状态，未来若有人改成周期定时器会破坏 showEvent 重置分支。

---

## P1 全清单（16 条，按建议修复顺序）

### A. UB / 数据损坏类（最高优先）

1. **`src/ai/tileinference.cpp:477-494` — needPad 去垫片越界读**。去垫片索引 `out.values[c*(inferRows*inferCols)+...]` 假定模型输出形状=填充后形状，且读取发生在 :497 形状校验**之前**，:493 还先覆写了 out.shape 使校验永远查不出。用户装载带下采样的 ONNX 模型（输出 [1,C,H/2,W/2]）即堆越界读。d6261046 closed-loop hardening 的漏网点；tst_tileinference 无 needPad/形状不匹配用例。修法：先校验形状再去垫片。
2. **`src/metadata/atomicfile.h:125-134,144` — paleoWriteFileAtomic 只 flush 不 fsync**。注释自述「刷盘（flush/fsync）」但 QFile::flush 仅 fflush 用户态缓冲，随后直接 rename——断电窗口内目标文件可被「原子替换」为截断/空内容，恰是该工具要防的损坏。同文件 PaleoAtomicTransaction::commit（:295-306）同样只 flush；与 journal 路径的 QSaveFile(fdatasync) 口径不一致。使用点：catalog/frameworkstore.cpp:174 等受管文件落盘。
3. **`src/metadata/paleoprojectstore.cpp:421,448` — commitAll 两个回调单元无 try/catch**。enqueueWrite/saveAll 都包了 try/catch（注释明说异常裸穿会丢 writeFailed 信号、跳过调用方收尾），唯独 commitAll 的 catalogCommit()/qgzWrite() 裸调——provider/GDAL 抛异常即触发该文件自述的故障模式。

### B. 功能静默失效 / 永久锁死类

4. **`src/ui/seismic3d/seismic3dviewpanel.cpp:1710-1714`（写）+ `:1106-1112`（读）— 相机书签跨会话永远恢复失败**。保存只写 name+5 个数字段（target.z 从未保存），恢复却要求 nums.size()>=6 → 每条书签静默 continue 跳过；同段序列化逻辑在 btnCamDel_（:1091-1099）还有第二份复制。
5. **`src/workflow/mapversioncontroller_async.cpp:343-346` + `src/metadata/paleoprojectstore.cpp:486-490` — 跨工程 busy-token 残留，永久锁死同名图层编辑**。快照任务持锁后释放唯一走 jobCompleted 里 `metaDbPath()==tokenProject` 守卫；PaleoProjectStore 是全应用单例且 setProjectPaths 不清 m_busy。快照在飞时切工程 → 旧工程确定性层 id（如 constraints.H1）在新工程残留 busy，beginEdit 永久拒绝直至重启。修法：store 侧切工程清表 + workflow 侧按 tokenOwner 释放。
6. **`src/workflow/mappingworkbench_edit.cpp:372-390` — saveEditingVersion 部分完成态**。record() 成功后立即 acknowledge()（清空编辑日志），随后 mergeIntervals 失败 → 返回 false 但版本已入库；重试时 spatial_edit_events 从 previous.extra 继承（:288）会把已入档事件再复制一份，谱系重复。修法：acknowledge 移到全部合并成功之后。

### C. OOM 攻击面（外部/用户输入）

7. **`src/io/lasparser.cpp:194-317` — LasParser::parse() 旧入口无防护且被多条生产路径调用**。parseDoc() 有 500MB 闸+GB18030 嗅探，parse() 逐行全量装载无任何防护；welllogread.cpp:125 路由到它，下游 sectionworkbench/propertymodelworkflow/mappingworkbench_predict 均以用户工程 LAS 路径调用 → 4GB 畸形文件 OOM + 同一 GB18030 文件两口径解出不同井名。修法：旧入口薄封装 parseDoc。
8. **`src/io/dlisparser.cpp:1049,1149` + `src/io/lisparser.cpp:383,472` — parseHeader 头扫描无累积上限**。continuation 链无字节数上限（明示「头扫描不受限」），恶意「全后继位」DLIS/LIS 可把整个文件读进单个 QByteArray → 无界内存。
9. **`src/algorithms/geostat/sgs.cpp:176-180`、`sgs3.cpp:477-479` — SGS 无「实现数×格点数」组合闸**。cells 预算 1e8 × 64 实现 × 8B ≈ 51GB。需组合预算或 float 存储。
10. **`src/ai/remotepredictrouter.cpp:105` — 远端预测响应体无大小上限**。readAll() 无界；llmclient/wellfaciesservice 都有 8MiB 类上限，唯独用户自配 URL 的这条通道没有。

### D. 数值正确性

11. **`src/algorithms/cluster/*` — std::uniform_int/real_distribution、std::shuffle 输出是标准库实现定义**，与项目自订口径冲突（geostat/sgs.h:16-17 明文禁用同族并声明原因），supervised.h:74 还承诺 "identical"。CI 有 Windows 侧，聚类/kmeans++/SOM 结果跨平台漂移。修法：mt19937_64 原始 u64 派生（sgs_internal.h:37 unitRandom 已示范）+ 手写 Fisher–Yates。
12. **`src/algorithms/inversion/wavelet.cpp:42` — 高斯消元主元用绝对阈值 1e-12**（linsolve.cpp:26 是 1e-12·scale 相对判据）。地震道幅度跨数个量级：小幅度被误判奇异拒绝、大幅度病态 Toeplitz 解（wl 可 4096）被放行。

### E. 架构契约（技术债主项）

13. **Qgs\* 越过封装层散布 workflow（40 文件）与 ui（72 文件），无任何检查器把守**——直接架空 docs/PALEO_QGIS_PLAN.md §2「业务代码不散布 QGIS API、迁移只改 src/qgis 一层」的核心承诺（QGIS 大版本迁移成本从 1 层膨胀为 112 文件）。domain 层 0 泄漏（干净）。check_layering.py 词表不含 Qgs 调用。典型：workflow/evidenceinteraction.cpp:12-14、mappingworkbench_compose.cpp:159-175、faultinterpretationcontroller.h:126-157（头文件暴露 QgsVectorLayer*）。
14. **`src/ui/paleomainwindow_workbench.cpp:67-1126` — attachWorkbench 1060 行巨型视图函数**：直接操作 QgsProject layerTreeRoot、直接 new QgsRasterLayer、右键菜单里跑 QgsMapToolIdentify + 全表 WKB 比对找回要素、串删除级联/版本登记/编辑会话——「视图只发信号」在主壳最热文件名存实亡（头文件 :143 还自我声明不碰 Qgs*）。同族 P1：`paleomainwindow_attach.cpp:82-108` 视图层 QFile 全量读层位资产（GUI 线程同步 IO）。
15. **`tools/check_layering.py --symbol-audit`（"直接 new"盲区的唯一机械护栏）没接进任何闸门**——不在 ctest、不在 CI lint。实测当前 0 命中（洞是「未来可能被绕过」而非「正在被绕过」），接线成本一行。同类：`--transitive` 52 条边/83 个受染 TU 的重头出闸债无防增长闸。
16. **master 有文档化红项而 CI 结构性看不见**：tst_wellcomposite_visual golden 17/48 超容差（功能回归候选）与 perf 抖动混在同一条 continue-on-error 非阻断通道（ci.yml:171,178,228-231 + TODOS.md:239-252），无升级/分诊机制；linux-asan 曾静默死过一轮（跑已删除脚本从不报错）是实锤前科。

---

## 各域要点速览

### src/domain / catalog / metadata
P1 见 #2/#3/#5。P2 值得注意：metastore.cpp:31-57 连接名 qHash 碰撞可静默串库（复用前不比对 databaseName）；catalog versions 表缺 (asset_id, version_number) 唯一约束、currentVersion 靠行序 tie-break（map_versions 有 UNIQUE 而这里没有）；releasestore/layouttemplatestore 完全没有 readOnly 通道（#80 只读工程下打开即失败）；mapversionstore publish() 与 snapshotInputForPublish() 约 90 行门禁逐字复制。健康度：富模型+成熟 undo/journal，测试上乘；风险在耐久性口径与「同一门禁双份维护」。

### src/io
P1 见 #7/#8。P2：zipExtractBytes 不校验条目 CRC32（xlsx 链无兜底）；sfpkgreader NPY dtype 字节序静默忽略（大端 .npy 全部静默错值）；segyreader open() 失败路径未清索引；constraintstore 活动事务内 DDL（e077b861 同类风险面）。P3：laswriter ±Inf 写 "inf"、wellcompositexml 数值全 String 类型+深度截 2 位小数、encodingdetect 尾部截断误判。健康度：防御密度全仓最高（游标+need() 模式、历代加固真实生效、原子写普及）；短板是「同一格式双入口口径不统一」。

### src/algorithms
P1 见 #9/#11/#12。P2：SGS 热路径逐格堆分配/拷贝（linsolve 每格 ~130KB churn）；协克里金对称矩阵填 2 倍冗余；全目录零并行（R 个实现天然可并行，per-realization 派生流即可）；finiteCells 统计只反映最后一个实现。P3：swArchie 溢出不查、horizontrack 朴素二阶矩消碳（petrophys 已有 Welford 范本）。NaN vs nodata(-9999) 出口契约统一自觉。健康度：数值纪律显著高于科研代码平均（NaN 闸/负方差拒收/发散拒发布），无 P0。

### src/services / ai / linkage
P1 见 #1/#10。P2：远端失败后本地 ORT 降级在主线程槽内同步执行（模型 SHA256 全读+会话构建+warmup，GUI 冻结秒~分钟级）；onnxpredictionservice 推理与加载同锁全局串行（4 线程池退化单线程）。P3：pythonenv 无超时无取消（挂死即 UI 锁死）；scriptrunner 半行缓冲无上限；Python 进程 IO toLocal8Bit vs UTF-8 混用（Windows 中文乱码）。健康度：jobrunner 三段式/crashreport async-signal-safe/errorhub 锁外发信号均为教科书级；AI 剩余风险集中在「用户自备模型与自配端点」信任边界。

### src/workflow
P1 见 #5/#6。P2：图层实例化失败静默丢弃（快照缺层且返回 true+error 非空的自相矛盾状态，mapversioncontroller.cpp:69-72/async:181-182）；多层原生提交非原子（层 A 已落库层 B 失败 → 无版本快照）；generateFactor 吞掉 declare 失败仍 emit productReady；runConstraintIDW 与 generateFactor 两条入口的约束谱系语义不一致（活图层 vs 冻结快照，与 3738e8a4 语义相悖）。P3：constraintfactorjobs「统一异步面」生产零调用、双异步栈并存；mutationSeq 陈旧判据过严误取消长作业。健康度：异步纪律（QPointer/三重代次）接近教科书级；风险在跨工程持久态清理与多步编排尾段部分完成态。

### src/ui 主壳
P1 见 #14。P2：attach_predict.cpp:109-111 无守卫 removeRecursively 整个父目录（constraint 版的子串守卫也可被路径含 "paleo-sf-" 绕过——灾难性误删风险）；裸 QMessageBox 违反 DESIGN.md Tier-2 收敛（attach_shell.cpp:719-722）；装饰暗色死接线（setDark 零调用，暗色主题下比例尺/图例仍浅色 token）；导入按钮 tooltip 双门控互踩（enabled 按钮顶失败文案）；视图直接开 sqlite 且去抖路径同步落库。P3：三个 contour handler ~85 行逐字复制（dropTemp 守卫已各不相同）；ThemeIconEngine 暗色逐像素重着色无缓存；syncSeismicVolumeToDocks 两处 return 吞后续资产。健康度：机械纪律（白名单/幂等守卫/QPointer/世代号/webview 双白名单）少见地好；债在 attachWorkbench 巨函数与 attach_* 家族复制漂移。

### src/ui 专业页面
P1 见 #4。P2：seismicsection wiggle 纵向无裁剪（放大窄时窗每帧仍 O(可见道×总样本)）+ hover 无条件全幅重绘（大剖面卡顿源）；wellcompositestore/datalist_undo/dataopsmodel 视图树内嵌 JSON 持久化（落盘细节做得对但职责错位，undo/sidecar 契约散落三处）。P3：seismicsectionpaint 死三元（两分支同串）；datalist 无验证 static_cast 布局指针 + resize 风暴 findChild；鹰眼任意键触发拖动；字符数×7 估算旗宽。健康度：connect 零无接收者 lambda、异步回调 QPointer/weak_ptr+sceneGuard 双保险、真实 LOD 策略——工程纪律高于常见 Qt 项目；风险在保存/恢复小功能无回归覆盖与超大剖面性能悬崖。DESIGN.md 合规：全域硬编码色 495 处全部登记在 ui-token-exceptions.json 且带理由，check_ui_tokens 0 违规。

### src/qgis / src/app
P1 见 #13。P2：后台开工程的 worker 线程第二 QgsProject 与进程级单例并存（instance() 跨线程访问主线程 QObject 的隐性依赖；takeMapLayer/moveToThread 连 provider 亲和一起搬的契约 QGIS 未书面保证——升级敏感）。P3：PreviewMapCanvas 裸 QgsMapLayer* 键无悬垂防护（全靠唯一消费者自造克隆层）；ErrorHub 全局指针 teardown 期悬垂窗口；mkproject 绕过 QgisRuntime 双份初始化事实；--prefix 被 QGIS_PREFIX_PATH env 静默压过；restoreRasterCrs 每层一次 GDALOpen；appcontext projectOpened 巨 lambda ~260 行九连绑。健康度：QgsApplication 生命周期/图层所有权三重钩子/工程写盘原子/关闭顺序系统性修过——嵌入质量高。与蓝图偏离：qgis-integration 收敛层承诺被实质架空（P1-13）；metadata 目录布局是 interim convention；CRS 策略（datum-free+PROJ 操作注入）是自研增量但升级敏感。

### 构建体系 / 测试布线 / CI
P1 见 #15/#16。P2：新 worktree 首 configure 漏 include 的坑已写成操作手册 12 天无根因立项；continue-on-error job 无腐烂告警（asan 前科）；transitive 债无防增长闸；lint 缺 --selftest（假绿晚 30 分钟才被捞）。P3：CalligraRenderer.cmake+officepreviewrenderer 零引用必然 bitrot；词表缺 QStyle 家族；仓库根追踪了取证快照垃圾文件（goal-loop-prompts.zip、leaked-run/ 等）。健康度：382/382 测试零漏注、扫描器自带正反夹具、依赖三路钉哈希带一致性门——布线无腐烂；最大风险在「闸门外的守卫量」（有工具无闸门 ×3，全靠人的记忆）。

---

## 四个系统性主题（跨域复发模式）

1. **同一功能双入口/双份维护，口径漂移**：LasParser parse/parseDoc、mapversionstore publish/snapshotInputForPublish、约束 IDW 活图层/冻结快照两入口、constraintfactorjobs 双异步栈（生产走一套、测试验证另一套）、cokriging 两重载、contour handler 三份复制、parseRange issues=nullptr。这是本仓 bug 的第一大来源——每处 P1/P2 几乎都能找到一个「另一份」。
2. **有工具无闸门**：symbol-audit（写好了 0.5s 跑完，不进任何门）、transitive 债计数（无防增长闸）、continue-on-error job（无腐烂告警）、Qgs 泄漏（无检查器）。机械护栏密度全仓领先，但守恒量全在闸门外。
3. **多步编排尾段部分完成态**：saveEditingVersion acknowledge 过早、多层 commit 非原子、predict 登记+井表合并失败的「成功产物+失败报文」。普遍选择「如实报错+重试自愈」，个别点留脏谱系。
4. **用户自备输入的信任边界**：ONNX 模型形状（越界读）、自配远端端点（无界响应+主线程降级）、恶意 DLIS/LIS/ZIP（无界累积/CRC 不校验）。d6261046 加固覆盖了大部分面，漏掉的恰是对抗性输入路径。

## 建议修复顺序

1. 一天内可闭合的小改：#1（先校验再去垫片）、#3（包 try/catch）、#4（字段对称）、#6（acknowledge 移位）、#15（symbol-audit/selftest 接进 ctest+CI）。
2. 一周内：#2（fsync）、#5（busy-token 清理）、#7（parse 薄封装 parseDoc）、#10（响应上限）、#12（相对主元）。
3. 专项立项：#8/#9（输入累积与内存组合闸）、#11（跨平台随机数统一）、#13/#14（Qgs 收敛 + attachWorkbench 拆解，宜与 contour 模板收敛同做）、#16（CI 红项分诊通道）。
