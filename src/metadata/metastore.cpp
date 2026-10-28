// 层：数据
#include "metastore.h"
#include "storeerrors_internal.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QScopeGuard>

namespace
{
using paleo::store_detail::setError;

bool execSql(QSqlDatabase &db, const QString &sql, QString *error)
{
  QSqlQuery q(db);
  if (!q.exec(sql))
  {
    setError(error, q.lastError().text());
    return false;
  }
  return true;
}

} // namespace

namespace MetaStore
{

QSqlDatabase openConnection(const QString &path, const QString &connectionName, QString *error,
                            bool readOnly, int userVersion, const SchemaMigration &migrate)
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
  // #476：各 store 连接名 = 前缀 + 32 位 qHash(path)——两条路径哈希碰撞到
  // 同一连接名时，复用已开连接会把 A 库交给 B 库的 store（静默串库）。
  // 复用前比对 databaseName，指向别的库就拒绝并如实报错。
  if (db.isOpen() &&
      QFileInfo(db.databaseName()).absoluteFilePath() !=
          QFileInfo(path).absoluteFilePath())
  {
    setError(error,
             QStringLiteral("connection name collision: %1 is already open on %2 "
                            "while %3 was requested")
                 .arg(connectionName, db.databaseName(), path));
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
    // #733：等待写锁而不是立刻 SQLITE_BUSY。单写者纪律下第二个写者仍可能
    // 来自同进程的另一条连接（后台任务 vs UI）；不设 busy_timeout 时写入
    // 直接失败。journal_mode 维持默认 rollback journal——
    // docs/SCHEMA_MIGRATION.md §6 明文「project.sqlite 不采用 WAL」（WAL 是
    // catalog.sqlite 的已落地例外，不外推），故这里不设 WAL。
    if (!execSql(db, QStringLiteral("PRAGMA busy_timeout = 5000"), error))
      return {};
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
  if (!ensureUserVersion(db, error, userVersion, migrate))
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

bool ensureUserVersion(QSqlDatabase &db, QString *error, int userVersion,
                       const SchemaMigration &migrate)
{
  const int v = readUserVersion(db, error);
  if (v < 0)
    return false;
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
  if (v == userVersion)
  {
    // 版本已到位也跑一次幂等 ensure（不另起事务：调用方可能已有事务在身，
    // 如 saveVersion 事务内的 latest()；DDL 幂等，加入既有事务即可）。
    // 旧构建「先 bump 版本后建表」的崩溃窗口会留下「版本新、表旧」的库，
    // 少了这一步那些表永远补不回来（#736）。
    if (migrate && !migrate(db, v, v, error))
      return false;
    return true;
  }
  // v < userVersion（含 0）：新库/遗留库 → 单事务内逐级推进到当前版本。
  // #736：BEGIN IMMEDIATE 先取写锁（迁移中途撞锁好过改了一半）；每一步
  // schema 迁移与 user_version 的写入同事务——任一步失败整体回滚，版本号
  // 与表结构同生同死，不再有「版本已新、表还没建」的中间态。
  if (!execSql(db, QStringLiteral("BEGIN IMMEDIATE"), error))
    return false;
  bool committed = false;
  const auto rollbackUnlessCommitted = qScopeGuard([&] {
    if (!committed)
    {
      QSqlQuery q(db);
      q.exec(QStringLiteral("ROLLBACK"));
    }
  });
  for (int step = v; step < userVersion; ++step)
  {
    if (migrate && !migrate(db, step, step + 1, error))
      return false;
  }
  {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA user_version = %1").arg(userVersion)))
    {
      setError(error, QStringLiteral("cannot set PRAGMA user_version to %1: %2")
                          .arg(userVersion)
                          .arg(q.lastError().text()));
      return false;
    }
  }
  if (!execSql(db, QStringLiteral("COMMIT"), error))
    return false;
  committed = true;
  return true;
}

} // namespace MetaStore
