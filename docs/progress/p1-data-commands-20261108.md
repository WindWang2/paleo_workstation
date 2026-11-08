# P1 data-commands 交付记录（2026-11-08）

任务：数据操作编排服务化与可信执行回执（P1，分支
`codex/paleo-p1-data-commands-20261108`，基线 origin/master `86454671`）。
目标：把数据管理操作收敛为「明确意图 → 功能层准备与执行 → 真实执行回执 →
同一撤销栈及界面反馈」。

## 一、实现入口（工作包 → 代码）

| 工作包 | 实现 | 说明 |
|---|---|---|
| W1 迁移可执行核心 | `src/services/dataopsstores.h`（新，header-only）；`src/workflow/dataopscommandservice.{h,cpp}`（新）；`src/ui/pages/dataopsundo.h`、`src/ui/pages/dataops/dataopscommands.h` 改薄转发；`dataopsmodel.h` 只留 `AssetRowInfo`/`buildAssetRow`/`OperationsHistory` | 四个 sidecar 存储对象（TagStore/AssetOverrideStore/EntityOverrideStore/RecycleBin，含 `EntityOverride`/`RecycleEntry`）下沉数据层，文件契约零变化（`.paleo/*.json` 键值结构原样）；命令对象/宏/共享撤销栈下沉功能层。旧 include 路径与全部公开类型可用（datalist/entitypanel/datapage/测试零改写）。侧车持久化仍走 `services/jsonsidecar.h` 门面；`AssetRowInfo` 读取字段与 `buildAssetRow` 签名不动（P2 兼容） |
| W2 冻结目标/准备/守卫 | `DataOpsCommandService::{guardBatch,guardFailure}` + `DataOpsContext::alive()` + `DataOpsSession`（会话代数） | 请求=普通值 ID 集；准备阶段去重（merged 计数）+ 分类（不存在/已满足/回收中/角色与标签非法），每条请求的最终去向都在回执 items 里。入口守卫=QPointer 生命周期 + `isOpen` + `refusesWrites` + catalogPath 绑定值 + 会话代数五重校验；同 cat 同路径重复绑定零副作用（`refreshAssetTable` 每刷都走的现实约束）。模态对话框返回后重新验证目标（`目标实体不存在（工程已变化…）`）。`mutationSeq` 拒绝规则（assetassociation）不属本任务面，未触碰 |
| W3 目录事务边界 | `TransferLinkCmd::apply`（BatchSave 包裹两步）；`DataOpMacro::setBatchCatalog`（目录族宏单提交边界）；`attachOrTransfer` 的 addLink 段（独立 BatchSave） | 转移=「置未决+挂目标」一个提交边界：第二阶段失败 → `batch.abort()` 整体滚回（不留半途态）；落盘失败 → #169 内存回滚。宏两种模式：目录族（abort 整批回滚）/混合（逐 child 补偿、核对每次结果）。addLink 无 removeLink 对偶 → 回执如实标「新建关联（不可撤销）」，不伪装可撤销 |
| W4 侧车失败恢复 | `BatchTagCmd`/`BatchTypeOverrideCmd`（单次提交+前态快照）；`SoftDeleteCmd`/`BatchRemoveCmd`/`BatchRestoreCmd`/`EntityEditCmd`/`EntityCreateCmd`（save 失败内存还原）；store 新增 `restoreSnapshot`/`setTagsExact`/`captureState`/`restoreState` | `batchAddTag`/`batchChangeType` 的「先写 store 再包装命令」已删除；N 资产同类变更一次侧车提交。save 失败 → 内存整批还原到前态（`FailedRecovered`+`sidecar-write` 阶段披露）；补偿链核对每次结果，补偿失败保留两条证据（`FailedPartial`+`compensation`）。不宣称跨存储 ACID |
| W5 执行回执/撤销一致性 | `BatchReceipt`/`ReceiptItem`/`OpOutcome{Changed,NoChange,Rejected,FailedRecovered,FailedPartial}`；`DataOpCommand::{receipt,storeIntact}`；`DataOpsUndoStack::{lastReceipt,push/undo/redo 消费真实结果}` | 请求数/去重数/各集合互相核对（`items.size()==requested-merged`）。全成功可撤销批次仍一条历史；无变化不添加虚假命令也不清 redo；提交失败且全部恢复不产生成功历史；留有部分变化（补偿失败）→ 两支栈整栈作废并披露。深度/生命周期约定不变 |
| W6 现有入口消费同一结果 | `datalist_batch.cpp`（batchAddTag/batchChangeType/removeAssetsSoft/detach/setPrimary）、`datalist_dnd.cpp::applyEntityDrop`、`datalist.cpp`（拖到标签→`handleAssetsDroppedOnTag`）、`datalist_indexops.cpp::renameSelectedAsset`、`datalist_undo.cpp`（回收站恢复/`receiptStatusText`）、`entitypanel_crud.cpp`（建/改实体经服务薄适配） | 成功历史、成功数、「可撤销」提示全部在回执确认之后；混合批次分开说明可撤销范围（addLink 永不标可撤销）；失败状态不被成功文案覆盖（`StatusSpy` 定向断言钉死）；批次一次收口刷新 |
| W7 真实存储/故障注入测试 | `tests/tst_dataops_commands.cpp`（12 用例）、`tests/tst_dataops_receipts.cpp`（13 用例） | 见下节 |
| W8 真实交互验证 | `tests/tst_dataops_ui_receipts.cpp`（7 用例，原生 DataPage/DataListPanel） | 见下节 |

构建接线：`cmake/extra-p1-data-commands.cmake`（登记于根 CMakeLists
`_paleo_extra_manifest`）；功能层源编入 `paleo_workflow`，命令/回执测试仅链
`paleo_workflow+paleo_store`（无 QtWidgets）；UI 级测试链 `paleo_ui`。

## 二、验证证据

### 新增测试（全部通过）
- `tst_dataops_commands` **12/12**（数据用例 10 + init/cleanup）：挂接/转移/设主/重命名全链+undo/redo+独立
  重开核验；转移第二阶段失败（注入=空实体 id，attachLink 天然拒绝）不留未决
  中间态；locked 注入双路径（服务守卫 Rejected / 命令直驱到提交边界
  FailedRecovered+#169 回滚）；标签批次单条历史+精确撤销（不误删既有标签）；
  已存在标签 NoChange 不入栈不清 redo；设主撤销跨资产恢复前主；会话守卫
  （重绑后旧命令拒绝、新会话合法操作成功）；只读守卫；软删/恢复往返重开核验。
- `tst_dataops_receipts` **13/13**（数据用例 11 + init/cleanup）：回执计数互核（含去重）；侧车写失败（注入=
  目录占位，QSaveFile 既有失败路径）内存/盘一致+阶段披露+第二次合法操作成功；
  撤销路径侧车失败 → 整栈作废如实披露；locked 目录族宏（注入=setLockedReadOnly）
  abort 整批回滚+重开核验；混合批次可撤销范围分离（撤销只回转移，addLink 留存）；
  宏补偿成功（FailedRecovered）/补偿失败（注入=ProbeCommand 最小测试注入，
  记录命中阶段与次数；宏/栈/回执消费逻辑是生产代码）双证据+部分变化落盘核验；
  目标实体消失整批拒绝；类型改写侧车失败恢复；失败后全部入口恢复可用。
- `tst_dataops_ui_receipts` **7/7**（数据用例 5 + init/cleanup；原生数据页交互）：菜单批量打标签成功→
  撤销→重做（原始 JSON 落盘核验+RAW 字节未改写）；多资产拖放混合批次
  （新挂 1/转移 1/不可撤销范围如实披露；目录重开核验）；侧车写失败（状态栏
  无成功文案+无虚假历史，解除后重试成功）；软删确认→行消失→撤销恢复；
  只读确认后守卫拒绝（无成功文案）。

### 既有回归
- `tst_panels_dataops` 104/105：唯一红 `dataops_d10_…SplitterResize` 为
  **origin/master 存量红**（`importledgerdialog.h` 的 setSizes，已用
  git stash 在纯基线上复现确认；不在本任务写集）。
- `tst_dataopssidecar` 8/8、`tst_dataops_incremental` 8/8、`tst_ui_blocking` 9/9。

### Source gates（本地全绿）
layering / layering_strict / layering_symbol_audit / layering_qgs_audit /
qgis_spread(_strict) / ui_invariants(_strict) / ui_size(_strict) /
file_budget(_strict) / i18n / ui_tokens(_strict) / docs_drift / cpp_mainline
全部通过。file-budget 基线**只缩不涨**：`datalist.cpp:fn:DataListPanel`
442→437（拖标签意图移出构造函数）、`entitypanel.cpp:file` 1462→1453
（setSharedOps 实现移入 `_crud.cpp`）。

### 翻译
两份 ts 手工增量登记本任务 56 条文案（zh 身份翻译、en unfinished 骨架，
各 +292 行）；未做全量刷新（lupdate 全量跑会清掉 #829 等既有条目——已回滚
改手工）。

## 三、期间修出的产品级缺陷（测试驱动）

1. `bindSession` 漏设 `m_ctx.session` → 会话守卫从未生效（tst 会话用例抓出）。
2. 会话变化判定依赖旧绑定 `valid()` → 旧 catalog 对象销毁后误判「无变化」，
   旧历史漏进新会话（改为 everBound+prevCat 判定）。
3. 宏失败 child 的回执条目丢失（证据不完整）→ 失败 child 回执同样并入。
4. UI 接线后 `removeAssetsSoft` 丢失收口刷新（原 pushCommand 附带）——
   `tst_panels_dataops::d1_batchRemoveSoft` 抓出。

## 四、实际限制与诚实边界

- **目录事务与多份 JSON 侧车不是一个原子存储**：混合宏（目录+侧车）失败时
  目录段 abort、侧车段逐 child 补偿；补偿失败如实披露 FailedPartial 并整栈
  作废，不假装跨存储回滚。同步调用内的补偿已核对；跨进程/崩溃恢复不在本
  任务面（未新建崩溃恢复框架），重开后的目录与侧车状态是恢复验证的独立依据
  （测试已按此口径核验）。
- locked 注入（setLockedReadOnly）走「save 在批次挂起前拒绝」路径：服务入口
  表现为准备守卫整批 Rejected；命令/宏直驱表现为 mutator 拒绝→单行/整批
  回滚（catalog-abort/commit 阶段披露）。两者都是真实的目录提交失败注入；
  「children 全部内存成功后 flush 才失败」的 #169 分支由 DataCatalog 自有
  测试钉住，本任务在其上构建。
- 宏补偿失败注入使用 `ProbeCommand` 最小测试注入（夹具记录命中阶段+次数）；
  其余全部故障注入为真实存储失败点（目录占位/locked/空实体 id）。
- EntityCreate 的撤销=sidecar 软删（catalog 无 removeEntity API，GAPS 既有
  语义）——回执如实呈现，未伪装实体删除。
- 回收站「不可恢复清理」「清空记录」及版本回滚行为未改（W1 边界）。
- `dataops_d10_…SplitterResize` 为存量红（见上），未在本任务修复（文件不在
  写集）。

## 五、与并行任务/PR 的关系

- 与 #906（I/O/投影/发布恢复）、#907（CI 钉版）、#910（document-data-io）：
  无路径交集（已核对 open PR 文件清单）。
- P1/P2 共有 `datalist.cpp/.h`：本任务只动命令服务/回执/意图接线块，未触碰
  查询/筛选/漏斗接线；`AssetRowInfo`/`buildAssetRow`/`rebuildRowSnapshot`
  读取行为未改。
- `#911` 的焦点撤销路由复用（`undoOp/redoOp` 语义不变）。
