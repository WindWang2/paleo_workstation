# Independent Review R2B — 对抗性复审（2026-10-18）

- 复审人：Independent Reviewer 2B（对抗性挑战者，未参与任何修复编写）
- 复审对象：`git diff 366aef97...HEAD`（11 个修复 commit）
- 方法：只读代码级推演（不编译、不 ctest）；每项构造反例场景，推翻失败同样如实记录
- 结论速览：**推翻成功 1 项（R2B-MF-01，FA-001 xlsx 路径预算口径分裂）；其余 9 类攻击面全部推翻失败**，附 6 条非阻断观察（R2B-OBS-01…06）

| 攻击面 | 结果 |
|---|---|
| FC-001 INPUT 冻结回放 | 推翻失败（4 个场景逐一判定；memory provider 为潜在裂口但无在库调用方，见 OBS-02） |
| FB-001 并列裁决 | 推翻失败（给出迭代序无关性证明 + 与 GeologicalSmoothing 语义等价证明） |
| FB-002 worker 预算 | 推翻失败（逐数组盘点，漏项 < 1%（2D）/ 预算余量 ~19%（3D），均低于 20% 阈） |
| FA-001 预算闸 | **推翻成功 → R2B-MF-01**（xlsx 路径 cellBudget 每表重置，与 .xml 路径全簿口径分裂，多表放大洞仍在） |
| FC-003 publish stale 守卫 | 推翻失败（死层路径如实失败不崩溃；守卫不对称小瑕疵见 OBS-03） |
| FA-003 混合失败合并 | 推翻失败（分片序确定，先到者裁决，两口径均自洽） |
| FD-003 同地址层复用 | 推翻失败（确认新代码行为正确；重加回表语义见 OBS-04） |
| FE-003 runtime 钉版 | 推翻失败（tag/资产/sha256 三要素均实证核验；缓存旁路见 OBS-05） |
| 新测试假红绿/假稳定 | 未发现假绿；2 处环境敏感（OBS-06）；FA-003 测试的并发时序被证明确定 |
| 仓库规则合规 | 无违规（断言只收紧、语义改动均在修复授权范围内、分层全绿） |

---

## 1. FC-001 — 预测 INPUT 冻结 URI 快照（aca2aa61）

攻击目标：构造「冻结→回放」不等价的 INPUT，让新代码静默错值。

### a) 带 provider 选项的 source（`path|subset=...`、`gpkg|layername=x`）

**推翻失败。** 冻结面是 `input->source()` 整串（`predictionworkflow.cpp:270`），不是掐掉 `|` 后缀的路径——provider 选项随快照完整保留；回放侧 `QgsRasterLayer/QgsVectorLayer(uri, name, provider)`（:398-404）与 `QgisLayerService::instantiate` 的构造调用（`qgislayerservice.cpp:187-189`，`QgsVectorLayer(decl->source, ..., "ogr")`）**逐参一致**。ogr provider 对 URI 中的 `layername=`/`subset=` 组件在打开时重新解析，回放层与原层同源同选项。父版本溯源改读 `job->inputUri.section('|',0,0)`（:281），与旧代码从活层 `source()` 取首段的口径相同（旧代码 `predictionworkflow.cpp` :263 同为 `section('|',0,0)`），无漂移。

### b) 内存层（memory provider）做 INPUT

**潜在裂口，但在库不可达 → 不构成 MUST_FIX。** memory provider 的 source 只携带 schema（`Point?crs=...&field=...`），**不携带要素数据**；回放 `QgsVectorLayer(uri, name, "memory")` 得到 valid 的空层（isValid()==true），随后 Processing 在空层上运行——不是崩溃，但可能是静默的退化产出（算法层大概率报空 extent 或产出全 nodata，取决于算法）。这是「冻结回放」范式的固有限制（ConstraintWorkflow 同款范式同样不覆盖 memory 层）。逐调用方排查：

- UI 预测页只发标量参数（`paleomainwindow_attach_predict.cpp:49-51`，页面无 INPUT 输入面）；
- 工作流自动解析路径来自 LayerDeclaration，instantiate 只造 `gdal`/`ogr` 文件层（`qgislayerservice.cpp:186-189`）；
- 唯一直传活层的入口是测试（`tst_workflows.cpp:136/215/347`、`tst_predict_assembly.cpp:134`），全部经 `layers.instantiate` 文件层。

判定：当前代码库无任何路径能把 memory 层送进 INPUT；记录为 R2B-OBS-02（范式边界，接新调用方时的纪律项）。

### c) CRS 动态/无效的层

**推翻失败。** `input->crs().toWkt()` → `QgsCoordinateReferenceSystem::fromWkt` 的往返在 PROJ6+ 对动态 CRS（带历元）稳定；无效 CRS 的 `toWkt()` 返回空串 → 回放侧 `if (!job->inputCrsWkt.isEmpty())` 跳过 setCrs（:407-408），层用 provider 从文件自读的 CRS——与原层「无效 CRS 覆盖」的差异仅在用户显式设置无效 CRS 的病态输入上，且方向是回退到文件 CRS（更合理）。另有一个正向验证：`instantiate` 在返回前应用 `MappingArtifactWriter::restoreRasterCrs/restoreVectorCrs`（`qgislayerservice.cpp:199-202`），冻结发生在其后，**侧车恢复的 CRS 已进快照**，回放等价。

### d) prepare 与 compute 之间删除源文件

**新行为是如实失败，且优于旧行为。** 推演两条时间线：

- 旧行为：活层指针跨线程传给 Processing（本身即 #235-2 违例）。若层仍存活且 GDAL 已持有 fd，Linux 下 unlink 后仍可从未链接 inode 读出数据——预测可能「成功」，产物基于一个用户已删除的数据版本（静默错值方向）；若层对象已死则 UB。
- 新行为：worker 重建 `QgsRasterLayer(uri,...)` 打不开 → `isValid()==false` → `fail(tr("无法读取预测输入图层：%1"))`（:405-406），错误消息含具体 URI，任务失败、无产物、无登记。如实失败。

### 附加检查（任务书外）

- 冻结代码不删除 instantiate 产物——auto-resolve 路径的层由 QgsProject 所有（`instantiate` 内 addMapLayer），无泄漏；caller 直传层归调用方所有，行为同旧。
- INPUT 为字符串路径（非层指针）时：`value<QgsMapLayer*>()` 转换失败 → 冻结块跳过 → params 原样传 Processing（QGIS 接受路径串）——与旧代码逐位一致。
- ONNX 分支不经冻结（自有 onnxSourcePath 快照，:160-163），不受本改动影响。

**FC-001 裁定：修复成立。** 记忆层裂口为 OBS-02。

---

## 2. FB-001 — 转面多数滤波并列裁决确定化（84622320）

### 迭代序无关性证明

新规则（`faciespolygonize.cpp:176-186`）：

```cpp
const int center = grid.at( i );
int best = center;
int bestCount = counts.value( center, 0 );
for ( auto it = counts.cbegin(); it != counts.cend(); ++it )
  if ( it.value() > bestCount ||
       ( it.value() == bestCount && best != center && it.key() < best ) )
  { best = it.key(); bestCount = it.value(); }
```

不变式：(I1) `bestCount == counts[best]` 恒成立（初始化即真；两分支均同步赋值）。(I2) `bestCount` 单调不降（分支一严格升、分支二持平）。(I3) center 只能被**严格更高**计数置换（分支二要求 `best != center`，分支一要求 `>`）。

先证 center 必在 counts：中心格位于自身 3×3 窗口（dx=dy=0 恒界内），且非 nodata（`if (grid.at(i)==kNodata) continue` 在计数前拦截），故 `counts[center] >= 1`——「中心不在 counts」不可达，`counts.value(center,0)` 无 0 起步的退化分支。

任意迭代序下终态：
1. `bestCount == M`（全体计数最大值）：反设终态 `bestCount < M`，取 `counts[w*]=M`，w* 被访问时由 I2 其时 `bestCount ≤ 终态 < M` → 分支一触发 → bestCount 升至 M，与 I2 单调矛盾。
2. 若 `counts[center] == M`：由 I3，center 无法被置换，终态 `best == center`。
3. 若 `counts[center] < M`：终态 `best != center`（其计数 < M=bestCount）。对任意满足 `counts[w]==M && w < best终` 的 w：w 被访问时，若当时 `bestCount < M` → 分支一 → `best=w ≤ w`；若已 `==M` → best≠center 且 `w<best` → 分支二 → `best=w`。两种情况访问后 `best ≤ w`，而 best 只会在同计数下变小（分支二），故终态 = max-count 挑战者中的最小键。

即终态 = **「count(center)==M ? center : argmin(key | count==M)」**，与访问序无关。∎

### 与 GeologicalSmoothing 逐语义对照（`paleoalgorithms.cpp:1125-1150`）

GS 的裁决：先在全体 distinct（含 center）里取 `counts 最大、键更小` 者，再 `if (centerIdx >= 0 && counts[centerIdx] == counts[best]) result = center`。展开后同为「center 计数并列最大 → center；否则 max-count 组取数值最小」。两者逐语义等价：

| 情形 | GS | smoothMajority（新） | 一致 |
|---|---|---|---|
| center 计数严格最大 | best 落 center | center 不被置换 | ✓ |
| center 并列最大 | result=center 覆盖 | 分支二对 center 不触发 | ✓ |
| center 落败，max 组并列 | 取 values 最小 | 取 key 最小 | ✓ |
| center 为 nodata | 提前 `dst=center`（:1087-1090） | 提前 `continue`（:159） | ✓（双双不进入裁决） |

「中心不在 counts」在两实现中均不可达（GS 的 center 同样先过 isNoData 拦截、自身必入 3×3 窗口）——两实现不存在该分叉上的行为差异。

### 测试有效性核验（对抗侧）

`faciesPolygonsSmoothTieDeterministicAcrossHashSeeds` 的机制成立，需两项 Qt6 事实，均在本机头文件实证：

- `qHash(int, seed) = QHashPrivate::hash(size_t(key), seed)`（`/usr/include/qt6/QtCore/qhashfunctions.h:122`）——**整型键的哈希混入种子**，全局种子改变 QHash<int,int> 的桶位即迭代序；
- `QHashPrivate::Data` 构造时 `seed = QHashSeed::globalSeed()`（`qhash.h:566`）——smoothMajority 的函数级 counts 在 `qSetGlobalQHashSeed(seed)` 之后构造，吃到测试种子。

旧代码「先遍历到的并列者胜」→ 键 {3,7}（各 4 票）的胜者随种子变 → 图斑/PROVENANCE 漂移 → 测试红。PROVENANCE 归一化只需摘 `"input"`：该 JSON 里唯一随夹具名（含 seed）变化的字段是 `"input": src`（`faciespolygonize.cpp:1427`），其余均为计数/参数/网格戳，与种子无关——不会假红。

**FB-001 裁定：修复成立，数学与参照系双重确认。**

---

## 3. FB-002/FB-003 — SGS 并行工作集与聚合输出计入内存预算（fe6ef110）

### 2D（`sgs.cpp` runRealization，:205-215）逐堆分配盘点

| 分配 | 大小 | 是否入估算 |
|---|---|---|
| `SimulatedLattice::m_values.assign(cols*rows)` | cells×8B | ✓（lattice.values） |
| `std::vector<std::size_t> path(cells)` | cells×8B | ✓（path） |
| `std::vector<double> gaussianField(cells)` | cells×8B | ✓（gaussianField） |
| `staticNeighborhood`（≤64×4B）+ `simulatedNeighborhood`/`neighborhood`（≤64×CondPoint(32B)）+ queryNearest 堆/collected（≤64 项） | 各 ≤2-4KB | ✗ 漏项 |
| `KrigingWorkspace`（a 为 n²、n≤64 → ≤32KB；b/weights/LuWorkspace 同量级） | ≤~34KB | ✗ 漏项 |
| `mt19937_64`（栈对象 3.1KB 状态） | 0 堆 | — |
| 共享面（deduped/NormalScoreTable/NeighborIndex/输出张量） | 每 worker 0 份 | 计入 outputBytes ✓ |

漏项合计 ≈ 40KB / worker，对 cells≥1e6 的工作区（≥24MB）占比 < 0.2%。**`3·cells·8B` 估算成立**（漏项 ≪ 20%）。

### 3D（`sgs3.cpp` runRealization，:505-514）逐堆分配盘点

| 分配 | 实际字节/目标 | 备注 |
|---|---|---|
| `path` / `gaussianField` / `m_values` | 各 n×8B = 24B/n | |
| `m_lookup`（unordered_map<uint64,uint32>，reserve(n)） | 节点 24B 载荷 → glibc chunk 32B + 桶数组 ~1.06n×8B ≈ **41B/n**（libstdc++ 快哈希不缓存节点哈希） | `reset()` 的「同目标集复用索引」快路径在真实调用面**不可达**（lattice 是 runRealization 栈对象，每实现重建），与逐 worker 一份的记账口径一致 |
| `m_simulatedByGroup` | 组数×~40B | 可忽略 |
| 邻域/KrigingWorkspace 同 2D | ≤~40KB | 可忽略 |
| 合计实际 | **~65B/n** | 预算 80B/n，余量 ~19% |

**`n×80B` 估算成立且保守**（「56B/项保守估」的注释口径略低估实际 ~41B，但 80B 的总预算仍覆盖）。

### 峰值公式核对

并行档峰值 = 输出张量（先建，:190-192）+ workers×工作区，与 `workersForPeakBudget(outputBytes, workspacePerWorker)` 的算式一致；workers 收缩不减 R（每实现独立派生流 `seed + r·黄金比`，:206-207，与 worker 分派无关）——**收缩不动成果语义**成立，`parallelRealizationsMatchSequential` 既有口径保持。

### 已知的未闸路径（修复自己披露，判定为「记录在案的设计选择」而非缺陷）

串行档（有回调或 R==1）不经 kPeakBudgetBytes：2D 最坏 {R=53, cells=1e8} 峰值 ≈ 4.24GB 输出 + 2.4GB 工作区 ≈ 6.6GB > 6GiB；3D 串行更甚（n×65B 工作区在 n=5e8 时 ~35GB）。`sgs_internal.h` 修改后的注释明说「串行档……不再向下收」，是披露过的既有行为，非本次修复引入的回归。但注意一个组合事实（未见诸任何文档，记为 R2B-OBS-01）：**fillSgs 调 sgs3 时恒带 progress/cancelled 回调（`sgsfill.cpp:303-311`），因此 FB-002 的 worker 收缩对生产上的 SGS 充填链路永远不生效**——充填链路的内存上限实际只由 #468（单带输出 ≤4GiB）+ FB-003（聚合 ≤4GiB）+ 无闸的串行工作区（~65B×单带目标数）三者构成，极端参数下工作区仍是主导项。建议后续把串行档工作区也纳入拒收或文档化最坏值。

### FB-003 聚合闸

`R×ni×nj×nk×4B > 4GiB` 入口拒收（`sgsfill.cpp:126-139`），位于任何重分配之前（out->assign 在 :220，zoneTargets 构建在其后）——拒收便宜且诚实。`grid.cellCount()` 为 `int ni*nj*nk`（`stratgrid.h:67`），测试取 1024×1024×2047=2,146,435,328 恰在 int32 界内，闸内乘法全程 int64，无溢出。

**FB-002/FB-003 裁定：估算与闸门成立**（任务书的 20% 漏项标准未触发）；串行档/OBS-01 如上披露。

---

## 4. FA-001/FA-002 — 工作簿列填充总量闸 + .xml 大小闸（fb6f7b7c）

### 「先 headers 后 rows」与跨 sheet 语义

`finishRow` 判定链（`outsourceworkbook.cpp:104-129`）：`sheet.headers.isEmpty()` 时首个含文本行成为 headers（计账 `cellBudget -= headers.size()`），其后每行 `-= row.size()`。换表 `finishSheet` 只重置 sheet 对象，**不重置 cellBudget**（.xml 路径）——表头与数据行一律计费，无免费额度。检查点在每 token 读取前（:145-150），超限过冲 ≤ 一行（≤16384 格），可忽略。

### 任务书场景：100 个 sheet 各 4.1M/100 格

- **SpreadsheetML（.xml）路径**：cellBudget 声明在函数级（:102），全簿累计 → 4.1M > 4M → 尾部拒收，文案如实（「累计单元格超预算……整簿拒收」）。**预算是全局的，行为合理（拒收）**。✓
- **wellcompositexml 两读口**：cellBudget 均为函数级、跨 sheet 累计（`wellcompositexml.cpp:94,572,995`）✓。
- **xlsx 路径：推翻成功，见 R2B-MF-01。**

### R2B-MF-01（MUST_FIX）：readXlsx 的 cellBudget 每表重置，「整簿拒收」名不副实

`outsourceworkbook.cpp:500`：`qint64 cellBudget = kMaxWorkbookCells;` 位于 `for (const QPair<QString, QString> &item : sheetTargets)` 循环**体内**——每张表重置 4M 预算；而 `result.sheets.append(sheet)`（:674）把每张表的行全量驻留到函数返回。错误文案却写「工作表「%2」**累计**单元格超预算（%3）——文件疑似畸形，**整簿拒收**」（:528），自述语义与实现分裂。

可利用构造（全部闸门皆绿）：workbook.xml 列 100 张表；每张表 244 行、每行仅一个 `<c r="XFD1" t="s"><v>0</v></c>`（~30 字节/行）→ 每表 XML ≈7KB、100 表全档 <1MB（zip 包/单条目 512MiB 上限远未触碰）；每行 `padTo(&row, 16384)` 填充 QStringList ≈128KB/行 → 每表 244×16384=3,997,696 < 4,000,000 过闸；**全簿驻留 100×~32MB ≈ 3.2GB**——这正是 FA-001 威胁模型（「~40B 换 ~128KB、GUI 线程 OOM/假死」）在 xlsx 格式上的原样复现，而 xlsx 恰是三种格式中最常用的一种。

对比：.xml 路径同参数全簿 4.1M 直接拒收——同一闸门、同一常量、同一文案，两种格式行为不同。修法明确：把 cellBudget 声明提到 sheetTargets 循环之外（与 readSpreadsheetMl 对齐），文案去掉按表归因即可；回归测试应补多表叠加夹具（现有两夹具均为单表）。

**严重度**：P2（与 FA-001 原发现同级——受控环境读入畸形工作簿的内存 DoS 面；previewdoc/cuttingsdoc/mkproject/目录扫描各接线仍在）。

### FA-002 直读大小闸

`info.size() > 512MB` 在 readAll 之前（`outsourceworkbook.cpp:751-757`；`wellcompositexml.cpp:69-77,919-925`），与 zip 层 512MiB（`ziparchive.h:18,22`）同口径。测试用稀疏 `resize(512MB+1)` 验证闸先于读——旧代码 readAll 后 XML 解析报错、文案无「512MB」→ 真红。成立（测试内存足迹见 OBS-06）。

---

## 5. FC-002/FC-003 — 保存 retain 语义 + legacy 发布 stale 守卫（d0a23078）

### 任务书场景：legacy 层在 startPublish 之后被 removeLayer/destroyed

时间线推演：

1. startPublish（GUI 线程）：`snapshotInputForPublish` 已把 legacy 记录的 **file path** 固化进 `job->input.files`；守卫挂 `destroyed → markStale`（`mapversioncontroller_async.cpp:446`）。
2. 用户删层 → `destroyed` → `stale=true`。**注意：删除图层对象不删除磁盘文件**。
3. worker 侧 `computeSnapshot` → `prepareMapSnapshot`（`mapsnapshot.cpp:42-94`）逐文件处理死层 path：
   - 文件仍在：正常哈希+拷贝（拷贝的是删除时刻的自洽文件内容），compute 成功；
   - 文件也被删：`if (!QFileInfo(source).isFile())` 且 operation=="publish" → `out.error = "地图来源文件缺失：%1"`（:53）**如实失败**；若在拷贝中途被删：open/read 失败（:71/:76）或哈希不符（:80）同样如实失败。
4. commit（GUI 线程）：stale 检查在一切之前（`mapversioncontroller_async.cpp:459-461`）→「发布来源在快照期间被编辑，请重试」→ isPublished 保持 false，无孤儿版本（staging 由 cleanup 的 discardPreparedMapSnapshot 清）。

**裁定：不崩溃、如实失败、可重试——FC-003 守卫在该场景成立。**

其余推演：

- 层在 startPublish 之前已死：`m_layers->layer()` 返回 nullptr → `else if (layer)` 跳过 → 无守卫但文件仍在 → 快照照常（语义上 legacy=当前文件状态，成立）。
- 只 startEditing 后无净改动就 stopEditing：`editingStopped` 保守置 stale → 误拒一次（保守方向的假阳性，与 save 路径 launchSaveSnapshot :341 完全同款口径，对称成立）。
- 冻结（saved_layers）来源不挂守卫：由 64 位 sha256 期望值兜底（`mapsnapshot.cpp:56-58,80`），篡改会在哈希比对处如实失败 ✓。
- stale 检查先于 `j.error`：compute 已因他因失败时 stale 文案优先——两条都是诚实失败，自洽。

### R2B-OBS-03（非阻断）：publish 完成路径不摘守卫，与 save 路径不对称

`launchSaveSnapshot` 的 jobCompleted 回调调 `m_async->disconnectSourceWatches()`（:382），`startPublish` 的 jobCompleted **没有**（:480-487）、`if (!task)` 失败路径也没有（save 的 :392 有）。后果：legacy 发布完成后其 ~10 连接/层 残留到下一次 save/publish 才被 `disconnectSourceWatches()`（:326/:426）清掉；lambda 按值持 shared_ptr，无悬垂，多次发布连接数暂态累积。无正确性影响（新任务新 stale 旗），纯资源卫生——建议补一行对称。

### FC-002（同步 saveVersion retain 语义）

旧行为「*error 写失败文本却返回成功 v」确属自相矛盾态；新行为 qWarning + 干净返回。全库检索确认无调用方依赖旧行为（`tst_versions` 的 retain 族断言只查 `updateVersionExtra`/`versionById`，:1264/:1325-1327；`startSaveVersion` :165-167 的 snapshotFinished 消费 `result.version>0`）。与异步路径（#480：snapshotFinished 捎带 retainError 文本）口径不同但均有意为之且注释在案。成立。

---

## 6. FA-003 — 并行扫描读错误回落顺序路径（a70ddfea）

### 混合失败（一个分片取消、另一个 io 错）合并循环先命中谁

合并循环按分片下标 0..N-1 顺序（`segyreader.cpp:1104-1128`），**与线程完成时序无关**——每个 `!sh.ok` 分片按其下标被访问：

- 先遇 cancelled 分片（下标更小）→ checkpoint 语义：`m_lastScanPartial=true` + 连续前缀 `m_scannedOffset` + `*error="cancelled"` → openCached 落 checkpoint 返回 false。后序分片的 io 错不再上报——但 checkpoint 只含连续前缀（丢弃后序分片条目是「连续前缀」口径的正确行为），续扫时该 io 错区域会再次如实暴露。
- 先遇 io 错分片 → 不置 partial → openCached 走 `resetState()+顺序 open()`（openCached :163-165 一带）重扫；若用户取消仍持守，顺序扫描自身按取消语义收尾。

两种结局都确定（由下标序而非竞态决定）、都诚实、失败模式各自自洽——**取消优先还是 io 优先都 acceptable，且选择是确定的**。推翻失败。

### io 错分支的状态残留

返回 false 时 `m_index/m_badTraceOffsets` 已并入前序分片条目、`m_geometry` 未冻结——调用方 `resetState()` 后全清，无脏状态渗透。✓

### 回归测试的确定性证明（对抗侧最想攻的点）

`parallelScanIoErrorFallsBackToSequential` 的截断发生在**首个进度回调**。每个分片在自己的第 0 条 trace 即触发回调（`(i - sh.from) % 128 == 0` 在首条成立，:1088），且回调在分片线程内同步执行——因此**任一分片推进到第 1 条 trace 之前，截断（首个到达的回调内 compare_exchange + resize）必然已完成**；不存在「分片先扫完再截断」的竞态窗口。截断后保留 2400 道 < 分片 1 起点 3000 → 分片 1-3 全部 io 失败，分片 0 部分成功——确定性触发回落路径。测试稳定。✓

**FA-003 裁定：修复成立。**

---

## 7. FD-002/FD-003/FD-004 — 预览工具/记账/ask seam（c56a70e2）

### FD-003 任务书场景：A 置换出去 → destroyed → 同地址新层 B 进表

逐步推演（`previewmapcanvas.cpp:106-134,177-203`）：

1. `setLayers({A})`：三层记账 `m_visible[A]=true` 等；`watchLayer(A)` 设 `paleoPreviewWatched=true` 属性 + destroyed 钩子。
2. `setLayers({C})`（新表不含 A）：**新增摘除循环**清 `m_visible/m_opacity/m_blend[A]`；A 的 destroyed 连接保留（sender 死时 Qt 自动断）。
3. A destroyed → `forgetLayer(A)`：`m_layers.contains(A)` 为 false（步骤 2 已置换）→ 早退 ✓（:196-197）。无悬垂键残留。
4. 同地址新层 B `setLayers/addLayer`：
   - `watchLayer(B)`：属性是 QObject 动态属性，随对象生灭——B 是**新对象**，`property("paleoPreviewWatched")==false` → 重新挂钩 ✓（:179-184，注释明示「属性随层对象生死」）；
   - `m_visible.contains(B)`（同地址键）已被步骤 2 清掉 → B 拿到新鲜默认值 true/1.0/SourceOver，**不继承 A 的陈旧设置** ✓。

旧代码对照：步骤 2 不清记账 → A 的隐藏/透明度条目永存 → B 同地址直取 → 「TOC 可见、画布不画」正是修复所述缺陷。**推翻失败，修复正确。**

### R2B-OBS-04（非阻断，权衡确认）：置换出表再回表的层丢失用户显式设置

新语义下「层离开预览表」即丢三层记账；若层对象存活且此前经 `setLayerOpacity` 已把 0.5 写进**层对象本体**（:256 `layer->setOpacity`），重进表后 `m_opacity` 镜像重置 1.0 而层本体仍 0.5 → 滑杆读数与渲染短暂脱钩（重排/叠加不受影响，同表路径无此问题）。该权衡与既有 `removeLayer`（:165-175，同样只清镜像不复位层本体）语义一致——修复选择向既有 API 口径对齐而非另创新规，可接受；如要更严，可在摘除时同步 `layer->setOpacity(1.0)/setBlendMode(SourceOver)` 复位本体。

### FD-002 deactivate 清理

与 identify（:324）/profile（:416）既有的 deactivate-藏带契约逐字同款；QGIS 工具切换链（setMapTool 先 deactivate 旧工具，previewmaptools.cpp:532 注释）保证调用时画布存活；析构路径（scene 先亡 SIGSEGV 面）由既有的 ~PreviewMeasureTool 特判承担，deactivate 不在析构链上被调用。测试 `measureDeactivateHidesBandAndKeepsPoints` 真红绿（旧码无 override 无 isBandVisible → 编译红；或行为红）。成立。

### FD-004 ask seam 分层合规

`nativeattributetablehost.h` 仅新增 `<functional>` + `std::function` 成员——**无 ui/ include**（qgis 层 QtWidgets 本豁免，QMessageBox 原样保留为降级路径）；注入点在视图层 `attributetablepanel.cpp`（视图引 qgis/ 不在禁令表）。seam 语义与原生路径逐分支等价（Cancel→false / Save→saveEdits / Discard→落 rollback），offscreen 直存分支与 allowCancel=false 两键路径原样。合规且语义不变。

---

## 8. FE-003 — AppImage runtime 钉版（cc6ed122）

三项实证核验（只读网络检查）：

1. **tag 存在**：`AppImage/type2-runtime` release `20251108` 真实存在且含 `runtime-x86_64` 资产（GitHub releases 页）；
2. **sha256 匹配**：实际下载该资产计算摘要 = `2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`，与 `RUNTIME_SHA256` 钉值**逐位一致**；
3. **失败即断**：脚本 `set -euo pipefail`（make_appimage.sh:9），`sha256sum -c` 失败直接中止，不会带病继续。

兼容性风险（任务书明示低危记录）：type2-runtime 为静态 ELF、Type-2 格式自 2019 后稳定，20251108 距今 ~11 个月的钉版比 continuous 反而更保守；主要残留是「未来 AppImage 规范演进需人工升钉」的维护成本。

### R2B-OBS-05（非阻断）：缓存旁路校验

`if [ ! -x "$RUNTIME" ]`（:38）使 sha 校验**只覆盖新下载**：升级本脚本后，build/appimage 里修复前从 continuous 下载的旧 runtime 会被原样复用、不经任何校验（还可能瞒过「钉版已换」的事实）。建议无条件 `sha256sum -c`（文件已在本地时开销可忽略）或缓存键带上 sha 前缀。

FE-004（LO profile）：`QTemporaryDir` 惰性建 + 服务存续期保活 + 析构清理，替换固定 pid 名——预布设 symlink 面与 /tmp 永久泄漏两个问题同时收口；单转换在途（m_convProc 单实例）保证单 profile 复用无锁竞争。成立（测试环境敏感点见 OBS-06）。

---

## 9. 全部新测试的假红绿/假稳定排查

逐个判定（详细推演已分布在各节）：

| 测试 | 旧码上 | 判定 |
|---|---|---|
| tst_workflows FC-001 三段断言 | 红（params 仍含 INPUT 指针 → QVERIFY2 命中） | 真红绿 ✓ |
| tst_algorithms FB-001 六种子 | 红（QHash 种子经 qHash(int,seed) 混入 + Data 构造捕获种子，两事实实证；{3,7} 并列胜者随种子漂移） | 真红绿 ✓ |
| tst_geostat_sgs FB-002 直测 | 编译红（新函数） | 可接受红 ✓ |
| tst_sgsfill FB-003 | 红-by-crash（17GB assign → bad_alloc/OOM-kill；非优雅红但方向正确） | 可接受红 ✓ |
| tst_io_outsource 放大夹具 | 红（旧码 39MB 全吞、result.ok=true → QVERIFY(!ok) 败） | 真红绿 ✓ |
| tst_io_outsource 超限稀疏 | 红（旧码 readAll→解析错文案无「512MB」） | 真红绿 ✓ |
| tst_wellfacies 双闸 | 同上两条机制 | 真红绿 ✓ |
| tst_segy FA-003 | 红（旧码 io 错落 partial checkpoint → openCached false）；新码时序确定性已证明（首回调同步于各分片第 0 条） | 真红绿 ✓ |
| tst_versions FC-003 worker-gate | 红（旧码无守卫 → compute 复制已编辑文件 → 发布成功 → !ok 断言败）；gate 持唯一 worker 使编辑确定落在窗口内 | 真红绿 ✓ |
| tst_previewmap_canvas FD-003 | 红（旧记账未摘 → isLayerVisible(a)==false） | 真红绿 ✓ |
| tst_previewmap_tools FD-002 | 编译红（isBandVisible 新增）或行为红 | 可接受红 ✓ |
| tst_evolutionplayer FD-001 | 编译红（reloadFrames 新 API）；cleanup() 有 AreaRules::reset()，无跨用例污染 | 可接受红 ✓ |
| tst_import FE-004 | 红（旧码固定 pid 名命中禁用断言）；**环境敏感见 OBS-06** | 真红绿 ✓ |
| tst_perf FE-002 两槽 | 编译红；新断言绑生产真值（pool=4 实证 paleotaskservice.cpp:220；2048 为 2 的幂、double 除法精确、ceil(2048.0)=2048 确定命中；拒收路径 1e10>1e8 必抛 QgsProcessingException） | 可接受红 ✓ |
| tst_catalogstore FA-004 | 红-by-crash（resize(2e9)×~100B）；新码确定 | 可接受红 ✓ |
| tst_factorworkflow FE-001 | 红永真→真断言（字面量与 litholexicon.cpp:37-53/55-70 生产值逐串核对一致，「未知」经 fall-through 往返成立） | 真红绿 ✓ |

**假绿（旧码也绿）：0 个。** 假稳定（CI 必挂）：0 个。环境敏感 2 处 → R2B-OBS-06：

- `tst_import::documentConversionProfileDirIsRandomAndCleanedUp` 的 `QCOMPARE(after.size(), 0)` 扫的是**共享 /tmp**（TMPDIR 不在每测试沙箱之列）：开发机上旧二进制遗留的 `paleo-lo-profile-*` 目录、或并行运行的其他转换都会造成假红（CI 干净容器不受影响）。
- 三个 512MB 稀疏文件夹具在旧码路径上会真实 readAll 出 512MB QByteArray：内存受限 runner 可能以 OOM-kill 收场——仍是红，但红的原因不是断言。

---

## 10. 仓库规则合规

- **不放宽断言**：全 diff 被删除的断言仅 4 行（LithoLexicon 永真 QCOMPARE、tst_perf 两条自证、一条自算 cellSize），全部属「永真/自证 → 真值」的收紧方向；无任何既有断言被弱化或删除。✓
- **不改地质语义**：FB-001 改变并列裁决的胜者选择，但旧行为是「跨进程不确定」（QHash 随机种子）——不存在可称为「语义」的稳定旧值；新值钉死并与 GeologicalSmoothing 对齐。FB-002 收缩 worker 数：每实现种子独立于分派（`seed + r·黄金比`），结果逐位不变。FB-003/FA-001*/FA-002/FA-004 是拒收面扩展（畸形输入从 OOM/崩溃改为如实报错），不动合法输入产出。FC-002 只改错误文本的归属面。✓（*FA-001 的 xlsx 半壁见 R2B-MF-01，属「修不足」而非「改语义」。）
- **分层合规**：触达的 src 文件头三行层标注齐全（nativeattributetablehost.h=QGIS 封装、workflows.h=功能、dataimportservice.h=数据、attributetablepanel.cpp=视图、evolutionplayerpanel.h=视图、sgs_internal.h=数据，逐一核过）；qgis 层新增仅 `<functional>`（无 ui/ include，FD-004 seam 合规）；功能层 predictionworkflow 新增 qgis core 头（qgsrasterlayer/qgsvectorlayer/qgscoordinatereferencesystem——非 QtWidgets、非 ui/）；数据层 dataimportservice.h 新增 QTemporaryDir（QtCore）。✓

---

## 结论汇总

**推翻成功（MUST_FIX）— 1 项：**

- **R2B-MF-01**（FA-001，`src/io/outsourceworkbook.cpp:500`）：readXlsx 的 cellBudget 在 sheetTargets 循环体内声明、每表重置，与 .xml 路径的全簿累计口径、与自身「整簿拒收」文案、与 4M 常量的意图三方分裂；100 表 × 各 ~4M 格的 <1MB xlsx 可绕过全部闸门驻留 ~3.2GB——FA-001 的威胁模型在最常用格式上原样存活。修法：cellBudget 提升到循环外 + 多表回归夹具。

**非阻断观察（OBS）：**

- **R2B-OBS-01**：fillSgs 恒带回调（sgsfill.cpp:303-311）→ sgs3 永远走串行档，FB-002 的 worker 收缩对生产 SGS 充填链路不生效；串行档工作区（~65B×单带目标数）仍无上界，极端参数下是链路内存主导项。
- **R2B-OBS-02**：FC-001 冻结回放对 memory provider 层不等价（schema 回放、数据丢失、isValid 仍 true）；当前无在库调用方可达，属范式纪律项。
- **R2B-OBS-03**：startPublish 的 jobCompleted/失败路径缺 `disconnectSourceWatches()`，与 launchSaveSnapshot 不对称；守卫连接暂态累积，无正确性影响。
- **R2B-OBS-04**：FD-003 置换出表再回表的层丢三层记账，且 m_opacity 镜像与层本体 opacity 可能短暂脱钩（与既有 removeLayer 口径一致，权衡确认非缺陷）。
- **R2B-OBS-05**：make_appimage.sh 对已缓存 runtime 跳过 sha 校验（`if [ !-x ]`），钉版对旧缓存不生效。
- **R2B-OBS-06**：tst_import FE-004 断言扫共享 /tmp（脏环境假红）；三个 512MB 稀疏夹具在旧码路径真实分配 512MB（受限 runner 以 OOM 代红）。

其余全部攻击面（FC-001 四场景、FB-001 确定性、FB-002 估算、FC-003 死层、FA-003 混合失败、FD-003 同址复用、FE-003 钉版、新测试红绿面、仓库规则）**推翻失败，修复经得起对抗推演**。
