// 层：数据
#include "io/shaequals.h"
#include "mapversionstore.h"
#include "mapsnapshot.h"
#include "storeerrors_internal.h"
#include "metastore.h"
#include "managedpath.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDebug>
#include <QRegularExpression>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QSet>
#include <QUuid>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace
{
using paleo::store_detail::setError;

  // readOnly 实例走独立连接名（QSQLITE_OPEN_READONLY，#80）。
  QString mapVersionConnectionName( const QString &path, bool readOnly )
  {
    return QStringLiteral( "paleo_mapversions_" ) + QString::number( qHash( path ) ) +
           ( readOnly ? QStringLiteral( "_ro" ) : QString() );
  }

  // 逐列补建：PRAGMA table_info 查缺 → ALTER TABLE ADD COLUMN（可空列）。
  // CREATE TABLE IF NOT EXISTS 对既有表不加列，新 schema 靠这里前向升级。
  bool ensureColumn( QSqlDatabase &db, const QString &table, const QString &name,
                     const QString &type, QString *error )
  {
    QSqlQuery info( db );
    if ( !info.exec( QStringLiteral( "PRAGMA table_info(%1)" ).arg( table ) ) )
    {
      setError( error, info.lastError().text() );
      return false;
    }
    while ( info.next() )
      if ( info.value( 1 ).toString() == name )
        return true; // 已有该列
    info.finish();
    QSqlQuery alter( db );
    if ( !alter.exec( QStringLiteral( "ALTER TABLE %1 ADD COLUMN %2 %3" )
                          .arg( table, name, type ) ) )
    {
      setError( error, alter.lastError().text() );
      return false;
    }
    return true;
  }

  // 幂等建表 + 逐列补建（既有库补列：旧行一律 NULL → 读侧归一为未发布，
  // 不回写）。#736：本函数即 MetaStore 的迁移步（幂等，可按步重复调用），
  // 由 openConnection 在版本落后时于事务内逐级执行、版本已到位时作 ensure。
  bool mapVersionEnsureSchema(QSqlDatabase &db, QString *error)
  {
    QSqlQuery schema( db );
    if ( !schema.exec( QStringLiteral( "CREATE TABLE IF NOT EXISTS map_versions("
                                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                       "horizon TEXT NOT NULL,"
                                       "version INTEGER NOT NULL,"
                                       "provenance TEXT NOT NULL,"
                                       "state TEXT NOT NULL DEFAULT 'Editing',"
                                       "published_path TEXT,"
                                       "created_utc TEXT NOT NULL,"
                                       "pdf_asset_id TEXT,"
                                       "pdf_sha256 TEXT,"
                                       "residual_summary TEXT)" ) ) )
    {
      setError( error, schema.lastError().text() );
      return false;
    }
    if ( !schema.exec( QStringLiteral( "CREATE TABLE IF NOT EXISTS map_products("
                                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                       "horizon TEXT NOT NULL,"
                                       "kind TEXT NOT NULL,"
                                       "path TEXT NOT NULL,"
                                       "created_utc TEXT NOT NULL,"
                                       "asset_id TEXT,"
                                       "sha256 TEXT)" ) ) )
    {
      setError( error, schema.lastError().text() );
      return false;
    }
    if ( !schema.exec( QStringLiteral( "CREATE UNIQUE INDEX IF NOT EXISTS idx_map_versions_horizon_version "
                                       "ON map_versions(horizon, version)" ) ) )
    {
      setError( error, schema.lastError().text() );
      return false;
    }
    for ( const auto &col : { QStringLiteral( "pdf_asset_id" ),
                              QStringLiteral( "pdf_sha256" ),
                              QStringLiteral( "residual_summary" ) } )
      if ( !ensureColumn( db, QStringLiteral( "map_versions" ), col,
                          QStringLiteral( "TEXT" ), error ) )
        return false;
    for ( const auto &col : { QStringLiteral( "asset_id" ), QStringLiteral( "sha256" ) } )
      if ( !ensureColumn( db, QStringLiteral( "map_products" ), col,
                          QStringLiteral( "TEXT" ), error ) )
        return false;
    return true;
  }

  // Lazily opens the connection and guarantees both tables (idempotent,
  // forward-compatible: absent columns keep NULL defaults on old rows).
  bool mapVersionEnsureOpen( const QString &path, bool readOnly, QString *error )
  {
    // PW-111：QSqlDatabase 连接线程绑定——跨线程复用是未定义行为（Qt 只给一条
    // 警告并返回无效连接，下游被误报成「驱动不可用」）。登记连接所属线程，
    // 跨线程调用如实失败。
    {
      static QMutex ownersMutex;
      static QHash<QString, QThread *> owners;
      const QString name = mapVersionConnectionName( path, readOnly );
      QMutexLocker lock( &ownersMutex );
      const auto it = owners.constFind( name );
      if ( it != owners.constEnd() && it.value() != QThread::currentThread() &&
           QSqlDatabase::contains( name ) )
      {
        setError( error, QStringLiteral( "地图版本库连接 %1 属于另一线程，拒绝跨线程使用" ).arg( name ) );
        return false;
      }
      owners.insert( name, QThread::currentThread() ); // 连接已移除则改归当前线程
    }
    QSqlDatabase db = MetaStore::openConnection(
        path, mapVersionConnectionName( path, readOnly ), error, readOnly,
        MetaStore::kUserVersion,
        []( QSqlDatabase &db, int, int, QString *err ) { return mapVersionEnsureSchema( db, err ); } );
    if ( !db.isValid() )
      return false;
    // 只读实例不建表/不补列（#80）——schema 归写实例；可写路径的 schema 已在
    // openConnection 内确保（版本落后时在事务内逐级迁移，#736）。
    return true;
  }

  MapVersion rowToVersion( const QSqlQuery &q )
  {
    MapVersion v;
    v.id = q.value( 0 ).toInt();
    v.horizon = q.value( 1 ).toString();
    v.version = q.value( 2 ).toInt();
    v.provenance = q.value( 3 ).toString();
    const QString rawState = q.value( 4 ).toString();
    v.publishedPath = q.value( 5 ).toString();
    v.createdUtc = q.value( 6 ).toString();
    v.pdfAssetId = q.value( 7 ).toString();
    v.pdfSha256 = q.value( 8 ).toString();
    v.residualSummary = q.value( 9 ).toString();
    // 旧 schema 行（pdf_asset_id 为空）读成未发布，不回写（§177/§260）——
    // 新语义的 Published 必然带 PDF 资产 id，缺 id 的历史行不是有效发布。
    v.state = ( rawState == QLatin1String( "Published" ) && !v.pdfAssetId.isEmpty() )
                  ? QStringLiteral( "Published" )
                  : QStringLiteral( "Editing" );
    return v;
  }

  QString nowUtc()
  {
    return QDateTime::currentDateTimeUtc().toString( Qt::ISODateWithMs );
  }

  // Copy a file into the snapshot and drop every write bit — Published 快照只读.
  bool copyReadOnly( const QString &src, const QString &dst, QString *error )
  {
    if ( QFile::exists( dst ) )
      QFile::remove( dst );
    if ( !QFile::copy( src, dst ) )
    {
      setError( error, QStringLiteral( "cannot snapshot %1 → %2" ).arg( src, dst ) );
      return false;
    }
    QFile::setPermissions( dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                    QFileDevice::ReadGroup | QFileDevice::ReadOther );
    return true;
  }

  QString sanitizedFileName( const QString &layerId )
  {
    QString name = layerId;
    name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
    return name.isEmpty() ? QStringLiteral("layer") : name;
  }

  // PW-023：数据集 = 主文件 + 同目录 sidecar（.shx/.dbf/.prj/.aux.xml/世界文件…）。
  // 返回 (尾缀, 绝对路径)，尾缀是文件名去掉主文件「基名」后的部分（如 ".dbf"、
  // ".tif.aux.xml"），冻结/发布时拼到目标基名后保持 GDAL/OGR 可发现。
  QList<QPair<QString, QString>> datasetSidecars(const QString &base)
  {
    QList<QPair<QString, QString>> out;
    const QFileInfo bi(base);
    const QString stem = bi.completeBaseName();
    const QString suffix = bi.suffix().toLower();
    static const QStringList kTails = {
        ".shx", ".dbf", ".prj", ".cpg", ".qix", ".sbn", ".sbx", ".shp.xml",
        ".tfw", ".tifw", ".tiffw", ".wld", ".jgw", ".pgw", ".gfw", ".qmd", ".qml"};
    const QDir dir = bi.absoluteDir();
    const QStringList entries = dir.entryList(QDir::Files);
    for (const QString &fn : entries) {
      if (fn.compare(bi.fileName(), Qt::CaseInsensitive) == 0) continue;
      if (fn.size() <= stem.size() || !fn.startsWith(stem + '.', Qt::CaseInsensitive)) continue;
      const QString tail = fn.mid(stem.size());
      const QString lt = tail.toLower();
      const bool known = kTails.contains(lt) ||
          (!suffix.isEmpty() && (lt == "." + suffix + ".aux.xml" || lt == "." + suffix + ".ovr" ||
                                 lt == "." + suffix + ".msk"));
      if (known) out.append({tail, dir.filePath(fn)});
    }
    return out;
  }

  QString frozenStem(const QString &name)
  {
    const int dot = name.lastIndexOf('.');
    return dot > 0 ? name.left(dot) : name;
  }

  // PW-023：legacy（非冻结）发布路径的图层文件清单单一实现——native
  // publish() 与 prepared snapshotInputForPublish() 共用，避免两侧「伴生
  // 文件收不收 / 缺失图层跳不跳」口径分叉（此前 prepared 路径只收主文件，
  // shapefile 发布快照静默缺 .dbf/.shx/.prj）。
  // 语义与 native 历史口径一致：base 为空或不在盘的图层在 legacy 口径下
  // 跳过（冻结路径「缺源即拒」——旧行没有不可变清单，兼容读取优先）。
  struct LegacyPublishLayer
  {
    QString layerId;
    QString base;                                 // 主文件绝对路径
    QList<QPair<QString, QString>> sidecars;      // (尾缀, 绝对路径)
  };
  QVector<LegacyPublishLayer> legacyPublishLayers(const QString &horizon,
                                                  const QVector<LayerDeclaration> &decls)
  {
    QVector<LegacyPublishLayer> out;
    for (const auto &d : decls)
    {
      if (d.horizon != horizon)
        continue;
      const QString base = d.source.section('|', 0, 0);
      if (base.isEmpty() || !QFile::exists(base))
        continue;
      LegacyPublishLayer layer{d.layerId, base, datasetSidecars(base)};
      out.append(layer);
    }
    return out;
  }

  QString snapshotHash(QJsonObject provenance) {
    provenance.remove(QStringLiteral("snapshot_hash"));
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(provenance).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
  }

  const char kSelectCols[] =
      "id,horizon,version,provenance,state,published_path,created_utc,"
      "pdf_asset_id,pdf_sha256,residual_summary";

  // Same file schema as PaleoProjectStore's commit journal, written here so
  // the metadata store does not include the project store. commitAll ignores
  // stages outside queued/catalog_done/qgz_done/complete, so a publish op is
  // never replayed as a catalog commit.
  bool safePublicationOpId(const QString &opId) {
    if (opId.isEmpty() || opId.size() > 200 || opId.contains(QLatin1String(".."))) return false;
    for (const QChar c : opId) {
      if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':')) return false;
      const ushort u = c.unicode();
      if (u < 0x20 || u == 0x7F) return false;
    }
    return true;
  }
  bool writePublicationJournal(const QString &directory, const QString &opId, const QString &stage,
                               const QString &digest, QString *error) {
    if (!safePublicationOpId(opId) || stage.isEmpty()) {
      setError(error, QStringLiteral("发布记档标识无效")); return false;
    }
    if (!QDir().mkpath(directory)) {
      setError(error, QStringLiteral("无法创建发布记档目录 %1").arg(directory)); return false;
    }
    QJsonObject entry;
    entry.insert(QStringLiteral("journal_version"), 1);
    entry.insert(QStringLiteral("op_id"), opId);
    entry.insert(QStringLiteral("stage"), stage);
    entry.insert(QStringLiteral("input_digest"), digest);
    entry.insert(QStringLiteral("updated_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    const QString path = QDir(directory).filePath(opId + QStringLiteral(".json"));
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const QByteArray bytes = QJsonDocument(entry).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size() || !file.commit()) {
      file.cancelWriting();
      setError(error, QStringLiteral("无法写入发布记档 %1").arg(path));
      return false;
    }
    return true;
  }
  QString publicationDigest(int versionId, int versionNumber, const QString &horizon,
                            const QString &stagingDirectory, const QString &finalDirectory,
                            const QString &residualSummary, const QString &manifestSha) {
    const QJsonObject digest{{QStringLiteral("version_id"), versionId},
                             {QStringLiteral("horizon"), horizon},
                             {QStringLiteral("version_number"), versionNumber},
                             {QStringLiteral("staging_directory"), stagingDirectory},
                             {QStringLiteral("final_directory"), finalDirectory},
                             {QStringLiteral("residual_summary"), residualSummary},
                             {QStringLiteral("manifest_sha256"), manifestSha}};
    return QString::fromUtf8(QJsonDocument(digest).toJson(QJsonDocument::Compact));
  }
  bool pathInsideProject(const QString &projectDir, const QString &path) {
    if (path.isEmpty() || path.contains(QLatin1String(".."))) return false;
    const QString absolute = QFileInfo(path).absoluteFilePath();
    const QString root = QDir(projectDir).absolutePath();
    return absolute == root || absolute.startsWith(root + QLatin1Char('/'));
  }
} // namespace

MapVersionStore::MapVersionStore( const QString &metaSqlitePath )
  : m_dbPath( metaSqlitePath )
{
}

bool MapVersionStore::open( QString *error )
{
  if (!mapVersionEnsureOpen(m_dbPath, m_readOnly, error)) return false;
  // A hash mismatch or an unreadable journal is reported and left on disk.
  // Opening the project must still succeed.
  if (!m_readOnly) recoverInterruptedPublications();
  return true;
}

int MapVersionStore::currentVersion( const QString &horizon ) const
{
  return latest( horizon ).version;
}

MapVersion MapVersionStore::latest( const QString &horizon, QString *error ) const
{
  if ( !mapVersionEnsureOpen( m_dbPath, m_readOnly, error ) )
    return MapVersion();
  QSqlQuery q( QSqlDatabase::database( mapVersionConnectionName( m_dbPath, m_readOnly ) ) );
  q.prepare( QStringLiteral( "SELECT " ) + QLatin1String( kSelectCols ) +
             QStringLiteral( " FROM map_versions WHERE horizon=? ORDER BY version DESC LIMIT 1" ) );
  q.addBindValue( horizon );
  if ( !q.exec() )
  {
    // PW-111：SQL 失败如实上报，不与「无版本」混同。
    setError( error, q.lastError().text() );
    return MapVersion();
  }
  if ( !q.next() )
    return MapVersion();
  return rowToVersion( q );
}

QVector<MapVersion> MapVersionStore::versions( const QString &horizon ) const
{
  QVector<MapVersion> out;
  if ( !mapVersionEnsureOpen( m_dbPath, m_readOnly, nullptr ) )
    return out;
  QSqlQuery q( QSqlDatabase::database( mapVersionConnectionName( m_dbPath, m_readOnly ) ) );
  q.prepare( QStringLiteral( "SELECT " ) + QLatin1String( kSelectCols ) +
             QStringLiteral( " FROM map_versions WHERE horizon=? ORDER BY version" ) );
  q.addBindValue( horizon );
  if ( !q.exec() )
    return out;
  while ( q.next() )
    out.append( rowToVersion( q ) );
  return out;
}

MapVersion MapVersionStore::saveVersion( const QString &horizon, const QString &provenanceJson,
                                         QString *error, const QVector<LayerDeclaration> &layers )
{
  if ( m_readOnly )
  {
    setError( error, QStringLiteral("工程目录被另一个实例锁定——本实例只读，版本写入被拒绝") );
    return MapVersion();
  }
  if ( !mapVersionEnsureOpen( m_dbPath, m_readOnly, error ) )
    return MapVersion();

  QSqlDatabase db = QSqlDatabase::database( mapVersionConnectionName( m_dbPath, m_readOnly ) );

  // 最近一次登记的 PDF 产物引用（asset_id + sha256）抄进新版本行；
  // 导出不改已冻结的版本行——下一次保存才继承产物引用（§260）。
  QString pdfAssetId, pdfSha256;
  {
    QSqlQuery prod( db );
    prod.prepare( QStringLiteral( "SELECT asset_id,sha256 FROM map_products"
                                  " WHERE horizon=? AND kind='pdf' ORDER BY id DESC LIMIT 1" ) );
    prod.addBindValue( horizon );
    if ( prod.exec() && prod.next() )
    {
      pdfAssetId = prod.value( 0 ).toString();
      pdfSha256 = prod.value( 1 ).toString();
    }
  }

  // The saved revision owns immutable bytes; publication never reads a later working file.
  QJsonParseError parseError;
  const auto provenanceDocument = QJsonDocument::fromJson(provenanceJson.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !provenanceDocument.isObject()) {
    setError(error, QStringLiteral("地图版本来源记录 JSON 损坏，原记录未改写")); return {};
  }
  auto provenance = provenanceDocument.object();
  const QString projectDir = QFileInfo(m_dbPath).absolutePath();
  QString freezeDir;
  // #439：未传入图层的保存保持旧发布路径（发布时仍按调用方 decls 复制）。
  // 只有真正传入 layers 的保存才写入 snapshot_schema / saved_layers 不可变清单。
  if (!layers.isEmpty()) {
    freezeDir = QDir(projectDir).filePath("artifacts/map-versions/" +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QJsonArray savedLayers;
    const auto freeze = [&](const QString &source, const QString &name, QJsonObject &record) {
      const QString expected = paleo::managedpath::sha256FileHex(source, error);
      if (expected.isEmpty()) return false;
      if (!QDir().mkpath(freezeDir) ||
          !copyReadOnly(source, QDir(freezeDir).filePath(name), error)) return false;
      const QString path = QDir(freezeDir).filePath(name);
      const QString sha = paleo::managedpath::sha256FileHex(path, error);
      if (sha.isEmpty()) return false;
      if (sha != expected) {
        setError(error, QStringLiteral("保存快照时来源文件发生变化：%1").arg(source)); return false;
      }
      record.insert("path", QDir(projectDir).relativeFilePath(path));
      record.insert("sha256", sha);
      return true;
    };
    for (const auto &decl : layers) {
      if (decl.horizon != horizon) continue;
      const QString base = decl.source.section('|', 0, 0);
      if (!QFileInfo(base).isFile()) {
        // An unavailable optional/reference layer remains explicitly visible in the manifest.
        savedLayers << QJsonObject{{"layer_id", decl.layerId}, {"missing", true},
                                  {"original_source", decl.source}};
        continue;
      }
      QJsonObject record{{"layer_id", decl.layerId}, {"horizon", horizon}, {"type", decl.type},
                         {"source_suffix", decl.source.mid(base.size())}};
      const QString name = QString::number(savedLayers.size()) + "_" +
                           sanitizedFileName(decl.layerId) + "." + QFileInfo(base).suffix();
      if (!freeze(base, name, record)) {
        QDir(freezeDir).removeRecursively();
        return {};
      }
      QJsonArray sidecars;
      for (const auto &sc : datasetSidecars(base)) {
        QJsonObject scRecord{{"suffix", sc.first}};
        if (!freeze(sc.second, frozenStem(name) + sc.first, scRecord)) {
          QDir(freezeDir).removeRecursively();
          return {};
        }
        sidecars << scRecord;
      }
      if (!sidecars.isEmpty()) record.insert("sidecars", sidecars);
      savedLayers << record;
    }
    QJsonObject savedPdf;
    if (!pdfAssetId.isEmpty()) {
      QSqlQuery product(db);
      product.prepare("SELECT path FROM map_products WHERE horizon=? AND kind='pdf' AND asset_id=? ORDER BY id DESC LIMIT 1");
      product.addBindValue(horizon); product.addBindValue(pdfAssetId);
      if (product.exec() && product.next() && QFileInfo(product.value(0).toString()).isFile()) {
        if (!freeze(product.value(0).toString(), "layout.pdf", savedPdf)) {
          QDir(freezeDir).removeRecursively();
          return {};
        }
        // Historical integrations used short test hashes; enforce full scientific fingerprints.
        if (pdfSha256.size() == 64 && !paleo::io::shaEquals(savedPdf.value("sha256").toString(), pdfSha256)) {
          setError(error, QStringLiteral("PDF 内容与已登记指纹不一致，请重新导出后保存"));
          QDir(freezeDir).removeRecursively();
          return {};
        }
      }
    }
    provenance.insert("snapshot_schema", "paleo_map_snapshot_v1");
    provenance.insert("saved_layers", savedLayers);
    provenance.insert("saved_pdf", savedPdf);
    provenance.insert("snapshot_hash", snapshotHash(provenance));
  }

  if (!db.transaction()) {
    setError(error, db.lastError().text());
    if (!freezeDir.isEmpty()) QDir(freezeDir).removeRecursively();
    return {};
  }
  bool stored = false;
  const auto rollback = qScopeGuard([&] {
    if (!stored) {
      db.rollback();
      if (!freezeDir.isEmpty()) QDir(freezeDir).removeRecursively();
    }
  });
  QSqlQuery q( db );
  q.prepare( QStringLiteral( "INSERT INTO map_versions(horizon,version,provenance,state,"
                             "created_utc,pdf_asset_id,pdf_sha256)"
                             " VALUES(?,?,?,'Editing',?,?,?)"
                             " RETURNING id" ) );
  q.addBindValue( horizon );
  q.addBindValue( latest( horizon ).version + 1 );
  q.addBindValue( QString::fromUtf8(QJsonDocument(provenance).toJson(QJsonDocument::Compact)) );
  q.addBindValue( nowUtc() );
  q.addBindValue( pdfAssetId.isEmpty() ? QVariant() : QVariant( pdfAssetId ) );
  q.addBindValue( pdfSha256.isEmpty() ? QVariant() : QVariant( pdfSha256 ) );
  if ( !q.exec() || !q.next() )
  {
    setError( error, q.lastError().text() );
    if (!freezeDir.isEmpty()) QDir(freezeDir).removeRecursively();
    return MapVersion();
  }
  const int id = q.value( 0 ).toInt();
  q.finish();

  QSqlQuery sel( db );
  sel.prepare( QStringLiteral( "SELECT " ) + QLatin1String( kSelectCols ) +
               QStringLiteral( " FROM map_versions WHERE id=?" ) );
  sel.addBindValue( id );
  if ( !sel.exec() || !sel.next() )
  {
    setError( error, sel.lastError().text() );
    return MapVersion();
  }
  const auto result = rowToVersion(sel);
  sel.finish();
  if (!db.commit()) { setError(error, db.lastError().text()); return {}; }
  stored = true;
  return result;
}

bool MapVersionStore::recordLayoutProduct( const QString &horizon, const QString &pdfPath,
                                           const QString &assetId, const QString &sha256,
                                           QString *error )
{
  if ( m_readOnly )
  {
    setError( error, QStringLiteral("工程目录被另一个实例锁定——本实例只读，产物登记被拒绝") );
    return false;
  }
  if ( !mapVersionEnsureOpen( m_dbPath, m_readOnly, error ) )
    return false;
  QSqlQuery q( QSqlDatabase::database( mapVersionConnectionName( m_dbPath, m_readOnly ) ) );
  q.prepare( QStringLiteral( "INSERT INTO map_products(horizon,kind,path,created_utc,asset_id,sha256)"
                             " VALUES(?,'pdf',?,?,?,?)" ) );
  q.addBindValue( horizon );
  q.addBindValue( pdfPath );
  q.addBindValue( nowUtc() );
  q.addBindValue( assetId.isEmpty() ? QVariant() : QVariant( assetId ) );
  q.addBindValue( sha256.isEmpty() ? QVariant() : QVariant( sha256 ) );
  if ( !q.exec() )
  {
    setError( error, q.lastError().text() );
    return false;
  }
  return true;
}

bool MapVersionStore::hasLayoutProduct( const QString &horizon, QString *error ) const
{
  if ( !mapVersionEnsureOpen( m_dbPath, m_readOnly, error ) )
    return false;
  QSqlQuery q( QSqlDatabase::database( mapVersionConnectionName( m_dbPath, m_readOnly ) ) );
  q.prepare( QStringLiteral( "SELECT COUNT(*) FROM map_products WHERE horizon=? AND kind='pdf'" ) );
  q.addBindValue( horizon );
  if ( !q.exec() || !q.next() )
  {
    // PW-111：SQL 失败如实上报，不与「无产物」混同。
    setError( error, q.lastError().text() );
    return false;
  }
  return q.value( 0 ).toInt() > 0;
}

QString MapVersionStore::latestLayoutProduct( const QString &horizon, QString *error ) const
{
  if ( !mapVersionEnsureOpen( m_dbPath, m_readOnly, error ) )
    return QString();
  QSqlQuery q( QSqlDatabase::database( mapVersionConnectionName( m_dbPath, m_readOnly ) ) );
  q.prepare( QStringLiteral( "SELECT path FROM map_products"
                             " WHERE horizon=? AND kind='pdf' ORDER BY id DESC LIMIT 1" ) );
  q.addBindValue( horizon );
  if ( !q.exec() || !q.next() )
  {
    // PW-111：SQL 失败如实上报，不与「无产物」混同。
    setError( error, q.lastError().text() );
    return QString();
  }
  return q.value( 0 ).toString();
}

bool MapVersionStore::residualSummaryComplete( const QString &summaryJson,
                                               int *covered, int *total )
{
  int cov = -1, tot = -1, missing = -1;
  const QJsonDocument doc = QJsonDocument::fromJson( summaryJson.toUtf8() );
  if ( doc.isObject() )
  {
    const QJsonObject o = doc.object();
    tot = o.value( QLatin1String( "wells_total" ) ).toInt( -1 );
    cov = o.value( QLatin1String( "covered" ) ).toInt( -1 );
    missing = o.value( QLatin1String( "missing" ) ).toArray().size();
  }
  if ( covered )
    *covered = cov;
  if ( total )
    *total = tot;
  return tot > 0 && cov == tot && missing == 0;
}

int MapVersionStore::requiredNumericResiduals( int wellsTotal )
{
  if ( wellsTotal <= 0 )
    return 0;
  return qMax( 1, wellsTotal * 3 / 4 );
}

int MapVersionStore::numericResidualCount( const QString &summaryJson )
{
  const QJsonDocument doc = QJsonDocument::fromJson( summaryJson.toUtf8() );
  if ( !doc.isObject() )
    return 0;
  int n = 0;
  const QJsonArray rows = doc.object().value( QLatin1String( "rows" ) ).toArray();
  for ( const QJsonValue &v : rows )
    if ( v.toObject().value( QLatin1String( "kind" ) ).toString() == QLatin1String( "residual" ) )
      ++n;
  return n;
}

QString MapVersionStore::commitPublicationPlacement(int versionId, int versionNumber, const QString &horizon,
                                                    const QString &stagingDirectory, const QString &finalDirectory,
                                                    const QString &residualSummary, QString *error) {
  if (versionId <= 0 || versionNumber <= 0 || stagingDirectory.isEmpty() || finalDirectory.isEmpty() ||
      stagingDirectory == finalDirectory || !stagingDirectory.contains(QLatin1String(".staging-")) ||
      !pathInsideProject(projectDirectory(), stagingDirectory) || !pathInsideProject(projectDirectory(), finalDirectory)) {
    setError(error, QStringLiteral("发布快照无法原子落位：%1").arg(finalDirectory));
    return {};
  }
  const QString manifestSha = paleo::managedpath::sha256FileHex(
      QDir(stagingDirectory).filePath(QStringLiteral("evidence-snapshot.json")), error);
  if (manifestSha.size() != 64) {
    if (error && error->isEmpty()) setError(error, QStringLiteral("发布清单指纹无法读取"));
    return {};
  }
  const QString opId = QStringLiteral("map-publish-%1").arg(versionId);
  const QString digest = publicationDigest(versionId, versionNumber, horizon, stagingDirectory, finalDirectory,
                                           residualSummary, manifestSha);
  const QString journalDir = QDir(projectDirectory()).filePath(QStringLiteral("artifacts/metadata/commit_journal"));
  auto journal = [&](const QString &stage) {
    QString journalError;
    const bool wrote = writePublicationJournal(journalDir, opId, stage, digest, &journalError);
    if (!wrote && error && error->isEmpty()) *error = journalError;
    return wrote;
  };
  if (QFileInfo::exists(finalDirectory)) {
    // Leave any existing journal alone. A mismatched result must not be marked
    // complete, and it must not be deleted.
    setError(error, QStringLiteral("发布快照无法原子落位：%1").arg(finalDirectory));
    return {};
  }
  if (!journal(QStringLiteral("publish_prepared"))) return {};
  if (!QDir().rename(stagingDirectory, finalDirectory)) {
    // publish_prepared plus a missing final directory is cleaned on reopen.
    setError(error, QStringLiteral("发布快照无法原子落位：%1").arg(finalDirectory));
    return {};
  }
  if (!journal(QStringLiteral("publish_renamed"))) {
    if (error && error->isEmpty())
      setError(error, QStringLiteral("发布记档无法落盘，重开工程后会补登记"));
    return {};
  }
  if (m_renameProbe) m_renameProbe();
  QSqlQuery update(QSqlDatabase::database(mapVersionConnectionName(m_dbPath, m_readOnly)));
  update.prepare(QStringLiteral("UPDATE map_versions SET state='Published', published_path=?, residual_summary=?"
                                " WHERE id=? AND state!='Published'"));
  update.addBindValue(finalDirectory);
  update.addBindValue(residualSummary);
  update.addBindValue(versionId);
  if (!update.exec() || update.numRowsAffected() != 1) {
    const QString sqlError = update.lastError().text();
    update.finish();
    QSqlQuery current(QSqlDatabase::database(mapVersionConnectionName(m_dbPath, m_readOnly)));
    current.prepare(QStringLiteral("SELECT state, published_path FROM map_versions WHERE id=?"));
    current.addBindValue(versionId);
    const bool alreadyThisResult = current.exec() && current.next() &&
        current.value(0).toString() == QLatin1String("Published") &&
        QFileInfo(current.value(1).toString()).absoluteFilePath() == QFileInfo(finalDirectory).absoluteFilePath();
    if (alreadyThisResult) {
      QString ignored;
      writePublicationJournal(journalDir, opId, QStringLiteral("complete"), digest, &ignored);
      if (error) error->clear();
      return finalDirectory;
    }
    // This attempt just renamed the directory. Remove only that directory so
    // a retry can publish again. Mark the journal complete so recovery does
    // not register a directory that no longer exists. Do not point the
    // caller's staging guard at the final path.
    QDir(finalDirectory).removeRecursively();
    journal(QStringLiteral("complete"));
    setError(error, sqlError.isEmpty() ? QStringLiteral("发布登记失败") : sqlError);
    return {};
  }
  QString ignored;
  if (!writePublicationJournal(journalDir, opId, QStringLiteral("complete"), digest, &ignored))
    qWarning() << "MapVersionStore: publication registered but journal completion failed" << opId << ignored;
  if (error) error->clear();
  return finalDirectory;
}

void MapVersionStore::recoverInterruptedPublications() {
  if (m_readOnly || m_dbPath.isEmpty()) return;
  const QString projectDir = projectDirectory();
  const QString journalDir = QDir(projectDir).filePath(QStringLiteral("artifacts/metadata/commit_journal"));
  QDir dir(journalDir);
  if (!dir.exists()) return;
  QSet<QString> publishedPaths;
  {
    QSqlQuery paths(QSqlDatabase::database(mapVersionConnectionName(m_dbPath, m_readOnly)));
    if (paths.exec(QStringLiteral("SELECT published_path FROM map_versions"
                                  " WHERE published_path IS NOT NULL AND published_path != ''")))
      while (paths.next()) publishedPaths.insert(QFileInfo(paths.value(0).toString()).absoluteFilePath());
  }
  const QStringList names = dir.entryList({QStringLiteral("map-publish-*.json")}, QDir::Files, QDir::Name);
  for (const QString &name : names) {
    const QString path = dir.filePath(name);
    const QString opId = QFileInfo(path).completeBaseName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
      qWarning() << "MapVersionStore: unreadable publication journal" << path;
      continue;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      qWarning() << "MapVersionStore: corrupt publication journal" << path;
      continue;
    }
    const QJsonObject header = document.object();
    const QString stage = header.value(QStringLiteral("stage")).toString();
    if (header.value(QStringLiteral("op_id")).toString() != opId || stage.isEmpty()) {
      qWarning() << "MapVersionStore: corrupt publication journal" << path;
      continue;
    }
    if (stage == QLatin1String("complete")) continue;
    QJsonParseError digestError;
    const QJsonDocument digestDocument = QJsonDocument::fromJson(
        header.value(QStringLiteral("input_digest")).toString().toUtf8(), &digestError);
    if (digestError.error != QJsonParseError::NoError || !digestDocument.isObject()) {
      qWarning() << "MapVersionStore: publication journal digest is unreadable" << opId;
      continue;
    }
    const QJsonObject digest = digestDocument.object();
    const QString horizon = digest.value(QStringLiteral("horizon")).toString();
    const int versionId = digest.value(QStringLiteral("version_id")).toInt();
    const int versionNumber = digest.value(QStringLiteral("version_number")).toInt();
    const QString staging = digest.value(QStringLiteral("staging_directory")).toString();
    const QString finalDirectory = digest.value(QStringLiteral("final_directory")).toString();
    const QString residual = digest.value(QStringLiteral("residual_summary")).toString();
    const QString expectedSha = digest.value(QStringLiteral("manifest_sha256")).toString();
    const QString storedDigest = header.value(QStringLiteral("input_digest")).toString();
    auto finish = [&]() {
      QString ignored;
      if (!writePublicationJournal(journalDir, opId, QStringLiteral("complete"), storedDigest, &ignored))
        qWarning() << "MapVersionStore: could not complete publication journal" << opId << ignored;
    };
    const bool finalExists = !finalDirectory.isEmpty() && QFileInfo::exists(finalDirectory);
    if (stage == QLatin1String("publish_prepared") && !finalExists) {
      if (QFileInfo::exists(staging)) {
        const bool disposable = staging.contains(QLatin1String(".staging-")) &&
            pathInsideProject(projectDir, staging) &&
            !publishedPaths.contains(QFileInfo(staging).absoluteFilePath());
        if (!disposable) {
          qWarning() << "MapVersionStore: refused to delete an unrecognized staging path" << staging;
          continue;
        }
        QDir(staging).removeRecursively();
      }
      finish();
      continue;
    }
    if (stage == QLatin1String("publish_renamed") && !finalExists) {
      finish();
      continue;
    }
    if (stage != QLatin1String("publish_renamed") && stage != QLatin1String("publish_prepared")) {
      qWarning() << "MapVersionStore: unrecognized publication stage" << opId << stage;
      continue;
    }
    if (!pathInsideProject(projectDir, finalDirectory)) {
      qWarning() << "MapVersionStore: publication path is outside the project" << finalDirectory;
      continue;
    }
    const QString actualSha = paleo::managedpath::sha256FileHex(
        QDir(finalDirectory).filePath(QStringLiteral("evidence-snapshot.json")));
    if (expectedSha.size() != 64 || !paleo::io::shaEquals(actualSha, expectedSha)) {
      qWarning() << "MapVersionStore: publication manifest does not match the journal; left untouched" << opId;
      continue;
    }
    QSqlQuery row(QSqlDatabase::database(mapVersionConnectionName(m_dbPath, m_readOnly)));
    row.prepare(QStringLiteral("SELECT horizon, version, state, published_path FROM map_versions WHERE id=?"));
    row.addBindValue(versionId);
    if (!row.exec() || !row.next() || row.value(0).toString() != horizon || row.value(1).toInt() != versionNumber) {
      qWarning() << "MapVersionStore: publication journal does not match a saved version; left untouched" << opId;
      continue;
    }
    const QString state = row.value(2).toString();
    const QString publishedPath = row.value(3).toString();
    row.finish();
    if (state == QLatin1String("Published")) {
      if (QFileInfo(publishedPath).absoluteFilePath() == QFileInfo(finalDirectory).absoluteFilePath()) finish();
      else qWarning() << "MapVersionStore: publication journal points at a different published result" << opId;
      continue;
    }
    if (state != QLatin1String("Editing")) {
      qWarning() << "MapVersionStore: publication journal version is not editable; left untouched" << opId;
      continue;
    }
    QSqlQuery update(QSqlDatabase::database(mapVersionConnectionName(m_dbPath, m_readOnly)));
    update.prepare(QStringLiteral("UPDATE map_versions SET state='Published', published_path=?, residual_summary=?"
                                  " WHERE id=? AND horizon=? AND version=? AND state!='Published'"));
    update.addBindValue(finalDirectory);
    update.addBindValue(residual);
    update.addBindValue(versionId);
    update.addBindValue(horizon);
    update.addBindValue(versionNumber);
    if (!update.exec() || update.numRowsAffected() != 1) {
      qWarning() << "MapVersionStore: could not finish interrupted publication" << opId << update.lastError().text();
      continue;
    }
    finish();
  }
}

// #476：publish()/snapshotInputForPublish() 共用发布门禁——此前两处约 90 行
// 逐字复制（JSON 解析、快照指纹、PDF 绑定、§177、D10、#394 注释），漏改一份
// 即 native/prepared 发布口径分叉。这里收口为单一实现（错误文案保持原样）。
bool MapVersionStore::validatePublishGates(const MapVersion &latestV, const QString &horizon,
                                           const QString &residualSummary, QJsonObject *provenanceOut,
                                           bool *frozenOut, QString *error) {
  QJsonParseError provenanceError;
  const auto savedDocument = QJsonDocument::fromJson(latestV.provenance.toUtf8(), &provenanceError);
  if (provenanceError.error != QJsonParseError::NoError || !savedDocument.isObject()) {
    setError(error, QStringLiteral("保存版本的来源/证据 JSON 损坏，发布被拒绝")); return false;
  }
  const auto provenance = savedDocument.object();
  const bool frozen = provenance.value("snapshot_schema").toString() == "paleo_map_snapshot_v1";
  if (provenance.contains("snapshot_schema") &&
      (!frozen || !provenance.value("saved_layers").isArray() || !provenance.value("saved_pdf").isObject() ||
       provenance.value("snapshot_hash").toString() != snapshotHash(provenance))) {
    setError(error, QStringLiteral("保存版本的快照结构或证据指纹损坏，发布被拒绝")); return false;
  }
  if (provenance.contains("evidence_snapshot") && !provenance.value("evidence_snapshot").isArray()) {
    setError(error, QStringLiteral("保存版本的证据清单格式损坏，发布被拒绝")); return false;
  }
  if (provenance.value("feature_summary").toObject().value("quality_counts").toObject().value("Blocking").toInt() > 0) {
    setError(error, QStringLiteral("保存版本存在阻断性证据问题，请复核后保存新版本")); return false;
  }
  // 只有完整 SHA-256 才是科学指纹。历史短哈希仍留在版本行上，不拿来否定已冻结的文件指纹。
  if (frozen && latestV.pdfSha256.size() == 64 &&
      provenance.value("saved_pdf").toObject().value("sha256").toString() != latestV.pdfSha256) {
    setError(error, QStringLiteral("保存的 PDF 指纹与成果版本绑定不一致")); return false;
  }
  // 发布只读版本行（§260）：PDF 资产 id+SHA-256 必须由 recordLayoutProduct
  // 登记、再经 saveVersion 抄进本行；行上没有 = 没走「导出→保存」。
  if ( latestV.pdfAssetId.isEmpty() || latestV.pdfSha256.isEmpty() )
  {
    setError( error, QStringLiteral( "层位 %1 的当前版本没有 PDF 资产记录 — 导出 PDF 后再保存" )
                         .arg( horizon ) );
    return false;
  }
  // 每口井都要有残差或原因（§177）；缺多少口写进错误文案。
  int covered = -1, total = -1;
  if ( !residualSummaryComplete( residualSummary, &covered, &total ) )
  {
    const QString missingText =
        ( total > 0 && covered >= 0 )
            ? QStringLiteral( "还有 %1/%2 口井没有残差或原因" ).arg( total - covered ).arg( total )
            : QStringLiteral( "还没有残差摘要" );
    setError( error, QStringLiteral( "层位 %1 %2 — 先在验证页运行验证" )
                         .arg( horizon, missingText ) );
    return false;
  }
  // D10：数值残差下限——完备性只保证「行或原因」，发布还要够数量的数值行
  // （15/20 口径；差多少写进文案）。
  {
    const int numericCount = numericResidualCount( residualSummary );
    const int required = requiredNumericResiduals( total );
    if ( numericCount < required )
    {
      setError( error, QStringLiteral( "层位 %1 数值残差只有 %2/%3 口（发布需 ≥%4）"
                                      " — 先在验证页运行验证，补齐井分层/时深/测网覆盖" )
                           .arg( horizon )
                           .arg( QString::number( numericCount ), QString::number( total ),
                                 QString::number( required ) ) );
      return false;
    }
  }
  if (provenanceOut) *provenanceOut = provenance;
  if (frozenOut) *frozenOut = frozen;
  return true;
}

QString MapVersionStore::publish( const QString &horizon, const QVector<LayerDeclaration> &decls,
                                  const QString &residualSummary, QString *error )
{
  // #394：只读守卫必须与 saveVersion 同位——缺失时先在工程目录落
  // result/<horizon>/v<N>/ 文件树再撞 SQL 只读失败（半完成副作用）。
  if ( m_readOnly )
  {
    setError( error, QStringLiteral("工程目录被另一个实例锁定——本实例只读，发布被拒绝") );
    return QString();
  }
  if ( !paleo::managedpath::isSafePathSegment( horizon ) )
  {
    setError( error, QStringLiteral( "非法层位名称：%1" ).arg( horizon ) );
    return QString();
  }

  if ( !mapVersionEnsureOpen( m_dbPath, m_readOnly, error ) )
    return QString();
  recoverInterruptedPublications();

  const MapVersion latestV = latest( horizon );
  if ( latestV.version <= 0 )
  {
    setError( error, QStringLiteral( "层位 %1 还没有保存的版本，无法发布" ).arg( horizon ) );
    return QString();
  }
  if ( latestV.state == QLatin1String( "Published" ) )
  {
    setError( error, QStringLiteral( "层位 %1 当前版本已发布 — 保存新版本后再发布" ).arg( horizon ) );
    return QString();
  }
  QJsonObject provenance;
  bool frozen = false;
  if ( !validatePublishGates( latestV, horizon, residualSummary, &provenance, &frozen, error ) )
    return {};

  // result/<horizon>/v<N>/ 挨着工程 meta 库（工程目录的 result/）。
  const QString projectDir = QFileInfo( m_dbPath ).absolutePath();
  const QString finalSnapDir = QDir(projectDir).filePath(
      QStringLiteral("result/%1/v%2").arg(horizon).arg(latestV.version));
  QString snapDir = finalSnapDir + ".staging-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  bool published = false;
  const auto cleanup = qScopeGuard([&] {
    if (!published) QDir(snapDir).removeRecursively();
  });
  if ( !QDir().mkpath( snapDir ) )
  {
    setError( error, QStringLiteral( "无法创建发布快照目录 %1" ).arg( snapDir ) );
    return QString();
  }

  const auto copyFrozen = [&](const QJsonObject &record, const QString &name) {
    const QString relative = record.value("path").toString();
    const QString source = paleo::managedpath::resolveManagedPath(projectDir, relative);
    if (source.isEmpty() || record.value("sha256").toString().size() != 64 ||
        !paleo::io::shaEquals(paleo::managedpath::sha256FileHex(source, error), record.value("sha256").toString())) {
      setError(error, QStringLiteral("保存版本的成果快照缺失或指纹不一致：%1").arg(relative));
      return false;
    }
    const QString destination = QDir(snapDir).filePath(name);
    if (!copyReadOnly(source, destination, error)) return false;
    if (!paleo::io::shaEquals(paleo::managedpath::sha256FileHex(destination, error), record.value("sha256").toString())) {
      setError(error, QStringLiteral("发布复制期间快照内容发生变化：%1").arg(relative)); return false;
    }
    return true;
  };
  if (frozen) {
    for (const auto &entry : provenance.value("saved_layers").toArray()) {
      const auto record = entry.toObject();
      if (record.value("missing").toBool()) {
        setError(error, QStringLiteral("保存版本缺少图层来源：%1").arg(record.value("layer_id").toString()));
        return {};
      }
      const QString name = QFileInfo(record.value("path").toString()).fileName();
      if (!copyFrozen(record, name)) return {};
      for (const auto &sc : record.value("sidecars").toArray()) {
        const QJsonObject scRecord = sc.toObject();
        if (!copyFrozen(scRecord, frozenStem(name) + scRecord.value("suffix").toString())) return {};
      }
    }
  } else {
    // Old saved rows have no immutable manifest; retain the legacy read path.
    for (const auto &layer : legacyPublishLayers(horizon, decls)) {
      QString name = sanitizedFileName(layer.layerId);
      const QString suffix = QFileInfo(layer.base).suffix();
      if (!suffix.isEmpty()) name += '.' + suffix;
      if (!copyReadOnly(layer.base, QDir(snapDir).filePath(name), error)) return {};
      for (const auto &sc : layer.sidecars)
        if (!copyReadOnly(sc.second, QDir(snapDir).filePath(frozenStem(name) + sc.first), error))
          return {};
    }
  }

  // 布局产物（PDF）快照：版本行绑定的 OUTPUT 资产必须进快照（「这份 PDF
  // 已经在快照里」）；没有资产归属的旧产物行一并带上。
  if (frozen) {
    if (!copyFrozen(provenance.value("saved_pdf").toObject(), "layout.pdf")) return {};
  } else {
    bool boundCopied = false;
    QSqlQuery q( QSqlDatabase::database( mapVersionConnectionName( m_dbPath, m_readOnly ) ) );
    q.prepare( QStringLiteral( "SELECT path,asset_id FROM map_products"
                               " WHERE horizon=? AND kind='pdf' AND (asset_id=? OR asset_id IS NULL)" ) );
    q.addBindValue( horizon );
    q.addBindValue( latestV.pdfAssetId );
    if ( !q.exec() )
    {
      setError( error, q.lastError().text() );
      return QString();
    }
    while ( q.next() )
    {
      const QString pdf = q.value( 0 ).toString();
      const bool isBound = q.value( 1 ).toString() == latestV.pdfAssetId;
      if ( !QFile::exists( pdf ) )
      {
        if ( isBound )
        {
          setError( error, QStringLiteral( "发布的 PDF 文件已丢失：%1" ).arg( pdf ) );
          return QString();
        }
        continue;
      }
      if ( !copyReadOnly( pdf, QDir( snapDir ).filePath( QFileInfo( pdf ).fileName() ), error ) )
        return QString();
      if ( isBound )
        boundCopied = true;
    }
    if ( !boundCopied )
    {
      setError( error, QStringLiteral( "版本行绑定的 PDF 资产 %1 不在产物表里" )
                           .arg( latestV.pdfAssetId ) );
      return QString();
    }
  }

  QSaveFile manifest(QDir(snapDir).filePath("evidence-snapshot.json"));
  const auto manifestBytes = QJsonDocument(provenance).toJson(QJsonDocument::Indented);
  if (!manifest.open(QIODevice::WriteOnly) || manifest.write(manifestBytes) != manifestBytes.size() || !manifest.commit()) {
    setError(error, manifest.errorString());
    return {};
  }
  QFile::setPermissions(manifest.fileName(), QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                            QFileDevice::ReadGroup | QFileDevice::ReadOther);

  const QString placed = commitPublicationPlacement(latestV.id, latestV.version, horizon, snapDir, finalSnapDir,
                                                   residualSummary, error);
  if (placed.isEmpty()) return {};
  published = true;
  return placed;
}

QString MapVersionStore::projectDirectory() const { return QFileInfo(m_dbPath).absolutePath(); }
MapSnapshotInput MapVersionStore::snapshotInputForSave(const QString &horizon, const QString &provenanceJson,
    const QVector<LayerDeclaration> &layers, QString *error) {
  MapSnapshotInput input;
  if (m_readOnly || !paleo::managedpath::isSafePathSegment(horizon) || !mapVersionEnsureOpen(m_dbPath, m_readOnly, error)) {
    // 只读/非法层位/打开失败三种来源共用一句泛化文案，但打开失败时
    // ensureOpen 已写下具体原因（驱动/线程/SQL）——不得覆盖（数据库打不开
    // 的真实理由曾被替换成误导性的「需要可写工程和合法层位」）。
    if (error && error->isEmpty())
      setError(error, QStringLiteral("地图快照保存需要可写工程和合法层位"));
    return input;
  }
  QJsonParseError parse;
  if (!QJsonDocument::fromJson(provenanceJson.toUtf8(), &parse).isObject() || parse.error != QJsonParseError::NoError) {
    setError(error, QStringLiteral("地图版本来源 JSON 损坏")); return input;
  }
  input.projectDirectory = projectDirectory(); input.horizon = horizon; input.operation = "save"; input.provenanceJson = provenanceJson;
  input.freezeLayers = !layers.isEmpty();
  for (const auto &layer : layers) {
    if (layer.horizon != horizon) continue;
    const auto base = layer.source.section('|', 0, 0);
    input.files << QVariantMap{{"layer_id", layer.layerId}, {"horizon", horizon}, {"type", layer.type},
      {"source", base}, {"source_suffix", layer.source.mid(base.size())}};
  }
  QSqlQuery product(QSqlDatabase::database(mapVersionConnectionName(m_dbPath, m_readOnly)));
  product.prepare("SELECT path,asset_id,sha256 FROM map_products WHERE horizon=? AND kind='pdf' ORDER BY id DESC LIMIT 1");
  product.addBindValue(horizon);
  if (!product.exec()) { setError(error, product.lastError().text()); return {}; }
  if (product.next()) {
    input.pdfAssetId = product.value(1).toString(); input.pdfSha256 = product.value(2).toString();
    if (!input.pdfAssetId.isEmpty()) input.files << QVariantMap{{"source", product.value(0).toString()}, {"sha256", input.pdfSha256}, {"pdf", true}};
  }
  input.valid = true; return input;
}
MapSnapshotInput MapVersionStore::snapshotInputForPublish(const QString &horizon,
    const QVector<LayerDeclaration> &decls, const QString &residualSummary, QString *error) {
  // #394：只读守卫必须与 saveVersion 同位——缺失时先在工程目录落
  // result/<horizon>/v<N>/ 文件树再撞 SQL 只读失败（半完成副作用）。
  if ( m_readOnly )
  {
    setError( error, QStringLiteral("工程目录被另一个实例锁定——本实例只读，发布被拒绝") );
    return {};
  }
  if ( !paleo::managedpath::isSafePathSegment( horizon ) )
  {
    setError( error, QStringLiteral( "非法层位名称：%1" ).arg( horizon ) );
    return {};
  }

  if ( !mapVersionEnsureOpen( m_dbPath, m_readOnly, error ) )
    return {};

  const MapVersion latestV = latest( horizon );
  if ( latestV.version <= 0 )
  {
    setError( error, QStringLiteral( "层位 %1 还没有保存的版本，无法发布" ).arg( horizon ) );
    return {};
  }
  if ( latestV.state == QLatin1String( "Published" ) )
  {
    setError( error, QStringLiteral( "层位 %1 当前版本已发布 — 保存新版本后再发布" ).arg( horizon ) );
    return {};
  }
  QJsonObject provenance;
  bool frozen = false;
  if ( !validatePublishGates( latestV, horizon, residualSummary, &provenance, &frozen, error ) )
    return {};

  MapSnapshotInput input; input.projectDirectory = projectDirectory(); input.horizon = horizon;
  input.operation = "publish"; input.provenanceJson = latestV.provenance; input.residualSummary = residualSummary;
  input.versionId = latestV.id; input.versionNumber = latestV.version;
  input.pdfAssetId = latestV.pdfAssetId; input.pdfSha256 = latestV.pdfSha256;
  if (frozen) {
    for (const auto &entry : provenance.value("saved_layers").toArray()) {
      auto record = entry.toObject().toVariantMap();
      if (record.value("missing").toBool()) { setError(error, QStringLiteral("保存版本缺少图层来源")); return {}; }
      const auto source = paleo::managedpath::resolveManagedPath(input.projectDirectory, record.value("path").toString());
      if (source.isEmpty()) { setError(error, QStringLiteral("保存快照路径无效")); return {}; }
      record.insert("source", source);
      // 伴生文件（.dbf/.shx/.prj…）随主文件一并进快照：native publish() 的
      // 冻结路径复制它们（frozenStem(name)+suffix），prepared 路径此前漏收，
      // shapefile 发布快照会静默缺件（PW-023）。sidecar_of 记主文件在
      // input.files 里的下标，prepareMapSnapshot 据此拼目标名。
      const int baseIndex = input.files.size();
      input.files << record;
      for (const auto &sc : record.value("sidecars").toList()) {
        const auto scRecord = sc.toMap();
        const QString scSource = paleo::managedpath::resolveManagedPath(
            input.projectDirectory, scRecord.value("path").toString());
        if (scSource.isEmpty()) { setError(error, QStringLiteral("保存快照路径无效")); return {}; }
        input.files << QVariantMap{{"layer_id", record.value("layer_id")}, {"source", scSource},
                                   {"sha256", scRecord.value("sha256")},
                                   {"sidecar_of", baseIndex}, {"sidecar", scRecord.value("suffix")}};
      }
    }
    auto pdf = provenance.value("saved_pdf").toObject().toVariantMap();
    pdf.insert("source", paleo::managedpath::resolveManagedPath(input.projectDirectory, pdf.value("path").toString())); pdf.insert("pdf", true);
    input.files << pdf;
  } else {
    // legacy（非冻结）：主文件 + 伴生文件 + 绑定 PDF，收集口径与 native
    // publish() 共用 legacyPublishLayers（缺失/空源图层一并跳过）。
    for (const auto &layer : legacyPublishLayers(horizon, decls)) {
      const int baseIndex = input.files.size();
      input.files << QVariantMap{{"layer_id", layer.layerId}, {"source", layer.base}, {"legacy", true}};
      for (const auto &sc : layer.sidecars)
        input.files << QVariantMap{{"layer_id", layer.layerId}, {"source", sc.second},
                                   {"legacy", true}, {"sidecar_of", baseIndex}, {"sidecar", sc.first}};
    }
    QSqlQuery product(QSqlDatabase::database(mapVersionConnectionName(m_dbPath, m_readOnly)));
    product.prepare("SELECT path FROM map_products WHERE horizon=? AND kind='pdf' AND asset_id=? ORDER BY id DESC LIMIT 1");
    product.addBindValue(horizon); product.addBindValue(latestV.pdfAssetId);
    if (!product.exec() || !product.next()) { setError(error, QStringLiteral("版本绑定的 PDF 产物缺失")); return {}; }
    input.files << QVariantMap{{"source", product.value(0).toString()}, {"sha256", latestV.pdfSha256}, {"pdf", true}};
  }
  input.valid = true; return input;
}
MapVersion MapVersionStore::commitPreparedVersion(PreparedMapSnapshot &snapshot, QString *error) {
  if (m_readOnly || snapshot.input.operation != "save" || snapshot.input.projectDirectory != projectDirectory() ||
      !verifyPreparedMapSnapshot(snapshot, error) || !mapVersionEnsureOpen(m_dbPath, m_readOnly, error)) {
    if (error && error->isEmpty()) *error = QStringLiteral("地图快照工程已切换或不可写"); return {};
  }
  auto db = QSqlDatabase::database(mapVersionConnectionName(m_dbPath, m_readOnly));
  if (!db.transaction()) { setError(error, db.lastError().text()); return {}; }
  const auto rollback = qScopeGuard([&] { if (!snapshot.committed) db.rollback(); });
  QSqlQuery insert(db);
  insert.prepare("INSERT INTO map_versions(horizon,version,provenance,state,created_utc,pdf_asset_id,pdf_sha256) VALUES(?,?,?,'Editing',?,?,?) RETURNING id");
  insert.addBindValue(snapshot.input.horizon); insert.addBindValue(latest(snapshot.input.horizon).version + 1);
  insert.addBindValue(QString::fromUtf8(QJsonDocument::fromVariant(snapshot.provenance).toJson(QJsonDocument::Compact)));
  insert.addBindValue(nowUtc()); insert.addBindValue(snapshot.input.pdfAssetId.isEmpty() ? QVariant() : QVariant(snapshot.input.pdfAssetId));
  insert.addBindValue(snapshot.input.pdfSha256.isEmpty() ? QVariant() : QVariant(snapshot.input.pdfSha256));
  if (!insert.exec() || !insert.next()) { setError(error, insert.lastError().text()); return {}; }
  const int id = insert.value(0).toInt();
  insert.finish();
  // 提交后的取值按 RETURNING 的 id 读回本行（同 saveVersion 的 sel 查询）：
  // 用 latest() 会在它查询失败时返回 version=0 的空对象，把已成功落库的
  // 保存误报为失败；读回失败如实报错并走上面的回滚护栏。
  QSqlQuery sel(db);
  sel.prepare(QStringLiteral("SELECT ") + QLatin1String(kSelectCols) +
              QStringLiteral(" FROM map_versions WHERE id=?"));
  sel.addBindValue(id);
  if (!sel.exec() || !sel.next()) { setError(error, sel.lastError().text()); return {}; }
  const auto result = rowToVersion(sel);
  sel.finish();
  if (!db.commit()) { setError(error, db.lastError().text()); return {}; }
  snapshot.committed = true; return result;
}
QString MapVersionStore::commitPreparedPublication(PreparedMapSnapshot &snapshot, QString *error) {
  if (!mapVersionEnsureOpen(m_dbPath, m_readOnly, error)) return {};
  recoverInterruptedPublications();
  const auto latestV = latest(snapshot.input.horizon);
  if (m_readOnly || snapshot.input.operation != "publish" || snapshot.input.projectDirectory != projectDirectory() ||
      latestV.id != snapshot.input.versionId || latestV.state == "Published" || !verifyPreparedMapSnapshot(snapshot, error)) {
    if (error && error->isEmpty()) *error = QStringLiteral("发布版本已变化或工程不可写"); return {};
  }
  const QString placed = commitPublicationPlacement(snapshot.input.versionId, snapshot.input.versionNumber,
      snapshot.input.horizon, snapshot.directory, snapshot.finalDirectory, snapshot.input.residualSummary, error);
  if (placed.isEmpty()) return {};
  // Leave snapshot.directory on the staging path. It has been renamed away, so
  // a failed caller cannot delete the registered result by discarding it.
  snapshot.committed = true;
  return placed;
}

bool MapVersionStore::isPublished( const QString &horizon ) const
{
  return latest( horizon ).state == QLatin1String( "Published" );
}

QString MapVersionStore::publishedPath( const QString &horizon ) const
{
  const MapVersion v = latest( horizon );
  return v.state == QLatin1String( "Published" ) ? v.publishedPath : QString();
}
