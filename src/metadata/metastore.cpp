// 层：数据
#include "metastore.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace
{
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }
} // namespace

namespace MetaStore
{

QSqlDatabase openConnection(const QString &path, const QString &connectionName, QString *error)
{
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
    const QDir dir = QFileInfo(path).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
    {
      setError(error, QStringLiteral("cannot create directory for %1").arg(path));
      return {};
    }
    db.setDatabaseName(path);
    if (!db.open())
    {
      setError(error, db.lastError().text());
      return {};
    }
  }
  // A rejected future schema leaves its connection in Qt's registry. Always
  // recheck the gate before allowing a store to create or alter any tables.
  if (!ensureUserVersion(db, error))
    return {};
  return db;
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

bool ensureUserVersion(QSqlDatabase &db, QString *error)
{
  const int v = readUserVersion(db, error);
  if (v < 0)
    return false;
  if (v == kUserVersion)
    return true;
  if (v > kUserVersion)
  {
    setError(error,
             QStringLiteral("project.sqlite schema user_version %1 is newer than this build "
                            "supports (%2); refusing to open — upgrade the application")
                 .arg(v)
                 .arg(kUserVersion));
    return false;
  }
  // v < kUserVersion（含 0）：新库/遗留库 → 推进到当前版本。
  QSqlQuery q(db);
  if (!q.exec(QStringLiteral("PRAGMA user_version = %1").arg(kUserVersion)))
  {
    setError(error, QStringLiteral("cannot set PRAGMA user_version to %1: %2")
                            .arg(kUserVersion)
                            .arg(q.lastError().text()));
    return false;
  }
  return true;
}

} // namespace MetaStore
