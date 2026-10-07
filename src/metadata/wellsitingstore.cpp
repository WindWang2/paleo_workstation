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

QString wellSitingConnectionName(const QString &path)
{
    return QStringLiteral("paleo_wellsiting_") + QString::number(qHash(path));
}

bool wellSitingEnsureOpen(const QString &path, QString *error)
{
    QSqlDatabase db = MetaStore::openConnection(path, wellSitingConnectionName(path), error);
    if (!db.isValid())
        return false;
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
} // namespace

WellSitingStore::WellSitingStore(const QString &metaSqlitePath,
                                 PaleoProjectStore *projectStore)
    : m_dbPath(metaSqlitePath), m_store(projectStore)
{
}

bool WellSitingStore::open(QString *error)
{
    return wellSitingEnsureOpen(m_dbPath, error);
}

bool WellSitingStore::save(const paleo::siting::ScenarioSet &set, QString *error)
{
    if (m_readOnly) {
        setError(error, QStringLiteral("WellSitingStore 只读（工程被其他实例锁定）"));
        return false;
    }
    QJsonObject root = QJsonObject::fromVariantMap(set.toMap());
    root.insert(QStringLiteral("schema_version"), 1);
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    const QString nowUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    const auto write = [&](QString *err) -> bool {
        if (!wellSitingEnsureOpen(m_dbPath, err))
            return false;
        QSqlQuery q(QSqlDatabase::database(wellSitingConnectionName(m_dbPath)));
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
    if (!wellSitingEnsureOpen(m_dbPath, error))
        return false;
    QSqlQuery q(QSqlDatabase::database(wellSitingConnectionName(m_dbPath)));
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
    root.remove(QStringLiteral("schema_version"));
    set = paleo::siting::ScenarioSet::fromMap(root);
    return true;
}
