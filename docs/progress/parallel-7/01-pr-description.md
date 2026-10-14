# refactor(01): 数据管理、资产版本、资料图谱与导入预览的现有能力深化

## 范围与所有权

方向 01（数据目录/资产版本/导入预览）。分支 `goal7/data-catalog-preview-20261014`，
基线 `origin/master` = `b0520429c21f858a1c053b876f511680088564a9`（执行时实测未移动）。

生产代码 13 文件（全部在本方向所有权内）：
`src/catalog/datacatalog.{h,cpp}`、`src/io/ingestplan.cpp`、`src/io/dataimport_folder.cpp`、
`src/io/dataimport_internal.h`、`src/workflow/assetops.cpp`、`src/workflow/folderimport.cpp`、
`src/ui/pages/datalist.{h,cpp}`、`datalist_batch.cpp`、`datalist_dnd.cpp`、
`datalist_filter.cpp`、`datalist_table.cpp`、`datalist_undo.cpp`、`dataopsviews.h`。

## 修复清单（8 项，均经 S1/S2/S3 只读审计 + 主 Agent 亲核，未被任何 Open Issue/PR 占用）

1. **A1 文件夹导入失败行半截 journal 入库（中危）**：行失败可发生在 addAsset/
   addVersion 之后（受管复制失败/SEG-Y 打开失败），半截 ops 随提交入库 → 零版本
   资产/「行报 Failed 但资产已登记」；同批同内容后续文件 dedup 命中被撤销行 →
   悬挂链接。修复：`DataCatalog::StagingCheckpoint`（staging 副本专用 COW 快照，
   内存/索引/序号/overlay 记账/journal 整体退回）+ `executeIngestPlan` 逐项、
   `executePlannedItem` 单项重试的行级回滚（含暂存受管文件删除与空目录剪枝）。
2. **A2 单文件导入取消后仍提交（中危）**：`FolderImportWorkflow::importFile` 的
   finished 处理器无条件 commitImport，与 dataops 队列「取消的作业不提交」语义
   冲突。修复：`ImportJob::discardOnCancel`——取消即不提交，session 析构删暂存，
   produce 本就失败时保留真实原因。文件夹作业「已入库的行保留」口径不变。
3. **A3 批量改型后视图陈旧（中危）**：增量分支只更新虚拟模型——表视图类型列
   陈旧、类型过滤不重算、图标/分组视图灌未过滤全量行破坏过滤态。修复：补
   `applyListFilter` 重算（表体按 `m_tableDirty` 重灌、选区快照还原）+
   `FlatAssetModel::sameRowsAs` 同集同序同数据跳过 `setRows`（虚拟视图选区
   不再被无差别 reset）。
4. **B1 undo vault 持久化契约（低-中）**：`.paleo/undo_stack.json` 直写非原子、
   损坏静默回空——对齐 dataops sidecar 契约（QSaveFile 原子写 + QJsonParseError
   损坏告警）。
5. **B2 open() 删除面跨工程泄漏（中，窗口窄）**：活动批次内删除后重开工程，
   残留 `m_removedAssets/m_removedVersions/m_linksFullRewrite` 在下一次 save 把
   陈旧 DELETE 打到新工程（ast-N/ver-N 跨工程高概率撞号）。修复：open() 复位块
   补清三个删除面。
6. **B3 回滚丢目标版本 extra（低）**：回滚新版本不携带 depthMd/图例/
   wellParseReport 等语义元数据（井道按深度挂图回滚后失效）。修复：携带
   target.extra，剥除 stale/staleReason（防 removeStaleVersions 视当前版本可清）。
7. **C1 批量命令 N 次整表重建（中，perf）**：批量挂接/转移/打标签逐条
   pushCommand，每条全量刷新（与 dataopsundo.h 设计注释相悖）。修复：新增
   `pushCommands`（N 条一次入栈一次收口刷新；栈不在场 qDeleteAll 防泄漏），
   三处批量调用点转换。
8. **C2 行内控件裸捕 catalog 指针（低）**：ok/撤销/设主三钮 lambda 按值捕获
   `DataCatalog*`，壳换绑/销毁后点击即野指针（m_ctx.cat 是 QPointer，三处不一致）。
   修复：QPointer 守卫。

## 测试（5 个新增用例，见 ledger 逐项对照）

- `tst_import.cpp::failedFolderRowCommitsNothing`（A1）
- `tst_folderimport.cpp::singleFileImportCancelledDiscardsCommit`（A2）
- `tst_assetpaging.cpp::batchTypeOverrideRefreshesViewsAndKeepsFilter`（A3）
- `tst_catalog.cpp::openResetsPendingRemovalFaces`（B2）
- `tst_assetops.cpp::rollbackCarriesTargetExtraWithoutStaleMarks`（B3）
- `tst_assetpaging.cpp::batchAddTagRefreshesOnce`（C1 观察面：刷新恰一次 + sidecar 功能对拍）

## 七任务冲突排除

- Open PR 仅 #437（80 文件实拉清单 write-deny）：本分支零触碰（见 R7 反向验证
  `git diff origin/master...HEAD --name-only` 对照）。
- Open Issues 77 个（#352–#444）全部排除；与 #387 同类异站的 geojson 图例
  readAll 无闸问题**留档不修**（避免与 #387 修复者撞车）。
- `src/workflow/derivedassets.*`（PR #437 保护区）零触碰；图谱域零触碰。

## 验证（实际执行记录）

- 基线 SHA：`b0520429`（= origin/master，执行期间未移动）。
- 构建：`cmake --build build --target paleo -j4` **exit 0**（全局 flock 串行；
  vendored 闭包 + Qt6/GEOS/GDAL/QGIS/keychain 钉扎，环境记录见 ledger 轮 1）。
- **先红后绿**：`git stash push -- src/` 旧生产码重编后，6 个新用例全部如实
  失败（A1 `failed=1≠2`、A2 `err 不含「已取消」`、A3 `类型列 well_log≠tops`、
  B2 `ast-1 被误删`、B3 `depthMd=0≠1849.35`、C1 `refreshSpy 4≠1`）；恢复后
  同六用例 0 failed。
- 最终测试矩阵（修复后）：ctest `-j4`：tst_import ✓ tst_folderimport ✓
  tst_assetops ✓ tst_assetpaging ✓ tst_catalog ✓（串行 ctest 13.87s + 单跑
  45/45；七方向并发时一次无断言输出的瞬态红，复跑即绿，归因见 ledger「环境
  事件」）。回归面：tst_catalogstore / tst_catalog_catalogindex /
  tst_cataloghealth / tst_io_importledger / tst_dataopssidecar /
  tst_dataops_incremental / tst_officepreview（ctest）+ tst_perf_catalog 16/16、
  tst_projectdata 6/6、tst_datapreview 41+1skip（直跑）全部通过。
- 门槛：`python3 tools/check_layering.py --strict` 通过；`git diff --check`
  干净。
- 反向验证：`git diff origin/master...HEAD --name-only` 22 文件与 PR #437 的
  80 文件清单 `comm -12` **零交集**；全部落在方向 01 所有权内
  （未触碰 datacatalog_versions.cpp / constraintstore / datagraph* /
  derivedassets）。
- 独立 review：S5（未参与实现）审出 2 个测试 blocker（不存在的
  `hasAsset` API；A3 末条断言与 D1.10 选区语义矛盾）与 4 条非 blocker 建议，
  blocker 已修复、NB1/NB3/NB4 已采纳（canonical 路径口径、事件取舍注释、
  pushCommand 所有权对齐），NB2/NB5/NB6 留档于 ledger。

## 已知风险与未处理项

- 性能探针 `folderImportCheckpointOverheadProbe`（PALEO_IMPORT_SCALE 门控）
  本轮未在满载盘上采集规模数据——七方向并发把 458G 工程分区两度打满，
  不在受压环境下伪报基准（诚实跳过；检查点为 COW 快照，理论逐行近零成本）。
- B1/C2 为防御性加固，无直接用户可见行为变化。
- 「留给 Issue Agent」表：geojson 图例 readAll 无超限闸（#387 同类异站）、
  S1 审计的 S1#B/C/D/F/G/J/K 低危项（部分疑与 #418/#395 指纹重叠，保守避让）、
  S5 的 NB2（回滚删文件失败可见性）/NB5（改型后 chipbar 徽标）/NB6（批量
  撤销宏粒度）。
