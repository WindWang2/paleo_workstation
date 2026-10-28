// 层：数据
#include "layermanifest.h"
#include "storeerrors_internal.h"
#include "metastore.h"
#include "paleoprojectstore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QSet>
#include <QVariant>

#include <atomic>
#include <utility>

namespace
{
using paleo::store_detail::setError;

  // The header fixes the member set to just m_dbPath, so connections are held by
  // QtSql's connection registry: one named QSQLITE connection per sqlite file.
  // Repeated LayerManifest instances over the same path share the connection,
  // which is what makes reopen/roundtrip semantics work.
  // readOnly 实例走独立连接名（QSQLITE_OPEN_READONLY，#80）——同进程里可写
  // 与只读实例不得复用同一条连接。
  // review M4：连接名不再取 QThread* 地址——池线程过期后地址会被新线程复用，
  // contains() 为真而 database() 属于已死线程（返回无效 → 误报 driver 不可用）。
  // 改用进程内单调递增、永不复用的线程序号；非主线程退出时（thread_local
  // 析构）移除本线程建立的命名连接，不再随池线程过期泄漏。
  std::atomic<quint64> s_threadSerial{0};

  struct ThreadConnections
  {
    quint64 serial = ++s_threadSerial;
    bool mainThread = QCoreApplication::instance() &&
                      QThread::currentThread() == QCoreApplication::instance()->thread();
    QSet<QString> names;
    ~ThreadConnections()
    {
      // 主线程连接由工程关闭路径 closeConnectionsFor 收口；进程退出期不碰 QtSql。
      if (mainThread)
        return;
      for (const QString &name : std::as_const(names))
        QSqlDatabase::removeDatabase(name);
    }
  };

  ThreadConnections &threadConnections()
  {
    thread_local ThreadConnections tc;
    return tc;
  }

  QString manifestConnectionName(const QString &path, bool readOnly)
  {
    ThreadConnections &tc = threadConnections();
    const QString name = QStringLiteral("paleo_layermanifest_") + QString::number(qHash(path)) +
                         QStringLiteral("_t") + QString::number(tc.serial) +
                         (readOnly ? QStringLiteral("_ro") : QString());
    tc.names.insert(name);
    return name;
  }

  const QString kColumns = QStringLiteral("layer_id,horizon,type,source,style_ref,grp,title");

  // 幂等建表 + 按需补列（老库没有 title 列——旧行 title 为 NULL，显示退回
  // layerId）。#736：本函数即 MetaStore 的迁移步（幂等，可按步重复调用），
  // 由 openConnection 在版本落后时于事务内逐级执行、版本已到位时作 ensure。
  bool manifestEnsureSchema(QSqlDatabase &db, QString *error)
  {
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

  // Lazily opens the connection and guarantees the schema. Lets every public
  // method work even if the caller skipped open().
  bool manifestEnsureOpen(const QString &path, bool readOnly, QString *error)
  {
    QSqlDatabase db = MetaStore::openConnection(
        path, manifestConnectionName(path, readOnly), error, readOnly,
        MetaStore::kUserVersion,
        [](QSqlDatabase &db, int, int, QString *err) { return manifestEnsureSchema(db, err); });
    if (!db.isValid())
      return false;
    // 只读实例不建表/不补列（#80）——schema 归写实例；可写路径的 schema 已在
    // openConnection 内确保（版本落后时在事务内逐级迁移，#736）。
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
  return manifestEnsureOpen(m_dbPath, m_readOnly, error);
}

bool LayerManifest::upsert(const LayerDeclaration &decl, QString *error)
{
  if (m_readOnly)
  {
    setError(error, QStringLiteral("工程目录被另一个实例锁定——本实例只读，图层清单写入被拒绝"));
    return false;
  }
  const bool isMainThread = QCoreApplication::instance()
                                ? (QThread::currentThread() == QCoreApplication::instance()->thread())
                                : true;
  if (m_writeQueueEnforced && !isMainThread && !PaleoProjectStore::isWriteQueueActive())
  {
    setError(error, QStringLiteral("LayerManifest 写入必须通过主线程或 PaleoProjectStore::enqueueWrite 调度（requested database does not belong to the calling thread）"));
    return false;
  }
  if (decl.layerId.isEmpty())
  {
    setError(error, QStringLiteral("layer declaration requires a non-empty layerId"));
    return false;
  }
  if (!manifestEnsureOpen(m_dbPath, m_readOnly, error))
    return false;

    // 'instantiated' is intentionally not written: schema has no column for it.
  QSqlQuery q(QSqlDatabase::database(manifestConnectionName(m_dbPath, m_readOnly)));
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
  return removeMany({layerId}, error);
}

bool LayerManifest::removeMany(const QStringList &layerIds, QString *error)
{
  if (m_readOnly)
  {
    setError(error, QStringLiteral("工程目录被另一个实例锁定——本实例只读，图层清单写入被拒绝"));
    return false;
  }
  const bool isMainThread = QCoreApplication::instance()
                                ? (QThread::currentThread() == QCoreApplication::instance()->thread())
                                : true;
  if (m_writeQueueEnforced && !isMainThread && !PaleoProjectStore::isWriteQueueActive())
  {
    setError(error, QStringLiteral("LayerManifest 写入必须通过主线程或 PaleoProjectStore::enqueueWrite 调度（requested database does not belong to the calling thread）"));
    return false;
  }
  if (!manifestEnsureOpen(m_dbPath, m_readOnly, error))
    return false;

  auto db = QSqlDatabase::database(manifestConnectionName(m_dbPath, m_readOnly));
  if (!db.transaction()) { setError(error, db.lastError().text()); return false; }
  QSqlQuery q(db);
  q.prepare(QStringLiteral("DELETE FROM layer_declarations WHERE layer_id=?"));
  for (const auto &layerId : layerIds) {
    q.bindValue(0, layerId);
    if (!q.exec()) {
      setError(error, q.lastError().text());
      db.rollback();
      return false;
    }
  }
  if (!db.commit()) { setError(error, db.lastError().text()); db.rollback(); return false; }
  return true;
}

bool LayerManifest::find(const QString &layerId, LayerDeclaration *out, QString *error) const
{
  if (!out) { setError(error, QStringLiteral("find requires an output declaration")); return false; }
  *out = {};
  if (!manifestEnsureOpen(m_dbPath, m_readOnly, error)) return false;
  QSqlQuery q(QSqlDatabase::database(manifestConnectionName(m_dbPath, m_readOnly)));
  q.prepare(QStringLiteral("SELECT ") + kColumns + QStringLiteral(" FROM layer_declarations WHERE layer_id=?"));
  q.addBindValue(layerId);
  if (!q.exec()) { setError(error, q.lastError().text()); return false; }
  if (q.next()) *out = rowToDecl(q);
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
  if (!manifestEnsureOpen(m_dbPath, m_readOnly, error))
    return false;

  QSqlQuery q(QSqlDatabase::database(manifestConnectionName(m_dbPath, m_readOnly)));
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
  if (!manifestEnsureOpen(m_dbPath, m_readOnly, nullptr))
    return out;

  // Horizon-agnostic declarations (horizon='') belong to every horizon view.
  QSqlQuery q(QSqlDatabase::database(manifestConnectionName(m_dbPath, m_readOnly)));
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
  if (!manifestEnsureOpen(m_dbPath, m_readOnly, nullptr))
    return out;

  QSqlQuery q(QSqlDatabase::database(manifestConnectionName(m_dbPath, m_readOnly)));
  if (!q.exec(QStringLiteral("SELECT DISTINCT horizon FROM layer_declarations "
                             "WHERE horizon<>'' ORDER BY horizon")))
    return out;
  while (q.next())
    out.append(q.value(0).toString());
  return out;
}
