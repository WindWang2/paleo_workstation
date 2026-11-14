// 层：数据
#include "catalogindex.h"
#include "datacatalog.h"

void CatalogIndex::entityMetadataChanged(int row, const CatalogEntity &entity) {
  if (entity.entityType != QLatin1String("well")) return;
  if (m_wellNameKeys.contains(entity.id)) {
    const auto old = m_wellNameKeys.value(entity.id); m_wellsByName[old].remove(entity.id);
    if (m_wellsByName.value(old).isEmpty()) m_wellsByName.remove(old);
  }
  if (m_modelWellKeys.contains(entity.id)) {
    const auto old = m_modelWellKeys.value(entity.id); m_wellsByModelId[old].remove(entity.id);
    if (m_wellsByModelId.value(old).isEmpty()) m_wellsByModelId.remove(old);
  }
  const QString key = DataCatalog::normalizeWellName(entity.name);
  m_wellNameKeys[entity.id] = key; m_wellsByName[key].insert(entity.id, row);
  const qlonglong modelId = entity.extra.value(QStringLiteral("areaModelWellId")).toLongLong();
  m_modelWellKeys[entity.id] = modelId;
  if (modelId > 0) m_wellsByModelId[modelId].insert(entity.id, row);
}
void CatalogIndex::assetMetadataChanged(int row, const CatalogAsset &asset) {
  if (m_assetNameKeys.contains(asset.id)) {
    const auto old = m_assetNameKeys.value(asset.id); m_assetsByName[old].remove(asset.id);
    if (m_assetsByName.value(old).isEmpty()) m_assetsByName.remove(old);
  }
  m_assetNameKeys[asset.id] = asset.displayName;
  m_assetsByName[asset.displayName].insert(asset.id, row);
}
