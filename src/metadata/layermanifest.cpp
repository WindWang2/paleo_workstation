// 层：数据
#include "layermanifest.h"
#include "metastore.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace
{
  // The header fixes the member set to just m_dbPath, so connections are held by
  // QtSql's connection registry: one named QSQLITE connection per sqlite file.
  // Repeated LayerManifest instances over the same path share the connection,
  // which is what makes reopen/roundtrip semantics work.
  QString connectionNameFor(const QString &path)
  {
    return QStringLiteral("paleo_layermanifest_") + QString::number(qHash(path));
  }

  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  const QString kColumns = QStringLiteral("layer_id,horizon,type,source,style_ref,grp,title");

  // Lazily opens the connection and guarantees the schema. Lets every public
  // method work even if the caller skipped open().
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
    // 共享 schema 门（docs/SCHEMA_MIGRATION.md）：新库/遗留库采纳当前
    // user_version，未来版本拒开——在建任何表之前执行。T7 矩阵暴露的洞：
    // 检查原先只在首次 open 时跑，拒开后连接留在注册表里处于 open 态，
    // 之后的读调用（all()/latest()...）经缓存的 open 连接绕过版本门直接
    // 建表。移到连接确保之后每次执行（一次 PRAGMA，幂等便宜）。
    if ( !MetaStore::ensureUserVersion(db, error ) )
      return false;

    QSqlQuery schema(db);
    if (!schema.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS layer_declarations("
                                    "layer_id TEXT PRIMARY KEY,"
                                    "horizon TEXT,"
                                    "type TEXT,"
                                    "source TEXT,"
                                    "style_ref TEXT,"
                                    "grp TEXT,"
                                    "title TEXT)")))
    {
      setError(error, schema.lastError().text());
      return false;
    }
    // 老库没有 title 列——按需补列，旧行 title 为 NULL（显示退回 layerId）。
    QSqlQuery cols(db);
    if (cols.exec(QStringLiteral("PRAGMA table_info(layer_declarations)")))
    {
      bool hasTitle = false;
      while (cols.next())
        if (cols.value(1).toString() == QLatin1String("title"))
          hasTitle = true;
      if (!hasTitle &&
          !schema.exec(QStringLiteral("ALTER TABLE layer_declarations ADD COLUMN title TEXT")))
      {
        setError(error, schema.lastError().text());
        return false;
      }
    }
    return true;
  }

  LayerDeclaration rowToDecl(const QSqlQuery &q)
  {
    LayerDeclaration d;
    d.layerId = q.value(0).toString();
    d.horizon = q.value(1).toString();
    d.type = q.value(2).toString();
    d.source = q.value(3).toString();
    d.styleRef = q.value(4).toString();
    d.group = q.value(5).toString();
    d.title = q.value(6).toString();
    d.instantiated = false; // runtime-only flag — never read back as true
    return d;
  }
} // namespace

LayerManifest::LayerManifest(const QString &metaSqlitePath)
  : m_dbPath(metaSqlitePath)
{
}

bool LayerManifest::open(QString *error)
{
  return ensureOpen(m_dbPath, error);
}

bool LayerManifest::upsert(const LayerDeclaration &decl, QString *error)
{
  if (m_readOnly)
  {
    setError(error, QStringLiteral("工程目录被另一个实例锁定——本实例只读，图层清单写入被拒绝"));
    return false;
  }
  if (decl.layerId.isEmpty())
  {
    setError(error, QStringLiteral("layer declaration requires a non-empty layerId"));
    return false;
  }
  if (!ensureOpen(m_dbPath, error))
    return false;

    // 'instantiated' is intentionally not written: schema has no column for it.
  QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
  q.prepare(QStringLiteral("INSERT OR REPLACE INTO layer_declarations(") + kColumns +
            QStringLiteral(") VALUES(?,?,?,?,?,?,?)"));
  q.addBindValue(decl.layerId);
  q.addBindValue(decl.horizon);
  q.addBindValue(decl.type);
  q.addBindValue(decl.source);
  q.addBindValue(decl.styleRef);
  q.addBindValue(decl.group);
  q.addBindValue(decl.title);
  if (!q.exec())
  {
    setError(error, q.lastError().text());
    return false;
  }
  return true;
}

bool LayerManifest::remove(const QString &layerId, QString *error)
{
  if (m_readOnly)
  {
    setError(error, QStringLiteral("工程目录被另一个实例锁定——本实例只读，图层清单写入被拒绝"));
    return false;
  }
  if (!ensureOpen(m_dbPath, error))
    return false;

  QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
  q.prepare(QStringLiteral("DELETE FROM layer_declarations WHERE layer_id=?"));
  q.addBindValue(layerId);
  if (!q.exec())
  {
    setError(error, q.lastError().text());
    return false;
  }
  return true;
}

bool LayerManifest::readAll(QVector<LayerDeclaration> *out, QString *error) const
{
  if (!out)
  {
    setError(error, QStringLiteral("readAll requires an output vector"));
    return false;
  }
  out->clear();
  if (!ensureOpen(m_dbPath, error))
    return false;

  QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
  if (!q.exec(QStringLiteral("SELECT ") + kColumns +
              QStringLiteral(" FROM layer_declarations ORDER BY layer_id")))
  {
    setError(error, q.lastError().text());
    return false;
  }
  while (q.next())
    out->append(rowToDecl(q));
  return true;
}

QVector<LayerDeclaration> LayerManifest::all() const
{
  QVector<LayerDeclaration> out;
  readAll(&out, nullptr);
  return out;
}

QVector<LayerDeclaration> LayerManifest::forHorizon(const QString &h) const
{
  QVector<LayerDeclaration> out;
  if (!ensureOpen(m_dbPath, nullptr))
    return out;

  // Horizon-agnostic declarations (horizon='') belong to every horizon view.
  QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
  q.prepare(QStringLiteral("SELECT ") + kColumns +
            QStringLiteral(" FROM layer_declarations WHERE horizon=? OR horizon='' ORDER BY layer_id"));
  q.addBindValue(h);
  if (!q.exec())
    return out;
  while (q.next())
    out.append(rowToDecl(q));
  return out;
}

QStringList LayerManifest::horizons() const
{
  QStringList out;
  if (!ensureOpen(m_dbPath, nullptr))
    return out;

  QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
  if (!q.exec(QStringLiteral("SELECT DISTINCT horizon FROM layer_declarations "
                             "WHERE horizon<>'' ORDER BY horizon")))
    return out;
  while (q.next())
    out.append(q.value(0).toString());
  return out;
}
