// 层：数据
#include "catalogstore.h"
#include "catalogstore_internal.h"

#include "../metadata/metastore.h"

#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>

namespace
{
using paleo::store_detail::setError;
using paleo::catalog_detail::applyWritablePragmas;
using paleo::catalog_detail::catalogConnectionName;
using paleo::catalog_detail::execSql;
using paleo::catalog_detail::execScript;
using paleo::catalog_detail::ensureVersionNumberUniqueIndex;
using paleo::catalog_detail::forgetConnection;
using paleo::catalog_detail::kCatalogUserVersion;
using paleo::catalog_detail::kSchemaSql;
} // namespace

// 连接/schema 域（方向 99 拆分）：连接生命周期（close/resumeConnection/
// attachWritable/connectPrimary）、DDL 装载与可写 pragma、事务三件套
// （BEGIN IMMEDIATE / COMMIT / ROLLBACK——事务边界语义逐条保留）。

void CatalogStore::close()
{
  if (m_open && m_writable && !m_connectionName.isEmpty())
  {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (db.isValid() && db.isOpen())
    {
      QSqlQuery q(db);
      q.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    }
  }
  m_inTxn = false;
  m_open = false;
  m_writable = false;
  if (m_connectionName.isEmpty())
    return;
  {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (db.isValid())
      db.close();
  }
  if (QSqlDatabase::contains(m_connectionName))
    QSqlDatabase::removeDatabase(m_connectionName);
  m_connectionName.clear();
}

bool CatalogStore::resumeConnection(bool readOnly, int expectedRevision, QString *error)
{
  // Only reconnect. openProject already did migration/integrity/backup work.
  if (readOnly && !QFileInfo::exists(m_sqlitePath)) return true;
  if (!connectPrimary(readOnly, error)) return false;
  m_open = true;
  m_writable = !readOnly;
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  QSqlQuery revision(db);
  if (!revision.exec(QStringLiteral("SELECT value FROM catalog_meta WHERE key='catalog_revision'")) ||
      !revision.next() || revision.value(0).toInt() != expectedRevision) {
    setError(error, QStringLiteral("目录库在后台读取期间已变化，请重新打开工程"));
    return false;
  }
  return readOnly || applyWritablePragmas(db, error);
}

bool CatalogStore::attachWritable(QString *error)
{
  if (m_sqlitePath.isEmpty())
  {
    setError(error, QStringLiteral("catalog sqlite path is empty"));
    return false;
  }
  m_connectionName = catalogConnectionName(this, m_sqlitePath, false);
  bool opened = false;
  {
    QSqlDatabase db = MetaStore::openConnection(m_sqlitePath, m_connectionName, error, false,
                                                kCatalogUserVersion);
    opened = db.isValid() && db.isOpen();
    if (opened && !applyWritablePragmas(db, error))
      opened = false;
    else if (opened && !execScript(db, kSchemaSql, error))
      opened = false;
    else if (opened)
      ensureVersionNumberUniqueIndex(db); // #476：宽容唯一索引（存量重复行仅告警）
  }
  if (!opened)
  {
    forgetConnection(m_connectionName);
    m_connectionName.clear();
    m_open = false;
    m_writable = false;
    return false;
  }
  m_open = true;
  m_writable = true;
  return true;
}

namespace
{
  // PW-077：SQLite 文件头偏移 60 处 4 字节大端 user_version。打开失败后直接
  // 读头判定「未来 schema」，不依赖 MetaStore 的错误文案。
  // 返回 -1 = 不是可识别的 SQLite 头。
  int probeSqliteUserVersion(const QString &path)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return -1;
    const QByteArray h = f.read(100);
    if (h.size() < 100 || !h.startsWith(QByteArrayLiteral("SQLite format 3")))
      return -1;
    const auto *p = reinterpret_cast<const uchar *>(h.constData()) + 60;
    return int((quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) |
               quint32(p[3]));
  }
} // namespace

bool CatalogStore::connectPrimary(bool readOnly, QString *error, bool *schemaRejected)
{
  if (schemaRejected)
    *schemaRejected = false;
  m_connectionName = catalogConnectionName(this, m_sqlitePath, readOnly);
  QString local;
  bool opened = false;
  {
    QSqlDatabase db = MetaStore::openConnection(m_sqlitePath, m_connectionName, &local,
                                                readOnly, kCatalogUserVersion);
    opened = db.isValid() && db.isOpen();
  }
  if (!opened)
  {
    forgetConnection(m_connectionName);
    m_connectionName.clear();
    if (schemaRejected)
      *schemaRejected = probeSqliteUserVersion(m_sqlitePath) > kCatalogUserVersion;
    setError(error, local.isEmpty() ? QStringLiteral("cannot open catalog sqlite") : local);
    return false;
  }
  return true;
}

bool CatalogStore::begin(QString *error)
{
  if (m_inTxn)
    return true;
  if (!m_open || !m_writable)
  {
    setError(error, QStringLiteral("catalog sqlite is not open for write"));
    return false;
  }
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  if (!execSql(db, QStringLiteral("BEGIN IMMEDIATE"), error))
    return false;
  m_inTxn = true;
  return true;
}

bool CatalogStore::commit(QString *error)
{
  if (!m_inTxn)
    return true;
  QSqlDatabase db = QSqlDatabase::database(m_connectionName);
  if (!execSql(db, QStringLiteral("COMMIT"), error))
    return false;
  m_inTxn = false;
  return true;
}

void CatalogStore::rollback()
{
  if (!m_inTxn || m_connectionName.isEmpty())
  {
    m_inTxn = false;
    return;
  }
  QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
  if (db.isValid() && db.isOpen())
  {
    QSqlQuery q(db);
    q.exec(QStringLiteral("ROLLBACK"));
  }
  m_inTxn = false;
}
