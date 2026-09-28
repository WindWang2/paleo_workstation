// 层：数据
#include "mapversionstore.h"
#include "metastore.h"
#include "../catalog/datacatalog.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace
{
  QString connectionNameFor( const QString &path )
  {
    return QStringLiteral( "paleo_mapversions_" ) + QString::number( qHash( path ) );
  }

  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
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

  // Lazily opens the connection and guarantees both tables (idempotent,
  // forward-compatible: absent columns keep NULL defaults on old rows).
  bool ensureOpen( const QString &path, QString *error )
  {
    const QString connName = connectionNameFor( path );
    QSqlDatabase db = QSqlDatabase::contains( connName )
                          ? QSqlDatabase::database( connName )
                          : QSqlDatabase::addDatabase( QStringLiteral( "QSQLITE" ), connName );
    if ( !db.isValid() )
    {
      setError( error, QStringLiteral( "QSQLITE driver is not available" ) );
      return false;
    }
    if ( !db.isOpen() )
    {
      const QDir dir = QFileInfo( path ).absoluteDir();
      if ( !dir.exists() && !dir.mkpath( QStringLiteral( "." ) ) )
      {
        setError( error, QStringLiteral( "cannot create directory for %1" ).arg( path ) );
        return false;
      }
      db.setDatabaseName( path );
      if ( !db.open() )
      {
        setError( error, db.lastError().text() );
        return false;
      }
      // 共享 schema 门（docs/SCHEMA_MIGRATION.md）：建表/补列之前执行。
      if ( !MetaStore::ensureUserVersion( db, error ) )
        return false;
    }

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
    // 既有库补列（旧行一律 NULL → 读侧归一为未发布，不回写）。
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
    name.replace( QLatin1Char( '.' ), QLatin1Char( '_' ) );
    return name;
  }

  const char kSelectCols[] =
      "id,horizon,version,provenance,state,published_path,created_utc,"
      "pdf_asset_id,pdf_sha256,residual_summary";
} // namespace

MapVersionStore::MapVersionStore( const QString &metaSqlitePath )
  : m_dbPath( metaSqlitePath )
{
}

bool MapVersionStore::open( QString *error )
{
  return ensureOpen( m_dbPath, error );
}

int MapVersionStore::currentVersion( const QString &horizon ) const
{
  return latest( horizon ).version;
}

MapVersion MapVersionStore::latest( const QString &horizon ) const
{
  if ( !ensureOpen( m_dbPath, nullptr ) )
    return MapVersion();
  QSqlQuery q( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
  q.prepare( QStringLiteral( "SELECT " ) + QLatin1String( kSelectCols ) +
             QStringLiteral( " FROM map_versions WHERE horizon=? ORDER BY version DESC LIMIT 1" ) );
  q.addBindValue( horizon );
  if ( !q.exec() || !q.next() )
    return MapVersion();
  return rowToVersion( q );
}

QVector<MapVersion> MapVersionStore::versions( const QString &horizon ) const
{
  QVector<MapVersion> out;
  if ( !ensureOpen( m_dbPath, nullptr ) )
    return out;
  QSqlQuery q( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
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
                                         QString *error )
{
  if ( !ensureOpen( m_dbPath, error ) )
    return MapVersion();

  QSqlDatabase db = QSqlDatabase::database( connectionNameFor( m_dbPath ) );

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

  QSqlQuery q( db );
  q.prepare( QStringLiteral( "INSERT INTO map_versions(horizon,version,provenance,state,"
                             "created_utc,pdf_asset_id,pdf_sha256)"
                             " VALUES(?,?,?,'Editing',?,?,?)"
                             " RETURNING id" ) );
  q.addBindValue( horizon );
  q.addBindValue( latest( horizon ).version + 1 );
  q.addBindValue( provenanceJson );
  q.addBindValue( nowUtc() );
  q.addBindValue( pdfAssetId.isEmpty() ? QVariant() : QVariant( pdfAssetId ) );
  q.addBindValue( pdfSha256.isEmpty() ? QVariant() : QVariant( pdfSha256 ) );
  if ( !q.exec() || !q.next() )
  {
    setError( error, q.lastError().text() );
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
  return rowToVersion( sel );
}

bool MapVersionStore::recordLayoutProduct( const QString &horizon, const QString &pdfPath,
                                           const QString &assetId, const QString &sha256,
                                           QString *error )
{
  if ( !ensureOpen( m_dbPath, error ) )
    return false;
  QSqlQuery q( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
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

bool MapVersionStore::hasLayoutProduct( const QString &horizon ) const
{
  if ( !ensureOpen( m_dbPath, nullptr ) )
    return false;
  QSqlQuery q( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
  q.prepare( QStringLiteral( "SELECT COUNT(*) FROM map_products WHERE horizon=? AND kind='pdf'" ) );
  q.addBindValue( horizon );
  if ( !q.exec() || !q.next() )
    return false;
  return q.value( 0 ).toInt() > 0;
}

QString MapVersionStore::latestLayoutProduct( const QString &horizon ) const
{
  if ( !ensureOpen( m_dbPath, nullptr ) )
    return QString();
  QSqlQuery q( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
  q.prepare( QStringLiteral( "SELECT path FROM map_products"
                             " WHERE horizon=? AND kind='pdf' ORDER BY id DESC LIMIT 1" ) );
  q.addBindValue( horizon );
  if ( !q.exec() || !q.next() )
    return QString();
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

QString MapVersionStore::publish( const QString &horizon, const QVector<LayerDeclaration> &decls,
                                  const QString &residualSummary, QString *error )
{
  if ( !DataCatalog::isSafePathSegment( horizon ) )
  {
    setError( error, QStringLiteral( "非法层位名称：%1" ).arg( horizon ) );
    return QString();
  }

  if ( !ensureOpen( m_dbPath, error ) )
    return QString();

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
  // 发布只读版本行（§260）：PDF 资产 id+SHA-256 必须由 recordLayoutProduct
  // 登记、再经 saveVersion 抄进本行；行上没有 = 没走「导出→保存」。
  if ( latestV.pdfAssetId.isEmpty() || latestV.pdfSha256.isEmpty() )
  {
    setError( error, QStringLiteral( "层位 %1 的当前版本没有 PDF 资产记录 — 导出 PDF 后再保存" )
                         .arg( horizon ) );
    return QString();
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
    return QString();
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
      return QString();
    }
  }

  // result/<horizon>/v<N>/ 挨着工程 meta 库（工程目录的 result/）。
  const QString projectDir = QFileInfo( m_dbPath ).absolutePath();
  const QString snapDir = QDir( projectDir ).filePath(
      QStringLiteral( "result/%1/v%2" ).arg( horizon ).arg( latestV.version ) );
  if ( !QDir().mkpath( snapDir ) )
  {
    setError( error, QStringLiteral( "无法创建发布快照目录 %1" ).arg( snapDir ) );
    return QString();
  }

  // 成果 gpkg（该层位声明的文件图层）快照。
  for ( const LayerDeclaration &d : decls )
  {
    if ( d.horizon != horizon )
      continue;
    const QString base = d.source.section( QLatin1Char( '|' ), 0, 0 );
    if ( base.isEmpty() || !QFile::exists( base ) )
      continue;
    const QString suffix = QFileInfo( base ).suffix();
    QString name = sanitizedFileName( d.layerId );
    if ( !suffix.isEmpty() )
      name += QLatin1Char( '.' ) + suffix;
    if ( !copyReadOnly( base, QDir( snapDir ).filePath( name ), error ) )
      return QString();
  }

  // 布局产物（PDF）快照：版本行绑定的 OUTPUT 资产必须进快照（「这份 PDF
  // 已经在快照里」）；没有资产归属的旧产物行一并带上。
  {
    bool boundCopied = false;
    QSqlQuery q( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
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

  QSqlQuery up( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
  up.prepare( QStringLiteral( "UPDATE map_versions SET state='Published', published_path=?,"
                              " residual_summary=? WHERE id=?" ) );
  up.addBindValue( snapDir );
  up.addBindValue( residualSummary );
  up.addBindValue( latestV.id );
  if ( !up.exec() )
  {
    setError( error, up.lastError().text() );
    return QString();
  }
  return snapDir;
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
