#include "mapversionstore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    }

    QSqlQuery schema( db );
    if ( !schema.exec( QStringLiteral( "CREATE TABLE IF NOT EXISTS map_versions("
                                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                       "horizon TEXT NOT NULL,"
                                       "version INTEGER NOT NULL,"
                                       "provenance TEXT NOT NULL,"
                                       "state TEXT NOT NULL DEFAULT 'Editing',"
                                       "published_path TEXT,"
                                       "created_utc TEXT NOT NULL)" ) ) )
    {
      setError( error, schema.lastError().text() );
      return false;
    }
    if ( !schema.exec( QStringLiteral( "CREATE TABLE IF NOT EXISTS map_products("
                                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                       "horizon TEXT NOT NULL,"
                                       "kind TEXT NOT NULL,"
                                       "path TEXT NOT NULL,"
                                       "created_utc TEXT NOT NULL)" ) ) )
    {
      setError( error, schema.lastError().text() );
      return false;
    }
    return true;
  }

  MapVersion rowToVersion( const QSqlQuery &q )
  {
    MapVersion v;
    v.id = q.value( 0 ).toInt();
    v.horizon = q.value( 1 ).toString();
    v.version = q.value( 2 ).toInt();
    v.provenance = q.value( 3 ).toString();
    v.state = q.value( 4 ).toString();
    v.publishedPath = q.value( 5 ).toString();
    v.createdUtc = q.value( 6 ).toString();
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
  q.prepare( QStringLiteral( "SELECT id,horizon,version,provenance,state,published_path,created_utc"
                             " FROM map_versions WHERE horizon=? ORDER BY version DESC LIMIT 1" ) );
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
  q.prepare( QStringLiteral( "SELECT id,horizon,version,provenance,state,published_path,created_utc"
                             " FROM map_versions WHERE horizon=? ORDER BY version" ) );
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

  QSqlQuery q( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
  q.prepare( QStringLiteral( "INSERT INTO map_versions(horizon,version,provenance,state,created_utc)"
                             " VALUES(?,?,?,'Editing',?)"
                             " RETURNING id" ) );
  q.addBindValue( horizon );
  q.addBindValue( latest( horizon ).version + 1 );
  q.addBindValue( provenanceJson );
  q.addBindValue( nowUtc() );
  if ( !q.exec() || !q.next() )
  {
    setError( error, q.lastError().text() );
    return MapVersion();
  }
  const int id = q.value( 0 ).toInt();
  q.finish();

  QSqlQuery sel( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
  sel.prepare( QStringLiteral( "SELECT id,horizon,version,provenance,state,published_path,created_utc"
                               " FROM map_versions WHERE id=?" ) );
  sel.addBindValue( id );
  if ( !sel.exec() || !sel.next() )
  {
    setError( error, sel.lastError().text() );
    return MapVersion();
  }
  return rowToVersion( sel );
}

bool MapVersionStore::recordLayoutProduct( const QString &horizon, const QString &pdfPath,
                                           QString *error )
{
  if ( !ensureOpen( m_dbPath, error ) )
    return false;
  QSqlQuery q( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
  q.prepare( QStringLiteral( "INSERT INTO map_products(horizon,kind,path,created_utc)"
                             " VALUES(?,'pdf',?,?)" ) );
  q.addBindValue( horizon );
  q.addBindValue( pdfPath );
  q.addBindValue( nowUtc() );
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

QString MapVersionStore::publish( const QString &horizon, const QVector<LayerDeclaration> &decls,
                                  QString *error )
{
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
  if ( !hasLayoutProduct( horizon ) )
  {
    setError( error, QStringLiteral( "层位 %1 还没有布局产物（PDF）— 先完成图件导出" ).arg( horizon ) );
    return QString();
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

  // 布局产物（PDF）快照。
  {
    QSqlQuery q( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
    q.prepare( QStringLiteral( "SELECT path FROM map_products WHERE horizon=? AND kind='pdf'" ) );
    q.addBindValue( horizon );
    if ( !q.exec() )
    {
      setError( error, q.lastError().text() );
      return QString();
    }
    while ( q.next() )
    {
      const QString pdf = q.value( 0 ).toString();
      if ( !QFile::exists( pdf ) )
        continue;
      if ( !copyReadOnly( pdf, QDir( snapDir ).filePath( QFileInfo( pdf ).fileName() ), error ) )
        return QString();
    }
  }

  QSqlQuery up( QSqlDatabase::database( connectionNameFor( m_dbPath ) ) );
  up.prepare( QStringLiteral( "UPDATE map_versions SET state='Published', published_path=?"
                              " WHERE id=?" ) );
  up.addBindValue( snapDir );
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
