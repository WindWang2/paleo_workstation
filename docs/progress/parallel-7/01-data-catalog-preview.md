# 方向 01 Ledger：数据管理、资产版本、资料图谱与导入预览的现有能力深化

- 分支：`goal7/data-catalog-preview-20261014`
- 基线 SHA：`b0520429c21f858a1c053b876f511680088564a9`（= origin/master，2026-10-14 复核无移动）
- Worktree：`../paleo-wt-goal7-data-catalog-preview`
- 构建接线：vendor/prefix、vendor/onnxruntime → `pw-seismic-runtime-closure-20260930-215353/vendor/*`（共享只读 symlink）；Qt6/GEOS/GDAL/QGIS 钉扎 init cache `/home/kevin/project/paleo-goal7-init-01-data-catalog.cmake`（`cmake -C`）；qt6-5compat 补位 `paleo-wt-goal7-deps/qt6-5compat/extracted`。configure/build/ctest 一律 `flock -x /tmp/paleo_workstation_global_build_j4.lock`，`-j4`。

## R0 基线（S1/S2/S3 三只读审计 + 主 Agent 抽查核实）

### 排除指纹（执行时实测）

- Open PR：仅 `#437`（80 文件，`gh pr view 437 --json files` 实拉，含
  `src/catalog/datacatalog_versions.cpp`、`src/io/constraintstore.*`、
  `src/services/datagraph.*`、`src/workflow/datagraphworkflow.cpp`、
  `src/workflow/derivedassets.*`、`src/ui/pages/datagraphpage.*` 及其测试
  tst_datacatalog_sqlite/tst_derivedassets/tst_versions 等）→ 全部 write-deny。
- Open Issues（执行时实测 77 个：#352–#444）：全部列入动态排除。与本域强相关而
  必须避让的：#381(ziparchive)/#382(sfpkg)/#383(partialread)/#384(dlis)/#385(lis)/
  #386(las parseRange)/#387(井文本层位 readAll OOM)/#390(金字塔 worker 裸捕)/
  #394/#395(store 只读/共享)/#354(SEG-Y resumeScan)/#361(dataopsundo redo)/
  #364(datanavtree UAF)/#372(时间切片)/#376(预览 grab)/#380(officepreviewsession)/
  #396(PreviewRenderCache)/#401(datalist_undo showHealthCheck)/#411–#413(ui 低危)/
  #414(io 低危)/#415(workflow 低危)/#418(catalog 低危)/#443/#444(图谱)。
- 新增观察：geojson 图例收集 readAll 无闸（S2#10）与 #387 同类异站——**留给
  Issue Agent 表**，本轮不动（避免与 #387 修复者撞车）。

### 领域事实（抽查核实过的主干）

- catalog：内存四表 QVector 为唯一查询事实源，SQLite 仅持久化（WAL，事务
  BEGIN IMMEDIATE）；资产身份 `ast-N/ver-N` + sha256 内容指纹；stage 词表
  RAW|DERIVED|INTERMEDIATE|OUTPUT；purgelease 进程内互斥 + 损坏恢复 .bak 链。
- 导入：produce-then-commit——worker 只写 ImportSession（staging 副本 +
  暂存根 + journal），owner 一次 `commitImport` 事务落盘；单文件路径有
  「Failed 且 journal 非空 → 整份作废」守卫（dataimport_session.cpp:417），
  文件夹路径**无等价守卫**（本轮 A1 缺口）。
- UI：datalist 五视图一栈 + FlatAssetModel 批量 fetch 增量通道；
  dataopsimportqueue「取消的作业不提交」与 FolderImportWorkflow::importFile
  无条件提交**语义不一致**（本轮 A2 缺口）。

### 候选工作列表（全部经核实未占用）

| 编号 | 缺口 | 位置 | 级别 |
|------|------|------|------|
| A1 | 文件夹导入失败行半截 journal 照常提交 → 零版本资产/已登记但行报 Failed；同批同内容文件会 dedup 命中被撤销行 → 悬挂链接 | io/ingestplan.cpp executeIngestPlan 无行级回滚 | 中 |
| A2 | importFile 取消后仍提交（与 dataops 队列语义冲突） | workflow/folderimport.cpp:89-90 | 中 |
| A3 | 批量改类型走增量通道但表视图类型列/过滤可见性陈旧；图标/分组视图灌未过滤全量行破坏过滤态 | ui/pages/datalist_batch.cpp:104-117 | 中 |
| B1 | undo_stack.json 非原子写 + 损坏静默回空（与 sidecar QSaveFile 契约不一致） | ui/pages/datalist_undo.cpp:157-162, 87-89 | 低-中 |
| B2 | open() 复位脏面漏清 m_removedAssets/m_removedVersions/m_linksFullRewrite → 活动批次内重开工程后陈旧删除集打到新工程 | catalog/datacatalog.cpp:332-361 | 中（窗口窄） |
| B3 | rollbackToVersion 不携带目标版本 extra → 回滚丢 depthMd/图例/解析报告 | workflow/assetops.cpp:177-188 | 低 |
| C1 | 批量挂接/标签逐条 pushCommand，每条整表重建（与 dataopsundo.h:16-18 设计注释相悖） | ui/pages/datalist_undo.cpp:342-349 | 中（perf） |
| C2 | datalist_table 行内控件 lambda 裸捕 DataCatalog*（m_ctx.cat 是 QPointer，三处不一致） | ui/pages/datalist_table.cpp:320-461 | 低 |

低优先暂缓（记录不实施）：S1#B(output 孤儿序号)、S1#C/D/F/G/J/K（多为低危，
部分疑入 #418/#395 指纹，保守避让）、S2#4/5/6/9/11/12、S3#4/6/7/8。

### 基线构建/测试状态

- configure：首次缺 GEOS_C_LIB → init cache 补钉 GEOS 后重跑（BUILDING.md
  已知接线问题）。（进行中——受全局锁排队）
- 构建与测试结果将逐轮追加于下。

## 轮次记录

### 轮 1（2026-10-14）：8 项缺口修复 + 回归测试

实施清单（全部先核实、后修改、配对测试）：

| 项 | 生产改动 | 测试 |
|----|---------|------|
| A1 行级失败隔离 | `catalog/datacatalog.{h,cpp}` 新增 `StagingCheckpoint`/`checkpointStaging`/`rollbackStaging`（staging 专用，COW 快照逐字段退回）；`io/ingestplan.cpp` `executeIngestPlan` 逐项检查点 + `rollbackFailedRow`（journal/内存/序号/overlay 记账整体退回 + 暂存受管文件删除 + 空目录剪枝）；`io/dataimport_folder.cpp` `executePlannedItem` 单项重试同口径；`io/dataimport_internal.h` 声明共享 | `tst_import.cpp::failedFolderRowCommitsNothing`（坏 SEG-Y×2 + 好 LAS：2 Failed 行零入库、无零版本资产、无悬挂链接、第二份不 dedup 命中被撤销行、重开对拍） |
| A2 取消不提交 | `workflow/folderimport.cpp`：`ImportJob::discardOnCancel`（单文件作业置位），finished 处理器在 cancelRequested 且未提交时跳过 commit、如实报「导入已取消」（produce 本就失败时保留真实原因） | `tst_folderimport.cpp::singleFileImportCancelledDiscardsCommit`（同步 requestCancel 后事件泵驱动；断言零资产+暂存清+线程零违规） |
| A3 改型视图收口 | `ui/pages/datalist_batch.cpp`：增量分支补 `applyListFilter` 重算（表类型列经 `m_tableDirty` 重灌、图标/分组拿过滤行集、选区经 `m_selKeep` 快照还原）；`ui/pages/datalist_filter.cpp` + `dataopsviews.h`：`sameRowsAs` 同集同序同数据跳过 `setRows`（虚拟视图选区不再被无差别重置） | `tst_assetpaging.cpp::batchTypeOverrideRefreshesViewsAndKeepsFilter`（模态驱动真 batchChangeType：类型列即时 tops、well_log 过滤下目标行隐藏、虚拟视图选区保持） |
| B1 undo vault 契约 | `ui/pages/datalist_undo.cpp`：`saveUndoVault` 改 QSaveFile 原子写 + commit 失败留痕；`loadUndoVault` QJsonParseError 损坏告警 | （契约与 sidecar 同款——tst_dataopssidecar 已钉死该模式；本轮改动即对齐，不加重复测试） |
| B2 删除面复位 | `catalog/datacatalog.cpp` `open()` 补清 `m_removedAssets/m_removedVersions/m_linksFullRewrite` | `tst_catalog.cpp::openResetsPendingRemovalFaces`（双工程同号 ast-1：批次挂起中删除后切工程，B 落盘不被陈旧 DELETE 误删） |
| B3 回滚携带 extra | `workflow/assetops.cpp`：`nv.extra = target.extra`（stale/staleReason 剥除——防 removeStaleVersions 视当前版本可清） | `tst_assetops.cpp::rollbackCarriesTargetExtraWithoutStaleMarks` |
| C1 批量收口刷新 | `ui/pages/datalist.{h,cpp}` + `datalist_undo.cpp`：新增 `pushCommands`（N 条一次入栈一次刷新，栈不在场 qDeleteAll 防泄漏）；`datalist_dnd.cpp`/`datalist.cpp`/`datalist_batch.cpp` 三处批量调用点转换 | `tst_assetpaging.cpp::batchAddTagRefreshesOnce`（4 项打标签 entityRefreshRequested 恰 1 次 + sidecar 功能对拍） |
| C2 行控件野指针 | `ui/pages/datalist_table.cpp`：ok/撤销/设主三钮 lambda 裸捕 `DataCatalog*` → `QPointer` 守卫（对齐 m_ctx.cat 防护口径） | （防御性加固：UAF 触发需壳销毁 catalog 后点击陈旧控件，无既有测试面；行为等价、编译期可验证） |

构建/环境记录：
- 首次 configure 缺 GEOS_C_LIB → init cache 补钉；但首次失败把 `GEOS_C_LIB-NOTFOUND` 写进缓存，
  `set(... CACHE ...)` 无 FORCE 不覆盖（#398 同类坑）→ `rm -rf build` 后干净重配成功。
- vendored lrelease 链系统 Qt6Core（Arch 6.12 vs vendored 6.10.2）报
  `GNU_PROPERTY_1_NEEDED_INDIRECT_ACCESS` → 构建时
  `LD_LIBRARY_PATH=<prefix>/usr/lib/x86_64-linux-gnu`。
- 系统路径 Qt6Keychain（Qt 6.11 版本标签）被 find 落入 → 命令行 `-DQt6Keychain_DIR=`
  共享提取包 + `-DCMAKE_{EXE,SHARED}_LINKER_FLAGS` 双 rpath-link（对齐方向 02/05 接线）；
  注意 `-U` 若在 `-C/-D` 之后会反过来删掉刚钉的值（本次踩坑，改用 -D 直写）。
- `python3 tools/check_layering.py --strict` 通过；`git diff --check` 干净。
- `cmake --build build --target paleo -j4` 退出 0，`build/paleo` 产出（659MB）。

### 轮 1 补：S5 独立 review（未参与实现）与闭环

S5 结论：生产代码 8 处修复未发现行为级缺陷（快照字段与成员清单逐一核对完备、
线程归属成立、COW 别名/事件面/序号语义正确）；不可合入点全部在测试：

- **B1（blocker）** `tst_catalog.cpp` 用了不存在的 `DataCatalog::hasAsset`（编译必败）
  → 已改 `!reread.assetById(...).id.isEmpty()`。
- **B2（blocker）** A3 测试末条 `QCOMPARE(contains(target), false)` 与 D1.10 语义矛盾
  （m_pageSelection 按设计保留被过滤 id，既有测试钉死；不能反向改实现）→ 已删该断言
  并注明取舍；选区保持断言改为经由 m_pageSelection/表还原的口径并修正注释。
- NB1（已修）：`rollbackFailedRow` 路径形态统一 canonical（symlink 前缀下 startsWith
  恒假 → 暂存漏删 → moveStagedFiles 把孤儿搬进工程/整批拒交）。
- NB3（已修）：A2 取消分支补注释（ImportFailed 事件不补发的取舍）。
- NB4（已修）：`pushCommand` 在栈不在场时 delete 裸指针（对齐 pushCommands）。
- NB2/NB5/NB6 留档（删文件失败可见性、chipbar 徽标、撤销宏粒度）——不阻塞本轮。

测试与复跑结果：见轮 2 追加。

### 轮 2（2026-10-15）：先红后绿闭环 + 最终验证矩阵

**先红验证**（`git stash push -- src/` 后旧生产码重编，6/6 新用例全部如实失败）：

| 用例 | 旧码失败断言（红证据） |
|------|----------------------|
| tst_import::failedFolderRowCommitsNothing | `failed: 1 != 2`（第二份坏 SEG-Y dedup 命中半截登记） |
| tst_folderimport::singleFileImportCancelledDiscardsCommit | `err 不含「已取消」`（取消仍提交） |
| tst_assetpaging::batchTypeOverride… | 类型列 `"well_log" != "tops"`（视图陈旧） |
| tst_assetpaging::batchAddTagRefreshesOnce | `refreshSpy: 4 != 1`（N 次整表重建） |
| tst_catalog::openResetsPendingRemovalFaces | `ast-1 被误删`（陈旧删除面跨工程命中） |
| tst_assetops::rollbackCarriesTargetExtra… | `depthMd: 0 != 1849.35`（回滚丢 extra） |

恢复 stash 重编后同六用例 0 failed（绿）。B2 用例曾因 tst_catalog 类外定义结构
（`private slots:` 声明 + `TestCatalog::` 限定定义）漏注册——已补声明并改限定名，
`-functions` 确认注册后才进入红绿流程。

**最终测试矩阵**（修复后）：
- ctest `-j4`（全局锁内）：tst_import ✓ tst_folderimport ✓ tst_assetops ✓
  tst_assetpaging ✓ tst_catalog ✓（串行 ctest 13.87s 绿 + 单跑 45/45 绿；
  与他方向并发 -j4 时出现过一次 36.5s 瞬态红，无断言失败输出，重跑即绿——
  归因见下「环境事件」）。
- 回归面：tst_catalogstore ✓ tst_catalog_catalogindex ✓ tst_cataloghealth ✓
  tst_io_importledger ✓ tst_dataopssidecar ✓ tst_dataops_incremental ✓
  tst_officepreview ✓（ctest）；tst_perf_catalog 16/16 ✓ tst_projectdata 6/6 ✓
  tst_datapreview 41+1skip ✓（直跑）。
- 探针：`folderImportCheckpointOverheadProbe` 为 env 门控（PALEO_IMPORT_SCALE），
  本轮未在满载盘上采集规模数据（诚实跳过，不伪报）。

**环境事件（如实记录）**：`/home/kevin/project`（nvme0n1p2，458G）在七方向并发
构建下两度打满（df 100%/0 avail）——git 对象写入 ENOSPC、ctest 三个套件瞬态
「设备上没有空间」。处置：删除本 worktree 可再生构建产物（已验证的测试二进制/
paleo 主程序，均可从 .a 快速重链）腾出 ~3G；受影响套件全部串行/直跑复绿。
git stash 曾因 ENOSPC 失败两次，腾出空间后成功。

**命令与退出码**（实际执行）：
- configure：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -C /home/kevin/project/paleo-goal7-init-01-data-catalog.cmake …` exit 0
  （Qt6/GEOS/GDAL/QGIS/keychain 钉扎 + 双 rpath-link，详见轮 1 环境记录）。
- build：`cmake --build build --target paleo -j4` exit 0（paleo 660MB 产出；
  空间紧张期间为红绿验证让路删除过，提交后重链复核）。
- test：`ctest --test-dir build -R …`（上述矩阵）。
- `python3 tools/check_layering.py --strict` → 检查通过：无层违规。
- `git diff --check` → 干净。
