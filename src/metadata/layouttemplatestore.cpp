// 层：数据
#include "layouttemplatestore.h"
#include "storeerrors_internal.h"
#include "metastore.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

// Same lazy-connection pattern as releasestore.cpp, under its own connection
// namespace so the project.sqlite stores coexist over the same sqlite file.
namespace
{
using paleo::store_detail::setError;

  QString layoutTemplateConnectionName(const QString &path)
  {
    return QStringLiteral("paleo_layouttemplatestore_") + QString::number(qHash(path));
  }

  bool layoutTemplateEnsureOpen(const QString &path, QString *error)
  {
    QSqlDatabase db = MetaStore::openConnection(path, layoutTemplateConnectionName(path), error);
    if (!db.isValid())
      return false;

    QSqlQuery schema(db);
    if (!schema.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS layout_templates("
            "template_id TEXT PRIMARY KEY,"
            "name TEXT UNIQUE NOT NULL,"
            "kind TEXT,"
            "page_size TEXT,"
            "landscape INTEGER,"
            "asset_id TEXT,"
            "version_id TEXT,"
            "sha256 TEXT,"
            "created_utc TEXT,"
            "updated_utc TEXT)")))
    {
      setError(error, schema.lastError().text());
      return false;
    }
    return true;
  }

  LayoutTemplateInfo rowToInfo(const QSqlQuery &q)
  {
    LayoutTemplateInfo info;
    info.id = q.value(0).toString();
    info.name = q.value(1).toString();
    info.kind = q.value(2).toString();
    info.pageSize = q.value(3).toString();
    info.landscape = q.value(4).toInt() != 0;
    info.assetId = q.value(5).toString();
    info.versionId = q.value(6).toString();
    info.sha256 = q.value(7).toString();
    info.createdUtc = q.value(8).toString();
    info.updatedUtc = q.value(9).toString();
    return info;
  }
} // namespace

LayoutTemplateStore::LayoutTemplateStore(const QString &metaSqlitePath)
  : m_dbPath(metaSqlitePath)
{
}

bool LayoutTemplateStore::open(QString *error)
{
  return layoutTemplateEnsureOpen(m_dbPath, error);
}

QString LayoutTemplateStore::create(const QString &name, const QString &kind,
                                    const QString &pageSize, bool landscape,
                                    const QString &assetId, const QString &versionId,
                                    const QString &sha256, QString *error)
{
  if (name.isEmpty())
  {
    setError(error, QStringLiteral("template name is empty"));
    return QString();
  }
  if (!layoutTemplateEnsureOpen(m_dbPath, error))
    return QString();
  if (nameExists(name))
  {
    setError(error, QStringLiteral("template name already exists: %1").arg(name));
    return QString();
  }

  QSqlDatabase db = QSqlDatabase::database(layoutTemplateConnectionName(m_dbPath));

  int maxSeq = 0;
  {
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("SELECT template_id FROM layout_templates")))
      while (q.next())
      {
        const QString id = q.value(0).toString();
        if (id.startsWith(QStringLiteral("lt-")))
          maxSeq = std::max(maxSeq, id.mid(3).toInt());
      }
  }
  const QString id = QStringLiteral("lt-%1").arg(maxSeq + 1);
  const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

  QSqlQuery ins(db);
  ins.prepare(QStringLiteral(
      "INSERT INTO layout_templates(template_id,name,kind,page_size,landscape,"
      "asset_id,version_id,sha256,created_utc,updated_utc) VALUES(?,?,?,?,?,?,?,?,?,?)"));
  ins.addBindValue(id);
  ins.addBindValue(name);
  ins.addBindValue(kind);
  ins.addBindValue(pageSize);
  ins.addBindValue(landscape ? 1 : 0);
  ins.addBindValue(assetId);
  ins.addBindValue(versionId);
  ins.addBindValue(sha256);
  ins.addBindValue(now);
  ins.addBindValue(now);
  if (!ins.exec())
  {
    setError(error, ins.lastError().text());
    return QString();
  }
  return id;
}

bool LayoutTemplateStore::updateContent(const QString &id, const QString &versionId,
                                        const QString &sha256, QString *error)
{
  if (!layoutTemplateEnsureOpen(m_dbPath, error))
    return false;
  QSqlQuery q(QSqlDatabase::database(layoutTemplateConnectionName(m_dbPath)));
  q.prepare(QStringLiteral(
      "UPDATE layout_templates SET version_id=?,sha256=?,updated_utc=? WHERE template_id=?"));
  q.addBindValue(versionId);
  q.addBindValue(sha256);
  q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  q.addBindValue(id);
  if (!q.exec() || q.numRowsAffected() != 1)
  {
    setError(error, q.lastError().isValid() ? q.lastError().text()
                                            : QStringLiteral("unknown template id: %1").arg(id));
    return false;
  }
  return true;
}

bool LayoutTemplateStore::rename(const QString &id, const QString &newName, QString *error)
{
  if (newName.isEmpty())
  {
    setError(error, QStringLiteral("template name is empty"));
    return false;
  }
  if (!layoutTemplateEnsureOpen(m_dbPath, error))
    return false;
  if (nameExists(newName))
  {
    setError(error, QStringLiteral("template name already exists: %1").arg(newName));
    return false;
  }
  QSqlQuery q(QSqlDatabase::database(layoutTemplateConnectionName(m_dbPath)));
  q.prepare(QStringLiteral(
      "UPDATE layout_templates SET name=?,updated_utc=? WHERE template_id=?"));
  q.addBindValue(newName);
  q.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  q.addBindValue(id);
  if (!q.exec() || q.numRowsAffected() != 1)
  {
    setError(error, q.lastError().isValid() ? q.lastError().text()
                                            : QStringLiteral("unknown template id: %1").arg(id));
    return false;
  }
  return true;
}

bool LayoutTemplateStore::remove(const QString &id, QString *error)
{
  if (!layoutTemplateEnsureOpen(m_dbPath, error))
    return false;
  QSqlQuery q(QSqlDatabase::database(layoutTemplateConnectionName(m_dbPath)));
  q.prepare(QStringLiteral("DELETE FROM layout_templates WHERE template_id=?"));
  q.addBindValue(id);
  if (!q.exec() || q.numRowsAffected() != 1)
  {
    setError(error, q.lastError().isValid() ? q.lastError().text()
                                            : QStringLiteral("unknown template id: %1").arg(id));
    return false;
  }
  return true;
}

QVector<LayoutTemplateInfo> LayoutTemplateStore::templates() const
{
  QVector<LayoutTemplateInfo> out;
  if (!layoutTemplateEnsureOpen(m_dbPath, nullptr))
    return out;
  QSqlQuery q(QSqlDatabase::database(layoutTemplateConnectionName(m_dbPath)));
  if (!q.exec(QStringLiteral(
          "SELECT template_id,name,kind,page_size,landscape,asset_id,version_id,sha256,"
          "created_utc,updated_utc FROM layout_templates ORDER BY name COLLATE NOCASE")))
    return out;
  while (q.next())
    out.append(rowToInfo(q));
  return out;
}

LayoutTemplateInfo LayoutTemplateStore::byName(const QString &name) const
{
  if (!layoutTemplateEnsureOpen(m_dbPath, nullptr))
    return LayoutTemplateInfo{};
  QSqlQuery q(QSqlDatabase::database(layoutTemplateConnectionName(m_dbPath)));
  q.prepare(QStringLiteral(
      "SELECT template_id,name,kind,page_size,landscape,asset_id,version_id,sha256,"
      "created_utc,updated_utc FROM layout_templates WHERE name=?"));
  q.addBindValue(name);
  if (!q.exec() || !q.next())
    return LayoutTemplateInfo{};
  return rowToInfo(q);
}

LayoutTemplateInfo LayoutTemplateStore::byId(const QString &id) const
{
  if (!layoutTemplateEnsureOpen(m_dbPath, nullptr))
    return LayoutTemplateInfo{};
  QSqlQuery q(QSqlDatabase::database(layoutTemplateConnectionName(m_dbPath)));
  q.prepare(QStringLiteral(
      "SELECT template_id,name,kind,page_size,landscape,asset_id,version_id,sha256,"
      "created_utc,updated_utc FROM layout_templates WHERE template_id=?"));
  q.addBindValue(id);
  if (!q.exec() || !q.next())
    return LayoutTemplateInfo{};
  return rowToInfo(q);
}

bool LayoutTemplateStore::nameExists(const QString &name) const
{
  return !byName(name).isNull();
}
