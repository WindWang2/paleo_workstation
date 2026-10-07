// 层：数据
#include "metastore.h"
#include "storeerrors_internal.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace
{
using paleo::store_detail::setError;

} // namespace

namespace MetaStore
{

QSqlDatabase openConnection(const QString &path, const QString &connectionName, QString *error,
                            bool readOnly, int userVersion)
{
  if (readOnly && !QFileInfo::exists(path))
  {
    // 只读实例不在被锁目录里建库（#80）。
    setError(error, QStringLiteral("%1 %2 does not exist (read-only instance does not "
                                   "create it)")
                        .arg(QFileInfo(path).fileName(), path));
    return {};
  }
  QSqlDatabase db = QSqlDatabase::contains(connectionName)
                        ? QSqlDatabase::database(connectionName)
                        : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
  if (!db.isValid())
  {
    setError(error, QStringLiteral("QSQLITE driver is not available"));
    return {};
  }
  if (!db.isOpen())
  {
    if (!readOnly)
    {
      const QDir dir = QFileInfo(path).absoluteDir();
      if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
      {
        setError(error, QStringLiteral("cannot create directory for %1").arg(path));
        return {};
      }
    }
    db.setDatabaseName(path);
    db.setConnectOptions(readOnly ? QStringLiteral("QSQLITE_OPEN_READONLY") : QString());
    if (!db.open())
    {
      setError(error, db.lastError().text());
      return {};
    }
  }
  if (readOnly)
  {
    // 只读：校验不推进——未来版本拒开，旧/新库（<= 当前）照读。
    const int v = readUserVersion(db, error);
    if (v < 0)
      return {};
    if (v > userVersion)
    {
      setError(error,
               QStringLiteral("%1 schema user_version %2 is newer than this build "
                              "supports (%3); refusing to open — upgrade the application")
                   .arg(QFileInfo(path).fileName())
                   .arg(v)
                   .arg(userVersion));
      return {};
    }
    return db;
  }
  // A rejected future schema leaves its connection in Qt's registry. Always
  // recheck the gate before allowing a store to create or alter any tables.
  if (!ensureUserVersion(db, error, userVersion))
    return {};
  return db;
}

int closeConnectionsFor(const QString &path)
{
  if (path.isEmpty())
    return 0;
  const QString target = QFileInfo(path).absoluteFilePath();
  QStringList victims;
  for (const QString &name : QSqlDatabase::connectionNames())
  {
    // open=false：只查注册信息，不触发重连。别的线程拥有的连接在 Qt6 下
    // 返回 invalid（并跳过）——只能由拥有线程关闭。
    QSqlDatabase db = QSqlDatabase::database(name, false);
    if (!db.isValid())
      continue;
    if (QFileInfo(db.databaseName()).absoluteFilePath() != target)
      continue;
    db.close();
    victims.append(name);
  }
  // removeDatabase 前必须释放本地 QSqlDatabase 拷贝（上面循环体内已出作用域）。
  for (const QString &name : victims)
    QSqlDatabase::removeDatabase(name);
  return victims.size();
}

int readUserVersion(QSqlDatabase &db, QString *error)
{
  QSqlQuery q(db);
  if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next())
  {
    setError(error, QStringLiteral("cannot read PRAGMA user_version: %1")
                            .arg(q.lastError().text()));
    return -1;
  }
  return q.value(0).toInt();
}

bool ensureUserVersion(QSqlDatabase &db, QString *error, int userVersion)
{
  const int v = readUserVersion(db, error);
  if (v < 0)
    return false;
  if (v == userVersion)
    return true;
  if (v > userVersion)
  {
    setError(error,
             QStringLiteral("%1 schema user_version %2 is newer than this build "
                            "supports (%3); refusing to open — upgrade the application")
                 .arg(QFileInfo(db.databaseName()).fileName())
                 .arg(v)
                 .arg(userVersion));
    return false;
  }
  // v < userVersion（含 0）：新库/遗留库 → 推进到当前版本。
  QSqlQuery q(db);
  if (!q.exec(QStringLiteral("PRAGMA user_version = %1").arg(userVersion)))
  {
    setError(error, QStringLiteral("cannot set PRAGMA user_version to %1: %2")
                            .arg(userVersion)
                            .arg(q.lastError().text()));
    return false;
  }
  return true;
}

} // namespace MetaStore
