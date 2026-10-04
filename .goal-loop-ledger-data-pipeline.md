# Goal-Loop Ledger — 方向 30：工区数据管线与健康管理

分支 `goal/data-pipeline-20261004`（base: dda2faa）。Oracle 五条验收对齐
goal-loop-prompts/30-data-pipeline.md。

## 范围裁决

- **做**（Oracle 全覆盖）：①批导残差收编（归位预览列/导入台账/未决批量归位）
  ④健康度仪表盘 ⑤回收站收编（批量恢复/物理删除/占用统计/滞留天数）
  ⑥版本对比与回滚。
- **不做**（记 PR 遗留）：②CRS/坐标管理、③XYZ/Excel 格式扩展——不在 Oracle
  验收面内，本批不铺开。

## 落地清单

| 批 | 内容 | 证据 |
|---|------|------|
| 1 | `DataCatalog::removeAsset`（血缘保护 + 单事务 + 索引 rebuild 回滚快照）；`CatalogStore::deleteAsset/deleteVersion/replaceAllLinks` | tst_assetops purgeDoubleClearsAndGuardsLineage（sqlite 重开双清） |
| 2 | `FolderPreviewRow.entityPreview`（plan 期归位预显）；`ImportLedger`（.paleo/import_ledger.json，窗口 50 批，QSaveFile 原子写）；`FolderImportWorkflow::recordLedger`（同步/任务池双路径）；`assetops::proposablePendingLinks/applyPendingResolutions`（恰一候选判据） | tst_folderimport previewEntityPreview/mixedGoodBadImportIsHonestAndLedgered（16/16） |
| 3 | `services/cataloghealth`（快速面：缺失/未决/词表违例/孤立实体/无版本 + `verifyExternalShas` 离线段）；`CatalogHealthDialog`（分类计数 + 双击跳转 + SHA 分步校验可取消） | tst_cataloghealth 5/5 |
| 4 | `assetops::rollbackToVersion`（新版本指向旧内容；受管拷贝后重算 SHA 防篡改；外链零拷贝）；`compareVersions`（元数据 diff + 文本首差异块）；`VersionTableDialog` + 上下文菜单「版本与回滚…」 | tst_assetops 8/8 |
| 5 | 回收站收编：6 列（大小/滞留）、占用统计、物理删除（二次确认 + 血缘拒绝如实列报）、批量恢复/软删单命令（`BatchRestoreCmd`/`BatchRemoveCmd`，一次落盘整组可撤销） | tst_ui_blocking 探针非空转 1 次/40 项 |
| ★ | **根因修复**：`projectDirFor`/`undoVaultPath` 推导少退一级——sidecar 实际落在 `<proj>/artifacts/.paleo/` 而非 `<proj>/.paleo/`，tst_ui_blocking 探针一直空转通过（writes=0 ≤ 2） | /tmp 词法实证 + 探针修复后 39 次重现旧行为 |

## 自审记录

### 第一轮（六维）

- **分层**：新文件头三行 `// 层：功能|数据|视图` ✓；视图只加信号/灌数据，
  计算全在 services/workflow ✓（layering×3 ctest 绿）。视图 include 白名单
  外零新增（cataloghealth=services、assetops/importledger=workflow 均允许）。
- **旁路写文件**：无。purge 在 catalog 提交后才删文件；rollback 拷贝失败/
  登记失败/落盘失败三路都清副本；台账走 sidecar 惯例（非 catalog 数据）。
- **版本完整性**：回滚=新版本行（parent=目标，extra.rollbackOf 留血统），
  历史不删；removeAsset 血缘保护拒绝被引用版本；测试断言 SHA 一致 + 重开
  后仍在/已清。
- **事务原子性**：removeAsset/purge/rollback/归位 全部单 BatchSave 单 flush，
  失败整批回滚（内存快照还原 + 失败路径测试覆盖）。
- **错误路径**：修复 removeAsset 错误文案双 `%2` 占位串台（.arg 全量替换
  语义）；SHA 校验取消如实标「未扫完」；purge 残留文件逐条列报；损坏 LAS
  不猜——未决如实。
- **i18n**：全部新 UI 串走 tr()（中文源串）；update_translations.sh 已跑，
  i18n×2 绿。
- 修复项：体检对话框双击跳转后 accept() 收起，跳转立即可见。

### 第二轮（修复后复查）

- 复查 removeAsset/assetops/purge 错误文案全部单占位或 multi-arg 形式 ✓。
- 复查 BatchRemoveCmd/BatchRestoreCmd undo 语义（软删 undo=逐条恢复 + 单
  save）✓；探针驱动 redo+undo 双写仍 ≤2 ✓。
- 复查 ledger load 幂等 / record 裁剪窗口 / JSON round-trip（fromVariant
  容忍缺键）✓。
- 结论：零 High/Medium。

## 遗留

- CRS 坐标域校验、XYZ/Excel 导入路径（方向 30 目标 2/3）。
- projectDirFor 修复的迁移影响：既有工程的旧 sidecar（若曾写到
  artifacts/.paleo/）不会被读取——软删清单/标签/改写会回到空（fix-forward，
  PR body 已注明）。
- 回收站过期策略只交付「滞留天数」列 + 排序；自动清理（N 天）未做。
- 版本对比的行级 diff 是「首差异块」样本，非全量 LCS。
