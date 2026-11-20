# P5 约束编辑生命周期、复制身份与撤销一致性（2026-11-19 批次）

- 分支：`codex/paleo-p5-edit-lifecycle-20261119`（基线 `origin/master` = `08ca3e52`，2026-11-19）
- 工作树：`../paleo-p5-edit-lifecycle-20261119`（受控依赖：共享 deb 闭包前缀
  `/home/kevin/projects/paleo-deb-prefix-20261119`，stamp 校验通过）
- 关联 issue：#1009（会话外撤销开第二会话）、#1017（失败分支内存幽灵项）、
  #1013（全量扫描/导入二次复杂度）、#990（复制后首匹配改错要素）、
  #1015（每条编辑命令重装+重读 sidecar）。均为源码核对确认现存后修复；
  未在本轮逐条运行旧 GUI 复现（修复按行为契约与新回归双向验证）。

## 行为改动（按工作包）

### W1/W2 修改上下文与单一撤销归属（#1009）

`ConstraintWorkflow` 新增私有修改上下文 `ModifyOrigin{User, ClosureReplay}`：

- `addConstraintInternal` / `removeConstraintInternal`：公共 API 以 `User`
  进入，行为与旧实现一致（可开会话时仍走 `ensureConstraintSession`）。
- `drawConstraintResult` 的 #742 闭包（undo=删除、redo=原参数重画）改走
  `closureUndoDraw` / `closureRedoDraw`（`ClosureReplay`）：
  - 重放绝不 `ensureConstraintSession`、不把补偿压进原生栈——同一操作只有
    闭包栈这一份撤销历史；
  - 同层位存在活会话时如实拒绝（`ClosureEditStack` 失败条目原地保留），
    会话结束后可继续重放；
  - 闭包携带归属存储身份（规范化 GPKG 路径，`closureGuard`）：切工程后
    重放拒绝而不是写到新库；无存储的内存工作流维持旧重放能力；
  - redo 的合法 id 再分配保留：新 id 写回闭包内共享 `current`，后续 undo
    删除的正是重画产生的那个业务对象（不经 FID/几何猜回）。

### W3 持久/内存/图层一致性（#1017）

`addConstraintInternal` 重排（store 无会话路径）：

1. 可提前判定的拒绝全部前移到任何持久写入之前：层位 subset 可表达性
  （旧实现先 append 后发现「层位名带 |」再补偿删除）、几何提交门、参数 JSON。
2. durable 成功前不发布内存项；`layers->declare` 失败时补偿回收持久行，
  **补偿返回值被检查**：
   - 补偿成功 → 干净回滚（内存零残留，几何缓存种子随 revision 失配自动失效）；
   - 补偿失败 → 「已保存、声明未完成」部分完成：返回 true + `*error` 警告
     （`drawConstraintResult` 据此返回 `SavedLayerFailed` 并照常登记闭包撤销），
     内存/信号/撤销项与 durable 事实一致——不再返回普通失败诱导下次重试
     制造重复约束。
3. `removeConstraintInternal` 会话路径同样回收内存投影（旧实现只回收直写路径）。

### W4 批量导入与有界查询（#1013）

- `ConstraintStore::appendBatch`：一次事务写完整批（批内唯一 + 存量 `byId`
  查重都在事务内、任何写入之前；任一失败整批回滚）；审计按同一
  `operationId` 逐条登记 create 事件（批谱系可追溯）。
- `ConstraintWorkflow::importConstraintBatch`：一批预验证（subset/活会话互斥/
  几何提交门/参数 JSON）+ 一次编号扫描 + 一次事务 + 一次内存与几何视图
  同步；`publishConstraintImport` 改走该入口（不再 N 次 `addConstraint`）。
  已提交后的声明失败＝「已保存、呈现未完成」（不回滚持久批）。
- 编号索引（`SequenceIndex`，store+revision 键控）：自写后记录 seen revision，
  连续绘制/导入不因自身写入触发重扫；他人写入（revision 变化）后重扫一次
  并入已知最大值——「另一实例写入后正确刷新」与「连续操作有界」同时成立。
- `removeConstraintInternal` 全量 load 4 次 → 2 次（定位 + 按层位读回兼做
  视图同步输入）；`seedConstraintGeometry` 把新增行几何类型并入缓存条目，
  单条绘制不再全量重读重解析。
- `ConstraintStore::debugLoadCount()` 诊断计数（load 次数 oracle，供回归断言）。

### W5 复制身份映射（#990）

- `MappingWorkbench::copyForEditing` 新重载（旧 4 参签名兼容转发）输出
  `CopyFeatureIdentity`：`copyFidBySourceUid`（有稳定 UID 来源，经
  `MappingFeatureIdentity::find` 解析）、`copyFidBySourceFid`（无 UID 历史
  来源，复制过程内按 provider 迭代顺序一对一位置映射）、
  `ambiguousSourceUids`（重复 UID → 必须拒绝）。映射只在本次复制结果内
  有效，不落盘、不改 RAW 来源、不把 provider FID 当长期身份。
- 画布右键「更改此要素的相」（`paleomainwindow_workbench_wells.cpp`）删除
  WKB+facies_code 首匹配扫描：按映射解析目标；重复 UID/映射缺失/副本要素
  已消失给出可理解拒绝（statusBar 10s），不静默取第一条。

### W6 编辑器一次绑定（#1015）

- `FaciesPartitionState` 新增绑定签名（`kBindingSignature` + 访问器）：
  `styleLayer` 仅在签名（版本 id + footprint sidecar 路径与 sha256 摘要 +
  相表 JSON）变化时重跑 `FaciesPartitionEditor::install` +
  `loadFootprint`。签名存图层实例：图层重建/换源自然重装；install 失败不记
  签名（下次照实重试，不跳过业务校验）。
- 普通属性编辑、undo/redo、选中变化（`editCommandEnded` → `styleLayer` 路径）
  只做轻量刷新：不重读 sidecar、不重装编辑生命周期、不重复连接信号
  （信号连接原有的 property 门保持）。
- 诊断计数：`FaciesPartitionEditor::installCount()`、
  `FaciesPartitionState::footprintReadCount()`（供回归断言 50 次编辑零重装）。

### W7 生产路径

- 绘制三态反馈：`drawConstraintResult` 的 `SavedLayerFailed` 通道保持，新增
  的部分完成（声明失败+回滚失败）经同一通道以「成功+警告」呈现
  （`TypedConstraintDrawController`/`ConstraintDrawController` 已有警告日志路径）。
- 闭包/映射生命周期：闭包持 `QPointer<ConstraintWorkflow>` + 存储身份；
  切工程/销毁后重放拒绝且不改写新对象。`assignFacies` 自带 fid 存在性复核，
  复制后被替换/重载场景在入口拒绝。

## 验证（命令与结果）

- 构建：`PALEO_JOBS=4 ./paleo-dev build`（deb 闭包前缀，Ninja+ccache）——全量
  通过（见下「记录」节最终 head 复验）。
- 新回归：`tests/tst_p5_edit_lifecycle.cpp`
  （`add_paleo_test(tst_p5_edit_lifecycle LIBS paleo_workflow
  Paleo::paleo_data_service)`，600s 超时）覆盖验收矩阵：
  1. `outOfSessionDrawUndoKeepsSingleOwner`——真实 starter（对齐生产
     `beginConstraintEdit` 语义）下会话外 draw→undo→redo→undo：行数、闭包栈
     位置、原生栈 index/count（零推进）、图层不可编辑、审计 create/delete
     序列、redo id 再分配。
  2. `closureReplayRefusedDuringLiveSession`——活会话中重放拒绝、条目保留、
     会话取消后重放成功。
  3. `closureGuardRejectsStoreSwitch`——切库重放拒绝、条目保留、切回后成功。
  4. `addConstraintPrevalidatesUnrepresentableHorizon`——「A|B」层位持久写前
     拒绝：零 durable 行、零成功信号、重试恰好一次成功。
  5. `removeConstraintBoundedLoads`——单条删除全量 load ≤3（旧 4+）。
  6. `importBatchIsBoundedAndAtomic`——100 与 1000 条导入 load 同为常数级
     （≤6）；重开 GPKG 核对行数/参数/审计（1000 create 事件）；自相交几何
     整批拒绝零残留；他人实例写入 c-9999 后继续导入不撞身份；会话中拒绝导入。
  7. `copyIdentityDisambiguatesDuplicateGeometries`——两个 WKB+相码完全一致
     的要素：选中第二个，复制映射解析后改相只落在被选对象；来源文件字节
     不变（哈希对比）。
  8. `styleLayerBindsOnceAcrossEditCommands`——50 次属性编辑命令 + 全量 undo：
     install/footprintRead 计数零增长，绑定与拓扑状态保持。
- 既有回归：tst_constraintediting / tst_constraintstore / tst_constraint_draw /
  tst_featureidentity / tst_mappingworkbench（见 PR 描述的具体命令与结果）。
- Source gates：layering 族（含 qgs_audit/strict）、qgis_spread、file_budget、
  ui_size/ui_invariants、i18n/ui_tokens、qgis_versions、docs_drift、
  cpp_mainline ——最终 head 全绿（见 PR 描述）。
- 本地真实 GUI 演示：见 PR 描述与截图清单。

## 所有权与并行接缝

- 只动了 P5 声明的文件；`mappingworkbench_edit.cpp` 仅 copyForEditing/
  publishConstraintImport；`workflows.h` 只加 ConstraintWorkflow 的私有/批量
  声明（P3 的分析声明块未触碰）；`mappingworkbench.cpp` 仅 styleLayer 绑定段。
- 新文件：`tests/tst_p5_edit_lifecycle.cpp`、`cmake/extra-p5-edit-lifecycle.cmake`
  （根 manifest 单条登记）、本进度记录。
- 未修改：mapversioncontroller/mapsnapshot/mapversionstore/evidencerecord/
  saveEditingVersion/单因素与 ONNX 算法/Data_service gitlink/任何基线。

## 已知限制

- `importConstraintBatch` 的「一次原生编辑宏」路线未取：导入在无会话路径上
  以单事务直写（原生会话与导入互斥，预验证即拒）；跨 Catalog、文件与图层
  服务仍不构成 ACID 事务——提交后的呈现失败按部分完成披露。
- 批量回滚（预验证失败路径）仍逐条 `store->remove`：失败路径不参与规模
  计数验收；成功路径一次事务。
- 复制身份映射的副本侧 fid 顺序配对依赖 `QgsVectorFileWriter` 按 provider
  迭代顺序写出（无过滤/无排序）；数量不匹配时映射缺省、调用方拒绝——
  不做几何猜测兜底。
