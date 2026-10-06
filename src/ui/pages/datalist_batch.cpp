// 层：视图
#include "datalist.h"
#include "datalistops.h"
#include "../../catalog/datacatalog.h"
#include "../../services/previewdoc.h"
#include "dataops/dataopscommands.h"
#include "dataops/dataopsexport.h"
#include "dataopspanelops.h"
#include "dataops/dataopsselection.h"
#include "dataopsviews.h"

#include <QAction>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include "../notifications/paleonotify.h"
#include <QUrl>

void DataListPanel::batchAttachToEntity()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  EntityPickerDialog dlg(this);
  dlg.loadEntities(m_ctx.cat, m_entityOv, QString());
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QString target = dlg.selectedEntityId();
  if (target.isEmpty())
    return;
  applyEntityDrop(QStringList(ids.values()), target);
}

void DataListPanel::batchChangeType()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  // 类型词表 = 库内类型 ∪ 分类器常用类型。
  QStringList vocab;
  for (const AssetRowInfo &r : m_rows)
    if (!vocab.contains(r.effectiveType) && !r.effectiveType.isEmpty())
      vocab << r.effectiveType;
  vocab << QStringLiteral("well_log") << QStringLiteral("well_head")
        << QStringLiteral("tops") << QStringLiteral("time_depth")
        << QStringLiteral("seismic") << QStringLiteral("horizon")
        << QStringLiteral("boundary") << QStringLiteral("auxiliary");
  vocab.removeDuplicates();
  vocab.sort();
  QStringList names;
  for (const QString &id : ids)
  {
    const CatalogAsset a = m_ctx.cat->assetById(id);
    names << a.displayName;
  }
  BatchTypeDialog dlg(names, vocab, this);
  dlg.setNote(tr("改型写入视图层改写表（可撤销，可清除回原型）；catalog 资产"
                 "记录的类型字段不动。"));
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QString newType = dlg.chosenType();
  if (newType.isEmpty())
    return;
  QStringList failures;
  QVector<AssetRowInfo> updatedRows;
  for (const QString &id : ids)
  {
    const QString prev = m_typeOv.overriddenType(id);
    if (!m_typeOv.setType(id, newType))
    {
      // 幂等跳过不算失败。
      continue;
    }
    if (!m_typeOv.save())
      failures << tr("%1：sidecar 写入失败").arg(id);
    else
    {
      if (m_opStack)
        m_opStack->push(new TypeOverrideCmd(m_ctx, id, newType, prev));
      for (AssetRowInfo &r : m_rows)
      {
        if (r.assetId == id)
        {
          r.effectiveType = newType;
          updatedRows.append(r);
        }
      }
    }
  }

  // 方向 52 B 线：增量通道更新——若虚拟视图在用，走 updateRows 增量发射 dataChanged，保留滚动与选中；
  // 仅在无虚拟视图或全量重构时回落 refreshAssetTable。
  if (m_virtualView && !updatedRows.isEmpty())
  {
    m_virtualView->updateRows(updatedRows);
    if (m_iconView)
      m_iconView->loadRows(m_rows);
    if (m_groupTree)
      m_groupTree->loadRows(m_rows);
    emit entityRefreshRequested();
  }
  else
  {
    refreshAssetTable();
    emit entityRefreshRequested();
  }

  if (m_history)
    m_history->push(tr("批量改类型 → %1（%2 项）").arg(newType).arg(ids.size()));
  if (!failures.isEmpty())
    showBatchFailureDetail(this, tr("批量改类型"), failures);
  else
    emit statusMessage(tr("已把 %1 个资产类型改为 %2（可撤销）").arg(ids.size()).arg(newType));
}

void DataListPanel::batchRemoveSoft()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  // D5.5：软删可撤销——确认说明这一点（不是破坏性删除）。
  if (!PaleoNotify::ask(this, tr("移除资产"),
                        tr("把 %1 个资产移入可回收清单？\n"
                           "（软删：可从「可回收清单」恢复，可撤销；"
                           "catalog 记录保留）").arg(ids.size())))
    return;
  // 方向 30：单命令批量软删（一次落盘、整组可撤销）——逐项命令会随 N 放大
  // recycle_bin.json 重写次数（tst_ui_blocking 探针的非空转门槛）。
  QVector<std::pair<QString, RecycleEntry>> entries;
  for (const QString &id : ids)
  {
    const CatalogAsset a = m_ctx.cat->assetById(id);
    RecycleEntry e;
    e.assetId = id;
    e.type = a.type;
    e.displayName = a.displayName;
    entries.append({id, e});
  }
  const QStringList idList(ids.constBegin(), ids.constEnd());
  pushCommand(new BatchRemoveCmd(m_ctx, idList, entries, tr("批量移除")));
  if (m_history)
    m_history->push(tr("批量移除 %1 项（软删）").arg(ids.size()));
  emit statusMessage(tr("已移除 %1 个资产到可回收清单（可撤销）").arg(ids.size()));
}

void DataListPanel::batchExportManifest()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  QVector<AssetRowInfo> rows;
  for (const AssetRowInfo &r : m_rows)
    if (ids.isEmpty() || ids.contains(r.assetId))
      rows.append(r);
  if (rows.isEmpty())
    return;
  const QString path = exportRowsToFile(this, rows);
  if (!path.isEmpty())
    emit statusMessage(tr("清单已导出：%1").arg(path));
}

void DataListPanel::batchOpenPreview()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty())
    return;
  const auto plan = batchOpenPreviewPlan(QStringList(ids.values()));
  for (const QString &id : plan.first)
    emit assetActivated(id);
  if (plan.second > 0)
    emit statusMessage(tr("已打开前 %1 项预览；其余 %2 项未打开（防标签爆炸）")
                           .arg(plan.first.size()).arg(plan.second));
}

void DataListPanel::batchAddTag()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  bool ok = false;
  const QString tag = QInputDialog::getText(this, tr("打标签"),
                                            tr("标签名（选中 %1 个资产）:")
                                                .arg(ids.size()),
                                            QLineEdit::Normal, QString(), &ok);
  if (!ok)
    return;
  int n = 0;
  for (const QString &id : ids)
    if (m_tags.addTag(id, tag))
    {
      m_tags.save();
      pushCommand(new TagCmd(m_ctx, id, tag, true));
      ++n;
    }
  if (n > 0 && m_history)
    m_history->push(tr("打标签「%1」（%2 项）").arg(tag).arg(n));
  emit statusMessage(n > 0 ? tr("已给 %1 个资产打标签「%2」").arg(n).arg(tag)
                           : tr("标签未变化（已存在或为空）"));
}

void DataListPanel::detachSingleAssetLink()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  for (const QString &id : ids)
    for (const EntityAssetLink &l : m_ctx.cat->linksForAsset(id))
      if (!l.unresolved)
      {
        pushCommand(new DetachLinkCmd(m_ctx, id, l.role, l.entityId));
        emit statusMessage(tr("已解挂 %1→%2（可撤销）").arg(id, l.entityId));
        return; // 单操作语义：首个选中资产的第一条已决链接
      }
  emit statusMessage(tr("选中资产没有已决关联"));
}

void DataListPanel::setPrimaryForSelection()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid())
    return;
  for (const QString &id : ids)
  {
    const QVector<EntityAssetLink> links = m_ctx.cat->linksForAsset(id);
    for (const EntityAssetLink &l : links)
      if (!l.unresolved && !l.isPrimary)
      {
        // 前主关联（撤销时恢复）。
        QString prevPrimary;
        for (const EntityAssetLink &o : links)
          if (!o.unresolved && o.isPrimary && o.entityId == l.entityId && o.role == l.role)
            prevPrimary = o.assetId;
        pushCommand(new SetPrimaryCmd(m_ctx, id, l.role, l.entityId, prevPrimary));
        emit statusMessage(tr("已把 %1 设为主关联（可撤销）").arg(id));
        return;
      }
  }
  emit statusMessage(tr("选中资产没有可提升的非主关联"));
}

void DataListPanel::editRoleForSelection()
{
  using namespace paleo::dataops;
  const QSet<QString> ids = currentAssetSelection();
  if (ids.isEmpty() || !m_ctx.valid() || ids.size() != 1)
    return;
  const QString assetId = *ids.constBegin();
  QString role, entityId;
  for (const EntityAssetLink &l : m_ctx.cat->linksForAsset(assetId))
    if (!l.unresolved)
    {
      role = l.role;
      entityId = l.entityId;
      break;
    }
  if (role.isEmpty())
  {
    emit statusMessage(tr("该资产没有已决关联可改角色"));
    return;
  }
  // 词表 = roleRegistry 按实体类型的合法角色（工程可扩词表如实进下拉）。
  QStringList vocab;
  QString linkEntityType;
  for (const EntityAssetLink &l : m_ctx.cat->linksForAsset(assetId))
    if (!l.unresolved)
    {
      linkEntityType = l.entityType;
      break;
    }
  for (const RoleDef &d : m_ctx.cat->roleRegistry().forEntity(linkEntityType))
    vocab << d.role;
  if (!vocab.contains(role))
    vocab << role;
  vocab.sort();
  RoleEditDialog dlg(this);
  dlg.loadRole(role, vocab);
  if (dlg.exec() != QDialog::Accepted || dlg.newRole() == role)
    return;
  // D4.7 + D5.5：addLink 无删除对偶 → 不可撤销，确认说明。
  if (!PaleoNotify::ask(
          this, tr("角色变更（不可撤销）"),
          tr("将为「%1」新增角色关联 %2（原 %3 关联保留）。\n"
             "此操作不可撤销。继续？")
              .arg(assetId, dlg.newRole(), role),
          PaleoNotify::AskButtons::YesNo, PaleoNotify::AskDefault::Platform,
          PaleoNotify::AskIcon::Warning))
    return;
  EntityAssetLink l;
  l.entityType = m_ctx.cat->entityById(entityId).entityType;
  l.entityId = entityId;
  l.assetId = assetId;
  l.role = dlg.newRole();
  l.isPrimary = false;
  QString err;
  if (m_ctx.cat->addLink(l, &err))
  {
    if (m_history)
      m_history->push(tr("角色变更 %1：%2→%3").arg(assetId, role, dlg.newRole()));
    refreshAssetTable();
    emit entityRefreshRequested();
    emit statusMessage(tr("已新增角色关联 %1（原关联保留，不可撤销）")
                           .arg(dlg.newRole()));
  }
  else
    emit statusMessage(tr("角色变更失败：%1").arg(err));
}

void DataListPanel::showAssetContextMenu(QObject *source, const QPoint &pos)
{
  using namespace paleo::dataops;
  // 选择集（右键不改变选择——Qt 菜单口径：右键在选中项上保持多选）。
  const SelectionMix mix = currentSelectionMix();
  ContextMenuSpec spec;
  spec.hasAssets = !mix.assetIds.isEmpty();
  spec.hasEntities = !mix.entityIds.isEmpty();
  spec.assetCount = int(mix.assetIds.size());
  spec.entityCount = mix.entityIds.size();
  if (spec.assetCount == 1)
  {
    const QString id = *mix.assetIds.constBegin();
    for (const AssetRowInfo &r : m_rows)
      if (r.assetId == id)
      {
        spec.singleAssetUnresolved = r.unresolved;
        spec.singleAssetResolved = !r.entityNames.isEmpty();
        spec.singleAssetIsHorizon = r.effectiveType == QLatin1String("horizon");
        spec.singleAssetIsTops = r.effectiveType == QLatin1String("well_stratification");
        break;
      }
  }
  spec.hasRecycleEntries = !m_recycle.entries().isEmpty();
  const QStringList keys = contextMenuActions(spec);
  if (keys.isEmpty())
    return;
  QMenu menu(this);
  menu.setObjectName(QStringLiteral("dataContextMenu"));
  const struct
  {
    const char *key;
    const char *text;
  } kTitles[] = {
    {"openPreview", QT_TR_NOOP("打开预览")},
    {"showVersions", QT_TR_NOOP("版本与回滚…")},
    {"gridHorizon", QT_TR_NOOP("网格化…")},
    {"editTops", QT_TR_NOOP("编辑分层…")},
    {"openPreviewAll", QT_TR_NOOP("批量打开预览（前 8 项）")},
    {"attachToEntity", QT_TR_NOOP("挂接到实体…")},
    {"detachLink", QT_TR_NOOP("解除挂接")},
    {"setPrimary", QT_TR_NOOP("设为主关联")},
    {"editRole", QT_TR_NOOP("编辑挂接角色…")},
    {"transferLink", QT_TR_NOOP("转移到其它实体…")},
    {"addTag", QT_TR_NOOP("打标签…")},
    {"changeType", QT_TR_NOOP("改类型…")},
    {"exportManifest", QT_TR_NOOP("导出清单 CSV/JSON…")},
    {"removeSoft", QT_TR_NOOP("移除（进可回收清单）")},
    {"showInFolder", QT_TR_NOOP("在文件管理器中显示")},
    {"renameEntity", QT_TR_NOOP("重命名实体…")},
    {"editCoords", QT_TR_NOOP("编辑坐标/备注…")},
    {"deleteEntity", QT_TR_NOOP("删除实体…")},
    {"focusEntities", QT_TR_NOOP("定位选中实体")},
    {"selectAll", QT_TR_NOOP("全选可见项")},
    {"invertSelection", QT_TR_NOOP("反选")},
    {"showRecycleBin", QT_TR_NOOP("可回收清单…")},
  };
  for (const QString &key : keys)
  {
    const char *text = nullptr;
    for (const auto &t : kTitles)
      if (key == QLatin1String(t.key))
      {
        text = t.text;
        break;
      }
    if (!text)
      continue;
    QAction *act = menu.addAction(tr(text));
    act->setData(key);
  }
  QWidget *src = qobject_cast<QWidget *>(source);
  const QPoint global = src ? src->mapToGlobal(pos) : QCursor::pos();
  QAction *picked = menu.exec(global);
  if (!picked)
    return;
  const QString key = picked->data().toString();
  if (key == QLatin1String("openPreview") || key == QLatin1String("openPreviewAll"))
    batchOpenPreview();
  else if (key == QLatin1String("showVersions"))
    showVersionTable();
  else if (key == QLatin1String("gridHorizon"))
    emit gridHorizonRequested(*mix.assetIds.constBegin()); // 意图信号回壳（视图不干活）
  else if (key == QLatin1String("editTops"))
    emit topsEditRequested(*mix.assetIds.constBegin()); // 意图信号回壳（视图不干活）
  else if (key == QLatin1String("attachToEntity"))
    batchAttachToEntity();
  else if (key == QLatin1String("detachLink"))
    detachSingleAssetLink();
  else if (key == QLatin1String("setPrimary"))
    setPrimaryForSelection();
  else if (key == QLatin1String("editRole"))
    editRoleForSelection();
  else if (key == QLatin1String("transferLink"))
    batchAttachToEntity(); // 转移 = 选目标实体（与挂接同一对话框）
  else if (key == QLatin1String("addTag"))
    batchAddTag();
  else if (key == QLatin1String("changeType"))
    batchChangeType();
  else if (key == QLatin1String("exportManifest"))
    batchExportManifest();
  else if (key == QLatin1String("removeSoft"))
    batchRemoveSoft();
  else if (key == QLatin1String("showInFolder"))
  {
    const QSet<QString> ids = currentAssetSelection();
    if (!ids.isEmpty() && m_doc)
    {
      const CatalogVersion v = m_doc->catalog()->currentVersion(*ids.constBegin());
      const QString abs = m_doc->absolutePathForVersion(v);
      if (!abs.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(abs).absolutePath()));
    }
  }
  else if (key == QLatin1String("renameEntity"))
    emit entityRenameRequested(mix.entityIds.isEmpty() ? QString() : mix.entityIds.front());
  else if (key == QLatin1String("editCoords") || key == QLatin1String("deleteEntity"))
    emit entityDeleteRequested(mix.entityIds.isEmpty() ? QString() : mix.entityIds.front());
  else if (key == QLatin1String("focusEntities"))
    emit entitiesFocusRequested(mix.entityIds);
  else if (key == QLatin1String("selectAll"))
    selectAllVisibleAssets();
  else if (key == QLatin1String("invertSelection"))
    invertAssetSelection();
  else if (key == QLatin1String("showRecycleBin"))
    showRecycleBin();
}
