// 层：数据
#include "datacatalog.h"
#include "../metadata/storeerrors_internal.h"

namespace
{
using paleo::store_detail::setError;
} // namespace

// 实体族（方向 99 拆分）：addEntity/hasEntity/entities/entityById/井名规范化
// 匹配/前缀 id 分配。查询事实源是内存四表 + D5 邻接索引。

bool DataCatalog::addEntity(const CatalogEntity &e, QString *error)
{
  if (!checkWriteThread("addEntity", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (e.id.isEmpty() || hasEntity(e.id))
  {
    setError(error, QStringLiteral("entity id empty or duplicate: %1").arg(e.id));
    return false;
  }
  m_entities.append(e);
  m_idx.entityAdded(m_entities.size() - 1, e.id, e.entityType); // D5.2 增量
  m_dirtyEntities.insert(e.id);
  if (save(error))
  {
    CatalogOp op;
    op.kind = CatalogOp::Kind::AddEntity;
    op.entity = e;
    recordOp(std::move(op));
    return true;
  }
  m_entities.removeLast();
  m_idx.rebuild(m_entities, m_assets, m_versions, m_links); // 回滚——罕见路径全量换正确性
  return false;
}

bool DataCatalog::hasEntity(const QString &id) const
{
  noteRead("hasEntity");
  return m_idx.entityRow(id) >= 0; // D5.1 O(1)
}

QVector<CatalogEntity> DataCatalog::entities(const QString &entityType) const
{
  noteRead("entities");
  QVector<CatalogEntity> out;
  if (entityType.isEmpty())
    return m_entities;
  const QVector<int> rows = m_idx.entityRowsByType(entityType); // D5.1
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_entities.at(r));
  return out;
}

CatalogEntity DataCatalog::entityById(const QString &id) const
{
  noteRead("entityById");
  const int row = m_idx.entityRow(id); // D5.1 O(1)
  return row >= 0 ? m_entities.at(row) : CatalogEntity();
}

QString DataCatalog::normalizeWellName(const QString &name)
{
  QString out;
  out.reserve(name.size());
  for (const QChar c : name)
  {
    if (c.isSpace() || c == QLatin1Char('-') || c == QLatin1Char('_'))
      continue;
    out.append(c.toLower());
  }
  return out;
}

QStringList DataCatalog::wellsMatchingName(const QString &name) const
{
  noteRead("wellsMatchingName");
  const QString needle = normalizeWellName(name);
  QStringList out;
  if (needle.isEmpty())
    return out;
  const QVector<int> rows = m_idx.entityRowsByType(QStringLiteral("well")); // D5.1
  for (int r : rows)
  {
    // D12：井身份只走规范化 name——uwi/别名匹配已随字段一并剥离。
    if (normalizeWellName(m_entities.at(r).name) == needle)
      out.append(m_entities.at(r).id);
  }
  return out;
}

QString DataCatalog::nextEntityId(const QString &prefix)
{
  noteRead("nextEntityId");
  ++m_mutationSeq; // 分配 id 也是 owner 侧状态变化（提交基线要看见）
  // WP2：前缀最大序号走索引（旧实现线性扫全实体表——导入井/辅助实体逐个
  // 调用即 O(N²)）。语义等价：只认「prefix 后跟 '-' 且余段纯数字」的既有 id。
  const int max = m_idx.maxEntitySeqForPrefix(prefix);
  return QStringLiteral("%1-%2").arg(prefix).arg(max + 1);
}
