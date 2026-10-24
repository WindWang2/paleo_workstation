// 层：视图
// EntityPanel D4.6 实体 CRUD（新建/重命名/删除；file-budget 拆分自 entitypanel.cpp）

#include "entitypanel.h"
#include "../../workflow/derivationworkflow.h"
#include <QHideEvent>
#include <QSet>
#include <QShowEvent>
#include "datalistops.h"
#include "derivationgraph.h"
#include "pageshared.h"
#include "../paleotheme.h"
#include "../../services/previewdoc.h"
#include "../../services/paleotaskservice.h" // F3：GeoJSON 统计任务池路径
#include "../../catalog/datacatalog.h"
#include "../../catalog/entityview.h"
#include "dataopspanelextra.h"
#include "dataopspanelops.h"
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include "../notifications/paleonotify.h"
#include <QDialog>  // 原经 <QMessageBox> 传递引入
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QFile>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <memory>
#include <tuple>

using namespace paleo::pagesinternal;

void EntityPanel::beginCreateEntity()
{
  using namespace paleo::dataops;
  if (!m_ctx.valid())
  {
    emit statusMessage(tr("工程未打开——无法新建实体"));
    return;
  }
  EntityCreateDialog dlg(this);
  // 重名校验（D4.1 同口径：井名唯一性按 catalog 现有名）。
  const QString type = dlg.chosenType();
  Q_UNUSED(type);
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QString name = dlg.chosenName();
  if (name.isEmpty())
  {
    emit statusMessage(tr("实体名不能为空"));
    return;
  }
  for (const CatalogEntity &e : m_ctx.cat->entities(dlg.chosenType()))
  {
    const QString existing = m_ctx.entityOverrides
                                 ? m_ctx.entityOverrides->displayName(e)
                                 : e.name;
    if (existing.compare(name, Qt::CaseInsensitive) == 0)
    {
      PaleoNotify::warning(this, tr("重名"),
                           tr("已存在同名实体「%1」").arg(name));
      return;
    }
  }
  CatalogEntity e;
  e.entityType = dlg.chosenType();
  e.name = name;
  e.hasSurface = e.entityType == QLatin1String("well") ||
                 e.entityType == QLatin1String("planned"); // 计划井带井口坐标
  e.surfaceX = dlg.chosenX();
  e.surfaceY = dlg.chosenY();
  e.id = m_ctx.cat->nextEntityId(e.entityType == QLatin1String("well")
                                     ? QStringLiteral("well")
                                 : e.entityType == QLatin1String("planned")
                                     ? QStringLiteral("planned")
                                     : QStringLiteral("aux"));
  if (m_stack)
  {
    m_stack->push(new EntityCreateCmd(m_ctx, e));
    if (m_history)
      m_history->push(tr("新建实体 %1").arg(name));
  }
  else
  {
    QString err;
    if (!m_ctx.cat->addEntity(e, &err))
    {
      emit statusMessage(tr("新建实体失败：%1").arg(err));
      return;
    }
  }
  refresh();
  emit entityRefreshRequested();
  emit statusMessage(tr("已新建实体「%1」（可撤销）").arg(name));
}

void EntityPanel::beginRenameEntity(const QString &entityId)
{
  using namespace paleo::dataops;
  if (!m_ctx.valid() || entityId.isEmpty())
    return;
  const CatalogEntity e = m_ctx.cat->entityById(entityId);
  if (e.id.isEmpty())
    return;
  EntityEditDialog dlg(this);
  dlg.loadEntity(e, m_ctx.entityOverrides ? m_ctx.entityOverrides->overrideFor(e.id)
                                          : EntityOverride());
  if (dlg.exec() != QDialog::Accepted)
    return;
  const QString newName = dlg.editedName();
  // D4.1 重名校验：新名不与其它同类实体撞名。
  for (const CatalogEntity &o : m_ctx.cat->entities(e.entityType))
    if (o.id != e.id)
    {
      const QString existing = m_ctx.entityOverrides
                                   ? m_ctx.entityOverrides->displayName(o)
                                   : o.name;
      if (existing.compare(newName, Qt::CaseInsensitive) == 0)
      {
        PaleoNotify::warning(this, tr("重名"),
                             tr("已存在同名实体「%1」").arg(newName));
        return;
      }
    }
  EntityOverride next;
  next.name = newName;
  next.hasCoords = true;
  next.surfaceX = dlg.editedX();
  next.surfaceY = dlg.editedY();
  next.note = dlg.editedNote();
  if (m_stack)
  {
    m_stack->push(new EntityEditCmd(m_ctx, e.id, next,
                                    m_ctx.entityOverrides
                                        ? m_ctx.entityOverrides->overrideFor(e.id)
                                        : EntityOverride(),
                                    false));
    if (m_history)
      m_history->push(tr("编辑实体 %1").arg(newName));
  }
  refresh();
  emit entityRefreshRequested();
  emit statusMessage(tr("实体「%1」已更新（可撤销）").arg(newName));
}

void EntityPanel::beginDeleteEntity(const QString &entityId)
{
  using namespace paleo::dataops;
  if (!m_ctx.valid() || entityId.isEmpty())
    return;
  const CatalogEntity e = m_ctx.cat->entityById(entityId);
  if (e.id.isEmpty())
    return;
  const int assetCount = int(m_ctx.cat->linksForEntity(entityId).size());
  EntityDeleteDialog dlg(assetCount, this);
  if (dlg.exec() != QDialog::Accepted)
    return;
  QList<DataOpCommand *> parts;
  if (dlg.assetsToRecycle())
  {
    // 资产一并软删。D-01：过滤 unresolved（幽灵回收条目）+ 按资产去重。
    QSet<QString> recycled;
    for (const EntityAssetLink &l : m_ctx.cat->linksForEntity(entityId))
    {
      if (l.unresolved || recycled.contains(l.assetId))
        continue;
      recycled.insert(l.assetId);
      const CatalogAsset a = m_ctx.cat->assetById(l.assetId);
      parts.append(new SoftDeleteCmd(m_ctx, a.id, a.displayName, a.type, true));
    }
  }
  else
  {
    // 保留资产：解除全部关联（可撤销）。
    for (const EntityAssetLink &l : m_ctx.cat->linksForEntity(entityId))
      if (!l.unresolved)
        parts.append(new DetachLinkCmd(m_ctx, l.assetId, l.role, l.entityId));
  }
  // 实体本身：sidecar 软删（可恢复；catalog 无 removeEntity API——GAPS）。
  parts.append(new SoftDeleteCmd(m_ctx, e.id, e.name, e.entityType, true));
  // PW-049：删除实体 = 一条宏命令，一次撤销完整复原。
  if (!m_stack)
    qDeleteAll(parts);
  else if (!m_stack->push(new DataOpMacro(tr("删除实体 %1").arg(e.name), parts)))
  {
    emit statusMessage(tr("删除实体失败：%1").arg(m_stack->lastError()));
    refresh();
    return;
  }
  if (m_history)
    m_history->push(tr("删除实体 %1（软删）").arg(e.name));
  refresh();
  emit entityRefreshRequested();
  emit statusMessage(tr("实体「%1」已移入可回收清单（可撤销）").arg(e.name));
}
