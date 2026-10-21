// 层：数据
#include "catalogstore.h"
#include "catalogstore_internal.h"

#include "../metadata/atomicfile.h"
#include "../metadata/metastore.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

// catalogstore 主 TU（方向 99 拆分后）：openProject 编排（建库/JSON 迁移/
// 既有库打开）、损坏恢复链（.bak 轮读/JSON 兜底/主库隔离重写）。连接/
// DDL/事务在 _schema TU，CRUD 写在 _crud TU，JSON 编解码与四表装载在
// _json TU；共享助手见 catalogstore_internal.h（paleo::catalog_detail）。

namespace
{
using paleo::store_detail::setError;
using paleo::catalog_detail::applyWritablePragmas;
using paleo::catalog_detail::catalogConnectionName;
using paleo::catalog_detail::execScript;
using paleo::catalog_detail::ensureVersionNumberUniqueIndex;
using paleo::catalog_detail::forgetConnection;
using paleo::catalog_detail::kCatalogUserVersion;
using paleo::catalog_detail::kSchemaSql;
using paleo::catalog_detail::loadTables;

  bool isSchemaReject(const QString &error)
  {
    return error.contains(QStringLiteral("newer than this build")) ||
           error.contains(QStringLiteral("unsupported catalog schema"));
  }

  bool readEpoch(QSqlDatabase &db, int *epoch, QString *error)
  {
    *epoch = 1; // 缺表 / 缺键 = 1，与缺 JSON schema_version 同口径
    QSqlQuery exists(db);
    if (!exists.exec(QStringLiteral(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name='catalog_meta'")))
    {
      setError(error, exists.lastError().text());
      return false;
    }
    if (!exists.next())
      return true;
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral(
            "SELECT value FROM catalog_meta WHERE key='schema_epoch'")))
    {
      setError(error, q.lastError().text());
      return false;
    }
    if (!q.next())
      return true;
    bool ok = false;
    const int v = q.value(0).toString().toInt(&ok);
    *epoch = (ok && v > 0) ? v : 1;
    return true;
  }

  bool integrityOk(QSqlDatabase &db, QString *detail)
  {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("PRAGMA integrity_check")))
    {
      setError(detail, q.lastError().text().isEmpty() ? QStringLiteral("integrity_check failed")
                                                      : q.lastError().text());
      return false;
    }
    bool any = false;
    while (q.next())
    {
      any = true;
      const QString row = q.value(0).toString();
      if (row.compare(QStringLiteral("ok"), Qt::CaseInsensitive) != 0)
      {
        setError(detail, row.isEmpty() ? QStringLiteral("integrity_check failed") : row);
        return false;
      }
    }
    if (!any)
    {
      setError(detail, QStringLiteral("integrity_check failed"));
      return false;
    }
    return true;
  }

  // #79：只在 integrity 已通过、连接已关闭之后调用。损坏主文件不得进 .bak。
  bool rotateFiles(const QString &primary, int keep, QString *error)
  {
    if (!QFile::exists(primary))
      return true;
    const QString bak = primary + QStringLiteral(".bak");
    keep = qBound(1, keep, 9);
    if (keep == 1)
    {
      for (int g = 2; g <= 9; ++g)
        QFile::remove(bak + QStringLiteral(".%1").arg(g));
    }
    else
    {
      QFile::remove(bak + QStringLiteral(".%1").arg(keep));
      for (int gen = keep - 1; gen >= 2; --gen)
      {
        const QString src = bak + QStringLiteral(".%1").arg(gen);
        if (QFile::exists(src))
          paleoReplaceFile(src, bak + QStringLiteral(".%1").arg(gen + 1));
      }
      if (QFile::exists(bak))
        paleoReplaceFile(bak, bak + QStringLiteral(".2"));
    }
    const QString bakTmp = bak + QStringLiteral(".tmp");
    QFile::remove(bakTmp);
    if (!QFile::copy(primary, bakTmp))
    {
      setError(error, QStringLiteral("cannot copy %1 to backup tmp %2").arg(primary, bakTmp));
      return false;
    }
    if (!paleoReplaceFile(bakTmp, bak))
    {
      QFile::remove(bakTmp);
      setError(error, QStringLiteral("cannot rotate %1 to %2").arg(primary, bak));
      return false;
    }
    return true;
  }

  void removeSqliteFamily(const QString &path)
  {
    QFile::remove(path);
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));
  }

  enum class JsonRead
  {
    Ok,
    Unsupported,
    Corrupt
  };

  JsonRead readJsonFile(const QString &path, CatalogStore::Tables *out, QString *error,
                        QString *parseDetail)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
      const QString why = f.errorString();
      if (parseDetail)
        *parseDetail = why;
      setError(error, QStringLiteral("cannot open catalog %1").arg(path));
      return JsonRead::Corrupt;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
    {
      if (parseDetail)
        *parseDetail = pe.errorString();
      setError(error, pe.errorString());
      return JsonRead::Corrupt;
    }
    QString ferr;
    if (!CatalogStore::fromJson(doc.object(), out, &ferr))
    {
      setError(error, ferr);
      if (parseDetail)
        *parseDetail = ferr;
      if (ferr.contains(QStringLiteral("unsupported catalog schema")))
        return JsonRead::Unsupported;
      return JsonRead::Corrupt;
    }
    return JsonRead::Ok;
  }

  QString migratedDest(const QString &jsonPath)
  {
    const QString base = jsonPath + QStringLiteral(".migrated");
    if (!QFileInfo::exists(base))
      return base;
    for (int i = 2; i < 100; ++i)
    {
      const QString alt = jsonPath + QStringLiteral(".migrated.%1").arg(i);
      if (!QFileInfo::exists(alt))
        return alt;
    }
    return jsonPath + QStringLiteral(".migrated-") +
           QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"));
  }

  // H2：「sqlite 文件在」≠「权威库在」。migrateFromJson（attachWritable 建库后
  // replaceAll 才 renameAside(json)）与 rewritePrimary（quarantine 先 remove 再
  // attachWritable）的崩溃窗口都会留下零长/无表 sqlite。把它当权威空库放行，
  // JSON/.bak 恢复链就永远不可达——工程以空目录成功打开，真实数据退化成盘上
  // 孤儿文件。开库前先探一次 sqlite_master（只读、不建表、不写 pragma）。
  // 只有 SchemaLess 一档改判：探不动（坏头/锁/WAL 只读不可读）时不夺权，
  // 照旧交给 openExisting 的可写连接与它既有的 recover 链——只读探针不得
  // 把「可写连接本可恢复的库」误判成坏库。
  enum class SqliteProbe
  {
    Missing,    // 文件不在
    Usable,     // 有 catalog 表——权威
    SchemaLess, // 能打开且 sqlite_master 可读，但零表
    Unreadable  // 打不开/读不出（坏头、锁、缺 entities 的异构库）
  };

  SqliteProbe probeSqlite(const QString &path, const QString &connectionName)
  {
    if (!QFileInfo::exists(path))
      return SqliteProbe::Missing;
    SqliteProbe probe = SqliteProbe::Unreadable;
    {
      QSqlDatabase db = MetaStore::openConnection(path, connectionName, nullptr, true,
                                                  kCatalogUserVersion);
      if (db.isValid() && db.isOpen())
      {
        QSqlQuery q(db);
        if (q.exec(QStringLiteral(
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name='entities'")) &&
            q.next())
          probe = SqliteProbe::Usable;
        else if (q.lastError().type() != QSqlError::NoError)
          probe = SqliteProbe::Unreadable; // 坏头等：sqlite_master 本身读不出
        else
        {
          // 有表但不是 catalog 的四表族：按读不出处理（不得当空库覆盖）。
          QSqlQuery any(db);
          probe = (any.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table'")) &&
                   !any.next())
                      ? SqliteProbe::SchemaLess
                      : SqliteProbe::Unreadable;
        }
      }
    }
    forgetConnection(connectionName);
    return probe;
  }

  // 「无表主库」时决定能否安全建空库：有 .bak 代或 json.migrated 说明数据可能
  // 还在恢复源里，必须先走 recover；一样都没有才按崩溃残留重建空工程。
  bool anyRecoverySource(const QString &sqlitePath, const QString &jsonPath)
  {
    for (int gen = 1; gen <= 9; ++gen)
    {
      const QString candidate = gen == 1 ? sqlitePath + QStringLiteral(".bak")
                                         : sqlitePath + QStringLiteral(".bak.%1").arg(gen);
      if (QFileInfo::exists(candidate))
        return true;
    }
    return QFileInfo::exists(jsonPath + QStringLiteral(".migrated"));
  }

  bool renameAside(const QString &src, const QString &dest)
  {
    if (!QFileInfo::exists(src))
      return true;
    if (QFile::rename(src, dest))
      return true;
    if (!QFile::copy(src, dest))
      return false;
    return QFile::remove(src);
  }
} // namespace

CatalogStore::~CatalogStore()
{
  close();
}

bool CatalogStore::createEmpty(Tables *out, QString *error)
{
  Tables tables;
  tables.meta.revision = 1;
  tables.meta.backupKeep = 3;
  if (!attachWritable(error))
  {
    removeSqliteFamily(m_sqlitePath);
    return false;
  }
  if (!replaceAll(tables, error))
  {
    close();
    removeSqliteFamily(m_sqlitePath);
    return false;
  }
  *out = tables;
  return true;
}

bool CatalogStore::migrateFromJson(Tables *out, QString *error)
{
  const QString jsonPath = jsonPathFor(m_projectDir);
  Tables parsed;
  QString parseDetail;
  QString perr;
  const JsonRead kind = readJsonFile(jsonPath, &parsed, &perr, &parseDetail);
  if (kind == JsonRead::Unsupported)
  {
    // 未来 JSON 不是损坏：不建 sqlite，不回退 .bak。
    setError(error, perr.contains(QStringLiteral("unsupported catalog schema"))
                        ? perr
                        : QStringLiteral("unsupported catalog schema"));
    return false;
  }

  bool fromBak = false;
  if (kind == JsonRead::Corrupt)
  {
    QString detail;
    bool got = false;
    for (int gen = 1; gen <= 9 && !got; ++gen)
    {
      const QString candidate = gen == 1 ? jsonPath + QStringLiteral(".bak")
                                         : jsonPath + QStringLiteral(".bak.%1").arg(gen);
      if (!QFileInfo::exists(candidate))
        continue;
      Tables loaded;
      QString berr;
      QString bdetail;
      const JsonRead br = readJsonFile(candidate, &loaded, &berr, &bdetail);
      if (br != JsonRead::Ok)
      {
        detail += QStringLiteral("%1: corrupt backup: %2; ")
                      .arg(candidate, bdetail.isEmpty() ? berr : bdetail);
        continue;
      }
      parsed = loaded;
      got = true;
    }
    if (!got)
    {
      setError(error, QStringLiteral("corrupt catalog %1: %2 (no usable %3[.2..9]: %4)")
                          .arg(jsonPath, parseDetail.isEmpty() ? perr : parseDetail,
                               jsonPath + QStringLiteral(".bak"),
                               detail.isEmpty() ? QStringLiteral("backup missing") : detail));
      return false;
    }
    fromBak = true;
    m_recovered = true;
    m_recoveryReason = QStringLiteral("%1: %2").arg(jsonPath, parseDetail);
  }

  if (!attachWritable(error))
  {
    removeSqliteFamily(m_sqlitePath);
    return false;
  }
  if (!replaceAll(parsed, error))
  {
    close();
    removeSqliteFamily(m_sqlitePath);
    return false;
  }

  if (fromBak)
  {
    const QString dest =
        jsonPath + QStringLiteral(".corrupt-") +
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"));
    if (!renameAside(jsonPath, dest))
      qWarning("catalog: cannot quarantine corrupt json %s", qPrintable(jsonPath));
  }
  else
  {
    const QString dest = migratedDest(jsonPath);
    if (!renameAside(jsonPath, dest))
      qWarning("catalog: cannot rename %s to %s", qPrintable(jsonPath), qPrintable(dest));
    else
      m_migratedFromJson = true;
  }
  *out = parsed;
  return true;
}

bool CatalogStore::recover(const QString &primaryError, Tables *out, QString *error)
{
  // 连接不得留在 .bak 上。只读打开备份，装进内存后立刻关掉。
  QString detail;
  bool anyBak = false;
  for (int gen = 1; gen <= 9; ++gen)
  {
    const QString candidate = gen == 1 ? m_sqlitePath + QStringLiteral(".bak")
                                       : m_sqlitePath + QStringLiteral(".bak.%1").arg(gen);
    if (!QFileInfo::exists(candidate))
      continue;
    anyBak = true;
    // 只读打开 WAL 库会要 -shm 写权限，等于改备份现场。拷到临时文件再读，
    // .bak 本体不打开、不改。
    const QString tmp = candidate + QStringLiteral(".read.tmp");
    removeSqliteFamily(tmp);
    if (!QFile::copy(candidate, tmp))
    {
      detail += QStringLiteral("%1: corrupt backup: cannot copy; ").arg(candidate);
      continue;
    }
    const QString name = catalogConnectionName(this, tmp, false);
    QString localErr;
    bool loadedOk = false;
    Tables loaded;
    {
      QSqlDatabase db = MetaStore::openConnection(tmp, name, &localErr, false,
                                                  kCatalogUserVersion);
      if (!db.isValid() || !db.isOpen())
      {
        // 打开失败：下面 forget。localErr 已填。
      }
      else
      {
        int epoch = 1;
        QString integ;
        if (!readEpoch(db, &epoch, &localErr))
          loadedOk = false;
        else if (epoch > kSchemaEpoch)
        {
          localErr = QStringLiteral("unsupported catalog schema epoch %1").arg(epoch);
          loadedOk = false;
        }
        else if (!integrityOk(db, &integ))
        {
          localErr = integ.isEmpty() ? QStringLiteral("integrity_check failed") : integ;
          loadedOk = false;
        }
        else
          loadedOk = loadTables(db, &loaded, &localErr);
      }
    }
    forgetConnection(name);
    removeSqliteFamily(tmp);
    if (!loadedOk)
    {
      detail += QStringLiteral("%1: corrupt backup: %2; ").arg(candidate, localErr);
      continue;
    }
    *out = loaded;
    m_recovered = true;
    m_recoveryReason = QStringLiteral("%1: %2").arg(m_sqlitePath, primaryError);
    m_open = false;
    m_writable = false;
    m_connectionName.clear();
    return true;
  }

  // 主库和 sqlite.bak 都不可用时，最后试 catalog.json / catalog.json.migrated。
  // 迁完后、下一次 open 快照之前崩溃，这两份还是上一代可读文本。
  const QString jsonPath = jsonPathFor(m_projectDir);
  const QStringList jsons{jsonPath, jsonPath + QStringLiteral(".migrated")};
  for (const QString &jp : jsons)
  {
    if (!QFileInfo::exists(jp))
      continue;
    Tables loaded;
    QString jerr;
    if (readJsonFile(jp, &loaded, &jerr, nullptr) != JsonRead::Ok)
      continue;
    *out = loaded;
    m_recovered = true;
    m_recoveryReason = QStringLiteral("%1: %2").arg(m_sqlitePath, primaryError);
    m_open = false;
    m_writable = false;
    m_connectionName.clear();
    return true;
  }

  setError(error, QStringLiteral("corrupt catalog %1: %2 (no usable %3[.2..9]: %4)")
                      .arg(m_sqlitePath, primaryError, m_sqlitePath + QStringLiteral(".bak"),
                           (!anyBak || detail.isEmpty()) ? QStringLiteral("backup missing")
                                                         : detail));
  return false;
}

bool CatalogStore::openExisting(bool readOnly, Tables *out, QString *error)
{
  QString openErr;
  if (!connectPrimary(readOnly, &openErr))
  {
    if (isSchemaReject(openErr))
    {
      setError(error, openErr);
      return false;
    }
    return recover(openErr, out, error);
  }

  enum class Step
  {
    Ready,
    Schema,
    Corrupt,
    Failed
  };
  Step step = Step::Ready;
  QString stepErr;
  int keep = 3;
  bool doRotate = false;
  {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    int epoch = 1;
    if (!readEpoch(db, &epoch, &stepErr))
      step = Step::Corrupt;
    else if (epoch > kSchemaEpoch)
    {
      step = Step::Schema;
      stepErr = QStringLiteral("unsupported catalog schema epoch %1").arg(epoch);
    }
    // A.1：integrity_check 必须先于任何会写盘的动作（WAL/journal pragma、
    // DDL）。否则损坏库一打开就被改写成「看似可写」，坏块被 pragma/DDL 的
    // 副产物盖住；且 pragma/DDL 失败原先被路由成硬错误直接 return false，
    // .bak/JSON 恢复链永远够不着——损坏库可能永远打不开工程。
    else if (!integrityOk(db, &stepErr))
      step = Step::Corrupt;
    else if (!readOnly && !applyWritablePragmas(db, &stepErr))
      step = Step::Corrupt; // 同上：写盘前置动作失败按损坏走 recover，不硬拒
    else if (!readOnly && !execScript(db, kSchemaSql, &stepErr))
      step = Step::Corrupt;
    else if (!readOnly)
    {
      ensureVersionNumberUniqueIndex(db); // #476：宽容唯一索引（存量重复行仅告警）
      {
        QSqlQuery q(db);
        if (q.exec(QStringLiteral(
                "SELECT value FROM catalog_meta WHERE key='backup_keep'")) &&
            q.next())
          keep = qBound(1, q.value(0).toString().toInt(), 9);
      }
      bool ckptOk = false;
      {
        QSqlQuery cq(db);
        if (cq.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")) && cq.next())
          ckptOk = cq.value(0).toInt() == 0;
      }
      if (!ckptOk)
        qWarning("catalog: wal_checkpoint failed for %s; skipping backup copy",
                 qPrintable(m_sqlitePath));
      else
        doRotate = true;
      if (!doRotate)
      {
        if (!loadTables(db, out, &stepErr))
          step = Step::Failed;
        else
        {
          m_open = true;
          m_writable = true;
        }
      }
    }
    else if (!loadTables(db, out, &stepErr))
      step = Step::Failed;
    else
    {
      m_open = true;
      m_writable = false;
    }
  }

  if (step == Step::Schema)
  {
    close();
    setError(error, stepErr);
    return false;
  }
  if (step == Step::Corrupt)
  {
    close();
    return recover(stepErr.isEmpty() ? QStringLiteral("integrity_check failed") : stepErr, out,
                   error);
  }
  if (step == Step::Failed)
  {
    close();
    setError(error, stepErr);
    return false;
  }
  if (!doRotate)
    return m_open;

  // 拷贝前必须关掉连接（Windows 共享锁）。integrity 已通过，坏库不会进 .bak。
  // 本次 open 新建的库不走这里（create / JSON 迁移直接返回）。
  const QString path = m_sqlitePath;
  close();
  QString rotErr;
  if (!rotateFiles(path, keep, &rotErr))
    qWarning("catalog: backup copy failed for %s: %s", qPrintable(path), qPrintable(rotErr));
  if (!attachWritable(error))
    return false;
  bool loaded = false;
  {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName);
    loaded = loadTables(db, out, error);
  }
  if (!loaded)
  {
    close();
    return false;
  }
  return true;
}

bool CatalogStore::openProject(const QString &projectDir, bool readOnly, Tables *out,
                               QString *error)
{
  close();
  m_recovered = false;
  m_recoveryReason.clear();
  m_migratedFromJson = false;
  m_projectDir = projectDir.trimmed();
  if (!out)
  {
    setError(error, QStringLiteral("catalog destination is null"));
    return false;
  }
  *out = Tables();
  if (m_projectDir.isEmpty())
  {
    setError(error, QStringLiteral("project directory is empty"));
    return false;
  }
  m_sqlitePath = sqlitePathFor(m_projectDir);
  const QString jsonPath = jsonPathFor(m_projectDir);
  const bool haveJson = QFileInfo::exists(jsonPath);
  const bool sqliteFilePresent = QFileInfo::exists(m_sqlitePath);
  // H2：权威性按「库可用」判，不按「文件在」判（详见 probeSqlite）。零长/无表
  // 库让位给 JSON 迁移 / .bak 恢复链；其余档位（含探不动）照旧走
  // openExisting——它自己的 recover 链负责坏库。
  const QString probeName = catalogConnectionName(this, m_sqlitePath, true);
  const bool schemaLessPrimary =
      sqliteFilePresent && probeSqlite(m_sqlitePath, probeName) == SqliteProbe::SchemaLess;
  const bool haveSqlite = sqliteFilePresent && !schemaLessPrimary;

  if (!haveSqlite)
  {
    if (readOnly)
    {
      if (!haveJson)
        return true; // #80：不建文件，查询为空
      QString parseDetail;
      QString perr;
      const JsonRead kind = readJsonFile(jsonPath, out, &perr, &parseDetail);
      if (kind == JsonRead::Ok)
        return true;
      if (kind == JsonRead::Unsupported)
      {
        setError(error, perr.contains(QStringLiteral("unsupported catalog schema"))
                            ? perr
                            : QStringLiteral("unsupported catalog schema"));
        *out = Tables();
        return false;
      }
      // 只读：可读 .bak 进内存，不隔离、不建 sqlite。
      QString detail;
      for (int gen = 1; gen <= 9; ++gen)
      {
        const QString candidate = gen == 1 ? jsonPath + QStringLiteral(".bak")
                                           : jsonPath + QStringLiteral(".bak.%1").arg(gen);
        if (!QFileInfo::exists(candidate))
          continue;
        Tables loaded;
        QString berr;
        QString bdetail;
        if (readJsonFile(candidate, &loaded, &berr, &bdetail) != JsonRead::Ok)
        {
          detail += QStringLiteral("%1: corrupt backup: %2; ")
                        .arg(candidate, bdetail.isEmpty() ? berr : bdetail);
          continue;
        }
        *out = loaded;
        m_recovered = true;
        m_recoveryReason = QStringLiteral("%1: %2").arg(jsonPath, parseDetail);
        return true;
      }
      *out = Tables();
      setError(error, QStringLiteral("corrupt catalog %1: %2 (no usable %3[.2..9]: %4)")
                          .arg(jsonPath, parseDetail.isEmpty() ? perr : parseDetail,
                               jsonPath + QStringLiteral(".bak"),
                               detail.isEmpty() ? QStringLiteral("backup missing") : detail));
      return false;
    }
    if (haveJson)
      return migrateFromJson(out, error);
    // 无表主库（零长/异构空库）且有恢复源：数据可能在 .bak 代或
    // json.migrated 里，先走 recover；一样都没有才按崩溃残留重建空工程
    // （这一档没有数据可丢——createEmpty 的崩溃窗口正是它的来源）。
    if (schemaLessPrimary && anyRecoverySource(m_sqlitePath, jsonPath))
      return recover(QStringLiteral("catalog sqlite has no tables"), out, error);
    return createEmpty(out, error);
  }
  return openExisting(readOnly, out, error);
}

bool CatalogStore::rewritePrimary(const Tables &tables, bool quarantineCorrupt, QString *error)
{
  const QString path = m_sqlitePath;
  if (path.isEmpty())
  {
    setError(error, QStringLiteral("catalog sqlite path is empty"));
    return false;
  }
  close();
  if (quarantineCorrupt)
  {
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"));
    const QStringList sources{path, path + QStringLiteral("-wal"), path + QStringLiteral("-shm")};
    for (const QString &src : sources)
    {
      if (!QFileInfo::exists(src))
        continue;
      QString dest;
      if (src.endsWith(QStringLiteral("-wal")))
        dest = path + QStringLiteral("-wal.corrupt-") + stamp;
      else if (src.endsWith(QStringLiteral("-shm")))
        dest = path + QStringLiteral("-shm.corrupt-") + stamp;
      else
        dest = path + QStringLiteral(".corrupt-") + stamp;
      if (!QFileInfo::exists(dest) && !QFile::copy(src, dest))
      {
        setError(error, QStringLiteral("cannot quarantine %1").arg(src));
        return false;
      }
      if (!QFile::remove(src))
      {
        setError(error, QStringLiteral("cannot remove quarantined %1").arg(src));
        return false;
      }
    }
  }
  m_sqlitePath = path;
  if (!attachWritable(error))
    return false;
  if (!replaceAll(tables, error))
    return false;
  m_recovered = false;
  m_recoveryReason.clear();
  return true;
}
