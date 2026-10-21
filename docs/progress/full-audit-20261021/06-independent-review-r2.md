# 独立复审 R2 —— Fix 1 正确性复核（2026-10-21）

复审人：独立代码复审员（未参与修复编写）。基线 `e6837e14` → HEAD（23 个修复
提交）。方法：逐 diff 读代码 + 追调用链到 catalog/vendor/算法核；对可疑点构造
反例论证（未运行被测代码）；只读检查器实跑（check_layering / --qgs-audit /
--strict、check_qgis_spread、check_ui_invariants、docs_drift、file 预算基线
核对、闸耗时实测）。未修改任何被审文件、未构建。

结论速览：**1 项 MUST_FIX（C-3 撞号重映射自身可造出 journal 内撞号，触发它
本要消灭的整批判废）；3 项 SHOULD_FIX；其余为文档级备注/外部归属**。A-01
最终版锁序干净。E-2 初冻基线如实（identifytool 2 文件系基线期已红的 #604
在册外交接，非本轮引入）。

---

## R2-A 正确性

### A-1【MUST_FIX】C-3 撞号重映射可分配出「journal 自己已占用的 id」，反而触发整批判废

`src/workflow/mapexport.cpp:196-205`（remapCollidingJournalIds 第一循环）：

```cpp
if (op.kind == CatalogOp::Kind::AddAsset &&
    !live->assetById(op.asset.id).id.isEmpty())
  assetMap.insert(op.asset.id, live->nextAssetId());
```

- `live->nextAssetId()`（datacatalog_assets.cpp:163-172）只跳过 **live 表里
  已存在**的 id（`while (!assetById(id).id.isEmpty())`），对「同批 journal
  将要添加的 id」一无所知；而 journal 侧未撞号的 op 不会被检测（第二判据
  `assetById(op.asset.id)` 为空即跳过，mapexport.cpp:199）。
- 反例（批量出图 ≥2 个新资产 + 窗口期内一笔最普通的无关写）：
  1. 快照时 live `m_assetSeq=10`。导出 staging 侧分配 `ast-11`、`ast-12`
     （两个层位各一个资产，dedup 未命中——渲染结果不同即不命中）。
  2. 窗口期内用户导入 1 个文件：live `nextAssetId()` → `ast-11`（seq→11），
     addAsset 的 T17 推进（datacatalog_assets.cpp:42-48）到 11。
  3. 发布 remap：op[ast-11] 撞号 → 映射为 `live->nextAssetId()` = ++seq=12
     → `ast-12`（live 没有它）→ `assetMap={ast-11→ast-12}`；
     op[ast-12]：live 无此 id → **不重映射**。
  4. applyJournal 重放：addAsset(ast-12) 成功 → … → addAsset(ast-12) 在
     datacatalog_assets.cpp:37 撞「duplicate id」→ 事务回滚 →
     publishMapExportJournal 失败 → 文件回滚 → `task->failPublication(
     "catalog 提交第 N 项失败：asset id is empty or duplicate: ast-12")`。
- ver-N 结构完全同型：journal 含 ver-21/ver-22，窗口期 live 消耗 ver-21 →
  remap ver-21→ver-22（journal 自有）→ addVersion duplicate 失败。
- 这正是 C-3 要修的场景本身（「窗口内任一无关写入不再整批判废」）：**只要
  导出产出 ≥2 个新 id、且窗口期无关写消耗的 id 数少于 journal 的 id 跨度，
  新代码就以一条更费解的 duplicate 文案把整批判废**。测试
  `unrelatedCatalogWriteDuringExportDoesNotDiscardResult`
  （tst_exportpipeline.cpp:325-358）用 50 个噪声资产把 live seq 推过整个
  journal 跨度（单资产 journal），恰好绕开本缺陷——不构成覆盖。
- 修复方向：分配新号时同时跳过 live 已有 id 与「本 journal 声明的全部
  AddAsset/AddVersion id」（把 journal id 集传入分配循环，或对
  assetMap/versionMap 的值再扫一遍 journal 并级联重映射）。

### A-2【SHOULD_FIX】C-3 漏改一处：mappingworkflow 厚度链 publish 不消费 idMap，声明层指向悬垂路径

`src/workflow/mappingworkflow.cpp:425-433`：

```cpp
if (job->ok && publishMapExportJournal(catalog, directory,
                                       job->staging->path(), job->journal, &error)) {
  for (auto decl : job->outputs) {
    const auto relative = QDir(job->staging->path()).relativeFilePath(decl.source);
    decl.source = QDir(directory).filePath(relative);
    if (!m_layers->declare(decl, &error)) break;
```

- `publishMapExportJournal` 缺省 `idMap=nullptr`，但内部**无条件**做
  remapCollidingJournalIds——撞号时文件被搬到新号路径；
- `decl.source`（derivedassets.h:26-29，`artifacts/derived/{asset}/{ver}/…`
  的 staging 绝对路径）按原 id 段拼接 → 指向不存在的文件，`m_layers->declare`
  落一条死链图层声明（静默）；
- 这是 C-3 改动**新引入**的失败面：旧代码窗口期任何写 → stale → 整链丢弃，
  不会出现「发布了但声明错路径」；新代码放行发布却不改写下游引用。
- 其余四个调用方（mapexport:384-392 / horizonbatchexport:549-563 /
  mapbookqueue:294-302 / geologicalexport:219-228）都取 idMap 并改写
  assetId/assetIds/registeredIds 后再 `versionFilePath(currentVersion(...))`
  解析——唯独厚度链既不传 idMap 也不改写 decl。
- 修复方向：传 idMap，对 `relative` 按段（或对 decl.source 全路径段）应用
  同一映射后再 declare。

### A-3 C-3 其余正确性核验（结论：方案成立）

- **journal op 集合**：四条导出链（mapexport / horizonbatchexport /
  mapbookqueue / geologicalexport）与厚度链对 staging 副本的写入只有
  `registerMapPdfAsset`（mapexport.cpp:147/167 → addAsset+addVersion）与
  `DerivedAssetRegistrar::commit`（addAsset+addVersion）。grep 证实这些文件
  无 addLink/updateLink/addEntity/updateVersionExtra 调用 → **journal 只含
  AddAsset/AddVersion，不含任何位置性 op**（UpdateLink/AttachLink/
  SetLinkPrimary 按 m_links 下标重放， Against evolved live 会静默打错行——
  但导出链不产生，remap 的 default 分支「导出链不产生」的注释与事实相符）。
  摘除 mutationSeq 闸门后，重放撞「duplicate id」仍是如实的冲突信号（A-1
  指出的是重映射自身的缺陷，不是重放语义问题）。
- **跨工程/并发窗口**：session 守卫 + projectDir 守卫保留；publish 全程在
  tasks 所属线程同步执行，两个导出的 finished 不并发，remap→applyJournal
  之间无交错面。`remapCollidingJournalIds` 里 `live->nextAssetId()` 会
  ++live 的 m_mutationSeq（即使后续失败）——只消耗 id 号，无正确性影响。
- **路径改写安全性**：rewritePathSegments 只会命中字面等于撞号 id 的段；
  当前 journal 的 AddVersion 全部是受管相对路径
  （`artifacts/output|derived/...`），不会误改外部绝对路径（若未来有人在
  staging journal 里放 unmanaged 版本需重看此函数——见 D-only 备注）。
- **dedup 时点**：registerMapPdfAsset 的 sha dedup 在快照上判，窗口期 live
  已入同 sha 版本时重放会产生重复版本（不失败）——与 #480 P3-3 已冻结输入
  的口径一致，可接受。

### A-4 A-01 崩溃窗口语义：自洽，两处备注

- 节流把「每 miss 必持久」放宽为「双阈值窗口」，丢失代价=重算哈希（自愈），
  与头注声明一致。**但生产侧没有任何 shutdown flush**：全仓 grep 无
  `flushDisk()/setDiskSaveThrottle` 生产调用（仅测试注入默认 1000ms/64）→
  正常退出也会丢最后一个窗口（≤64 条）。建议把 `flushDisk()` 接进应用退出
  或 DataImportService 收尾（DOCUMENT_ONLY，代价可控）。
- 头注（shacache.h:51-52）「invalidate/setDiskFile 后首个 miss … 立即落盘」
  对 invalidate 不成立：`forcedSaveLocked` 会刷新 `m_lastDiskSaveMs`
  （shacache.cpp:239），其后 1s 内且 <64 脏的首个 miss 照常被节流。commit
  message 的措辞（「换代后首 miss 与 invalidate/flushDisk 立即落盘」——即
  invalidate/flushDisk **本身**立即落盘）才是准确口径，头注建议改写
  （DOCUMENT_ONLY）。

### A-5 C-5 QVariant no-op 短路：与单键重载逐字同口径，边界语义可接受

datacatalog_versions.cpp:308-322 与单键重载（:50-56 的
`sameValue`）完全同构，且与本函数上方 frozen-key 判据用同一个
`QVariant::operator==`——类型不同但可互转的值（int 1 vs double 1.0）按相等
处理并跳过写入，方向是保守的（存储类型保持，不会漏掉真变更）。无效
QVariant：新值 invalid + 旧键不存在 → 视为 no-op 直接返回（旧实现会把
invalid 值插进 extra）；单键重载同款行为（「清不存在的键」为 no-op）——
一致，且语义上更合理。保留字/冻结键判据都在短路**之前**，无绕过
（DOCUMENT_ONLY：记录 map 重载 invalid 值=「插入 invalid」而非单键的
「删除键」这一既有不对称，与本次修复无关）。

### A-6 其余 R2-A 项（逐项核过，无翻案）

- **C-1**：retryLast 收编 sendRound 等价（aichatcontroller.cpp:442-449：
  placeholder + setConfig + `send(requestMessages(), tools())`）；被删手工
  序列与 sendUserText 路径逐行对齐，仅多了 tools()。✓
- **C-2**：暂存目录进 `m_dir/artifacts/reports`（同文件系统），rename 不再
  EXDEV；mkpath 失败如实早退；QTemporaryDir 析构清理已被测试断言。✓
- **C-6**：非流式 `m_body + tail`（llmclient.cpp:482）与流式分支同款；tail
  不计入 kMaxResponseBytes（仅剩缓冲字节，可忽略）。无新测试（SUSPECTED/
  平台时序，难触发）——可接受。✓
- **A-02**：`trim > int(bodyLen)` 钳制正确——trailer（checksum/traillen/
  padcount 及其计数）全在本段 body 字节内，合法文件恒有 trim ≤ bodyLen；
  lrBuf 虽是累计缓冲，padcount 读取位置（lrBuf 尾）就是本段尾。✓
- **A-04**：出队端守卫与入队端 queueConversion（dataimport_document.cpp:
  269-270）同文案同判据；状态复位齐全；递归深度 ≤ 队列长度。✓
- **A-05**：长度前缀==实写字节不变量恢复；截断可能劈开 UTF-8 序列（解码
  损耗替换，不再错位）。✓
- **D-04**：`extent().isEmpty()` 拒绝与另两路同口径。✓
- **K-3**：见 R2-B/B-3。
- **B-1 公式**：与权威实现 wavelet.cpp:154-170（a²=(π·f0·dt)²）一致；
  但**代码注释与 finding register 的数值方向写反了**：系数误写 2π² 时等效
  主频是 √2≈1.414× **偏高**（零点半宽 1/(2πf)≈6.37ms @25Hz），不是
  「0.798×/11.3ms/25Hz→20Hz」。修复本身正确、测试判据（9.0±1.5ms）对错
  版（6.37ms）可判红——仅注释误导后来者（DOCUMENT_ONLY，顺手改注释）。

---

## R2-B 并发与生命周期

### B-1 A-01 锁与节流：最终版无锁序问题（重点核查项）

逐路径核锁（shacache.cpp 全文）：

| 路径 | 持锁情况 |
|---|---|
| sha256Hex | 锁段1（查表）→ **放锁后** hashFile → 锁段2（insert + `maybeSaveDiskLocked` → `flushDiskLocked` → `saveDiskLocked`） |
| flushDisk()（公有） | 取锁 → flushDiskLocked |
| invalidate | 取锁 → forcedSaveLocked → saveDiskLocked |
| setDiskFile/setDiskSaveThrottle/setMemoryLimit/counts | 单锁 |

`maybeSaveDiskLocked/flushDiskLocked/saveDiskLocked` 均不取锁（注释明确
「调用方持锁」），无任何路径在持 m_mutex 时调用公有无锁版 flushDisk()；
saveDiskLocked 的 QSaveFile 同步 IO 在锁内——与旧代码同一定式（无新增
死锁面；盘 IO 阻塞并发查询的老问题反而因节流变小）。节流判定只用
QDateTime 取时，无事件循环重入。**结论：曾出现过的死锁形态（临界区内调
带锁入口）在最终版不存在。**

### B-2 C-4 回收时机 vs 测试直读 tempVolumePath：无冲突

`reclaimTempVolume` 只在 dock 的 finished 处理器调用；tst_inversion_workflow
与 tst_inversion_realarea 直读 `job.tempVolumePath` 的用例只跑 workflow
函数（prepare/compute/publish），不经 dock。发布成功后 showResult 只用
job 的统计字段，无任何下游再读 temp 路径（全仓 grep 仅「目录未连接」提示
文案）。`!m_catalog` 分支不回收（体是唯一成果，告知用户路径）的裁量合理。
备注（DOCUMENT_ONLY）：`gen != m_invGeneration` 早退（seismicsectiondock_
inversion.cpp:236-237）与新请求接管后旧任务的 paleo-inv-* 树仍泄漏——
泄漏面从「任何结局」缩到「被世代挤掉/dock 先亡」，已是大改善，如要收口
可在早退分支补一次 reclaim。

### B-3 K-3 / K-1 / K-2 / D-01 / D-02 / D-03（全部核过）

- **K-3**：BatchSave::flush 首次调用即置 `m_done`（datacatalog.cpp:748-754）
  → 失败后析构不再二次结算；endBatch 落盘失败时内存按快照回滚
  （datacatalog.cpp:709-732）→ 新增 `QFile::remove(abs)`
  （frameworkstore.cpp:266）删的是本 save 新写的 ver-N 新路径，与 :198/
  :210/:251 同口径，无「删掉旧版本字节」风险。✓
- **K-1**：rebuildScene 删 m_columnItems（correlationpanel.cpp:853-858）前
  `cancelDrag()`；m_dragColumn 指向的列项仅在该容器创建/销毁（:881/:853），
  单一删除点已被覆盖；mouseMove/Release 均先判 `m_dragColumn` 非空。✓
- **D-01**：过滤 `l.unresolved`（datacatalog.h:63 字段存在）+ QSet 按
  assetId 去重，消除空 id 幽灵条目与重复撤销命令。✓
- **D-02**：`delete chip` 即刻回收——reloadHorizons 全仓唯一调用点
  paleomainwindow.cpp:491（工程级层位表变化），从不由 chip 自身信号触发，
  无「删信号发送者」窗口；findChildren 快照迭代中删除安全。✓
- **D-03**：对话框按 objectName 复用；wellActivated 连接只建一次，捕获的
  m_canvasCtl/m_layerSvc/m_projectSvc 均为 PaleoMainWindow 构造注入后不再
  变更的成员（paleomainwindow.cpp:117-119，全仓无重赋值）→ 捕获指针与
  到时读成员等价；wellLayer 经 `dialog->property` 在点击时读取，不吃首份
  报告的陈旧捕获。✓
- **K-2**：exportLayoutAsync 全仓唯一调用方（layoutexportactions.cpp:289）
  传 `&fallbackReason` 并统一上报 → 单次上报达成，无误吞面。✓

---

## R2-C 算法与数据

- **B-1**：见 A-6（公式正确；注释数值方向反了）。
- **B-2 仿射换算数学**：vendored SgyAffineFit 语义 `x=a·il+b·xl+c,
  y=d·il+e·xl+f`（vendor/sbm/src/Data/Sgy/SgyCoordinateMapper.h:11-15）→
  相邻列 (Δil,Δxl) 的 CRS 单位距离 =
  `hypot(a·Δil+b·Δxl, d·Δil+e·Δxl)`——seismictaskservice_slice.cpp 的
  实现逐项吻合；逐列累计单调；`inlineNos/xlineNos.size()==width` 守卫齐全；
  测试用 25m 正交测网钉死 ~318.2m 期望（红态 12.73）。✓
  - 【SHOULD_FIX】**回落分支口径**：mapper 不可得时把**线号单位**的
    `slice.distances` 原样塞进按米消费的通道——消费端
    seismicsectioncanvas_nav.cpp:168-172 `totalDistanceM()` 对非空值直接
    `.back()` 当米用（paint 路径同），头注「消费端应如实标注」实际无人
    执行。建议回落分支清空 distances（让消费端显式兜底口径接管），或带
    单位标记。非回归（旧代码同样把线号当米传），但「如实降级」目前只停在
    注释里。
  - 【SHOULD_FIX-琐碎】seismictaskservice_slice.cpp:8-9 同一 commit 把
    `#include "domain/seismic/sgycoordinatemapper.h"` 加了两次（相邻两行，
    同名 include）。删一行。
- **B-4 进度账本**：
  - 相干时间切片路径：totalUnits=2·nIl·nXl，compute 段逐 inline 入账
    `(done+doneIl·nXl)/totalUnits`，done 固定、doneIl 单调 → 无回退、无
    >100%，末尾 `reportBytes(total,total)` 补满。✓（progress(il+1) 在处理
    前回调，超前一行——纯观感，不修也罢）
  - 【SHOULD_FIX】**startAttributeSlice 逐道族分支**（slice.cpp:775-816）：
    计数改成全分片共享原子列计数（对），但上报仍绑在 `x0==0` 分片存活期
    内——0 号分片先跑完自己的 chunk 后无人上报，`n==width` 的末次上报只有
    当最后一列恰好落在 0 号分片时才发出，join 后**没有**
    `reportBytes(totalUnits, totalUnits)` 终填（对照：相干分支 :709、时间
    切片路径 _attr.cpp:498 都有）。任务成功时进度条可能停在 <100%。加一行
    join 后终填即可。数值本身单调、≤100%（无 R2-C 意义上的账本倒挂）。
- **A-02/A-04/A-05/C-6/D-04**：见 A-6，全部成立、无边界翻案。

---

## R2-D 性能与资源

- **A-01 节流后最坏面**：miss 写盘率上限 = max(1 次/intervalMs,
  1 次/dirtyEntries) ——默认 1000ms/64 下突发导入从「每 miss 全表写」降
  ≥64×；间隔条件意味着**稀疏交互 miss（间隔 >1s）仍每次全表写**（与旧
  行为同，非回归，全表 ~8-13MB@65536 条）。磁盘层超限收缩挪到落盘时刻，
  超额 ≤ threshold 条，内存代价可忽略。结论：量级改善成立，「数十 GB 写
  放大」场景消除。
- **E-2 扫描面扩大的闸运行成本**：实测 `check_layering.py --qgs-audit`
  0.21s、`check_qgis_spread.py` 1.06s（本 worktree，含 9 个新目录）——CI
  可忽略。

---

## R2-E 测试与架构

### E-1 测试红绿判别（逐条核过）

| 修复 | 测试 | 判别力 |
|---|---|---|
| A-01 | tst_shacache diskWritesThrottledOnMiss | 旧代码 8 miss 后盘上 8 条 vs 断言 1 条 → 红；新代码绿。宽阈值 1h/64 无时序抖动。旧用例注入 (0,1)=每 miss 落盘解耦。✓ |
| C-1 | tst_aichatcontroller retry… | 断言重试轮请求体含 `"tools"`/`paleo.`——旧代码红。✓ |
| C-2 | tst_mappingworkbench backgroundConsistency… | TMPDIR=/dev/shm 构造跨 FS 红态 + 残留目录断言。/dev/shm 不存在时 QSKIP（诚实）。✓ |
| C-3 | tst_exportpipeline unrelated… | 旧代码 mutationSeq 判废 → 红。**但单资产 journal + 50 噪声恰好绕开 A-1 的 journal 内撞号**——补「journal ≥2 id + 少量噪声」用例后才能盖住 MUST_FIX。 |
| C-4 | tst_inversion_workflow reclaim… | 只测前缀守卫（单元）；dock 端到端回收未测（UI 面，可接受）。✓ |
| C-5 | tst_catalog mapExtraNoop… | seq/revision 双零副作用 + 真变更入账。✓ |
| D-02 | tst_chips | 旧代码同事件循环步 16 条 vs 断言 8 条 → 红。✓ |
| B-1 | tst_seismic_welltie 零点半宽 | 9.0±1.5ms vs 错版 6.37ms → 红。✓（注意断言注释里的 11.3ms 数值也是反的，见 A-6） |
| B-2 | tst_seismic_index | 318.2m vs 12.73 → 红；另断言单调。✓ |
| 无测试 | A-02 / A-04 / A-05 / D-01 / D-04 | A-05/D-01/D-04 夹具成本高、A-02 需坏 padcount 样本——加固型/潜伏型可辩，但登记在案。 |

### E-2 E-2 初冻基线如实性（实跑核对）

- qgs-audit-baseline 新增 **21** 行 src/ 条目（与基线头注「新增 21 文件」
  一致）；qgis-spread-baseline 新增 **23** 行（spread 按符号级口径计数更
  多，合理）；两基线**只增未删**；file-budget 两处只下调冻结值
  （entitypanel 1620→1619、attachWorkbench 319→298，随 D-01/D-03 实际缩
  小），符合「只缩不涨」。
- 实跑 `--qgs-audit` 与 `check_qgis_spread.py` 各报 **新增 2：src/ui/
  maptools/identifytool.{cpp,h}** ——两基线头注均明文「维持册外（#604 在修
  的已知新增红，不重复登记）」。查证：identifytool 由 #552 引入且**在基线
  commit e6837e14 之前已存在**（ancestor 验证通过），旧扫描面（workflow+ui）
  本就覆盖 src/ui/maptools ——即这两处红在基线上就红，非 E-2 扩面引入，
  归属 #604（EXTERNAL_OWNED）。影响面：CI lint Source gates
  （ci.yml:125-126）在本分支与 master 同红，合并不因本分支变差；#604 合入
  即转绿。
- `check_layering.py --strict` / `check_ui_invariants.py` / （E-8 后）
  ui invariants 实跑全绿；docs_drift 报 1 处（UI_LAYER_PLAN 主窗 26 vs 实
  27 TU）——该文档不在本 range 改动内，基线期已红（pre-existing，
  EXTERNAL_OWNED）。

### E-3 分层与 include 面

- 所有改动文件层标注不变；seismicattr.h 新增 `<functional>`（数据层纯头
  依赖，无 Qt/GIS 引入）；mapexport.h 加 `<QHash>`；D-03 的自由函数 helper
  在 src/ui TU 内（视图层自由函数，无跨层 include）；`--strict` 实跑绿。
- 唯一瑕疵：slice.cpp 重复 include（见 R2-C B-2）。

### E-4 与 10 个开放 PR 的冲突面（对照 01-conflict-matrix.md）

- 高重叠风险：**#604**（master lint：qgs 散布/函数行数/传递闸）与本 range
  的 tools/check_*、两 qgs 基线、file-budget-baseline 两行下调直接同面
  ——E-2 已在基线头注显式交接 identifytool；file-budget 两行 #604 从 master
  侧下调同值时必文本冲突（易解，取低值）。**#580** 文件族含
  paleomainwindow/mapexport：D-03 触碰 paleomainwindow_workbench.cpp、C-3
  触碰 mapexport.cpp（审计时按基线行号判 hunk 不重叠；本 range 在 :185 起
  插入 ~80 行会推移 #580 相邻 hunk，rebase 时按语义解）。其余按矩阵 hunk
  级核对结论维持。

---

## 意见汇总表

| # | 分类 | 对象 | 一句话 |
|---|------|------|--------|
| A-1 | **MUST_FIX** | mapexport.cpp:196-205 | remap 新号可撞 journal 自有 id → applyJournal duplicate → 恰在 C-3 目标场景整批判废（≥2 新资产 + 窗口期 1 笔无关写即触发）；现测试恰好绕开 |
| A-2 | SHOULD_FIX | mappingworkflow.cpp:425-433 | 厚度链 publish 不消费 idMap，撞号时 decl.source 指向悬垂路径（C-3 新引入的静默死链面） |
| C-3a | SHOULD_FIX | seismictaskservice_slice.cpp:266-283 | B-2 回落分支把线号单位 distances 塞进按米消费的通道（消费端无人「如实标注」）；建议清空交兜底或带单位 |
| C-3b | SHOULD_FIX | seismictaskservice_slice.cpp:775-816 | B-4 逐道族分支 join 后无终填 reportBytes，成功时进度可停 <100%（对照 :709/_attr.cpp:498） |
| — | SHOULD_FIX(琐碎) | seismictaskservice_slice.cpp:8-9 | sgycoordinatemapper.h 重复 include |
| D-only | DOCUMENT_ONLY | shacache.h:51 / 全仓 | A-01 无生产 shutdown flush（正常退出丢 ≤1 窗口）；头注「invalidate 后首 miss 立即落盘」措辞与实现不符 |
| D-only | DOCUMENT_ONLY | seismictaskservice_attr.cpp:428-431 / register B-1 | 2π² 错版的方向是主频 √2× 偏高（零点半宽 6.37ms），注释/register 的 0.798×/11.3ms 反了；修复与测试本身正确 |
| D-only | DOCUMENT_ONLY | seismicsectiondock_inversion.cpp:236 | C-4 世代挤掉/dock 先亡路径仍漏 paleo-inv-* 树（泄漏面已大幅收窄） |
| D-only | DOCUMENT_ONLY | mapexport.cpp rewritePathSegments | 未来若 staging journal 出现 unmanaged 版本需防误改外部路径段（当前 op 集不可达） |
| EXT | EXTERNAL_OWNED | qgs 两基线 identifytool 2 文件 | 基线期已红、#604 在修、基线头注已交接；分支不因 E-2 变差 |
| EXT | EXTERNAL_OWNED | docs_drift 1 处（主窗 26/27 TU） | 基线期已红，W8 文档不在本 range |
| — | FALSE_POSITIVE | （复审自查项） | 无：register 23 项中未发现误立项；A-01 锁序、C-5 QVariant、A-02 钳制、B-2 数学、K-1/K-2/K-3/D-01..D-04 均复核成立 |

## 对「宁缺毋滥但必须深查」两点的最终答复

- **C-3（改动面最大）**：方案方向（摘陈旧判据 + 撞号重映射 + 真冲突交
  applyJournal 事务）成立，journal 无位置性 op、跨工程守卫保留、五链中四链
  idMap 改写完整；但重映射算法自身有 A-1 的可达反例，且漏了厚度链（A-2）
  ——这两处不修，C-3 在其设计目标场景（批量 + 窗口期一笔无关写）仍会以
  更混淆的文案失败，或换一种静默错位成功。
- **A-01（曾引入死锁）**：最终版所有 save/flush 路径都在恰好一次持锁内，
  无嵌套取锁、无临界区调用公有无锁入口——死锁形态已不存在；剩余为
  shutdown flush 缺位与头注措辞（文档级）。
