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

// #814：kb/td 缺失值的 SQL 侧记账键。sqlite 数值列 NOT NULL，存不进 NaN，
// 只能写伪 0 + 一个「缺失」标记；标记放在用户前向扩展命名空间里会被
// upsertEntity 覆盖、被 loadTables 剥掉——用户在 extra 里放的同名键就此
// 消失。'#' 前缀是仓库既定的内部/审计命名空间（版本 extra 的 #history/
// #source 同款，updateVersionExtra 明文拒收带 '#' 的用户键），内部记账
// 因此不会与用户数据争名。kLegacyKbMissingKey 是旧构建落盘的无前缀键，
// 读侧继续认它（存量库/.bak 仍带这些键），写侧只写前缀键。
inline const QString kKbMissingKey = QStringLiteral("#kbMissing");
inline const QString kTdMissingKey = QStringLiteral("#tdMissing");
inline const QString kLegacyKbMissingKey = QStringLiteral("kbMissing");
inline const QString kLegacyTdMissingKey = QStringLiteral("tdMissing");

// #476：versions(asset_id, version_number) 唯一索引（部分索引：>1 的显式
// 编号族才参与——0/1 档是未编号/首版的存量语义面）——写侧 open 时尽力落
//（对照 mapversionstore 的 map_versions(horizon,version) UNIQUE INDEX）。
// 新库/干净库立即生效；存量库若有历史重复行则建索引失败：仅告警不拒开
// ——重复由 DataCatalog::addVersion 的内存闸挡新发，legacy 行原样保留
//（不引入数据迁移）。防御纵深：绕过内存闸的外部直写会被 sqlite 拒绝。
inline void ensureVersionNumberUniqueIndex(QSqlDatabase &db)
{
  QSqlQuery q(db);
  if (!q.exec(QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_versions_asset_number"
                             " ON versions(asset_id, version_number) WHERE version_number > 1")))
    qWarning() << "catalog: cannot create unique index on versions(asset_id, version_number):"
               << q.lastError().text();
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
