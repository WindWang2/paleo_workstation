// 层：数据
#include "metastore.h"

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
