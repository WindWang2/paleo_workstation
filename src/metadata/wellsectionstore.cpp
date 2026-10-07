// 层：数据
#include "wellsectionstore.h"
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

// 井 id/顶名是任意用户文本：入库前转义分隔符（\ 前缀），读回反转义。
// 未转义的旧行按原文读（向后兼容：安全名恒等）。
QString escapeField(const QString &in)
{
    QString out;
    out.reserve(in.size());
    for (const QChar c : in) {
        if (c == QLatin1Char('\\') || c == QLatin1Char(';') ||
            c == QLatin1Char('|') || c == QLatin1Char(','))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}
QString unescapeField(const QString &in)
{
    QString out;
    out.reserve(in.size());
    bool esc = false;
    for (const QChar c : in) {
        if (esc) {
            out += c;
            esc = false;
            continue;
        }
        if (c == QLatin1Char('\\')) {
            esc = true;
            continue;
        }
        out += c;
    }
    if (esc)
        out += QLatin1Char('\\'); // 尾悬挂反斜杠按原文保
    return out;
}
// 按未转义 sep 切分，**保留转义序列原文**（反斜杠不剥——内层分隔符的
// 转义要留给下一层；单元格再各自 unescapeField）。空段丢弃。
QStringList splitEscaped(const QString &in, QChar sep)
{
    QStringList out;
    QString cur;
    bool esc = false;
    for (const QChar c : in) {
        if (esc) {
            cur += c;
            esc = false;
            continue;
        }
        if (c == QLatin1Char('\\')) {
            cur += c; // 反斜杠随格保留（内层转义不提前剥）
            esc = true;
            continue;
        }
        if (c == sep) {
            if (!cur.isEmpty())
                out << cur;
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (esc)
        cur += QLatin1Char('\\');
    if (!cur.isEmpty())
        out << cur;
    return out;
}
QString wellSectionConnectionName(const QString &path)
{
    return QStringLiteral("paleo_wellsection_") + QString::number(qHash(path));
}

// 幂等建表：section_id 主键单节一行，version 随 save 递增。
bool wellSectionEnsureOpen(const QString &path, QString *error)
{
    QSqlDatabase db = MetaStore::openConnection(path, wellSectionConnectionName(path), error);
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
    QSqlQuery q(QSqlDatabase::database(wellSectionConnectionName(path)));
    q.prepare(QStringLiteral("SELECT version FROM well_section_edits WHERE section_id = ?"));
    q.addBindValue(sectionId);
    if (!q.exec())
    {
        // 连接级失败 ≠ 无行：返回 -1，save 拒写（防版本被压回 1）。
        setError(error, q.lastError().text());
        return -1;
    }
    if (!q.next())
        return 0;
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
    return wellSectionEnsureOpen(m_dbPath, error);
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
        if (!wellSectionEnsureOpen(m_dbPath, err))
            return {};
        // 写队列内读改写：版本号严格递增（round-trip 的推进口径）。
        const int current = versionOf(m_dbPath, sectionId, err);
        if (current < 0)
            return {};
        const int version = current + 1;
        QSqlQuery q(QSqlDatabase::database(wellSectionConnectionName(m_dbPath)));
        q.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO well_section_edits "
            "(section_id, well_ids, link_overrides, version, updated_utc) "
            "VALUES (?, ?, ?, ?, ?)"));
        q.addBindValue(sectionId);
        // 空 join 是 null QString（QSql 绑成 NULL 撞 NOT NULL）——钉空串；
        // 字段先转义（id/顶名可含分隔符）。
        QStringList escapedIds;
        escapedIds.reserve(wellIds.size());
        for (const QString &id : wellIds)
            escapedIds << escapeField(id);
        const QString wellIdsCsv = escapedIds.join(QLatin1Char(','));
        q.addBindValue(wellIdsCsv.isNull() ? QStringLiteral("") : wellIdsCsv);
        QStringList rows;
        rows.reserve(links.size());
        for (const WellSectionLinkOverride &o : links)
            rows << QStringLiteral("%1;%2;%3;%4")
                        .arg(escapeField(o.leftWellId),
                             escapeField(o.rightWellId),
                             escapeField(o.topName),
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
    if (!wellSectionEnsureOpen(m_dbPath, error))
        return rec;
    QSqlQuery q(QSqlDatabase::database(wellSectionConnectionName(m_dbPath)));
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
    for (const QString &cell :
         splitEscaped(q.value(0).toString(), QLatin1Char(',')))
        rec.wellIds << unescapeField(cell);
    for (const QString &row :
         splitEscaped(q.value(1).toString(), QLatin1Char('|')))
    {
        const QStringList parts =
            splitEscaped(row, QLatin1Char(';'));
        if (parts.size() != 4)
            continue; // 坏行跳过（转义后不应出现；未转义旧库的脏名行）
        WellSectionLinkOverride o;
        o.leftWellId = unescapeField(parts.at(0));
        o.rightWellId = unescapeField(parts.at(1));
        o.topName = unescapeField(parts.at(2));
        o.connected = parts.at(3) == QLatin1String("1");
        rec.linkOverrides.push_back(o);
    }
    rec.version = q.value(2).toInt();
    return rec;
}

QStringList WellSectionStore::sectionIds(QString *error) const
{
    QStringList out;
    if (m_dbPath.isEmpty())
    {
        setError(error, QStringLiteral("WellSectionStore 未绑定工程库路径"));
        return out;
    }
    if (!wellSectionEnsureOpen(m_dbPath, error))
        return out;
    QSqlQuery q(QSqlDatabase::database(wellSectionConnectionName(m_dbPath)));
    if (!q.exec(QStringLiteral("SELECT section_id FROM well_section_edits "
                               "ORDER BY section_id")))
    {
        setError(error, q.lastError().text());
        return out;
    }
    while (q.next())
        out << q.value(0).toString();
    return out;
}

bool WellSectionStore::remove(const QString &sectionId, QString *error)
{
    if (m_readOnly)
    {
        setError(error, QStringLiteral("WellSectionStore 只读（工程被其他实例锁定）"));
        return false;
    }
    if (m_dbPath.isEmpty())
    {
        setError(error, QStringLiteral("WellSectionStore 未绑定工程库路径"));
        return false;
    }
    const auto doRemove = [&](QString *err) -> bool {
        if (!wellSectionEnsureOpen(m_dbPath, err))
            return false;
        QSqlQuery q(QSqlDatabase::database(wellSectionConnectionName(m_dbPath)));
        q.prepare(QStringLiteral(
            "DELETE FROM well_section_edits WHERE section_id = ?"));
        q.addBindValue(sectionId);
        if (!q.exec())
        {
            setError(err, q.lastError().text());
            return false;
        }
        return true;
    };
    if (m_store)
    {
        const PaleoProjectStore::WriteResult result = m_store->enqueueWrite(
            [&doRemove]() -> PaleoProjectStore::WriteResult {
                QString err;
                return doRemove(&err)
                           ? PaleoProjectStore::WriteResult{true, QString()}
                           : PaleoProjectStore::WriteResult{false, err};
            });
        if (!result.ok)
        {
            setError(error, result.error);
            return false;
        }
        return true;
    }
    return doRemove(error);
}

} // namespace metadata
