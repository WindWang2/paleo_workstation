// 层：数据
#include "wellsectionstore.h"
#include "metastore.h"
#include "paleoprojectstore.h"

#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace
{
QString connectionNameFor(const QString &path)
{
    return QStringLiteral("paleo_wellsection_") + QString::number(qHash(path));
}

void setError(QString *error, const QString &text)
{
    if (error)
        *error = text;
}

// 幂等建表：section_id 主键单节一行，version 随 save 递增。
bool ensureOpen(const QString &path, QString *error)
{
    QSqlDatabase db = MetaStore::openConnection(path, connectionNameFor(path), error);
    if (!db.isValid())
        return false;
    QSqlQuery schema(db);
    if (!schema.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS well_section_edits ("
            "section_id TEXT PRIMARY KEY, "
            "well_ids TEXT NOT NULL, "
            "link_overrides TEXT NOT NULL, "
            "version INTEGER NOT NULL, "
            "updated_utc TEXT NOT NULL)")))
    {
        setError(error, schema.lastError().text());
        return false;
    }
    return true;
}

int versionOf(const QString &path, const QString &sectionId, QString *error)
{
    QSqlQuery q(QSqlDatabase::database(connectionNameFor(path)));
    q.prepare(QStringLiteral("SELECT version FROM well_section_edits WHERE section_id = ?"));
    q.addBindValue(sectionId);
    if (!q.exec() || !q.next())
    {
        if (error && q.lastError().isValid())
            *error = q.lastError().text();
        return 0;
    }
    return q.value(0).toInt();
}
} // namespace

namespace metadata {

WellSectionStore::WellSectionStore(const QString &metaSqlitePath,
                                   PaleoProjectStore *projectStore)
    : m_dbPath(metaSqlitePath), m_store(projectStore)
{
}

void WellSectionStore::rebind(const QString &metaSqlitePath,
                              PaleoProjectStore *projectStore)
{
    m_dbPath = metaSqlitePath;
    m_store = projectStore;
}

bool WellSectionStore::open(QString *error)
{
    if (m_dbPath.isEmpty())
    {
        setError(error, QStringLiteral("WellSectionStore 未绑定工程库路径"));
        return false;
    }
    return ensureOpen(m_dbPath, error);
}

WellSectionRecord WellSectionStore::save(
    const QString &sectionId, const QStringList &wellIds,
    const QVector<WellSectionLinkOverride> &links, QString *error)
{
    if (m_readOnly)
    {
        setError(error, QStringLiteral("WellSectionStore 只读（工程被其他实例锁定）"));
        return {};
    }
    if (m_dbPath.isEmpty())
    {
        setError(error, QStringLiteral("WellSectionStore 未绑定工程库路径"));
        return {};
    }
    const QString nowUtc =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    const auto write = [&](QString *err) -> WellSectionRecord {
        if (!ensureOpen(m_dbPath, err))
            return {};
        // 写队列内读改写：版本号严格递增（round-trip 的推进口径）。
        const int version = versionOf(m_dbPath, sectionId, err) + 1;
        QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
        q.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO well_section_edits "
            "(section_id, well_ids, link_overrides, version, updated_utc) "
            "VALUES (?, ?, ?, ?, ?)"));
        q.addBindValue(sectionId);
        // 空 join 是 null QString（QSql 绑成 NULL 撞 NOT NULL）——钉空串。
        const QString wellIdsCsv = wellIds.join(QLatin1Char(','));
        q.addBindValue(wellIdsCsv.isNull() ? QStringLiteral("") : wellIdsCsv);
        QStringList rows;
        rows.reserve(links.size());
        for (const WellSectionLinkOverride &o : links)
            rows << QStringLiteral("%1;%2;%3;%4")
                        .arg(o.leftWellId, o.rightWellId, o.topName,
                             o.connected ? QStringLiteral("1")
                                         : QStringLiteral("0"));
        const QString linksCsv = rows.join(QLatin1Char('|'));
        q.addBindValue(linksCsv.isNull() ? QStringLiteral("") : linksCsv);
        q.addBindValue(version);
        q.addBindValue(nowUtc);
        if (!q.exec())
        {
            setError(err, q.lastError().text());
            return {};
        }
        WellSectionRecord rec;
        rec.wellIds = wellIds;
        rec.linkOverrides = links;
        rec.version = version;
        return rec;
    };

    WellSectionRecord rec;
    QString writeErr;
    if (m_store)
    {
        // 单写纪律：剖面编辑产物写经工程写队列串行（§41.2）。
        const PaleoProjectStore::WriteResult result = m_store->enqueueWrite(
            [&write, &rec, &writeErr]() -> PaleoProjectStore::WriteResult {
                rec = write(&writeErr);
                return rec.valid()
                           ? PaleoProjectStore::WriteResult{true, QString()}
                           : PaleoProjectStore::WriteResult{false, writeErr};
            });
        if (!result.ok)
        {
            setError(error, result.error);
            return {};
        }
        return rec;
    }
    return write(error);
}

WellSectionRecord WellSectionStore::load(const QString &sectionId,
                                         QString *error) const
{
    WellSectionRecord rec;
    if (m_dbPath.isEmpty())
    {
        setError(error, QStringLiteral("WellSectionStore 未绑定工程库路径"));
        return rec;
    }
    if (!ensureOpen(m_dbPath, error))
        return rec;
    QSqlQuery q(QSqlDatabase::database(connectionNameFor(m_dbPath)));
    q.prepare(QStringLiteral(
        "SELECT well_ids, link_overrides, version FROM well_section_edits "
        "WHERE section_id = ?"));
    q.addBindValue(sectionId);
    if (!q.exec())
    {
        setError(error, q.lastError().text());
        return rec;
    }
    if (!q.next())
        return rec; // 新工程：无行 → version 0
    rec.wellIds = q.value(0).toString().split(QLatin1Char(','),
                                              Qt::SkipEmptyParts);
    for (const QString &row : q.value(1).toString().split(
             QLatin1Char('|'), Qt::SkipEmptyParts))
    {
        const QStringList parts = row.split(QLatin1Char(';'));
        if (parts.size() != 4)
            continue;
        WellSectionLinkOverride o;
        o.leftWellId = parts.at(0);
        o.rightWellId = parts.at(1);
        o.topName = parts.at(2);
        o.connected = parts.at(3) == QLatin1String("1");
        rec.linkOverrides.push_back(o);
    }
    rec.version = q.value(2).toInt();
    return rec;
}

} // namespace metadata
