// 层：数据
#include "datacatalog.h"
#include "../metadata/storeerrors_internal.h"

#include <QtGlobal>

#include <algorithm>

namespace
{
using paleo::store_detail::setError;

  // wave/data-integrity：role 词表诊断标记。addLink/attachLink 把诊断写进
  // note 尾部，invalidRoleLinks() 按标记扫描——诊断因此随 catalog.json
  // round-trip，重开工程后诊断面仍可查。
  const QString kUnknownRoleMark = QStringLiteral("未知角色: ");
  const QString kRoleTypeMismatchMark = QStringLiteral("角色与实体类型不符: ");

  // 诚实降级：role 不在词表、或实体类型不在该 role 的 entityTypes——不拦
  // 不丢不改词（词表是工程自定义的，project_area.json 可扩；硬拦会把合法
  // 自定义挡在旧二进制外），只产出诊断文本（无违例回空）。
  // 空 entityType（调用方未给）或词表未声明挂接域（自定义角色可省
  // entity_types）无从核对，不诊断。
  QString roleDiagnosis(const EntityAssetLink &l, const RoleRegistry &roles)
  {
    const RoleDef *def = roles.find(l.role);
    if (!def)
      return kUnknownRoleMark + l.role;
    if (!l.entityType.isEmpty() && !def->entityTypes.isEmpty() &&
        !def->entityTypes.contains(l.entityType))
      return kRoleTypeMismatchMark +
             QStringLiteral("%1 于 %2").arg(l.role, l.entityType);
    return QString();
  }

  // 把诊断追加到 note 尾部（已有 note 用「；」连接）并 qWarning 一次。
  void annotateRoleDiagnostics(EntityAssetLink &l, const RoleRegistry &roles)
  {
    const QString diagnosis = roleDiagnosis(l, roles);
    if (diagnosis.isEmpty())
      return;
    l.note = l.note.isEmpty() ? diagnosis
                              : l.note + QStringLiteral("；") + diagnosis;
    qWarning() << "catalog:" << diagnosis;
  }

} // namespace

// 链接族（方向 99 拆分）：addLink/attachLink/setLinkUnresolved/setLinkPrimary
// 四个链接 mutator + 链接查询面 + role 词表违例诊断面。

bool DataCatalog::addLink(const EntityAssetLink &l, QString *error)
{
  if (!checkWriteThread("addLink", error))
    return false;
  if (!ensureOpen(error))
    return false;
  // §3 修订：未决链接实体 id 留空（资产保留、不建不并）；已决链接仍必须有实体 id。
  if (l.assetId.isEmpty())
  {
    setError(error, QStringLiteral("link needs an asset id"));
    return false;
  }
  if (l.entityId.isEmpty() && !l.unresolved)
  {
    setError(error, QStringLiteral("resolved link needs an entity id"));
    return false;
  }
  // WP2：同 addVersion——全表快照换精确 undo（O(命中集)），失败回退路径
  // 语义不变：新链接摘除、被降级主关联复原。
  // 词表诊断先落 note（诚实降级——不拒收，见 invalidRoleLinks()）。
  EntityAssetLink stored = l;
  annotateRoleDiagnostics(stored, m_roles);
  m_links.append(stored);
  // §3：新的已决主关联入库后，同一 (entityType, entityId, role) 只保留这一条
  // 主关联——同角色旧主关联（例如同井同角色的旧版本资产）降级为非主。
  QVector<int> demotedRows;
  if (stored.isPrimary && !stored.unresolved)
    for (int i : m_idx.linkRowsForEntity(stored.entityId)) // D5.1 O(命中集)
      if (i != m_links.size() - 1 && m_links[i].isPrimary && !m_links[i].unresolved &&
          m_links[i].entityType == stored.entityType &&
          m_links[i].entityId == stored.entityId &&
          m_links[i].role == stored.role)
      {
        demotedRows.append(i);
        m_links[i].isPrimary = false;
      }
  m_dirtyLinkOrds.insert(m_links.size() - 1);
  for (int row : demotedRows)
    m_dirtyLinkOrds.insert(row);
  if (save(error))
  {
    m_idx.linkAdded(m_links.size() - 1, m_links.last()); // D5.2：纯追加——旧主关联
    CatalogOp op;                                         // 只降标志，邻接键不变
    op.kind = CatalogOp::Kind::AddLink;
    op.link = l; // 重放走同一 addLink（诊断/降级在目标 catalog 上重算）
    recordOp(std::move(op));
    return true;
  }
  for (int i : demotedRows)
    m_links[i].isPrimary = true;
  m_links.removeLast();
  m_idx.linksMutated(m_links);
  return false;
}

bool DataCatalog::attachLink(int index, const QString &entityId, QString *error)
{
  if (!checkWriteThread("attachLink", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (index < 0 || index >= m_links.size())
  {
    setError(error, QStringLiteral("link index out of range: %1").arg(index));
    return false;
  }
  // WP2：单行变更 + 命中集降级——快照换单行拷贝 + 降级行复原（O(命中集)）。
  const EntityAssetLink previousLink = m_links.at(index);
  EntityAssetLink &l = m_links[index];
  if (!l.unresolved)
  {
    setError(error, QStringLiteral("link %1 is not unresolved").arg(index));
    return false;
  }
  if (entityId.isEmpty())
  {
    setError(error, QStringLiteral("attach needs an entity id"));
    return false;
  }
  l.entityId = entityId;
  l.unresolved = false;
  l.note.clear();
  // 决议清空 note 后词表诊断重下——role 词表违例不因挂上实体而消失
  // （诚实降级，见 invalidRoleLinks()）。
  annotateRoleDiagnostics(l, m_roles);
  // 方向 44 挂接契约（收口，TODOS P3「attachLink 升主」递延的对账）：
  // 主文件只由显式操作变更——attachLink 不夺主。目标 (entity, role) 已有
  // 已决主关联时，新挂链接为成员（isPrimary=false）；无主才补主（首份语义，
  // 与导入路径 assignResolvedWellLogSlot 的「首条主/后到非主」一致）。
  // 显式夺主唯一入口 = setLinkPrimary（「设为主文件」按钮）。
  // 消费面依据：welllogset canonical 命名、petrophys 驱动文件选举、
  // sectionworkbench 快照恢复、mapping logVersion 都读 isPrimary——挂接
  // 换主会让这些面静默漂移（快照恢复直接拒绝），故收口。
  bool hasExistingPrimary = false;
  for (int i : m_idx.linkRowsForEntity(entityId)) // D5.1 O(命中集)
    if (i != index && m_links[i].isPrimary && !m_links[i].unresolved &&
        m_links[i].entityType == l.entityType && m_links[i].entityId == entityId &&
        m_links[i].role == l.role)
      hasExistingPrimary = true;
  l.isPrimary = !hasExistingPrimary;
  m_dirtyLinkOrds.insert(index);
  if (save(error))
  {
    m_idx.linksMutated(m_links); // entityId 获值——entity 邻接变化
    CatalogOp op;
    op.kind = CatalogOp::Kind::AttachLink;
    op.index = index;
    op.id = entityId;
    recordOp(std::move(op));
    return true;
  }
  m_links[index] = previousLink;
  m_idx.linksMutated(m_links);
  return false;
}

bool DataCatalog::setLinkUnresolved(int index, QString *error)
{
  if (!checkWriteThread("setLinkUnresolved", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (index < 0 || index >= m_links.size())
  {
    setError(error, QStringLiteral("link index out of range: %1").arg(index));
    return false;
  }
  // WP2：单行变更——快照换单行拷贝复原。
  const EntityAssetLink previousLink = m_links.at(index);
  EntityAssetLink &l = m_links[index];
  if (l.unresolved)
  {
    setError(error, QStringLiteral("link %1 is already unresolved").arg(index));
    return false;
  }
  // 回退未决：实体 id 清空、不再持主关联。资产与被共享的井实体保留（§3）。
  l.entityId.clear();
  l.unresolved = true;
  l.isPrimary = false;
  l.note.clear();
  m_dirtyLinkOrds.insert(index);
  if (save(error))
  {
    m_idx.linksMutated(m_links); // entityId 被清空——entity 邻接变化
    CatalogOp op;
    op.kind = CatalogOp::Kind::SetLinkUnresolved;
    op.index = index;
    recordOp(std::move(op));
    return true;
  }
  m_links[index] = previousLink;
  m_idx.linksMutated(m_links);
  return false;
}

bool DataCatalog::setLinkPrimary(int index, QString *error)
{
  if (!checkWriteThread("setLinkPrimary", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (index < 0 || index >= m_links.size())
  {
    setError(error, QStringLiteral("link index out of range: %1").arg(index));
    return false;
  }
  // WP2：单行标志变更 + 命中集降级——快照换单行拷贝 + 降级行复原。
  const EntityAssetLink previousLink = m_links.at(index);
  EntityAssetLink &l = m_links[index];
  if (l.unresolved || l.entityId.isEmpty())
  {
    setError(error, QStringLiteral("link %1 is not a resolved link").arg(index));
    return false;
  }
  l.isPrimary = true;
  // 与 addLink/attachLink 同一不变量：同一 (entityType, entityId, role) 只留
  // 这一条主关联——同井同角色的旧版本资产降级为非主，不复制字节。
  // 行集是提升前的快照——index 行本就在 entity 行集里，须显式排除。
  QVector<int> demotedRows;
  for (int i : m_idx.linkRowsForEntity(l.entityId)) // D5.1 O(命中集)
    if (i != index && m_links[i].isPrimary && !m_links[i].unresolved &&
        m_links[i].entityType == l.entityType && m_links[i].entityId == l.entityId &&
        m_links[i].role == l.role)
    {
      demotedRows.append(i);
      m_links[i].isPrimary = false;
    }
  m_dirtyLinkOrds.insert(index);
  for (int row : demotedRows)
    m_dirtyLinkOrds.insert(row);
  if (save(error))
  {
    // 同上：纯标志位变化——零重索引。
    CatalogOp op;
    op.kind = CatalogOp::Kind::SetLinkPrimary;
    op.index = index;
    recordOp(std::move(op));
    return true;
  }
  m_links[index] = previousLink;
  for (int i : demotedRows)
    m_links[i].isPrimary = true;
  m_idx.linksMutated(m_links);
  return false;
}

// ---- wave/data-integrity：role 词表违例诊断面 ----
// 带「未知角色: / 角色与实体类型不符: 」note 标记的链接（addLink/attachLink
// 的诚实降级写入）。按标记扫描 note：诊断随 catalog.json round-trip，重开
// 后仍可查；词表内的干净链接（含普通未决备注）不在其中。
QVector<EntityAssetLink> DataCatalog::invalidRoleLinks() const
{
  QVector<EntityAssetLink> out;
  for (const EntityAssetLink &l : m_links)
    if (l.note.contains(kUnknownRoleMark) || l.note.contains(kRoleTypeMismatchMark))
      out.append(l);
  return out;
}

QVector<EntityAssetLink> DataCatalog::linksForEntity(const QString &entityId) const
{
  noteRead("linksForEntity");
  // audit row 35：空 id 不等于「全部未决链接」——未决集合走 unresolvedLinks()；
  // 这里如实返回空集，不然调用方拿空串查询会静默命中全部未决链接。
  if (entityId.isEmpty())
    return {};
  QVector<EntityAssetLink> out;
  const QVector<int> rows = m_idx.linkRowsForEntity(entityId); // D5.1（行升序）
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_links.at(r));
  // B 包：同 (entity,role) 成员按 ordinal 升序展示。stable_sort 只按 ordinal
  // 排——不同角色间的相对序保持入库序；旧数据 ordinal 全 0 时输出与排序前
  // 完全一致，对既有消费方零扰动。
  std::stable_sort(out.begin(), out.end(),
                   [](const EntityAssetLink &a, const EntityAssetLink &b) {
                     return a.ordinal < b.ordinal;
                   });
  return out;
}

QVector<EntityAssetLink> DataCatalog::unresolvedLinks() const
{
  QVector<EntityAssetLink> out;
  for (const EntityAssetLink &l : m_links)
    if (l.unresolved)
      out.append(l);
  return out;
}

QVector<EntityAssetLink> DataCatalog::linksForAsset(const QString &assetId) const
{
  noteRead("linksForAsset");
  QVector<EntityAssetLink> out;
  const QVector<int> rows = m_idx.linkRowsForAsset(assetId); // D5.1（行升序）
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_links.at(r));
  return out;
}

QVector<EntityAssetLink> DataCatalog::links() const
{
  noteRead("links");
  return m_links;
}
