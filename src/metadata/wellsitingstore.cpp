// 层：数据
#include "wellsitingstore.h"
#include "storeerrors_internal.h"
#include "metastore.h"
#include "paleoprojectstore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace
{
using paleo::store_detail::setError;

constexpr auto kWellSitingRowId = "current"; // 单行文档表
// 落盘 schema_version（save 侧写入）。读侧同口径校验：载荷形状演进后旧
// 行不得被静默按新形状误解析（对照 metastore 的 user_version 门与
// mapversionstore 的 snapshot_schema 指纹门——显式拒绝）。
constexpr int kWellSitingSchemaVersion = 1;

QString wellSitingConnectionName(const QString &path, bool readOnly)
{
    // readOnly 实例走独立连接名（QSQLITE_OPEN_READONLY，#80）——同进程可写
    // 与只读实例不得复用同一条连接（LayerManifest 同构）。
    return QStringLiteral("paleo_wellsiting_") + QString::number(qHash(path)) +
           (readOnly ? QStringLiteral("_ro") : QString());
}

// 幂等建表。#736：本函数即 MetaStore 的迁移步（幂等，可按步重复调用），
// 由 openConnection 在版本落后时于事务内逐级执行、版本已到位时作 ensure。
// #395：readOnly 连接不建表/不迁移——schema 归写实例；read-write 打开会
// 撞写实例的排他锁（整库读不出）。
bool wellSitingEnsureSchema(QSqlDatabase &db, QString *error)
{
    QSqlQuery schema(db);
    if (!schema.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS well_siting_scenarios ("
            "id TEXT PRIMARY KEY, "
            "payload TEXT NOT NULL, "
            "updated_utc TEXT NOT NULL)")))
    {
        setError(error, schema.lastError().text());
        return false;
    }
    return true;
}

bool wellSitingEnsureOpen(const QString &path, bool readOnly, QString *error)
{
    QSqlDatabase db = MetaStore::openConnection(
        path, wellSitingConnectionName(path, readOnly), error, readOnly,
        MetaStore::kUserVersion,
        [](QSqlDatabase &db, int, int, QString *err) { return wellSitingEnsureSchema(db, err); });
    if (!db.isValid())
        return false;
    // 只读实例不建表/不迁移（#395/#80）——schema 归写实例；可写路径的 schema
    // 已在 openConnection 内确保（版本落后时在事务内逐级迁移，#736）。
    return true;
}
} // namespace

WellSitingStore::WellSitingStore(const QString &metaSqlitePath,
                                 PaleoProjectStore *projectStore)
    : m_dbPath(metaSqlitePath), m_store(projectStore)
{
}

bool WellSitingStore::open(QString *error)
{
    return wellSitingEnsureOpen(m_dbPath, m_readOnly, error);
}

bool WellSitingStore::save(const paleo::siting::ScenarioSet &set, QString *error)
{
    if (m_readOnly) {
        setError(error, QStringLiteral("WellSitingStore 只读（工程被其他实例锁定）"));
        return false;
    }
    QJsonObject root = QJsonObject::fromVariantMap(set.toMap());
    root.insert(QStringLiteral("schema_version"), kWellSitingSchemaVersion);
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    const QString nowUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    const auto write = [&](QString *err) -> bool {
        if (!wellSitingEnsureOpen(m_dbPath, false, err))
            return false;
        QSqlQuery q(QSqlDatabase::database(wellSitingConnectionName(m_dbPath, false)));
        q.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO well_siting_scenarios (id, payload, updated_utc) "
            "VALUES (?, ?, ?)"));
        q.addBindValue(QString::fromLatin1(kWellSitingRowId));
        q.addBindValue(QString::fromUtf8(payload));
        q.addBindValue(nowUtc);
        if (!q.exec()) {
            setError(err, q.lastError().text());
            return false;
        }
        return true;
    };

    if (m_store) {
        // 单写纪律：方案文档写经工程写队列串行（§41.2）。
        const PaleoProjectStore::WriteResult result = m_store->enqueueWrite(
            [&write]() -> PaleoProjectStore::WriteResult {
                QString err;
                if (write(&err))
                    return {true, QString()};
                return {false, err};
            });
        if (!result.ok) {
            setError(error, result.error);
            return false;
        }
        return true;
    }
    return write(error); // 测试直用（无写队列注入）
}

bool WellSitingStore::load(paleo::siting::ScenarioSet &set, QString *error) const
{
    if (!wellSitingEnsureOpen(m_dbPath, m_readOnly, error))
        return false;
    QSqlQuery q(QSqlDatabase::database(wellSitingConnectionName(m_dbPath, m_readOnly)));
    q.prepare(QStringLiteral(
        "SELECT payload FROM well_siting_scenarios WHERE id = ?"));
    q.addBindValue(QString::fromLatin1(kWellSitingRowId));
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return false;
    }
    if (!q.next()) {
        set = paleo::siting::ScenarioSet(); // 新工程：空集
        return true;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(q.value(0).toString().toUtf8());
    if (!doc.isObject()) {
        setError(error, QStringLiteral("方案集文档不是 JSON 对象"));
        return false;
    }
    QVariantMap root = doc.object().toVariantMap();
    // schema 门：形状不匹配的载荷拒读，不按当前 fromMap 静默误解析。
    const QJsonValue schemaValue = doc.object().value(QStringLiteral("schema_version"));
    if (!schemaValue.isDouble() || schemaValue.toInt() != kWellSitingSchemaVersion) {
        setError(error, QStringLiteral("不支持布井方案集 schema_version：%1")
                            .arg(schemaValue.isUndefined() ? QStringLiteral("缺失")
                                                           : schemaValue.toVariant().toString()));
        return false;
    }
    root.remove(QStringLiteral("schema_version"));
    set = paleo::siting::ScenarioSet::fromMap(root);
    return true;
}
