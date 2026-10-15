// 层：数据
#include "releasestore.h"
#include "storeerrors_internal.h"
#include "metastore.h"

#include <QDateTime>
#include <QHash>
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
using paleo::store_detail::setError;

  QString releaseStoreConnectionName(const QString &path)
  {
    return QStringLiteral("paleo_releasestore_") + QString::number(qHash(path));
  }

  bool releaseEnsureOpen(const QString &path, QString *error)
  {
    QSqlDatabase db = MetaStore::openConnection(path, releaseStoreConnectionName(path), error);
    if (!db.isValid())
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
    if (!d.title.isEmpty())
      o.insert(QStringLiteral("title"), d.title);
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
    d.title = o.value(QStringLiteral("title")).toString();
    d.instantiated = false;
    return d;
  }

  QString manifestKey(const LayerDeclaration &d)
  {
    return d.horizon + QLatin1Char('|') + d.type + QLatin1Char('|') + d.source +
           QLatin1Char('|') + d.styleRef + QLatin1Char('|') + d.group +
           QLatin1Char('|') + d.title;
  }
} // namespace

ReleaseStore::ReleaseStore(const QString &metaSqlitePath)
  : m_dbPath(metaSqlitePath)
{
}

bool ReleaseStore::open(QString *error)
{
  return releaseEnsureOpen(m_dbPath, error);
}

QString ReleaseStore::createRelease(const QString &name, const QString &note,
                                    const QVector<LayerDeclaration> &decls,
                                    QString *error)
{
  if (!releaseEnsureOpen(m_dbPath, error))
    return QString();

  QSqlDatabase db = QSqlDatabase::database(releaseStoreConnectionName(m_dbPath));

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
  if (!releaseEnsureOpen(m_dbPath, nullptr))
    return out;
  QSqlQuery q(QSqlDatabase::database(releaseStoreConnectionName(m_dbPath)));
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
    // 损坏的 manifest_json 如实标 manifestCorrupt——array().size()==0 会把
    // 损坏谎报成「零图层快照」（与 diff() 的损坏门禁同口径）。
    const QJsonDocument manifestDoc =
        QJsonDocument::fromJson(q.value(4).toString().toUtf8());
    r.manifestCorrupt = !manifestDoc.isArray();
    r.layerCount = manifestDoc.isArray() ? manifestDoc.array().size() : 0;
    out.append(r);
  }
  return out;
}

QVector<LayerDeclaration> ReleaseStore::manifestAt(const QString &releaseId) const
{
  QVector<LayerDeclaration> out;
  if (!releaseEnsureOpen(m_dbPath, nullptr))
    return out;
  QSqlQuery q(QSqlDatabase::database(releaseStoreConnectionName(m_dbPath)));
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
                        QStringList *added, QStringList *removed, QStringList *changed,
                        QString *error) const
{
  if (error)
    error->clear(); // 成功路径不带细节（与 batchjobqueue readState 同惯例）
  QString openErr;
  if (!releaseEnsureOpen(m_dbPath, &openErr))
  {
    if (error)
      *error = QStringLiteral("发布库打开失败：%1").arg(openErr);
    return false;
  }

  // Both releases must actually exist — an empty manifest is a valid snapshot,
  // so existence is checked against the table, not the payload size.
  QHash<QString, QString> payloadOf;
  {
    QSqlQuery q(QSqlDatabase::database(releaseStoreConnectionName(m_dbPath)));
    q.prepare(QStringLiteral("SELECT release_id, manifest_json FROM releases WHERE release_id IN (?,?)"));
    q.addBindValue(idA);
    q.addBindValue(idB);
    int found = 0;
    bool seenA = false, seenB = false;
    if (!q.exec())
    {
      if (error)
        *error = QStringLiteral("存在性查询失败：%1").arg(q.lastError().text());
      return false;
    }
    while (q.next())
    {
      ++found;
      if (q.value(0).toString() == idA)
        seenA = true;
      if (q.value(0).toString() == idB)
        seenB = true;
      payloadOf.insert(q.value(0).toString(), q.value(1).toString());
    }
    if (found < 2)
    {
      if (error)
      {
        // idA==idB 且存在时 IN(?,?) 只命中一行——missing 为空是「两侧同版本」，
        // 不是「不存在」，分开说；同一 id 只点一次名。
        QStringList missing;
        const auto appendOnce = [&missing](const QString &id) {
          if (!missing.contains(id))
            missing << id;
        };
        if (!seenA)
          appendOnce(idA);
        if (!seenB)
          appendOnce(idB);
        *error = missing.isEmpty()
                     ? QStringLiteral("两侧是同一个版本：%1").arg(idA)
                     : QStringLiteral("release 不存在：%1")
                           .arg(missing.join(QStringLiteral(", ")));
      }
      return false;
    }
  }

  // 清单载荷必须可解析为 JSON 数组——manifestAt 对损坏载荷静默返回空集，
  // 这里若不拦，diff 会把「快照损坏」谎报成「对方全部图层为新增」。空数组
  // 「[]」是合法快照（零图层发布），不是损坏。
  for (const QString &id : {idA, idB})
  {
    const QJsonDocument doc =
        QJsonDocument::fromJson(payloadOf.value(id).toUtf8());
    if (!doc.isArray())
    {
      if (error)
        *error = QStringLiteral("release %1 的清单数据损坏或不可解析").arg(id);
      return false;
    }
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
