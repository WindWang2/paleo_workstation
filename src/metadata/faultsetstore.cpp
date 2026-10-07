// 层：数据
#include "faultsetstore.h"
#include "storeerrors_internal.h"
#include "metastore.h"
#include "paleoprojectstore.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace
{
using paleo::store_detail::setError;

constexpr auto kFaultsetRowId = "current"; // 单行文档表

QString faultsetConnectionName(const QString &path)
{
    return QStringLiteral("paleo_faultset_") + QString::number(qHash(path));
}

bool columnExists(QSqlDatabase &db, const QString &column)
{
    QSqlQuery info(db);
    if (!info.exec(QStringLiteral("PRAGMA table_info(fault_set)")))
        return false;
    while (info.next()) {
        if (info.value(1).toString() == column)
            return true;
    }
    return false;
}

// 幂等建表。user_version 1→2 的列迁移：旧表没有 surface 时 ALTER 补上。
// 棒/切割仍在 payload，断面只进 surface 列（可空）。
bool faultsetEnsureOpen(const QString &path, QString *error)
{
    QSqlDatabase db = MetaStore::openConnection(path, faultsetConnectionName(path), error);
    if (!db.isValid())
        return false;
    QSqlQuery schema(db);
    if (!schema.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS fault_set ("
            "id TEXT PRIMARY KEY, "
            "payload TEXT NOT NULL, "
            "updated_utc TEXT NOT NULL, "
            "surface TEXT)")))
    {
        setError(error, schema.lastError().text());
        return false;
    }
    if (!columnExists(db, QStringLiteral("surface"))) {
        QSqlQuery alter(db);
        if (!alter.exec(QStringLiteral("ALTER TABLE fault_set ADD COLUMN surface TEXT"))) {
            setError(error, alter.lastError().text());
            return false;
        }
    }
    return true;
}
} // namespace

FaultSetStore::FaultSetStore(const QString &metaSqlitePath, PaleoProjectStore *projectStore)
    : m_dbPath(metaSqlitePath), m_store(projectStore)
{
}

bool FaultSetStore::open(QString *error)
{
    return faultsetEnsureOpen(m_dbPath, error);
}

bool FaultSetStore::save(const paleo::fault::FaultSet &set, QString *error)
{
    if (m_readOnly) {
        setError(error, QStringLiteral("FaultSetStore 只读（工程被其他实例锁定）"));
        return false;
    }
    const QByteArray payload = set.withoutSurfaces().toJson();
    const QByteArray surfaces = set.surfacesJson();
    const QString nowUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    const auto write = [&](QString *err) -> bool {
        if (!faultsetEnsureOpen(m_dbPath, err))
            return false;
        QSqlQuery q(QSqlDatabase::database(faultsetConnectionName(m_dbPath)));
        q.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO fault_set (id, payload, updated_utc, surface) "
            "VALUES (?, ?, ?, ?)"));
        q.addBindValue(QString::fromLatin1(kFaultsetRowId));
        q.addBindValue(QString::fromUtf8(payload));
        q.addBindValue(nowUtc);
        q.addBindValue(surfaces.isEmpty() || surfaces == QByteArray("{}")
                           ? QVariant()
                           : QVariant(QString::fromUtf8(surfaces)));
        if (!q.exec()) {
            setError(err, q.lastError().text());
            return false;
        }
        return true;
    };

    if (m_store) {
        // 单写纪律：FaultSet 文档写经工程写队列串行（§41.2）。
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

bool FaultSetStore::load(paleo::fault::FaultSet &set, QString *error) const
{
    if (!faultsetEnsureOpen(m_dbPath, error))
        return false;
    QSqlDatabase db = QSqlDatabase::database(faultsetConnectionName(m_dbPath));
    const bool hasSurface = columnExists(db, QStringLiteral("surface"));
    QSqlQuery q(db);
    q.prepare(hasSurface ? QStringLiteral("SELECT payload, surface FROM fault_set WHERE id = ?")
                         : QStringLiteral("SELECT payload FROM fault_set WHERE id = ?"));
    q.addBindValue(QString::fromLatin1(kFaultsetRowId));
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return false;
    }
    if (!q.next()) {
        set.clear(); // 新工程：空集
        return true;
    }
    if (!set.fromJson(q.value(0).toString().toUtf8(), error))
        return false;
    if (!hasSurface || q.value(1).isNull()) {
        set.clearSurfaces();
        return true;
    }
    return set.applySurfacesJson(q.value(1).toString().toUtf8(), error);
}
