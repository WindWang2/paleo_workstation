// 层：数据
#pragma once

#include "catalogstore.h"

#include "../metadata/storeerrors_internal.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>

// catalogstore TU 族（catalogstore.cpp / _schema / _crud / _json）共享的
// 实现细节。对外不可见——公共 API 只看 catalogstore.h；语义先例见
// metadata/storeerrors_internal.h（paleo::store_detail）。
namespace paleo::catalog_detail
{

using paleo::store_detail::setError;

// catalog.sqlite 的 user_version 版本域独立于 project.sqlite 的
// MetaStore::kUserVersion：早期构建复用 project 版本门，把 catalog 文件
// 标成了 1/2——兼容常量取 2 放行存量库。今后 project schema 推进不再
// 影响 catalog；catalog 自身的 schema 演进以 meta 表 schema_epoch 为准。
constexpr int kCatalogUserVersion = 2;

inline QString catalogConnectionName(const CatalogStore *self, const QString &path, bool readOnly)
{
  return QStringLiteral("paleo_catalog_%1_%2%3")
      .arg(QString::number(qHash(QFileInfo(path).absoluteFilePath()), 16),
           QString::number(reinterpret_cast<quintptr>(self), 16),
           readOnly ? QStringLiteral("_ro") : QString());
}

inline bool execSql(QSqlDatabase &db, const QString &sql, QString *error)
{
  QSqlQuery q(db);
  if (!q.exec(sql))
  {
    setError(error, q.lastError().text().isEmpty() ? sql : q.lastError().text());
    return false;
  }
  return true;
}

inline const char *kSchemaSql =
    "CREATE TABLE IF NOT EXISTS entities ("
    " id TEXT PRIMARY KEY NOT NULL,"
    " entity_type TEXT NOT NULL DEFAULT '',"
    " name TEXT NOT NULL DEFAULT '',"
    " surface_x REAL NOT NULL DEFAULT 0,"
    " surface_y REAL NOT NULL DEFAULT 0,"
    " has_surface INTEGER NOT NULL DEFAULT 0,"
    " kb REAL NOT NULL DEFAULT 0,"
    " td REAL NOT NULL DEFAULT 0,"
    " coordinate_status TEXT NOT NULL DEFAULT '',"
    " inline_min REAL NOT NULL DEFAULT 0,"
    " inline_max REAL NOT NULL DEFAULT 0,"
    " xline_min REAL NOT NULL DEFAULT 0,"
    " xline_max REAL NOT NULL DEFAULT 0,"
    " sample_interval_us REAL NOT NULL DEFAULT 0,"
    " start_time_ms REAL NOT NULL DEFAULT 0,"
    " corners_json TEXT NOT NULL DEFAULT '[]',"
    " extra_json TEXT NOT NULL DEFAULT '{}'"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_entities_type_name ON entities(entity_type, name);"
    "CREATE TABLE IF NOT EXISTS assets ("
    " id TEXT PRIMARY KEY NOT NULL,"
    " type TEXT NOT NULL DEFAULT '',"
    " format TEXT NOT NULL DEFAULT '',"
    " display_name TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE TABLE IF NOT EXISTS versions ("
    " id TEXT PRIMARY KEY NOT NULL,"
    " asset_id TEXT NOT NULL,"
    " stage TEXT NOT NULL DEFAULT '',"
    " version_number INTEGER NOT NULL DEFAULT 1,"
    " managed INTEGER NOT NULL DEFAULT 1,"
    " path TEXT NOT NULL DEFAULT '',"
    " source_uri TEXT NOT NULL DEFAULT '',"
    " sha256 TEXT NOT NULL DEFAULT '',"
    " file_name TEXT NOT NULL DEFAULT '',"
    " parent_version_ids_json TEXT NOT NULL DEFAULT '[]',"
    " extra_json TEXT NOT NULL DEFAULT '{}'"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_versions_asset ON versions(asset_id);"
    "CREATE TABLE IF NOT EXISTS entity_asset_links ("
    " ord INTEGER PRIMARY KEY NOT NULL,"
    " entity_type TEXT NOT NULL DEFAULT '',"
    " entity_id TEXT NOT NULL DEFAULT '',"
    " asset_id TEXT NOT NULL DEFAULT '',"
    " role TEXT NOT NULL DEFAULT '',"
    " is_primary INTEGER NOT NULL DEFAULT 1,"
    " unresolved INTEGER NOT NULL DEFAULT 0,"
    " ordinal INTEGER NOT NULL DEFAULT 0,"
    " note TEXT NOT NULL DEFAULT ''"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_links_entity_role "
    " ON entity_asset_links(entity_type, entity_id, role);"
    "CREATE INDEX IF NOT EXISTS idx_links_asset ON entity_asset_links(asset_id);"
    "CREATE TABLE IF NOT EXISTS catalog_meta ("
    " key TEXT PRIMARY KEY NOT NULL,"
    " value TEXT NOT NULL"
    ");";

inline bool execScript(QSqlDatabase &db, const char *sql, QString *error)
{
  const QStringList parts = QString::fromUtf8(sql).split(QLatin1Char(';'), Qt::SkipEmptyParts);
  for (QString part : parts)
  {
    part = part.trimmed();
    if (part.isEmpty())
      continue;
    if (!execSql(db, part, error))
      return false;
  }
  return true;
}

inline bool applyWritablePragmas(QSqlDatabase &db, QString *error)
{
  // WAL 是 catalog.sqlite 的例外（project.sqlite 仍不采用 WAL）。
  // synchronous=FULL：崩溃安全。禁止 synchronous=OFF。
  if (!execSql(db, QStringLiteral("PRAGMA busy_timeout = 5000"), error))
    return false;
  if (!execSql(db, QStringLiteral("PRAGMA journal_mode = WAL"), error))
    return false;
  if (!execSql(db, QStringLiteral("PRAGMA synchronous = FULL"), error))
    return false;
  return true;
}

inline void forgetConnection(const QString &name)
{
  if (name.isEmpty() || !QSqlDatabase::contains(name))
    return;
  {
    QSqlDatabase db = QSqlDatabase::database(name, false);
    if (db.isValid())
      db.close();
  }
  QSqlDatabase::removeDatabase(name);
}

// 四表 + meta 整体装载（连接已打开时）。定义在 catalogstore_json.cpp。
bool loadTables(QSqlDatabase &db, CatalogStore::Tables *out, QString *error);

} // namespace paleo::catalog_detail
