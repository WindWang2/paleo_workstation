// 层：视图
#include "datalist.h"
#include "datalistops.h"
#include "../../catalog/datacatalog.h"
#include "../../catalog/realizationset.h"
#include "../../services/previewdoc.h"
#include "../../workflow/assetops.h"
#include "../../workflow/importledger.h"
#include "../../workflow/storagegovernancecontroller.h"
#include "dataops/dataopscommands.h"
#include "dataops/dataopsmodel.h"
#include "dataopsundo.h"
#include "dataopspanelops.h"
#include "versiondialog.h"
#include "pendinglinkdialog.h"
#include "importledgerdialog.h"
#include "../dialogs/cataloghealthdialog.h"
#include "../dialogs/storagegovernancedialog.h"

#include <algorithm>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include "ui/notifications/notificationmanager.h"
#include <QPushButton>

using namespace paleo::pagesinternal;

namespace paleo::pagesinternal
{

// ---- T28：链接身份寻址 + undo 库 -----------------------------------------
// 动作（挂接/撤销/设为主版本）按 (assetId, role[, entityId]) 链接身份在
// 点击时刻重扫 links()，不用刷新时捕获的行下标——open() 重建或任何
// 中途变更后仍命中同一条链接。
int indexOfLink(DataCatalog *cat, const QString &assetId, const QString &role,
                const QString &entityId)
{
  const QVector<EntityAssetLink> ls = cat->links();
  for (int i = 0; i < ls.size(); ++i)
  {
    const EntityAssetLink &l = ls.at(i);
    if (l.assetId != assetId || l.role != role)
      continue;
    if (entityId.isEmpty())
    {
      if (l.unresolved)
        return i; // 未决链接：entityId 空
    }
    else if (!l.unresolved && l.entityId == entityId)
      return i;
  }
  return -1;
}

QString undoVaultPath(DataCatalog *cat)
{
  const QString cp = cat ? cat->catalogPath() : QString();
  if (cp.isEmpty() || !cat->isOpen())
    return QString();
  // catalog 在 <projectDir>/artifacts/metadata/catalog.json → 退三级到工程
  // 目录（方向 30 修正：与 dataopsmodel.h projectDirFor 同步——原两级推导
  // 落在 artifacts/ 下）。
  const QString projectDir = QFileInfo(
      QFileInfo(QFileInfo(cp).dir().absolutePath()).dir().absolutePath())
      .dir()
      .absolutePath();
  return QDir(projectDir).filePath(QStringLiteral(".paleo/undo_stack.json"));
}

// 副作用清洗：attach 的目标链接已不在（被外部改掉/新会话没有这条挂接）
// → 记录作废剔除；note 记忆只留仍未决链接能用的。
QVector<UndoRecord> loadUndoVault(DataCatalog *cat, QHash<QString, QString> *notesOut)
{
  QVector<UndoRecord> records;
  const QString path = undoVaultPath(cat);
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return records;
  const QJsonObject root =
      QJsonDocument::fromJson(f.readAll()).object();
  for (const QJsonValue &v : root.value(QStringLiteral("undo")).toArray())
  {
    const QJsonObject o = v.toObject();
    UndoRecord r;
    r.assetId = o.value(QStringLiteral("asset_id")).toString();
    r.entityId = o.value(QStringLiteral("entity_id")).toString();
    r.role = o.value(QStringLiteral("role")).toString();
    r.entityType = o.value(QStringLiteral("entity_type")).toString();
    r.note = o.value(QStringLiteral("note")).toString();
    const QJsonObject d = o.value(QStringLiteral("demoted")).toObject();
    r.demotedAssetId = d.value(QStringLiteral("asset_id")).toString();
    r.demotedEntityId = d.value(QStringLiteral("entity_id")).toString();
    r.demotedRole = d.value(QStringLiteral("role")).toString();
    r.demotedEntityType = d.value(QStringLiteral("entity_type")).toString();
    if (r.isValid() && indexOfLink(cat, r.assetId, r.role, r.entityId) >= 0)
      records.append(r); // 链接仍在（已决到同一实体）→ 撤销入口有效
  }
  if (notesOut)
    for (const QJsonValue &v : root.value(QStringLiteral("notes")).toArray())
    {
      const QJsonObject o = v.toObject();
      const QString key = o.value(QStringLiteral("asset_id")).toString() +
                          QLatin1Char('|') + o.value(QStringLiteral("role")).toString();
      notesOut->insert(key, o.value(QStringLiteral("note")).toString());
    }
  return records;
}

void saveUndoVault(DataCatalog *cat, const QVector<UndoRecord> &records,
                   const QHash<QString, QString> &notes)
{
  const QString path = undoVaultPath(cat);
  if (path.isEmpty())
    return;
  QDir().mkpath(QFileInfo(path).absolutePath());
  QJsonObject root;
  QJsonArray arr;
  for (const UndoRecord &r : records)
  {
    QJsonObject o;
    o.insert(QStringLiteral("asset_id"), r.assetId);
    o.insert(QStringLiteral("entity_id"), r.entityId);
    o.insert(QStringLiteral("role"), r.role);
    o.insert(QStringLiteral("entity_type"), r.entityType);
    o.insert(QStringLiteral("note"), r.note);
    if (!r.demotedAssetId.isEmpty())
    {
      QJsonObject d;
      d.insert(QStringLiteral("asset_id"), r.demotedAssetId);
      d.insert(QStringLiteral("entity_id"), r.demotedEntityId);
      d.insert(QStringLiteral("role"), r.demotedRole);
      d.insert(QStringLiteral("entity_type"), r.demotedEntityType);
      o.insert(QStringLiteral("demoted"), d);
    }
    arr.append(o);
  }
  root.insert(QStringLiteral("undo"), arr);
  QJsonArray noteArr;
  for (auto it = notes.constBegin(); it != notes.constEnd(); ++it)
  {
    const int sep = it.key().indexOf(QLatin1Char('|'));
    QJsonObject o;
    o.insert(QStringLiteral("asset_id"), it.key().left(sep));
    o.insert(QStringLiteral("role"), it.key().mid(sep + 1));
    o.insert(QStringLiteral("note"), it.value());
    noteArr.append(o);
  }
  root.insert(QStringLiteral("notes"), noteArr);
  QFile f(path);
  if (f.open(QIODevice::WriteOnly))
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
  // 写失败只在日志说明——undo 是增强面，不阻塞挂接本身。
  else
    qWarning() << "DataPage undo vault write failed:" << path;
}

} // namespace paleo::pagesinternal

void DataListPanel::registerCommands()
{
  using namespace paleo::dataops;
  const ShortcutSpec *specs = kShortcutSpecs;
  const int n = int(sizeof(kShortcutSpecs) / sizeof(kShortcutSpecs[0]));
  for (int i = 0; i < n; ++i)
  {
    CommandEntry e;
    e.id = QLatin1String(specs[i].id);
    e.title = tr(specs[i].title);
    e.category = tr(specs[i].category);
    e.shortcut = QLatin1String(specs[i].shortcut);
    e.keywords << e.title << e.category;
    m_reg.registerCommand(e);
  }
  const auto reg = [this](const char *id, const char *title, const char *cat,
                          std::function<void()> fn) {
    CommandEntry e;
    e.id = QLatin1String(id);
    e.title = tr(title);
    e.category = tr(cat);
    e.trigger = std::move(fn);
    e.keywords << e.title << tr(cat);
    m_reg.registerCommand(e);
  };
  reg("dataops.selectAllVisible", QT_TR_NOOP("全选可见项"), QT_TR_NOOP("选择"),
      [this] { selectAllVisibleAssets(); });
  reg("dataops.invertSel", QT_TR_NOOP("反选"), QT_TR_NOOP("选择"),
      [this] { invertAssetSelection(); });
  reg("dataops.attachBatch", QT_TR_NOOP("批量挂接到实体"), QT_TR_NOOP("批量"),
      [this] { batchAttachToEntity(); });
  reg("dataops.changeType", QT_TR_NOOP("批量改类型"), QT_TR_NOOP("批量"),
      [this] { batchChangeType(); });
  reg("dataops.removeSoft", QT_TR_NOOP("移除（软删）"), QT_TR_NOOP("批量"),
      [this] { batchRemoveSoft(); });
  reg("dataops.exportManifest", QT_TR_NOOP("导出清单 CSV/JSON"), QT_TR_NOOP("批量"),
      [this] { batchExportManifest(); });
  reg("dataops.openPreviewAll", QT_TR_NOOP("批量打开预览"), QT_TR_NOOP("批量"),
      [this] { batchOpenPreview(); });
  reg("dataops.addTag", QT_TR_NOOP("给选中打标签"), QT_TR_NOOP("批量"),
      [this] { batchAddTag(); });
  reg("dataops.recycleBin", QT_TR_NOOP("打开可回收清单"), QT_TR_NOOP("批量"),
      [this] { showRecycleBin(); });
  reg("dataops.clearFilter", QT_TR_NOOP("清除全部过滤"), QT_TR_NOOP("过滤"),
      [this] {
        m_filter.conditions.clear();
        m_activeTag.clear();
        applyListFilter();
        refreshChipBar();
      });
  reg("dataops.filterUnlinked", QT_TR_NOOP("过滤：未挂接"), QT_TR_NOOP("过滤"),
      [this] {
        m_filter.add({FilterDim::Unlinked, QString(), false});
        applyListFilter();
        refreshChipBar();
      });
  reg("dataops.filterPending", QT_TR_NOOP("过滤：有警告"), QT_TR_NOOP("过滤"),
      [this] {
        m_filter.add({FilterDim::Warned, QString(), false});
        applyListFilter();
        refreshChipBar();
      });
  reg("dataops.viewTree", QT_TR_NOOP("视图：树形"), QT_TR_NOOP("视图"),
      [this] { setViewMode(0); });
  reg("dataops.viewTable", QT_TR_NOOP("视图：列表"), QT_TR_NOOP("视图"),
      [this] { setViewMode(1); });
  reg("dataops.viewIcon", QT_TR_NOOP("视图：图标"), QT_TR_NOOP("视图"),
      [this] { setViewMode(2); });
  reg("dataops.viewVirtual", QT_TR_NOOP("视图：高速（大数据）"), QT_TR_NOOP("视图"),
      [this] { setViewMode(3); });
  reg("dataops.viewGroup", QT_TR_NOOP("视图：分组"), QT_TR_NOOP("视图"),
      [this] { setViewMode(4); });
  reg("dataops.refresh", QT_TR_NOOP("刷新数据列表"), QT_TR_NOOP("视图"),
      [this] { refreshAssetTable(); });
}

void DataListPanel::loadStoresForCatalog()
{
  using namespace paleo::dataops;
  PreviewDocService *svc = m_doc;
  DataCatalog *cat = svc ? svc->catalog() : nullptr;
  if (!cat || !cat->isOpen())
    return;
  const QString path = cat->catalogPath();
  if (path != m_lastCatalogPath)
  {
    // D5.6：项目会话切换 → 命令栈/历史清空（旧工程的命令对新 catalog 无意义）。
    m_lastCatalogPath = path;
    m_pageSelection.clear();
    m_assetPage = 0;
    if (m_opStack)
      m_opStack->clear();
    if (m_history)
      m_history->clear();
  }
  m_tags.load(cat);
  m_typeOv.load(cat);
  m_entityOv.load(cat);
  m_recycle.load(cat);
  m_ctx.cat = cat;
  m_ctx.tags = &m_tags;
  m_ctx.assetOverrides = &m_typeOv;
  m_ctx.entityOverrides = &m_entityOv;
  m_ctx.recycle = &m_recycle;
}

void DataListPanel::rebuildRowSnapshot()
{
  using namespace paleo::dataops;
  m_rows.clear();
  PreviewDocService *svc = m_doc;
  DataCatalog *cat = svc ? svc->catalog() : nullptr;
  if (!cat)
    return;
  const bool unresolvedOnly = property("paleo.page.filterUnresolved").toBool();
  const auto assets = cat->assets();
  m_rows.reserve(assets.size());
  for (const CatalogAsset &a : assets)
  {
    if (m_recycle.isRemoved(a.id))
      continue;
    AssetRowInfo row = buildAssetRow(cat, a, m_tags, m_typeOv, m_recycle, m_entityOv, assets.size() <= 200);
    if (unresolvedOnly && !row.unresolved)
      continue;
    m_rows.append(row);
  }
}

void DataListPanel::refreshUndoButtons()
{
  if (m_undoBtn)
  {
    // D5.2：菜单项（按钮同面）显示操作名——「撤销 挂接 …」。
    m_undoBtn->setEnabled(m_opStack && m_opStack->canUndo());
    m_undoBtn->setText(m_opStack && m_opStack->canUndo()
                           ? tr("撤销 %1").arg(m_opStack->undoText())
                           : tr("撤销"));
    m_undoBtn->setToolTip(m_opStack && m_opStack->canUndo()
                              ? m_opStack->undoText() : tr("无可撤销操作"));
  }
  if (m_redoBtn)
  {
    m_redoBtn->setEnabled(m_opStack && m_opStack->canRedo());
    m_redoBtn->setText(m_opStack && m_opStack->canRedo()
                           ? tr("重做 %1").arg(m_opStack->redoText())
                           : tr("重做"));
    m_redoBtn->setToolTip(m_opStack && m_opStack->canRedo()
                              ? m_opStack->redoText() : tr("无可重做操作"));
  }
}

void DataListPanel::undoOp()
{
  if (!m_opStack || !m_opStack->canUndo())
    return;
  const QString text = m_opStack->undo();
  refreshAssetTable();
  emit entityRefreshRequested();
  // D5.4：撤销后状态栏确认反馈。
  emit statusMessage(tr("已撤销：%1").arg(text));
}

void DataListPanel::redoOp()
{
  if (!m_opStack || !m_opStack->canRedo())
    return;
  const QString text = m_opStack->redo();
  refreshAssetTable();
  emit entityRefreshRequested();
  emit statusMessage(tr("已重做：%1").arg(text));
}

void DataListPanel::pushCommand(paleo::dataops::DataOpCommand *cmd)
{
  if (!m_opStack || !cmd)
    return;
  m_opStack->push(cmd);
  refreshAssetTable();
  emit entityRefreshRequested();
}

void DataListPanel::showRecycleBin()
{
  using namespace paleo::dataops;
  if (!m_ctx.valid())
    return;
  DataCatalog *cat = m_ctx.cat;
  const QString pd = projectDirFor(cat);

  // 方向 30：软删资产的受管字节占用（assetId → bytes；外链源不计）。
  const auto entrySizes = [&]() {
    QHash<QString, qint64> sizes;
    for (const RecycleEntry &e : m_recycle.entries())
    {
      qint64 t = 0;
      for (const CatalogVersion &v : cat->versionsForAsset(e.assetId))
      {
        if (!v.managed)
          continue;
        const QString abs = DataCatalog::resolvedVersionPath(pd, v);
        if (abs.isEmpty())
          continue;
        const QFileInfo fi(abs);
        if (fi.exists())
          t += fi.size();
      }
      sizes.insert(e.assetId, t);
    }
    return sizes;
  };

  RecycleBinDialog dlg(this);
  auto reload = [&]() {
    dlg.setEntrySizes(entrySizes());
    dlg.loadEntries(m_recycle.entries());
  };
  reload();
  connect(&dlg, &RecycleBinDialog::restoreRequested, this,
          [&](const QStringList &assetIds) {
            if (assetIds.isEmpty())
              return;
            // 方向 30：单命令批量恢复（一次落盘、整组可撤销）。
            QVector<std::pair<QString, RecycleEntry>> entries;
            for (const QString &id : assetIds)
              for (const RecycleEntry &x : m_recycle.entries())
                if (x.assetId == id)
                  entries.append({id, x});
            pushCommand(new BatchRestoreCmd(m_ctx, assetIds, entries));
            reload();
            emit statusMessage(tr("已恢复 %1 个资产（可整组撤销）").arg(assetIds.size()));
          });
  connect(&dlg, &RecycleBinDialog::restoreAllRequested, this, [&] {
    QStringList ids;
    QVector<std::pair<QString, RecycleEntry>> entries;
    for (const RecycleEntry &e : m_recycle.entries())
    {
      ids << e.assetId;
      entries.append({e.assetId, e});
    }
    if (ids.isEmpty())
      return;
    pushCommand(new BatchRestoreCmd(m_ctx, ids, entries));
    reload();
    emit statusMessage(tr("已全部恢复（%1 项，可整组撤销）").arg(ids.size()));
  });
  connect(&dlg, &RecycleBinDialog::purgeAllRequested, this, [&] {
    m_recycle.clearAll();
    m_recycle.save();
    reload();
    refreshAssetTable();
    emit statusMessage(tr("可回收清单已清空（不可撤销）"));
  });
  // 方向 30：物理删除（catalog + 磁盘双清；二次确认在对话框内）。
  connect(&dlg, &RecycleBinDialog::purgeRequested, this, [&](const QStringList &ids) {
    if (ids.isEmpty() || !cat->isOpen())
      return;
    const paleo::assetops::PurgeOutcome out =
        paleo::assetops::purgeAssets(cat, pd, ids);
    for (const QString &id : ids)
      m_recycle.purge(id); // 清 sidecar 条目（不可恢复路径同形）
    m_recycle.save();
    reload();
    refreshAssetTable();
    emit entityRefreshRequested();
    QString msg = tr("物理删除完成：%1 项，释放 %2 字节。")
                      .arg(out.purgedAssetIds.size())
                      .arg(out.bytesFreed);
    if (!out.failedAssets.isEmpty() || !out.leftoverFiles.isEmpty())
    {
      QStringList lines{msg};
      for (const QString &f : out.failedAssets)
        lines << tr("· 被拒：%1").arg(f);
      for (const QString &f : out.leftoverFiles)
        lines << tr("· 残留文件（请手动清理）：%1").arg(f);
      paleo::ui::NotificationManager::showWarning(&dlg, tr("物理删除（部分未完成）"), lines.join(QLatin1Char('\n')));
    }
    else
      emit statusMessage(msg);
  });
  dlg.exec();
}

void DataListPanel::showImportLedger()
{
  DataCatalog *cat = m_ctx.cat;
  if (!cat || !cat->isOpen())
  {
    emit statusMessage(tr("工程未打开，暂无导入台账"));
    return;
  }
  paleo::imports::ImportLedger ledger;
  ledger.load(cat);
  paleo::dataops::ImportLedgerDialog dlg(this);
  dlg.setBatches(ledger.batches());
  dlg.exec();
}

void DataListPanel::resolvePendingLinks()
{
  using namespace paleo::dataops;
  DataCatalog *cat = m_ctx.cat;
  if (!cat || !cat->isOpen())
    return;
  PendingLinkDialog dlg(this);
  const int total = cat->unresolvedLinks().size();
  dlg.setProposals(paleo::assetops::proposablePendingLinks(cat), total);
  connect(&dlg, &PendingLinkDialog::applyRequested, this,
          [&](const QVector<int> &linkIndexes) {
            QString err;
            const int n = paleo::assetops::applyPendingResolutions(cat, linkIndexes, &err);
            dlg.setApplied(n, linkIndexes.size());
            refreshAssetTable();
            emit entityRefreshRequested();
            if (n > 0)
              emit statusMessage(tr("已归位 %1 条未决链接（单事务落盘）").arg(n));
            if (!err.isEmpty())
              paleo::ui::NotificationManager::showWarning(&dlg, tr("归位失败"), err);
          });
  dlg.exec();
}

QVector<paleo::dataops::VersionRow>
DataListPanel::versionRowsForAsset(const QString &assetId) const
{
  using namespace paleo::dataops;
  QVector<VersionRow> rows;
  DataCatalog *cat = m_ctx.cat;
  if (!cat)
    return rows;
  const QString pd = projectDirFor(cat);
  const CatalogVersion cur = cat->currentVersion(assetId);
  for (const CatalogVersion &v : cat->versionsForAsset(assetId))
  {
    VersionRow r;
    r.versionId = v.id;
    r.versionNumber = v.versionNumber;
    r.stage = v.stage;
    r.managed = v.managed;
    r.fileName = v.fileName;
    r.sha256 = v.sha256;
    r.sourceUri = v.sourceUri;
    r.isCurrent = (v.id == cur.id);
    // 方向 47：realization 契约键 → 成员/统计面标签（缺键行不受影响）。
    {
      bool indexOk = false;
      const int idx = v.extra.value( paleo::realization::kKeyIndex ).toInt( &indexOk );
      const QString statToken = v.extra.value( paleo::realization::kKeyStatistic ).toString();
      if ( indexOk && idx >= 0 )
        r.memberNote = tr( "成员 #%1" ).arg( idx );
      else if ( !statToken.isEmpty() )
      {
        const QString label = paleo::realization::statisticDisplayLabel( statToken );
        r.memberNote = label.isEmpty() ? statToken : label;
      }
    }
    const QString abs = v.managed ? DataCatalog::resolvedVersionPath(pd, v) : v.path;
    if (!abs.isEmpty())
    {
      const QFileInfo fi(abs);
      if (fi.exists())
        r.sizeBytes = fi.size();
    }
    rows.append(r);
  }
  return rows;
}

void DataListPanel::showVersionTable()
{
  using namespace paleo::dataops;
  DataCatalog *cat = m_ctx.cat;
  if (!cat || !cat->isOpen())
    return;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.size() != 1)
  {
    emit statusMessage(tr("版本面对单资产——请只选一个资产"));
    return;
  }
  const QString assetId = ids.values().first();
  const CatalogAsset a = cat->assetById(assetId);
  if (a.id.isEmpty())
    return;
  const QString pd = projectDirFor(cat);

  VersionTableDialog dlg(this);
  dlg.setAssetTitle(a.displayName);
  dlg.setRows(versionRowsForAsset(assetId));
  dlg.setCompareHook([cat, pd](const QString &va, const QString &vb) {
    return paleo::assetops::compareVersions(pd, cat->versionById(va), cat->versionById(vb));
  });
  connect(&dlg, &VersionTableDialog::rollbackRequested, this,
          [&](const QString &versionId) {
            QString err;
            const paleo::assetops::RollbackOutcome out =
                paleo::assetops::rollbackToVersion(cat, pd, assetId, versionId, &err);
            if (out.versionId.isEmpty())
            {
              paleo::ui::NotificationManager::showWarning(&dlg, tr("回滚失败"), err);
              return;
            }
            dlg.setRowsRefreshed(
                versionRowsForAsset(assetId),
                tr("已回滚：新增 v%1（内容与所选版本一致，历史全保留）。")
                    .arg(out.versionNumber));
            refreshAssetTable();
            emit entityRefreshRequested();
            emit statusMessage(tr("「%1」已回滚到所选版本内容（新版本 v%2）")
                                   .arg(a.displayName)
                                   .arg(out.versionNumber));
          });
  dlg.exec();
}

void DataListPanel::refreshHealthReportInDialog()
{
  if (!m_healthDlg)
    return;
  m_healthDlg->setReport(m_healthBase, m_healthRecycleCount, m_healthRecycleBytes);
}

void DataListPanel::showStorageGovernance()
{
  if (!m_ctx.cat || !m_ctx.cat->isOpen()) { emit statusMessage(tr("工程未打开，无法扫描存储")); return; }
  if (m_storageDialog) { m_storageDialog->raise(); m_storageDialog->activateWindow(); return; }
  auto *dialog = new StorageGovernanceDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  m_storageDialog = dialog;
  auto *controller = m_storageController;
  connect(dialog, &StorageGovernanceDialog::scanRequested, controller, &StorageGovernanceController::scan);
  connect(dialog, &StorageGovernanceDialog::cancelRequested, controller, &StorageGovernanceController::cancel);
  connect(dialog, &QDialog::finished, controller, &StorageGovernanceController::cancel);
  connect(dialog, &StorageGovernanceDialog::previewRequested, controller, &StorageGovernanceController::requestPreview);
  connect(dialog, &StorageGovernanceDialog::confirmRequested, controller, &StorageGovernanceController::confirmPreview);
  connect(dialog, &StorageGovernanceDialog::verifyShaRequested, controller, &StorageGovernanceController::verifySha);
  connect(controller, &StorageGovernanceController::reportReady, dialog, &StorageGovernanceDialog::setReport);
  connect(controller, &StorageGovernanceController::previewReady, dialog, &StorageGovernanceDialog::setPreview);
  connect(controller, &StorageGovernanceController::stateChanged, dialog, &StorageGovernanceDialog::setBusy);
  connect(controller, &StorageGovernanceController::progress, dialog, &StorageGovernanceDialog::setProgress);
  connect(controller, &StorageGovernanceController::message, dialog, &StorageGovernanceDialog::setMessage);
  connect(controller, &StorageGovernanceController::shaReady, dialog,
    [dialog](const auto &issues, bool complete) {
      dialog->setMessage(complete ? tr("外链 SHA 复验完成：%1 个不一致版本。").arg(issues.size())
                                 : tr("外链 SHA 复验未完成；结果仅覆盖已扫部分。"));
    });
  connect(controller, &StorageGovernanceController::cleanupFinished, dialog, [this, dialog](const auto &outcome) {
    dialog->setMessage(tr("回收完成：实际释放 %1 B；残留文件 %2 个。请重新扫描。%3")
      .arg(outcome.bytesFreed).arg(outcome.leftoverFiles.size()).arg(outcome.leftoverFiles.join(QStringLiteral("\n"))));
    refreshAssetTable();
  });
  dialog->setBusy(controller->busy(), controller->cancellable());
  dialog->open();
  controller->scan();
}

void DataListPanel::showHealthCheck()
{
  DataCatalog *cat = m_ctx.cat;
  if (!cat || !cat->isOpen()) { emit statusMessage(tr("工程未打开，无法体检")); return; }
  CatalogHealthDialog dlg(this);
  StorageGovernanceController controller;
  controller.setCatalog(cat);
  m_healthDlg = &dlg;
  const auto quickScan = [&] {
    QStringList ids;
    for (const auto &entry : m_recycle.entries()) ids << entry.assetId;
    m_healthRecycleCount = ids.size();
    dlg.setShaState(tr("体检中；目录检查在后台执行，SHA 尚未复验。"));
    controller.healthScan(ids);
  };
  connect(&dlg, &CatalogHealthDialog::refreshRequested, &controller, quickScan);
  connect(&dlg, &CatalogHealthDialog::verifyShaRequested, &controller, &StorageGovernanceController::verifySha);
  connect(&dlg, &CatalogHealthDialog::cancelVerifyRequested, &controller, &StorageGovernanceController::cancel);
  connect(&controller, &StorageGovernanceController::stateChanged, &dlg, [&dlg](bool busy, bool) { dlg.setVerifyRunning(busy); });
  connect(&controller, &StorageGovernanceController::progress, &dlg, [&dlg](int n, int total, const QString &path) {
    dlg.setShaState(tr("后台检查 %1/%2：%3").arg(n).arg(total).arg(path));
  });
  connect(&controller, &StorageGovernanceController::healthReady, &dlg, [&](const auto &report, qint64 bytes, bool complete) {
    m_healthBase = report; m_healthRecycleBytes = bytes;
    refreshHealthReportInDialog();
    dlg.setShaState(complete ? tr("目录体检完成；外链 SHA 尚未复验。") : tr("体检未完成，结果仅覆盖已扫部分。"));
  });
  connect(&controller, &StorageGovernanceController::shaReady, &dlg, [&](const auto &issues, bool complete) {
    m_healthBase.issues.erase(std::remove_if(m_healthBase.issues.begin(), m_healthBase.issues.end(),
      [](const auto &issue) { return issue.kind == paleo::health::IssueKind::ShaMismatch; }), m_healthBase.issues.end());
    m_healthBase.issues += issues; m_healthBase.shaVerifyComplete = complete;
    refreshHealthReportInDialog();
    dlg.setShaState(complete ? tr("外链 SHA 复验完成。") : tr("外链 SHA 复验已取消，未扫完。"));
  });
  connect(&dlg, &CatalogHealthDialog::jumpToVersion, this, [&](const QString &versionId) {
    dlg.accept();
    emit versionActivated(versionId); // #199 体检 stale 版本定位——分支重写时补回
  });
  connect(&dlg, &CatalogHealthDialog::jumpToAsset, this, [&](const QString &id) { dlg.accept(); emit assetActivated(id); });
  connect(&dlg, &CatalogHealthDialog::jumpToEntity, this, [&](const QString &id) { dlg.accept(); emit entitiesFocusRequested({id}); });
  quickScan(); dlg.exec(); controller.cancel(); m_healthDlg = nullptr;
}
