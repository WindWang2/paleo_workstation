// 层：数据
#include "faultsetstore.h"
#include "metastore.h"
#include "paleoprojectstore.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace
{
constexpr auto kRowId = "current"; // 单行文档表

QString connectionNameFor(const QString &path)
{
    return QStringLiteral("paleo_faultset_") + QString::number(qHash(path));
}

void setError(QString *error, const QString &text)
{
    if (error)
        *error = text;
}

// 幂等建表：open() 后调用，CREATE IF NOT EXISTS（schema 前向升级照
// MapVersionStore 惯例走 PRAGMA table_info 补列；首版只有三列）。
bool ensureOpen(const QString &path, QString *error)
{
    QSqlDatabase db = MetaStore::openConnection(path, connectionNameFor(path), error);
    if (!db.isValid())
        return false;
    QSqlQuery schema(db);
    if (!schema.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS fault_set ("
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

FaultSetStore::FaultSetStore(const QString &metaSqlitePath, PaleoProjectStore *projectStore)
    : m_dbPath(metaSqlitePath), m_store(projectStore)
{
}

bool FaultSetStore::open(QString *error)
{
    return ensureOpen(m_dbPath, error);
}

bool FaultSetStore::save(const paleo::fault::FaultSet &set, QString *error)
{
    if (m_readOnly) {
        setError(error, QStringLiteral("FaultSetStore 只读（工程被其他实例锁定）"));
        return false;
    }
    const QByteArray payload = set.toJson();
    const QString nowUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    const auto write = [&](QString *err) -> bool {
        if (!ensureOpen(m_dbPath, err))
            return false;
        QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
        q.prepare(QStringLiteral("INSERT OR REPLACE INTO fault_set (id, payload, updated_utc) "
                                 "VALUES (?, ?, ?)"));
        q.addBindValue(QString::fromLatin1(kRowId));
        q.addBindValue(QString::fromUtf8(payload));
        q.addBindValue(nowUtc);
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
    if (!ensureOpen(m_dbPath, error))
        return false;
    QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
    q.prepare(QStringLiteral("SELECT payload FROM fault_set WHERE id = ?"));
    q.addBindValue(QString::fromLatin1(kRowId));
    if (!q.exec()) {
        setError(error, q.lastError().text());
        return false;
    }
    if (!q.next()) {
        set.clear(); // 新工程：空集
        return true;
    }
    return set.fromJson(q.value(0).toString().toUtf8(), error);
}
