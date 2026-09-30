// 层：数据
#include <QHash> // must precede the header: m_busy is a QHash member (fwd-decl only there)
#include "paleoprojectstore.h"
#include "atomicfile.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QSaveFile>

#include <cstdio>

namespace
{
  // data/commit-coord 记档阶段词表——单调推进；词表外一律按「需人工处置」。
  const QLatin1String kStageQueued( "queued" );
  const QLatin1String kStageCatalogDone( "catalog_done" );
  const QLatin1String kStageQgzDone( "qgz_done" );
  const QLatin1String kStageComplete( "complete" );
  const QLatin1String kStageCorrupt( "corrupt" );

  int commitStageRank( const QString &stage )
  {
    if ( stage == kStageQueued ) return 0;
    if ( stage == kStageCatalogDone ) return 1;
    if ( stage == kStageQgzDone ) return 2;
    if ( stage == kStageComplete ) return 3;
    return -1;
  }

  // opId 落成文件名 <opId>.json——与 catalog §3 段校验同一族规则：拒绝
  // 空串、路径分隔符、".."、盘符冒号、NUL 与控制字符。
  bool isSafeCommitOpId( const QString &opId )
  {
    if ( opId.isEmpty() || opId.size() > 200 || opId.contains( QLatin1String( ".." ) ) )
      return false;
    for ( const QChar c : opId )
    {
      if ( c == QLatin1Char( '/' ) || c == QLatin1Char( '\\' ) || c == QLatin1Char( ':' ) )
        return false;
      const ushort u = c.unicode();
      if ( u < 0x20 || u == 0x7F )
        return false;
    }
    return true;
  }
} // namespace

// §41.2 — sole write choke point for project.gpkg / metadata.sqlite / .qgz.
//
// Serialization mechanism: m_writeMutex is held for the whole duration of each
// enqueued write (and for the whole saveAll sequence), so under WAL's single
// writer there can never be two writers racing the .gpkg — a bypass would
// surface as SQLITE_BUSY, which is exactly what the §33 spine test asserts
// does not happen on the queue path.
//
// Signals are emitted AFTER the write mutex is released: a slot connected
// DirectConnection that re-entered the store would otherwise deadlock on the
// non-recursive mutex.

PaleoProjectStore::PaleoProjectStore( QObject *parent )
  : QObject( parent )
{
}

void PaleoProjectStore::setProjectPaths( const QString &qgzPath, const QString &gpkgPath, const QString &metaSqlitePath )
{
  m_qgzPath = qgzPath;
  m_gpkgPath = gpkgPath;
  m_metaPath = metaSqlitePath;
}

PaleoProjectStore::WriteResult PaleoProjectStore::enqueueWrite( const std::function<WriteResult()> &fn )
{
  if ( m_readOnly )
    return { false, tr( "工程目录被另一个实例锁定——本实例只读，工程文件写入被拒绝" ) };
  WriteResult result;
  {
    QMutexLocker locker( &m_writeMutex );
    result = fn();
  }

  // Target is unknown to the store for a free-form write — empty string.
  if ( result.ok )
    emit writeCompleted( QString() );
  else
    emit writeFailed( QString(), result.error );
  return result;
}

PaleoProjectStore::WriteResult PaleoProjectStore::saveAll( const std::function<WriteResult()> &gpkgCommit,
                                                           const std::function<WriteResult()> &writeQgz )
{
  if ( m_readOnly )
    return { false, tr( "工程目录被另一个实例锁定——本实例只读，工程保存被拒绝" ) };
  struct PendingEmission { bool failed; QString target; QString error; };
  QList<PendingEmission> pending;
  WriteResult result;

  {
    QMutexLocker locker( &m_writeMutex );

    // 1. gpkg commit — the authoritative data state. Failure aborts the whole
    //    sequence before any .qgz mutation happens.
    result = gpkgCommit();
    if ( result.ok )
      pending.append( { false, m_gpkgPath, QString() } );
    else
      pending.append( { true, m_gpkgPath, result.error } );

    if ( result.ok )
    {
      // 2. .qgz backup — .qgz is equally protected (a truncated zip is a
      //    corrupt project file). First save has nothing to back up.
      result = backupQgz();
      if ( !result.ok )
        pending.append( { true, m_qgzPath, result.error } );

      // 3. .qgz atomic write (temp+rename is the callback's responsibility —
      //    QgisProjectService::writeProject implements it). Failure here is
      //    recoverable: gpkg data state is already committed, display state
      //    is regenerable.
      if ( result.ok )
      {
        result = writeQgz();
        if ( result.ok )
          pending.append( { false, m_qgzPath, QString() } );
        else
          pending.append( { true, m_qgzPath, result.error } );
      }
    }
  }

  for ( const PendingEmission &sig : pending )
  {
    if ( sig.failed )
      emit writeFailed( sig.target, sig.error );
    else
      emit writeCompleted( sig.target );
  }
  return result;
}

// ---- data/commit-coord：journal + 幂等有序提交 ----
//
// 记档先于执行：先把 queued 记档落盘再跑任何单元——否则中途崩溃在
// recoverCommitJournal 里完全不可见，谈不上 honest failure。每个单元成功
// 后推进阶段；崩溃留下的记档阶段如实反映「哪个单元没跑完」。
//
// 幂等重入：journal=complete → 回调一律不执行、直接成功（opId 就是幂等键）。
// 未完成记档在 inputDigest 一致时续跑剩余单元；摘要不同（同 opId 换了输入）
// 或记档损坏 → 拒绝续跑、如实报错——绝不用旧摘要的半成品冒充新输入的结果。

QString PaleoProjectStore::projectDir() const
{
  const QString base = !m_qgzPath.isEmpty() ? m_qgzPath : m_metaPath;
  return base.isEmpty() ? QString() : QFileInfo( base ).absolutePath();
}

QString PaleoProjectStore::commitJournalDir() const
{
  const QString dir = projectDir();
  return dir.isEmpty()
             ? QString()
             : dir + QStringLiteral( "/artifacts/metadata/commit_journal" );
}

QString PaleoProjectStore::commitJournalPath( const QString &opId ) const
{
  return commitJournalDir() + QLatin1Char( '/' ) + opId + QStringLiteral( ".json" );
}

bool PaleoProjectStore::writeCommitJournal( const QString &opId, const QString &stage,
                                            const QString &digest, QString *error ) const
{
  const QString dir = commitJournalDir();
  if ( dir.isEmpty() )
  {
    if ( error )
      *error = tr( "no project paths set — commit journal has no home" );
    return false;
  }
  if ( !QDir().mkpath( dir ) )
  {
    if ( error )
      *error = tr( "cannot create commit journal dir %1" ).arg( dir );
    return false;
  }

  QJsonObject o;
  o.insert( QStringLiteral( "journal_version" ), 1 );
  o.insert( QStringLiteral( "op_id" ), opId );
  o.insert( QStringLiteral( "stage" ), stage );
  o.insert( QStringLiteral( "input_digest" ), digest );
  o.insert( QStringLiteral( "updated_utc" ),
            QDateTime::currentDateTimeUtc().toString( Qt::ISODateWithMs ) );

  // QSaveFile：旁写 + 成功后整体替换——崩在写中途也只会留下完整旧记档。
  const QString path = commitJournalPath( opId );
  QSaveFile f( path );
  f.setDirectWriteFallback( false );
  if ( !f.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
  {
    if ( error )
      *error = tr( "cannot write %1: %2" ).arg( path, f.errorString() );
    return false;
  }
  const QByteArray bytes = QJsonDocument( o ).toJson( QJsonDocument::Indented );
  if ( f.write( bytes ) != bytes.size() )
  {
    const QString detail = f.errorString();
    f.cancelWriting();
    if ( error )
      *error = tr( "short write to %1: %2" ).arg( path, detail );
    return false;
  }
  if ( !f.commit() )
  {
    if ( error )
      *error = tr( "cannot replace %1: %2" ).arg( path, f.errorString() );
    return false;
  }
  return true;
}

PaleoProjectStore::CommitOp PaleoProjectStore::readCommitJournal( const QString &path ) const
{
  CommitOp op;
  // opId 以文件名为权威——记档寻址就是 <opId>.json；内容与文件名不符即损坏。
  op.opId = QFileInfo( path ).completeBaseName();
  QFile f( path );
  if ( !f.open( QIODevice::ReadOnly ) )
  {
    op.stage = kStageCorrupt;
    return op;
  }
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson( f.readAll(), &pe );
  if ( pe.error != QJsonParseError::NoError || !doc.isObject() )
  {
    op.stage = kStageCorrupt;
    return op;
  }
  const QJsonObject o = doc.object();
  const QString id = o.value( QStringLiteral( "op_id" ) ).toString();
  const QString stage = o.value( QStringLiteral( "stage" ) ).toString();
  if ( id.isEmpty() || id != op.opId || stage.isEmpty() )
  {
    op.stage = kStageCorrupt;
    return op;
  }
  op.stage = stage;
  op.digest = o.value( QStringLiteral( "input_digest" ) ).toString();
  return op;
}

QVector<PaleoProjectStore::CommitOp> PaleoProjectStore::recoverCommitJournal() const
{
  QVector<CommitOp> out;
  const QString dir = commitJournalDir();
  if ( dir.isEmpty() )
    return out;
  // 与正在推进的提交错开——拿到的快照要么在阶段推进前、要么在推进后。
  QMutexLocker locker( &m_writeMutex );
  const QStringList names = QDir( dir ).entryList(
      QStringList{ QStringLiteral( "*.json" ) }, QDir::Files, QDir::Name );
  for ( const QString &name : names )
  {
    const CommitOp op = readCommitJournal( dir + QLatin1Char( '/' ) + name );
    if ( op.stage != kStageComplete )
      out.append( op ); // 含 corrupt——损坏记档同属未完成，要如实上报
  }
  return out;
}

int PaleoProjectStore::pruneCommitJournal( int keepComplete )
{
  const QString dir = commitJournalDir();
  if ( dir.isEmpty() )
    return 0;
  QMutexLocker locker( &m_writeMutex );
  // complete 记档按 mtime 新→旧排；超出 keepComplete 的旧档删除。
  const QFileInfoList entries = QDir( dir ).entryInfoList(
      QStringList{ QStringLiteral( "*.json" ) }, QDir::Files, QDir::Time );
  int kept = 0, removed = 0;
  for ( const QFileInfo &fi : entries )
  {
    const CommitOp op = readCommitJournal( fi.absoluteFilePath() );
    if ( op.stage != kStageComplete )
      continue; // 未完成/损坏记档永不清理——恢复上报面
    if ( kept >= keepComplete )
    {
      if ( QFile::remove( fi.absoluteFilePath() ) )
        ++removed;
    }
    else
      ++kept;
  }
  return removed;
}

PaleoProjectStore::WriteResult PaleoProjectStore::commitAll(
    const QString &opId, const QString &inputDigest,
    const std::function<WriteResult()> &catalogCommit,
    const std::function<WriteResult()> &qgzWrite )
{
  struct PendingEmission { bool failed; QString target; QString error; };
  QList<PendingEmission> pending;
  WriteResult result{ false, QString() };

  if ( m_readOnly )
    return { false, tr( "工程目录被另一个实例锁定——本实例只读，工程提交被拒绝" ) };
  // 执行前校验：没有写动作发生就不发射信号（与 saveAll 只对跑过的单元
  // 发信号同一约定）。
  if ( !catalogCommit || !qgzWrite )
    return { false, tr( "commitAll: both commit units are required" ) };
  if ( !isSafeCommitOpId( opId ) )
    return { false, tr( "commitAll: unsafe or empty op id: %1" ).arg( opId ) };
  if ( commitJournalDir().isEmpty() )
    return { false, tr( "commitAll: no project paths set — cannot locate commit journal" ) };
  const QString catalogTarget =
      projectDir() + QStringLiteral( "/artifacts/metadata/catalog.json" );
  const QString jPath = commitJournalPath( opId );

  {
    QMutexLocker locker( &m_writeMutex );

    int rank = 0;
    if ( QFile::exists( jPath ) )
    {
      const CommitOp existing = readCommitJournal( jPath );
      if ( existing.stage == kStageComplete )
        return { true, QString() }; // 幂等：同 opId 已完成 → no-op 成功
      if ( existing.stage == kStageCorrupt || commitStageRank( existing.stage ) < 0 )
        return { false, tr( "commit journal %1 is unreadable or has an unknown stage"
                            " — refusing to resume" ).arg( jPath ) };
      if ( existing.digest != inputDigest )
        return { false, tr( "commit op %1 journaled with different inputs"
                            " — refusing to resume" ).arg( opId ) };
      rank = commitStageRank( existing.stage ); // 续跑：跳过已完成单元
    }
    else
    {
      // journal-first：执行任何单元前先把 queued 记档落盘。
      QString jerr;
      if ( !writeCommitJournal( opId, kStageQueued, inputDigest, &jerr ) )
        return { false, tr( "cannot write commit journal %1: %2" ).arg( jPath, jerr ) };
    }
    result = { true, QString() };

    // 单元 1：catalog 提交（权威数据态先行——.qgz 只是其显示投影）。
    if ( rank < 1 )
    {
      result = catalogCommit();
      pending.append( { !result.ok, catalogTarget,
                        result.ok ? QString() : result.error } );
      if ( result.ok )
      {
        QString jerr;
        if ( writeCommitJournal( opId, kStageCatalogDone, inputDigest, &jerr ) )
          rank = 1;
        else
          result = { false, tr( "commit journal advance failed for op %1: %2" )
                                .arg( opId, jerr ) };
      }
    }

    // 单元 2：.qgz 备份 + 原子写（备份语义与 saveAll 一致）。
    if ( result.ok && rank < 2 )
    {
      result = backupQgz();
      if ( !result.ok )
        pending.append( { true, m_qgzPath, result.error } );

      if ( result.ok )
      {
        result = qgzWrite();
        pending.append( { !result.ok, m_qgzPath,
                          result.ok ? QString() : result.error } );
        if ( result.ok )
        {
          QString jerr;
          if ( writeCommitJournal( opId, kStageQgzDone, inputDigest, &jerr ) )
            rank = 2;
          else
            result = { false, tr( "commit journal advance failed for op %1: %2" )
                                  .arg( opId, jerr ) };
        }
      }
    }

    // 终态：全部单元成功 → 记 complete。记档失败本身如实报错——记档停在
    // qgz_done，recover 会把它报成未完成（重跑同 opId 只会补这一步）。
    if ( result.ok && rank < 3 )
    {
      QString jerr;
      if ( writeCommitJournal( opId, kStageComplete, inputDigest, &jerr ) )
        result = { true, QString() };
      else
        result = { false, tr( "commit finished but final journal mark failed: %1" )
                              .arg( jerr ) };
    }
  }

  for ( const PendingEmission &sig : pending )
  {
    if ( sig.failed )
      emit writeFailed( sig.target, sig.error );
    else
      emit writeCompleted( sig.target );
  }
  return result;
}

void PaleoProjectStore::markLayerBusy( const QString &layerId, const QString &taskId, const QString &reason )
{
  QMutexLocker locker( &m_busyMutex );
  m_busy.insert( layerId, { taskId, reason } );
}

void PaleoProjectStore::markLayerFree( const QString &layerId )
{
  QMutexLocker locker( &m_busyMutex );
  m_busy.remove( layerId );
}

bool PaleoProjectStore::layerBusy( const QString &layerId, QString *reason ) const
{
  QMutexLocker locker( &m_busyMutex );
  const auto it = m_busy.constFind( layerId );
  if ( it == m_busy.constEnd() )
    return false;
  if ( reason )
    *reason = QStringLiteral( "%1 — %2" ).arg( it->first, it->second ); // "taskId — reason"
  return true;
}

QVector<PaleoProjectStore::BusyEntry> PaleoProjectStore::busyLayers() const
{
  QMutexLocker locker( &m_busyMutex );
  QVector<BusyEntry> out;
  out.reserve( m_busy.size() );
  for ( auto it = m_busy.constBegin(); it != m_busy.constEnd(); ++it )
    out.append( { it.key(), it->first, it->second } );
  return out;
}

PaleoProjectStore::WriteResult PaleoProjectStore::backupQgz() const
{
  if ( m_qgzPath.isEmpty() || !QFile::exists( m_qgzPath ) )
    return { true, QString() };
  const QString bakPath = m_qgzPath + QStringLiteral( ".bak" );
  const QString bakTmp = bakPath + QStringLiteral( ".tmp" );
  QFile::remove( bakTmp );
  if ( !QFile::copy( m_qgzPath, bakTmp ) )
    return { false, tr( "Failed to back up %1 to %2" ).arg( m_qgzPath, bakPath ) };
  if ( !paleoReplaceFile( bakTmp, bakPath ) )
  {
    QFile::remove( bakTmp );
    return { false, tr( "Failed to replace backup %1" ).arg( bakPath ) };
  }
  return { true, QString() };
}
