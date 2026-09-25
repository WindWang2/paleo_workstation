#include "datacatalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace
{
  const int kSchemaVersion = 1;

  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  QJsonArray cornersToJson(const QVector<QPair<double, double>> &cs)
  {
    QJsonArray a;
    for (const auto &c : cs)
    {
      QJsonObject o;
      o.insert(QStringLiteral("x"), c.first);
      o.insert(QStringLiteral("y"), c.second);
      a.append(o);
    }
    return a;
  }

  QVector<QPair<double, double>> cornersFromJson(const QJsonArray &a)
  {
    QVector<QPair<double, double>> cs;
    for (const auto &v : a)
    {
      const QJsonObject o = v.toObject();
      cs.append({o.value(QStringLiteral("x")).toDouble(), o.value(QStringLiteral("y")).toDouble()});
    }
    return cs;
  }

  QJsonObject entityToJson(const CatalogEntity &e)
  {
    QJsonObject o;
    o.insert(QStringLiteral("id"), e.id);
    o.insert(QStringLiteral("entity_type"), e.entityType);
    o.insert(QStringLiteral("name"), e.name);
    o.insert(QStringLiteral("uwi"), e.uwi);
    o.insert(QStringLiteral("aliases"), QJsonArray::fromStringList(e.aliases));
    o.insert(QStringLiteral("surface_x"), e.surfaceX);
    o.insert(QStringLiteral("surface_y"), e.surfaceY);
    o.insert(QStringLiteral("has_surface"), e.hasSurface);
    o.insert(QStringLiteral("kb"), e.kb);
    o.insert(QStringLiteral("td"), e.td);
    o.insert(QStringLiteral("coordinate_status"), e.coordinateStatus);
    o.insert(QStringLiteral("inline_min"), e.inlineMin);
    o.insert(QStringLiteral("inline_max"), e.inlineMax);
    o.insert(QStringLiteral("xline_min"), e.xlineMin);
    o.insert(QStringLiteral("xline_max"), e.xlineMax);
    o.insert(QStringLiteral("sample_interval_us"), e.sampleIntervalUs);
    o.insert(QStringLiteral("start_time_ms"), e.startTimeMs);
    o.insert(QStringLiteral("corners"), cornersToJson(e.corners));
    if (!e.extra.isEmpty())
      o.insert(QStringLiteral("extra"), QJsonObject::fromVariantMap(e.extra));
    return o;
  }

  CatalogEntity entityFromJson(const QJsonObject &o)
  {
    CatalogEntity e;
    e.id = o.value(QStringLiteral("id")).toString();
    e.entityType = o.value(QStringLiteral("entity_type")).toString();
    e.name = o.value(QStringLiteral("name")).toString();
    e.uwi = o.value(QStringLiteral("uwi")).toString();
    for (const auto &v : o.value(QStringLiteral("aliases")).toArray())
      e.aliases.append(v.toString());
    e.surfaceX = o.value(QStringLiteral("surface_x")).toDouble();
    e.surfaceY = o.value(QStringLiteral("surface_y")).toDouble();
    e.hasSurface = o.value(QStringLiteral("has_surface")).toBool();
    e.kb = o.value(QStringLiteral("kb")).toDouble();
    e.td = o.value(QStringLiteral("td")).toDouble();
    e.coordinateStatus = o.value(QStringLiteral("coordinate_status")).toString();
    e.inlineMin = o.value(QStringLiteral("inline_min")).toDouble();
    e.inlineMax = o.value(QStringLiteral("inline_max")).toDouble();
    e.xlineMin = o.value(QStringLiteral("xline_min")).toDouble();
    e.xlineMax = o.value(QStringLiteral("xline_max")).toDouble();
    e.sampleIntervalUs = o.value(QStringLiteral("sample_interval_us")).toDouble();
    e.startTimeMs = o.value(QStringLiteral("start_time_ms")).toDouble();
    e.corners = cornersFromJson(o.value(QStringLiteral("corners")).toArray());
    e.extra = o.value(QStringLiteral("extra")).toObject().toVariantMap();
    return e;
  }

  QJsonObject linkToJson(const EntityAssetLink &l)
  {
    QJsonObject o;
    o.insert(QStringLiteral("entity_type"), l.entityType);
    o.insert(QStringLiteral("entity_id"), l.entityId);
    o.insert(QStringLiteral("asset_id"), l.assetId);
    o.insert(QStringLiteral("role"), l.role);
    o.insert(QStringLiteral("is_primary"), l.isPrimary);
    o.insert(QStringLiteral("unresolved"), l.unresolved);
    return o;
  }

  EntityAssetLink linkFromJson(const QJsonObject &o)
  {
    EntityAssetLink l;
    l.entityType = o.value(QStringLiteral("entity_type")).toString();
    l.entityId = o.value(QStringLiteral("entity_id")).toString();
    l.assetId = o.value(QStringLiteral("asset_id")).toString();
    l.role = o.value(QStringLiteral("role")).toString();
    l.isPrimary = o.value(QStringLiteral("is_primary")).toBool(true);
    l.unresolved = o.value(QStringLiteral("unresolved")).toBool(false);
    return l;
  }

  QJsonObject versionToJson(const CatalogVersion &v)
  {
    QJsonObject o;
    o.insert(QStringLiteral("id"), v.id);
    o.insert(QStringLiteral("asset_id"), v.assetId);
    o.insert(QStringLiteral("stage"), v.stage);
    o.insert(QStringLiteral("version_number"), v.versionNumber);
    o.insert(QStringLiteral("managed"), v.managed);
    o.insert(QStringLiteral("path"), v.path);
    o.insert(QStringLiteral("source_uri"), v.sourceUri);
    o.insert(QStringLiteral("sha256"), v.sha256);
    o.insert(QStringLiteral("file_name"), v.fileName);
    o.insert(QStringLiteral("parent_version_ids"), QJsonArray::fromStringList(v.parentVersionIds));
    if (!v.extra.isEmpty())
      o.insert(QStringLiteral("extra"), QJsonObject::fromVariantMap(v.extra));
    return o;
  }

  CatalogVersion versionFromJson(const QJsonObject &o)
  {
    CatalogVersion v;
    v.id = o.value(QStringLiteral("id")).toString();
    v.assetId = o.value(QStringLiteral("asset_id")).toString();
    v.stage = o.value(QStringLiteral("stage")).toString();
    v.versionNumber = o.value(QStringLiteral("version_number")).toInt(1);
    v.managed = o.value(QStringLiteral("managed")).toBool(true);
    v.path = o.value(QStringLiteral("path")).toString();
    v.sourceUri = o.value(QStringLiteral("source_uri")).toString();
    v.sha256 = o.value(QStringLiteral("sha256")).toString();
    v.fileName = o.value(QStringLiteral("file_name")).toString();
    for (const auto &p : o.value(QStringLiteral("parent_version_ids")).toArray())
      v.parentVersionIds.append(p.toString());
    v.extra = o.value(QStringLiteral("extra")).toObject().toVariantMap();
    return v;
  }
} // namespace

DataCatalog::DataCatalog(QObject *parent)
  : QObject(parent)
{
}

bool DataCatalog::open(const QString &projectDir, QString *error)
{
  m_dir = projectDir;
  m_revision = 0;
  m_entities.clear();
  m_assets.clear();
  m_versions.clear();
  m_links.clear();
  m_assetSeq = m_versionSeq = 0;

  QFile f(catalogPath());
  if (!f.exists())
    return save(error); // 初始化空 catalog（schema_version + revision 0）

  if (!f.open(QIODevice::ReadOnly))
  {
    setError(error, QStringLiteral("cannot open catalog %1").arg(catalogPath()));
    return false;
  }
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject())
  {
    setError(error, QStringLiteral("corrupt catalog %1: %2").arg(catalogPath(), pe.errorString()));
    return false;
  }
  const QJsonObject root = doc.object();
  m_revision = root.value(QStringLiteral("catalog_revision")).toInt();
  for (const auto &v : root.value(QStringLiteral("entities")).toArray())
    m_entities.append(entityFromJson(v.toObject()));
  for (const auto &v : root.value(QStringLiteral("assets")).toArray())
  {
    const CatalogAsset a{
        v.toObject().value(QStringLiteral("id")).toString(),
        v.toObject().value(QStringLiteral("type")).toString(),
        v.toObject().value(QStringLiteral("format")).toString(),
        v.toObject().value(QStringLiteral("display_name")).toString()};
    m_assets.append(a);
    bool ok = false;
    const int n = QString(a.id).mid(4).toInt(&ok); // "ast-N"
    if (ok)
      m_assetSeq = qMax(m_assetSeq, n);
  }
  for (const auto &v : root.value(QStringLiteral("versions")).toArray())
  {
    const CatalogVersion cv = versionFromJson(v.toObject());
    m_versions.append(cv);
    bool ok = false;
    const int n = QString(cv.id).mid(4).toInt(); // "ver-N"
    if (ok)
      m_versionSeq = qMax(m_versionSeq, n);
  }
  for (const auto &v : root.value(QStringLiteral("entity_asset_links")).toArray())
    m_links.append(linkFromJson(v.toObject()));
  return true;
}

bool DataCatalog::save(QString *error)
{
  const QDir dir = QFileInfo(catalogPath()).dir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
  {
    setError(error, QStringLiteral("cannot create catalog directory %1").arg(dir.absolutePath()));
    return false;
  }

  QJsonObject root;
  root.insert(QStringLiteral("schema_version"), kSchemaVersion);
  root.insert(QStringLiteral("catalog_revision"), ++m_revision);
  QJsonArray ents, asts, vers, lnks;
  for (const CatalogEntity &e : m_entities) ents.append(entityToJson(e));
  for (const CatalogAsset &a : m_assets)
  {
    QJsonObject o;
    o.insert(QStringLiteral("id"), a.id);
    o.insert(QStringLiteral("type"), a.type);
    o.insert(QStringLiteral("format"), a.format);
    o.insert(QStringLiteral("display_name"), a.displayName);
    asts.append(o);
  }
  for (const CatalogVersion &v : m_versions) vers.append(versionToJson(v));
  for (const EntityAssetLink &l : m_links) lnks.append(linkToJson(l));
  root.insert(QStringLiteral("entities"), ents);
  root.insert(QStringLiteral("assets"), asts);
  root.insert(QStringLiteral("versions"), vers);
  root.insert(QStringLiteral("entity_asset_links"), lnks);

  // 原子写：temp + rename（§41.2 惯例）。
  const QString tmp = catalogPath() + QStringLiteral(".partial");
  QFile f(tmp);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    setError(error, QStringLiteral("cannot write %1").arg(tmp));
    return false;
  }
  f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  f.close();
  if (::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(catalogPath()).constData()) != 0)
  {
    setError(error, QStringLiteral("cannot replace %1").arg(catalogPath()));
    return false;
  }
  emit changed();
  return true;
}

bool DataCatalog::addEntity(const CatalogEntity &e, QString *error)
{
  if (e.id.isEmpty() || hasEntity(e.id))
  {
    setError(error, QStringLiteral("entity id empty or duplicate: %1").arg(e.id));
    return false;
  }
  m_entities.append(e);
  return save(error);
}

bool DataCatalog::addAsset(const CatalogAsset &a, QString *error)
{
  if (a.id.isEmpty())
  {
    setError(error, QStringLiteral("asset id is empty"));
    return false;
  }
  m_assets.append(a);
  return save(error);
}

bool DataCatalog::addVersion(const CatalogVersion &v, QString *error)
{
  if (v.id.isEmpty() || v.assetId.isEmpty())
  {
    setError(error, QStringLiteral("version id or asset id is empty"));
    return false;
  }
  m_versions.append(v);
  return save(error);
}

bool DataCatalog::addLink(const EntityAssetLink &l, QString *error)
{
  if (l.entityId.isEmpty() || l.assetId.isEmpty())
  {
    setError(error, QStringLiteral("link needs entity and asset ids"));
    return false;
  }
  m_links.append(l);
  return save(error);
}

bool DataCatalog::hasEntity(const QString &id) const
{
  for (const CatalogEntity &e : m_entities)
    if (e.id == id)
      return true;
  return false;
}

QVector<CatalogEntity> DataCatalog::entities(const QString &entityType) const
{
  QVector<CatalogEntity> out;
  for (const CatalogEntity &e : m_entities)
    if (entityType.isEmpty() || e.entityType == entityType)
      out.append(e);
  return out;
}

CatalogEntity DataCatalog::entityById(const QString &id) const
{
  for (const CatalogEntity &e : m_entities)
    if (e.id == id)
      return e;
  return CatalogEntity();
}

QVector<CatalogAsset> DataCatalog::assets() const
{
  return m_assets;
}

CatalogAsset DataCatalog::assetById(const QString &id) const
{
  for (const CatalogAsset &a : m_assets)
    if (a.id == id)
      return a;
  return CatalogAsset();
}

QVector<CatalogVersion> DataCatalog::versionsForAsset(const QString &assetId) const
{
  QVector<CatalogVersion> out;
  for (const CatalogVersion &v : m_versions)
    if (v.assetId == assetId)
      out.append(v);
  return out;
}

CatalogVersion DataCatalog::versionById(const QString &id) const
{
  for (const CatalogVersion &v : m_versions)
    if (v.id == id)
      return v;
  return CatalogVersion();
}

CatalogVersion DataCatalog::currentVersion(const QString &assetId) const
{
  CatalogVersion best;
  for (const CatalogVersion &v : m_versions)
    if (v.assetId == assetId && v.versionNumber >= best.versionNumber)
      best = v;
  return best;
}

QVector<EntityAssetLink> DataCatalog::linksForEntity(const QString &entityId) const
{
  QVector<EntityAssetLink> out;
  for (const EntityAssetLink &l : m_links)
    if (l.entityId == entityId)
      out.append(l);
  return out;
}

QVector<EntityAssetLink> DataCatalog::linksForAsset(const QString &assetId) const
{
  QVector<EntityAssetLink> out;
  for (const EntityAssetLink &l : m_links)
    if (l.assetId == assetId)
      out.append(l);
  return out;
}

QVector<EntityAssetLink> DataCatalog::links() const
{
  return m_links;
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
  const QString needle = normalizeWellName(name);
  QStringList out;
  if (needle.isEmpty())
    return out;
  for (const CatalogEntity &e : m_entities)
  {
    if (e.entityType != QStringLiteral("well"))
      continue;
    if (normalizeWellName(e.name) == needle ||
        (!e.uwi.isEmpty() && e.uwi.compare(name, Qt::CaseInsensitive) == 0))
    {
      out.append(e.id);
      continue;
    }
    for (const QString &alias : e.aliases)
      if (normalizeWellName(alias) == needle)
      {
        out.append(e.id);
        break;
      }
  }
  return out;
}

QString DataCatalog::managedPath(const QString &stage, const QString &assetId,
                                 const QString &versionId, const QString &fileName)
{
  return QStringLiteral("%1/%2/%3/%4").arg(stage.toLower(), assetId, versionId, fileName);
}

QString DataCatalog::nextAssetId()
{
  return QStringLiteral("ast-%1").arg(++m_assetSeq);
}

QString DataCatalog::nextVersionId()
{
  return QStringLiteral("ver-%1").arg(++m_versionSeq);
}

QString DataCatalog::nextEntityId(const QString &prefix)
{
  int max = 0;
  for (const CatalogEntity &e : m_entities)
  {
    if (!e.id.startsWith(prefix + QLatin1Char('-')))
      continue;
    bool ok = false;
    const int n = e.id.mid(prefix.size() + 1).toInt(&ok);
    if (ok)
      max = qMax(max, n);
  }
  return QStringLiteral("%1-%2").arg(prefix).arg(max + 1);
}
