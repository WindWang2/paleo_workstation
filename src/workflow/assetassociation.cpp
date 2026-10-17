// 层：功能
#include "assetassociation.h"
#include "../catalog/datacatalog.h"
#include <QObject>
namespace paleo::assetassociation {
bool apply(DataCatalog *cat, const Change &change, QString *error) {
  const auto fail = [error](const QString &text) { if (error) *error = text; return false; };
  if (!cat || cat->mutationSeq() != change.expectedSequence)
    return fail(QObject::tr("资料已发生变化，请重新打开关联编辑器"));
  if (cat->assetById(change.assetId).id.isEmpty()) return fail(QObject::tr("文件不存在"));
  const auto entity = cat->entityById(change.entityId);
  if (entity.entityType != "well" && entity.entityType != "seismic_survey")
    return fail(QObject::tr("请选择工程中的井或地震测区"));
  bool validRole = false;
  for (const auto &role : cat->roleRegistry().forEntity(entity.entityType))
    validRole |= role.role == change.role;
  if (!validRole) return fail(QObject::tr("请选择与目标对象匹配的资料用途"));
  const auto links = cat->links();
  int target = -1;
  bool hasPrimary = false;
  for (int i = 0; i < links.size(); ++i) {
    const auto &link = links[i];
    if (!link.unresolved && link.entityId == entity.id && link.entityType == entity.entityType && link.role == change.role) {
      hasPrimary |= link.isPrimary && i != change.sourceLink;
      if (link.assetId == change.assetId) target = i;
    }
  }
  if (change.sourceLink >= 0 && (change.sourceLink >= links.size() || links[change.sourceLink].assetId != change.assetId))
    return fail(QObject::tr("原关联已变化，请重新选择"));
  if (change.sourceLink >= 0 && target >= 0 && target != change.sourceLink)
    return fail(QObject::tr("该文件已关联到此用途，请选“替换主文件”或保留现有关联"));
  const int row = change.sourceLink >= 0 ? change.sourceLink : target;
  EntityAssetLink link = row >= 0 ? links[row] : EntityAssetLink{};
  link.assetId = change.assetId; link.entityId = entity.id; link.entityType = entity.entityType;
  link.role = change.role; link.unresolved = false; link.note.clear();
  link.isPrimary = change.primary || !hasPrimary;
  if (target >= 0 && change.sourceLink < 0 && !change.primary) return true;
  DataCatalog::BatchSave batch(cat);
  if (!(row >= 0 ? cat->updateLink(row, link, error) : cat->addLink(link, error))) {
    batch.abort(); return false;
  }
  return batch.flush(error);
}
}
