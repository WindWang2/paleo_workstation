// 层：数据
#include "releasestore.h"
#include "metastore.h"

#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

// Same lazy-connection pattern as layermanifest.cpp, under its own connection
// namespace so both stores coexist over the same sqlite file.
namespace
{
  QString connectionNameFor(const QString &path)
  {
    return QStringLiteral("paleo_releasestore_") + QString::number(qHash(path));
  }

  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  bool ensureOpen(const QString &path, QString *error)
  {
    const QString connName = connectionNameFor(path);
    QSqlDatabase db = QSqlDatabase::contains(connName)
                          ? QSqlDatabase::database(connName)
                          : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    if (!db.isValid())
    {
      setError(error, QStringLiteral("QSQLITE driver is not available"));
      return false;
    }
    if (!db.isOpen())
    {
      const QDir dir = QFileInfo(path).absoluteDir();
      if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
      {
        setError(error, QStringLiteral("cannot create directory for %1").arg(path));
        return false;
      }
      db.setDatabaseName(path);
      if (!db.open())
      {
        setError(error, db.lastError().text());
        return false;
      }
    }
    // 共享 schema 门（docs/SCHEMA_MIGRATION.md）：建表之前执行。每次调用
    // 都查（缓存连接拒开后不得绕过版本门建表——与 layermanifest 同修）。
    if (!MetaStore::ensureUserVersion(db, error))
      return false;
    QSqlQuery schema(db);
    if (!schema.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS releases("
            "release_id TEXT PRIMARY KEY,"
            "name TEXT,"
            "note TEXT,"
            "created_utc TEXT,"
            "manifest_json TEXT)")))
    {
      setError(error, schema.lastError().text());
      return false;
    }
    return true;
  }

  // Same field set as ManifestProjection's embed format — kept self-contained
  // here so the store doesn't depend on a live QgsProject.
  QJsonObject declToJson(const LayerDeclaration &d)
  {
    QJsonObject o;
    o.insert(QStringLiteral("layerId"), d.layerId);
    o.insert(QStringLiteral("horizon"), d.horizon);
    o.insert(QStringLiteral("type"), d.type);
    o.insert(QStringLiteral("source"), d.source);
    o.insert(QStringLiteral("styleRef"), d.styleRef);
    o.insert(QStringLiteral("group"), d.group);
    return o;
  }

  LayerDeclaration declFromJson(const QJsonObject &o)
  {
    LayerDeclaration d;
    d.layerId = o.value(QStringLiteral("layerId")).toString();
    d.horizon = o.value(QStringLiteral("horizon")).toString();
    d.type = o.value(QStringLiteral("type")).toString();
    d.source = o.value(QStringLiteral("source")).toString();
    d.styleRef = o.value(QStringLiteral("styleRef")).toString();
    d.group = o.value(QStringLiteral("group")).toString();
    d.instantiated = false;
    return d;
  }

  QString manifestKey(const LayerDeclaration &d)
  {
    return d.horizon + QLatin1Char('|') + d.type + QLatin1Char('|') + d.source +
           QLatin1Char('|') + d.styleRef + QLatin1Char('|') + d.group;
  }
} // namespace

ReleaseStore::ReleaseStore(const QString &metaSqlitePath)
  : m_dbPath(metaSqlitePath)
{
}

bool ReleaseStore::open(QString *error)
{
  return ensureOpen(m_dbPath, error);
}

QString ReleaseStore::createRelease(const QString &name, const QString &note,
                                    const QVector<LayerDeclaration> &decls,
                                    QString *error)
{
  if (!ensureOpen(m_dbPath, error))
    return QString();

  QSqlDatabase db = QSqlDatabase::database(connectionNameFor(m_dbPath));

  int maxSeq = 0;
  {
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("SELECT release_id FROM releases")))
      while (q.next())
      {
        const QString id = q.value(0).toString();
        if (id.startsWith(QStringLiteral("rel-")))
          maxSeq = std::max(maxSeq, id.mid(4).toInt());
      }
  }
  const QString id = QStringLiteral("rel-%1").arg(maxSeq + 1);

  QJsonArray arr;
  for (const LayerDeclaration &d : decls)
    arr.append(declToJson(d));
  const QString json = QString::fromUtf8(
      QJsonDocument(arr).toJson(QJsonDocument::Compact));

  QSqlQuery ins(db);
  ins.prepare(QStringLiteral("INSERT INTO releases(release_id,name,note,created_utc,manifest_json)"
                             " VALUES(?,?,?,?,?)"));
  ins.addBindValue(id);
  ins.addBindValue(name);
  ins.addBindValue(note);
  ins.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  ins.addBindValue(json);
  if (!ins.exec())
  {
    setError(error, ins.lastError().text());
    return QString();
  }
  return id;
}

QVector<ReleaseInfo> ReleaseStore::releases() const
{
  QVector<ReleaseInfo> out;
  if (!ensureOpen(m_dbPath, nullptr))
    return out;
  QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
  if (!q.exec(QStringLiteral(
          "SELECT release_id,name,note,created_utc,manifest_json FROM releases "
          "ORDER BY CAST(substr(release_id,5) AS INTEGER)")))
    return out;
  while (q.next())
  {
    ReleaseInfo r;
    r.id = q.value(0).toString();
    r.name = q.value(1).toString();
    r.note = q.value(2).toString();
    r.createdUtc = q.value(3).toString();
    r.layerCount = QJsonDocument::fromJson(q.value(4).toString().toUtf8()).array().size();
    out.append(r);
  }
  return out;
}

QVector<LayerDeclaration> ReleaseStore::manifestAt(const QString &releaseId) const
{
  QVector<LayerDeclaration> out;
  if (!ensureOpen(m_dbPath, nullptr))
    return out;
  QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
  q.prepare(QStringLiteral("SELECT manifest_json FROM releases WHERE release_id=?"));
  q.addBindValue(releaseId);
  if (!q.exec() || !q.next())
    return out;
  const QJsonDocument doc = QJsonDocument::fromJson(q.value(0).toString().toUtf8());
  if (!doc.isArray())
    return out;
  for (const QJsonValue &v : doc.array())
    if (v.isObject())
      out.append(declFromJson(v.toObject()));
  return out;
}

bool ReleaseStore::diff(const QString &idA, const QString &idB,
                        QStringList *added, QStringList *removed, QStringList *changed) const
{
  if (!ensureOpen(m_dbPath, nullptr))
    return false;

  // Both releases must actually exist — an empty manifest is a valid snapshot,
  // so existence is checked against the table, not the payload size.
  {
    QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
    q.prepare(QStringLiteral("SELECT release_id FROM releases WHERE release_id IN (?,?)"));
    q.addBindValue(idA);
    q.addBindValue(idB);
    int found = 0;
    if (q.exec())
      while (q.next())
        ++found;
    if (found < 2)
      return false;
  }

  const QVector<LayerDeclaration> a = manifestAt(idA);
  const QVector<LayerDeclaration> b = manifestAt(idB);

  QSet<QString> idsA, idsB;
  QHash<QString, QString> keyA;
  for (const LayerDeclaration &d : a)
  {
    idsA.insert(d.layerId);
    keyA.insert(d.layerId, manifestKey(d));
  }
  for (const LayerDeclaration &d : b)
    idsB.insert(d.layerId);

  if (added)
    for (const LayerDeclaration &d : b)
      if (!idsA.contains(d.layerId))
        added->append(d.layerId);
  if (removed)
    for (const LayerDeclaration &d : a)
      if (!idsB.contains(d.layerId))
        removed->append(d.layerId);
  if (changed)
    for (const LayerDeclaration &d : b)
      if (idsA.contains(d.layerId) && keyA.value(d.layerId) != manifestKey(d))
        changed->append(d.layerId);
  return true;
}
