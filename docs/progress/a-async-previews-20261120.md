# A｜异步预览与交会数据读取——实施记录（2026-11-20 批）

- 分支：`codex/paleo-a-async-previews-20261120`（worktree `../paleo-a-async-previews-20261120`）
- 基线：`origin/master@1a79b74511a813ae28871f65e79b09b1fea9c884`（开工时核实；快照基线 3e9df32c 的 #1055/#1051/#1054 已在主干，范围文件核对仅 `seismictaskservice_attr.cpp`/`capabilityhostwiring.cpp`/根 CMakeLists 有他人改动，与本任务无冲突）
- 问题来源：#961（Crossplot 表头预热绕过 PaleoTaskService 与退出排空）、#974（地震预览重复任务池与缓存未纳入统一排空和预算）
- 开放 PR 复核：开工时仅 #1053（P4 ONNX）开放，文件集与本任务无交集；P1/P2/P3 已合入，按约束复用（不触碰 #1051 的索引/GeoTIFF/provenance 路径，SEG-Y 读取只经现有接口）

## Scope ledger（开工核查结论）

| 工作包 | 开工时生产调用链 | 已具备能力（复用） | 缺口 | 实施 |
|---|---|---|---|---|
| A-W1 | 主窗 attachWorkflows 自建 PreviewDoc + 256MiB SeismicTaskService；dock 构造自建 PaleoTaskService+SeismicTaskService；attachSections 注入在 link 早退之后 | dock 已有 QPointer/请求号/代次守卫；DataPreviewTabs 生产已走 setDocService | 生产 3 处重复地震缓存；dock 私有池不进 drainTasks | AppContext 创建并拥有共享 PreviewDocService+SeismicTaskService（同源 PaleoTaskService）；main.cpp 注入主窗；attachSections 注入先于早退；dock 构造去私有池，未注入给「地震任务服务未注入」原因；PreviewDocService 地震服务改懒建+共享注入（纯 LAS 门面不再占 256MiB，correlationpanel 的私有门面因此不再建缓存——未改该文件） |
| A-W2 | requestSection/requestLas 按 assetId/key 全局世代号（m_decodeSeq[assetId]），结果按 assetId 广播 | 已有世代号/陈旧丢弃、振幅先到层位后到、释放递增防撞号 | 同 asset 不同消费者互相顶替；关一个标签取消所有；广播串收 | 请求键改 `consumer|assetId`；QObject* consumer 析构→requestCancel；releaseSection/releaseLas 带消费者口径（匿名=全量失效，兼容旧语义）；结果信号尾带 consumer 标签（旧连接自动忽略尾参）；DataPreviewTabs 按标签认领+到达线号核对 |
| A-W3 | refreshSources 在 GUI 同步整本 inventory（含 analysisReadiness 文件 I/O + 逐井 parseHeader）；隐藏预热走 QtConcurrent 全局池 | WellLogSet 头缓存（size/mtime/change/birth 键控、成功/失败都缓存、上限 4096）；catalog 150ms 合并；就绪语义 | 可见路径同步大读取；预热未登记任务；worker 不碰活 catalog 的快照面缺失 | 所有清单入口统一进 snapshotCatalog（GUI、纯 catalog 读）→ buildInventory（worker：curveIndexFromLinks+computeReadiness）→ 应用前核对 epoch+mutationSeq；inventory 保留为 snapshot+build 的同步兼容壳（tst_welllog_ui 在用） |
| A-W4 | 隐藏预热 QtConcurrent::run 无取消/进度；预热与清单两条路径 | WellLogSet::warmHeaderCache 线程安全（保留 API） | 未登记任务；无合并上界；文件间不可取消 | 预热=清单任务本身（Document scope、隐藏 Low/quiet、可见 Normal/非 quiet）；在途+1 待处理封顶；同快照在途读取由激活方附着复用；每文件开始前查取消（单文件 parseHeader 不可中断——既有边界，如实声明）；取消批次不产出部分清单、不毒化头缓存 |
| A-W5 | SgyDataCache 有 UsedBytes/SetBudget/Hits/Misses/Evictions，未注册 CacheBudgetManager；驱逐跳过外部持有 | EvictableCache 协议（含 eviction 引用计数）、manager enforce/pressure | 地震实占不在统一预算 | 新增 `src/services/seismiccachebudget.{h,cpp}` 适配器（注册/注销、bytes=UsedBytes、SetBudget 目标收缩后复原配置容量、lastAccess 由生产 Find/Store/Clear 打点）；SeismicTaskService 持有适配器并在 7 处生产缓存路径 noteAccess |
| A-W6 | drainTasks 只排 m_taskSvc；泵事件期 catalog-changed 定时器可再提交预热 | PaleoTaskService shutdown 幂等+泵事件；main 先排空后析构窗口；AppContext 析构保活兜底 | 无生产者停止步 | drainTasks 先置 previewsQuiesced+PreviewDocService::quiesce（新请求拒收+真实原因）；控制器 150ms 合并槽/调度入口查门；projectOpened 推进清单 epoch 并取消在途清单任务 |
| A-W7 | 反演入口已有「任务服务未注入」文案；面板有 busy/progress/message | — | 清单刷新无反馈；部分失败不可见 | 可见刷新走任务中心（非 quiet）+面板消息；部分井源失败给计数消息；dock 提取入口未注入给画布原因；quiesce 拒绝给「应用退出中…」 |
| A-W8 | tst_taskservice（scope/会话/排空）、tst_previewdoc 四基线、tst_seismic_sectionui 四基线、tst_crossplot_controller 全链 | — | 共享所有权/消费者隔离/预算/排空门无回归 | 新增 tst_a_async_previews（注入幂等/双消费者/按消费者释放/析构取消/quiesce 拒绝/预算适配）；tst_welllogset +2（管线零漂移、文件间取消）；tst_crossplot_controller +1（任务登记/合并/quiesce 门；既有 3 处同步断言改 QTRY）；tst_seismic_sectionui +1（无私有池+注入恢复+预算注册） |

## 变更清单

生产代码：
- `src/app/appcontext.{h,cpp}`：共享 PreviewDocService/SeismicTaskService 创建与访问器；`previewsQuiesced` 门；drainTasks 先停生产者。
- `src/app/main.cpp`：`window.setSharedPreviewServices(...)` 注入（仅新增调用）。
- `src/app/crossplotcontroller.{h,cpp}`：scheduleSourceRefresh/startSourceRefresh 异步化（快照→任务→核对应用）、有界合并、epoch、部分失败消息；删除 QtConcurrent 预热。
- `src/services/crossplotsources.{h,cpp}`：CatalogSnapshot/InventoryOutcome/snapshotCatalog/buildInventory；inventory 同步兼容壳。
- `src/services/welllogset.{h,cpp}`：WellLogLinkSnapshot/snapshotWellLogLinks/curveIndexFromLinks（取消/进度/失败清单）；prepare 拆为共享 filesFromLinks。
- `src/services/wellreadiness.{h,cpp}`：snapshotReadinessInputs/computeReadiness 拆分（analysisReadiness=二者复合，语义不变）。
- `src/services/previewdoc.{h,cpp}`：setSeismicTaskService 共享注入+懒建地震服务、quiesce、consumer 维度请求键/释放、结果信号尾带 consumer 标签。
- `src/services/seismictaskservice.{h,cpp}` + `_data.cpp`/`_slice.cpp`：cacheBudgetAdapter_ 成员+注册；生产缓存路径 noteAccess（算法/数值口径零改动）。
- `src/services/seismiccachebudget.{h,cpp}`（新）。
- `src/ui/paleomainwindow.h`、`paleomainwindow_attach.cpp`、`_sections.cpp`：共享注入 API/成员（m_seismicTaskSvc 改非拥有指针）、注入先于早退；`_seismic.cpp`/`_wellsection.cpp` 仅 `.get()` 机械移除。
- `src/ui/seismicsection/seismicsectiondockwidget.{cpp}`、`seismicsectiondock_track.cpp`：构造去私有池；未注入守卫（track 两处、提取两处）。
- `src/ui/datapreview/datapreviewtabs.{h,cpp}`、`datapreviewtabseismic.cpp`、`datapreviewtabwelllog.cpp`：consumer 传参、按标签认领、到达线号核对。

测试/构建：
- `tests/tst_a_async_previews.cpp`（新）、`tst_welllogset.cpp`（+2）、`tst_crossplot_controller.cpp`（+1、3 处异步化）、`tst_seismic_sectionui.cpp`（+1）。
- `cmake/extra-a-async-previews.cmake`（新）+ 根 CMakeLists manifest 追加一行。

## 验证（实际执行记录）

- 平台：Linux x86_64（CachyOS，内核 7.3，glibc 2.43）；配置 RelWithDebInfo + Ninja。
- 依赖前缀：`/home/kevin/projects/paleo-deb-prefix-20261119/usr`（V 系列准备的 deb 闭包前缀，带 `.paleo-deb-closure.sha256` 收据；主仓 superbuild 前缀缺源码收据且无 debian13-base.lock，按最新 BUILDING 不可用于新 configure——改用共享 deb 前缀，只读复用）。注：本机 ctest 二进制与该前缀的老 expat 不兼容（缺 XML_GetCurrentColumnNumber64），运行时以 `LD_PRELOAD=/usr/lib/libexpat.so.1`（系统 expat 1.13.1，同 SONAME 向后兼容）解决——仅测试运行环境问题，与产物无关。
- 构建：`QGIS_PREFIX_PATH=... ./paleo-dev build`（重型构建共享 `paleo-parallel-20261120/build.lock` flock，-j4 上限）——5 个任务测试目标 + 相邻 6 目标全部编译通过（一次 gcc-16 ICE 重试即过）。
- 测试（`./paleo-dev test -j1 -R ...`，QGIS 前缀注入 + offscreen）：
  - 受影响 7 项 **100% 通过**：tst_a_async_previews（8 用例：注入幂等/双消费者隔离/按消费者释放/析构取消/quiesce 拒绝/预算适配收缩-pin-复原-注销）、tst_welllogset（+2 新用例：快照管线与 catalog 口径逐字段零漂移、文件间取消不毒化）、tst_welllogset_perf、tst_welllogset_tvd、tst_previewdoc（4 基线全过）、tst_seismic_sectionui（含 4 基线 + 新增 dock 无私有池/注入恢复/预算注册）、tst_crossplot_controller（含新增清单任务登记/合并/quiesce 门闭环）。
  - 相邻消费面 6 项 **100% 通过**：tst_welllog_ui、tst_welllog_consumers、tst_welllog_multiformat、tst_datapreview、tst_taskservice、tst_cache_core。
  - 首轮 tst_a_async_previews 曾段错误：测试把服务自身用作 consumer，服务析构时自连 destroyed 回调触达已销毁成员——已双修（生产加 m_destructing 析构守卫；测试改用普通 QObject），并在本轮全绿。
- Source gates（合入前按 #863 流程全量重跑，均绿）：
  - `check_layering.py --strict`：无层违规；`--qgs-audit`：134=baseline 0 新增；`--transitive`（QGIS_PREFIX 注入）：48 边/41 TU == 基线（首版曾把 io/cachebudget.h 扩散到 82 TU——已改为 seismictaskservice.cpp 内部适配器类 + TU 内直接 include，扩散清零）。
  - `check_file_budget.py --strict`：0 回升 0 收缩（attachWorkflows/attachSections 拆助手后低于冻结值，基线按「只缩不涨」同步下调 404→395、275→271）。
  - `check_qgis_spread.py`：169=baseline 0 新增；`check_ui_invariants` / `check_ui_size` / `check_ui_tokens` / `check_i18n` / `check_qgis_versions` / `check_docs_drift`：全绿。
  - `cmake -P cmake/verify-cpp-mainline.cmake`：OK。
- 未执行：Windows 侧验证（平台不可用）；tst_panels 等巨测全量（时间预算；受影响面已由上述 13 项覆盖）。

## 边界与未覆盖（如实声明）

- 单文件 `WellLogRead::parseHeader` 不可中断（本任务不修改 P1 parser）：取消粒度=文件之间；取消谓词即屏障的确定性测试覆盖「下个文件不启动」。
- SDK（vendor/sbm）内部 chunk/slice 缓存与 Data_service 侧内存未治理（只读了解，不改 vendor）；宿主另有的任意线 sectionCache（≤4 条计数上界）与画布纹理 LRU 为受控上界但未按字节入账——不宣称「全部地震进程内存已治理」。
- SgyDataCache 预算是软政策：被渲染端持有的图片（use_count>1）不驱逐，可能暂时超预算；清空缓存不等于释放外部持有对象。
- pinnedSkips 恒 0：vendor 不透出「因 pin 跳过」计数（Evictions 已含全部实际驱逐）。
- Windows 侧未本机验证（平台限制）；改动无平台分支（除既有代码）。
- #962/#963、#1051/#1053/#1054 范围、B/C 的脚本/命令域：未触碰。

## 独立 review（未参与实现的子 Agent，2026-11-21）

对全量 diff（含新文件）做了独立审查并实际编译了全部改动 TU。发现与处置：

| # | 级别 | 发现 | 处置 |
|---|---|---|---|
| 1 | P1 | attachSections 重构后函数体内 ~30 处 `dock` 未声明（paleo_ui 编不过） | 已修：助手改为返回 `seismic::SeismicSectionDockWidget*`（守卫 + 取 dock 一步），体内恢复 `auto *dock = ...` |
| 2 | P2 | 预算适配器 evictLRUEntries 的「读配置容量→SetBudget 收缩→复原」可被并发 enforce 穿插，把临时收缩值当配置容量复原（静默永久缩缓存） | 已修：新增独立 `m_evictMutex` 串行整段（不复用 m_accessMutex——尾部 noteAccess 会取它，非递归会自锁） |
| 3 | P2/P3 | 称 bookmark/wells/attr 共 5 处服务用点缺 null 守卫 | **核实为误报**：五处均在同函数既有 `if (!m_taskService)`/组合守卫之后（bookmark.cpp:146、wells.cpp:259-263、attr.cpp:20/116/209），逐函数比对确认 |
| 4 | P3 | 消费者析构只取消任务不作废世代号——QObject 地址复用后旧 Cancelled 回包可能带上新消费者标签串收 | 已修：destroyed 连接改为 `++世代号 + requestCancel`（section/LAS 两处） |
| 5 | P3 | consumer 口径 releaseSection 不清 m_shaVerified——关标签后外链换盘不重验（与改前语义漂移） | 已修：consumer 路径同样 `m_shaVerified.remove(assetId)`（保守方向，至多多算一次哈希） |
| 6 | P3 | 控制器注释夸大「复用/等待在途预热（不重启第二轮）」与实现不符（实际是合并待处理+完成后补一轮，靠头缓存省时） | 已修：注释改为如实表述 |
| 7 | P3 | buildInventory 进度逐井分母导致回跳（cluster::Control 契约要求单调） | 已修：先按就绪井合计总文件数，回调以全局累计/总数上报（井间 base+done） |
| 8 | P3 | 测试两处未用变量（tagA/adapter） | 已修：删除 |

review 确认干净的面：匿名释放的 QHash 键拷贝迭代（无迭代器失效）、filesFromLinks/inventory 与旧实现逐字段等价（告警文案/排序/storedHeader 口径）、信号尾参兼容（全仓无字符串 SIGNAL 连接，PMF 连接全部编译验证）、catalog 线程门（快照全在 GUI 线程、构建期零 catalog 访问）、epoch/mutationSeq 守卫可达、有界合并成立、quiesce 全入口覆盖、PaleoTask 生命周期安全、适配器无死锁、懒建地震服务无依赖方受影响、layering/i18n/file_budget/docs_drift 门禁绿、测试确定性纪律（信号量屏障非 sleep）。
