// 层：功能
#include "batchjobqueue.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QTimer>

#include <algorithm>

#include "../services/paleotaskservice.h"

namespace PaleoBatchQueue
{

namespace
{
constexpr int kStateVersion = 1;
constexpr int kMaxConcurrency = 8;

QJsonObject toJsonMap( const QVariantMap &map )
{
  return QJsonObject::fromVariantMap( map );
}

/// 规范化：QJsonObject 迭代序即键序（Qt 文档保证），Compact 输出稳定——
/// 参数键序不同不得影响指纹。
QByteArray canonicalJson( const QJsonObject &obj )
{
  return QJsonDocument( obj ).toJson( QJsonDocument::Compact );
}
} // namespace

QString itemStateText( ItemState state )
{
  switch ( state )
  {
    case ItemState::Pending:
      return QObject::tr( "待跑" );
    case ItemState::Queued:
      return QObject::tr( "排队中" );
    case ItemState::Running:
      return QObject::tr( "计算中" );
    case ItemState::Succeeded:
      return QObject::tr( "已完成" );
    case ItemState::Failed:
      return QObject::tr( "失败" );
    case ItemState::Skipped:
      return QObject::tr( "已跳过" );
    case ItemState::Cancelled:
      return QObject::tr( "已取消" );
  }
  return QObject::tr( "未知" );
}

// ---------------------------------------------------------------- BatchItem

QString BatchItem::fingerprintOf( const QString &methodId, const QString &horizon,
                                  const QVariantMap &params )
{
  QJsonObject o;
  o.insert( QStringLiteral( "method" ), methodId );
  o.insert( QStringLiteral( "horizon" ), horizon );
  // params 原样进指纹：任何参数变化都应视作不同作业。
  o.insert( QStringLiteral( "params" ), toJsonMap( params ) );
  return QString::fromLatin1(
      QCryptographicHash::hash( canonicalJson( o ), QCryptographicHash::Sha256 )
          .toHex() );
}

QStringList BatchItem::fatalMarkers()
{
  // 熔断词表：这些是「继续跑也必然每项都失败」的前置条件失效
  // （catalog 不可用 / 工程已关闭），重试无意义且会淹没报告真因。
  return { QStringLiteral( "catalog-unavailable" ),
           QStringLiteral( "catalog closed" ),
           QStringLiteral( "工程已关闭" ), QStringLiteral( "工程未打开" ) };
}

QJsonObject BatchItem::toJson() const
{
  QJsonObject o;
  o.insert( QStringLiteral( "item_id" ), itemId );
  o.insert( QStringLiteral( "method" ), methodId );
  o.insert( QStringLiteral( "horizon" ), horizon );
  o.insert( QStringLiteral( "params" ), toJsonMap( params ) );
  o.insert( QStringLiteral( "fingerprint" ), fingerprint );
  o.insert( QStringLiteral( "state" ), static_cast<int>( state ) );
  o.insert( QStringLiteral( "error" ), error );
  o.insert( QStringLiteral( "stage" ), stage );
  o.insert( QStringLiteral( "percent" ), percent );
  o.insert( QStringLiteral( "elapsed_ms" ), static_cast<double>( elapsedMs ) );
  return o;
}

BatchItem BatchItem::fromJson( const QJsonObject &obj, bool *ok )
{
  BatchItem it;
  it.itemId = obj.value( QStringLiteral( "item_id" ) ).toString();
  it.methodId = obj.value( QStringLiteral( "method" ) ).toString();
  it.horizon = obj.value( QStringLiteral( "horizon" ) ).toString();
  it.params = obj.value( QStringLiteral( "params" ) ).toObject().toVariantMap();
  it.fingerprint = obj.value( QStringLiteral( "fingerprint" ) ).toString();
  const int st = obj.value( QStringLiteral( "state" ) ).toInt( -1 );
  it.error = obj.value( QStringLiteral( "error" ) ).toString();
  it.stage = obj.value( QStringLiteral( "stage" ) ).toString();
  it.percent = obj.value( QStringLiteral( "percent" ) ).toInt();
  it.elapsedMs =
      static_cast<qint64>( obj.value( QStringLiteral( "elapsed_ms" ) ).toDouble() );
  const bool valid = !it.itemId.isEmpty() && !it.fingerprint.isEmpty() && st >= 0 &&
                     st <= static_cast<int>( ItemState::Cancelled );
  if ( valid )
    it.state = static_cast<ItemState>( st );
  if ( ok )
    *ok = valid;
  return it;
}

// ----------------------------------------------------------------- BatchSpec

QVector<BatchItem> BatchSpec::expand( QString *error ) const
{
  QVector<BatchItem> out;
  if ( batchId.trimmed().isEmpty() )
  {
    if ( error )
      *error = QObject::tr( "批次标识为空" );
    return out;
  }
  if ( horizons.isEmpty() )
  {
    if ( error )
      *error = QObject::tr( "层位集合为空——无可运行作业" );
    return out;
  }
  if ( methodIds.isEmpty() )
  {
    if ( error )
      *error = QObject::tr( "方法集合为空——无可运行作业" );
    return out;
  }

  // 逐项覆写表：paramTemplates["perItem"]["<horizon>|<method>"] = {...}
  const QVariant perItemRaw = paramTemplates.value( QStringLiteral( "perItem" ) );
  QVariantMap perItem;
  if ( perItemRaw.canConvert<QVariantMap>() )
    perItem = perItemRaw.toMap();

  for ( const QString &horizon : horizons )
  {
    for ( const QString &method : methodIds )
    {
      BatchItem it;
      it.itemId = horizon + QLatin1Char( '|' ) + method;
      it.methodId = method;
      it.horizon = horizon;
      it.params = paramTemplates;
      it.params.remove( QStringLiteral( "perItem" ) );
      const QVariant over = perItem.value( it.itemId );
      if ( over.canConvert<QVariantMap>() )
      {
        const QVariantMap overMap = over.toMap();
        for ( auto i = overMap.constBegin(); i != overMap.constEnd(); ++i )
          it.params.insert( i.key(), i.value() );
      }
      it.fingerprint = BatchItem::fingerprintOf( method, horizon, it.params );
      out.append( it );
    }
  }
  if ( error )
    error->clear();
  return out;
}

QJsonObject BatchSpec::toJson() const
{
  QJsonObject o;
  o.insert( QStringLiteral( "batch_id" ), batchId );
  QJsonArray horizonsArr;
  for ( const QString &h : horizons )
    horizonsArr.append( h );
  o.insert( QStringLiteral( "horizons" ), horizonsArr );
  QJsonArray methodsArr;
  for ( const QString &m : methodIds )
    methodsArr.append( m );
  o.insert( QStringLiteral( "methods" ), methodsArr );
  o.insert( QStringLiteral( "params" ), toJsonMap( paramTemplates ) );
  o.insert( QStringLiteral( "concurrency" ), concurrency );
  o.insert( QStringLiteral( "stop_on_fatal" ), stopOnFatal );
  return o;
}

BatchSpec BatchSpec::fromJson( const QJsonObject &obj, bool *ok )
{
  BatchSpec s;
  s.batchId = obj.value( QStringLiteral( "batch_id" ) ).toString();
  const QJsonArray horizons = obj.value( QStringLiteral( "horizons" ) ).toArray();
  for ( const QJsonValue &v : horizons )
    s.horizons << v.toString();
  const QJsonArray methods = obj.value( QStringLiteral( "methods" ) ).toArray();
  for ( const QJsonValue &v : methods )
    s.methodIds << v.toString();
  s.paramTemplates = obj.value( QStringLiteral( "params" ) ).toObject().toVariantMap();
  s.concurrency = qBound(
      1, obj.value( QStringLiteral( "concurrency" ) ).toInt( 2 ), kMaxConcurrency );
  s.stopOnFatal = obj.value( QStringLiteral( "stop_on_fatal" ) ).toBool( true );
  if ( ok )
    *ok = !s.batchId.isEmpty();
  return s;
}

// --------------------------------------------------------------- BatchReport

QVector<QPair<QString, int>> BatchReport::failureHistogram() const
{
  QHash<QString, int> counter;
  for ( const BatchItem &it : items )
  {
    if ( it.state != ItemState::Failed )
      continue;
    const QString key = it.error.isEmpty() ? QObject::tr( "未注明原因" ) : it.error;
    ++counter[key];
  }
  QVector<QPair<QString, int>> out;
  out.reserve( counter.size() );
  for ( auto it = counter.constBegin(); it != counter.constEnd(); ++it )
    out.append( qMakePair( it.key(), it.value() ) );
  // 降序：次数多的在前；同数按原因字典序，保证可复现。
  std::sort( out.begin(), out.end(),
             []( const QPair<QString, int> &a, const QPair<QString, int> &b ) {
               if ( a.second != b.second )
                 return a.second > b.second;
               return a.first < b.first;
             } );
  return out;
}

QString BatchReport::summary() const
{
  if ( trippedFatal )
    return QObject::tr( "批次 %1 熔断：%2（成功 %3 / 失败 %4 / 跳过 %5 / 取消 %6）" )
        .arg( batchId, fatalReason )
        .arg( succeeded )
        .arg( failed )
        .arg( skipped )
        .arg( cancelled );
  return QObject::tr( "批次 %1：成功 %2 / 失败 %3 / 跳过 %4 / 取消 %5（共 %6）" )
      .arg( batchId )
      .arg( succeeded )
      .arg( failed )
      .arg( skipped )
      .arg( cancelled )
      .arg( total );
}

QJsonObject BatchReport::toJson() const
{
  QJsonObject o;
  o.insert( QStringLiteral( "batch_id" ), batchId );
  o.insert( QStringLiteral( "total" ), total );
  o.insert( QStringLiteral( "succeeded" ), succeeded );
  o.insert( QStringLiteral( "failed" ), failed );
  o.insert( QStringLiteral( "skipped" ), skipped );
  o.insert( QStringLiteral( "cancelled" ), cancelled );
  o.insert( QStringLiteral( "tripped_fatal" ), trippedFatal );
  o.insert( QStringLiteral( "fatal_reason" ), fatalReason );
  o.insert( QStringLiteral( "total_elapsed_ms" ),
            static_cast<double>( totalElapsedMs ) );
  o.insert( QStringLiteral( "summary" ), summary() );

  QJsonArray hist;
  for ( const auto &pair : failureHistogram() )
  {
    QJsonObject h;
    h.insert( QStringLiteral( "reason" ), pair.first );
    h.insert( QStringLiteral( "count" ), pair.second );
    hist.append( h );
  }
  o.insert( QStringLiteral( "failure_histogram" ), hist );

  QJsonArray arr;
  for ( const BatchItem &it : items )
    arr.append( it.toJson() );
  o.insert( QStringLiteral( "items" ), arr );
  return o;
}

QString BatchReport::exportReport( const QString &path, QString *error ) const
{
  if ( path.trimmed().isEmpty() )
  {
    if ( error )
      *error = QObject::tr( "报告导出路径为空" );
    return QString();
  }
  const QFileInfo fi( path );
  if ( !QDir().mkpath( fi.absolutePath() ) )
  {
    if ( error )
      *error = QObject::tr( "无法创建报告目录 %1" ).arg( fi.absolutePath() );
    return QString();
  }
  QSaveFile f( path );
  f.setDirectWriteFallback( false );
  if ( !f.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
  {
    if ( error )
      *error = QObject::tr( "无法写入 %1：%2" ).arg( path, f.errorString() );
    return QString();
  }
  const QByteArray bytes = QJsonDocument( toJson() ).toJson( QJsonDocument::Indented );
  if ( f.write( bytes ) != bytes.size() )
  {
    f.cancelWriting();
    if ( error )
      *error = QObject::tr( "报告写入不完整：%1" ).arg( f.errorString() );
    return QString();
  }
  if ( !f.commit() )
  {
    if ( error )
      *error = QObject::tr( "无法提交报告 %1：%2" ).arg( path, f.errorString() );
    return QString();
  }
  if ( error )
    error->clear();
  return path;
}

// ------------------------------------------------------------------- Slot

/// 一个 JobRunner 槽位。JobRunner::start 忙则拒绝，故并发度 = 槽位数
/// （见 ledger D3）。槽位把三段式回调翻译成编排层的记账事件。
struct BatchQueue::Slot
{
  std::unique_ptr<paleo::jobs::JobRunner<BatchItem>> runner;
  int itemIndex = -1;   ///< 在跑项下标（-1 空）
};

// ---------------------------------------------------------------- BatchQueue

using paleo::jobs::CancelFn;
using paleo::jobs::ProgressFn;

BatchQueue::BatchQueue( PaleoTaskService *tasks, const QString &projectDir,
                        QObject *parent )
    : QObject( parent ), m_tasks( tasks ), m_projectDir( projectDir ),
      m_ctx( new QObject( this ) )
{
  setConcurrency( m_concurrency );
}

BatchQueue::~BatchQueue() = default;

void BatchQueue::setConcurrency( int n )
{
  m_concurrency = qBound( 1, n, kMaxConcurrency );
  // 槽位按需扩容（不缩——在飞项持有槽位，收缩会悬垂）。
  while ( static_cast<int>( m_slots.size() ) < m_concurrency )
  {
    auto slot = std::make_unique<Slot>();
    slot->runner = std::make_unique<paleo::jobs::JobRunner<BatchItem>>(
        m_ctx, thread() );
    slot->runner->setTaskService( m_tasks );
    m_slots.push_back( std::move( slot ) );
    m_slotItem.append( -1 );
    m_startedAtMs.append( 0 );
  }
}

QString BatchQueue::stateDirFor( const QString &projectDir )
{
  if ( projectDir.trimmed().isEmpty() )
    return QString();
  return projectDir + QStringLiteral( "/artifacts/metadata/batch_jobs" );
}

QString BatchQueue::statePath( const QString &batchId ) const
{
  const QString dir = stateDirFor( m_projectDir );
  if ( dir.isEmpty() || batchId.trimmed().isEmpty() )
    return QString();
  return dir + QLatin1Char( '/' ) + batchId + QStringLiteral( ".json" );
}

int BatchQueue::pendingCount() const
{
  int n = 0;
  for ( const BatchItem &it : m_items )
  {
    if ( it.state == ItemState::Pending || it.state == ItemState::Queued )
      ++n;
  }
  return n;
}

int BatchQueue::runningCount() const
{
  int n = 0;
  for ( const BatchItem &it : m_items )
  {
    if ( it.state == ItemState::Running )
      ++n;
  }
  return n;
}

int BatchQueue::finishedCount() const
{
  int n = 0;
  for ( const BatchItem &it : m_items )
  {
    if ( it.state == ItemState::Succeeded || it.state == ItemState::Failed ||
         it.state == ItemState::Skipped || it.state == ItemState::Cancelled )
      ++n;
  }
  return n;
}

bool BatchQueue::isFatal( const QString &error ) const
{
  if ( error.isEmpty() )
    return false;
  const QString lower = error.toLower();
  const QStringList markers = BatchItem::fatalMarkers();
  for ( const QString &marker : markers )
  {
    if ( lower.contains( marker.toLower() ) )
      return true;
  }
  return false;
}

void BatchQueue::tripFatal( const QString &reason )
{
  if ( m_trippedFatal )
    return;
  m_trippedFatal = true;
  m_fatalReason = reason;
}

void BatchQueue::markAllQueuedPending()
{
  // 熔断或取消后：待跑项一律作废（不谎报成功、不留幽灵 Queued）。
  for ( BatchItem &it : m_items )
  {
    if ( it.state == ItemState::Queued || it.state == ItemState::Pending )
    {
      it.state = ItemState::Cancelled;
      if ( it.error.isEmpty() )
        it.error = QObject::tr( "批次已取消" );
    }
  }
  m_fifo.clear();
}

void BatchQueue::finalizeItem( int index, ItemState state, const QString &error )
{
  if ( index < 0 || index >= m_items.size() )
    return;
  BatchItem &it = m_items[index];
  it.state = state;
  if ( state == ItemState::Succeeded )
    it.error.clear();
  else
    it.error = error;
  it.percent = ( state == ItemState::Succeeded ) ? 100 : it.percent;

  // 熔断判据：仅「继续跑也必然每项都失败」的前置失效才停全批。
  if ( state == ItemState::Failed && isFatal( error ) )
    tripFatal( error );

  // 逐项终态即回写——崩在批中也能恢复到最后一个已知终态。
  writeState();
  emit itemChanged( it.itemId );
  emitProgress();
}

void BatchQueue::emitProgress()
{
  emit batchProgress( finishedCount(), m_items.size() );
}

void BatchQueue::writeState()
{
  const QString path = statePath( m_batchId );
  if ( path.isEmpty() )
    return; // 无工程目录 = 不持久化（纯内存批次）

  QDir().mkpath( stateDirFor( m_projectDir ) );

  QJsonObject o;
  o.insert( QStringLiteral( "state_version" ), kStateVersion );
  o.insert( QStringLiteral( "batch_id" ), m_batchId );
  o.insert( QStringLiteral( "tripped_fatal" ), m_trippedFatal );
  o.insert( QStringLiteral( "fatal_reason" ), m_fatalReason );
  o.insert( QStringLiteral( "updated_utc" ),
            QDateTime::currentDateTimeUtc().toString( Qt::ISODateWithMs ) );
  // 判重集不必单列：每项自带 fingerprint（见 BatchItem::toJson），恢复时按
  // itemId 对齐逐项取指纹即可。曾另存 completed_fingerprints 数组——只写不读，
  // 与 items 里的指纹重复且可能与之不一致（双真源），已删。
  QJsonArray arr;
  for ( const BatchItem &it : m_items )
    arr.append( it.toJson() );
  o.insert( QStringLiteral( "items" ), arr );

  // QSaveFile：旁写 + 整体替换——崩在写中途只留完整旧记档。
  QSaveFile f( path );
  f.setDirectWriteFallback( false );
  if ( !f.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    return;
  const QByteArray bytes = QJsonDocument( o ).toJson( QJsonDocument::Indented );
  if ( f.write( bytes ) == bytes.size() )
    f.commit();
  else
    f.cancelWriting();
}

bool BatchQueue::readState( const BatchSpec &spec, QVector<BatchItem> *out,
                            QString *error )
{
  out->clear();
  const QString path = statePath( spec.batchId );
  if ( path.isEmpty() )
  {
    if ( error )
      *error = QObject::tr( "无工程目录，无法读取批次状态" );
    return false;
  }
  QFile f( path );
  if ( !f.open( QIODevice::ReadOnly ) )
  {
    if ( error )
      *error = QObject::tr( "无既有批次状态（首跑）" );
    return false; // 首跑不是错误
  }
  QJsonParseError pe{};
  const QJsonDocument doc = QJsonDocument::fromJson( f.readAll(), &pe );
  if ( pe.error != QJsonParseError::NoError || !doc.isObject() )
  {
    if ( error )
      *error = QObject::tr( "批次状态文件损坏：%1" ).arg( pe.errorString() );
    return false;
  }
  const QJsonObject o = doc.object();
  if ( o.value( QStringLiteral( "state_version" ) ).toInt() != kStateVersion )
  {
    if ( error )
      *error = QObject::tr( "批次状态版本不兼容" );
    return false;
  }

  QHash<QString, BatchItem> byId;
  const QJsonArray arr = o.value( QStringLiteral( "items" ) ).toArray();
  for ( const QJsonValue &v : arr )
  {
    bool ok = false;
    const BatchItem it = BatchItem::fromJson( v.toObject(), &ok );
    if ( !ok )
      continue; // 损坏项跳过——如实丢一项，不让整批恢复失败
    byId.insert( it.itemId, it );
  }

  // 终态后不写的项在崩时是 Running/Queued——按未完成处理，重跑。
  const QVector<BatchItem> fresh = spec.expand( error );
  if ( fresh.isEmpty() )
    return false;
  for ( BatchItem it : fresh )
  {
    const auto found = byId.constFind( it.itemId );
    if ( found != byId.constEnd() && found->fingerprint == it.fingerprint )
    {
      // 只有 Succeeded 是真正「完成」。Failed / Cancelled / 崩在批中的非终态
      // 都是**未达成**的工作，续跑一律回 Pending 重跑——否则失败项会被终态
      // 沿用锁死，再也重试不到（违反「失败作业可单独重试」）。
      if ( found->state == ItemState::Succeeded )
        it.state = ItemState::Succeeded;
    }
    // 指纹变了 = 参数已改 → 不复用旧终态，照新参数重跑。
    // 沿用的成功项改记为 Skipped：本趟它并未执行，把它算进「本批成功」会让
    // 报告口径失真（重跑一批全部已完成的批次时 succeeded 会虚高）。Skipped 是
    // 成功语义，故幂等与完成度判据不受影响。
    if ( it.state == ItemState::Succeeded )
    {
      it.state = ItemState::Skipped;
      it.error = QObject::tr( "该参数指纹此前已完成，本批未重复执行" );
      it.percent = 100;
    }
    out->append( it );
  }
  if ( error )
    error->clear();
  return true;
}

bool BatchQueue::start( const BatchSpec &spec, QString *error )
{
  return resume( spec, error );
}

bool BatchQueue::resume( const BatchSpec &spec, QString *error )
{
  if ( !m_tasks )
  {
    if ( error )
      *error = QObject::tr( "批次编排未绑定任务服务" );
    return false;
  }
  if ( !m_executor )
  {
    if ( error )
      *error = QObject::tr( "批次编排未设置作业执行体" );
    return false;
  }
  if ( m_running )
  {
    if ( error )
      *error = QObject::tr( "上一批次尚未结束" );
    return false;
  }

  BatchSpec spec2 = spec;
  spec2.concurrency = qBound( 1, spec2.concurrency, kMaxConcurrency );

  QString expandErr;
  const QVector<BatchItem> fresh = spec2.expand( &expandErr );
  if ( fresh.isEmpty() )
  {
    if ( error )
      *error = expandErr.isEmpty() ? QObject::tr( "批次定义为空" ) : expandErr;
    return false;
  }

  // 读既有状态续跑（无状态则按新定义起批）。
  QVector<BatchItem> items;
  QString readErr;
  if ( !readState( spec2, &items, &readErr ) || items.isEmpty() )
    items = fresh;

  m_items = items;
  m_indexById.clear();
  for ( int i = 0; i < m_items.size(); ++i )
    m_indexById.insert( m_items[i].itemId, i );

  // 幂等判重：同批内指纹重复的项，后者落 Skipped（不重复产生资产/版本）。
  // 判重集 = 沿用恢复来的成功终态指纹 ∪ 本批已扫过的指纹。
  QSet<QString> seen;
  for ( const BatchItem &it : std::as_const( m_items ) )
  {
    if ( it.state == ItemState::Succeeded || it.state == ItemState::Skipped )
      seen.insert( it.fingerprint );
  }
  for ( int i = 0; i < m_items.size(); ++i )
  {
    BatchItem &it = m_items[i];
    if ( it.state != ItemState::Pending && it.state != ItemState::Queued )
      continue; // 终态沿用，不重复计入
    if ( seen.contains( it.fingerprint ) )
    {
      it.state = ItemState::Skipped;
      it.error = QObject::tr( "参数指纹与批内已完成项相同" );
      it.percent = 100;
      continue;
    }
    seen.insert( it.fingerprint );
  }

  m_batchId = spec2.batchId;
  m_trippedFatal = false;
  m_fatalReason.clear();
  m_stopOnFatal = spec2.stopOnFatal;
  m_fifo.clear();
  m_running = true;
  m_batchStartedAtMs = QDateTime::currentMSecsSinceEpoch();
  setConcurrency( spec2.concurrency );

  // 非终态项入 FIFO（保持展开序 = FIFO 序）。
  for ( int i = 0; i < m_items.size(); ++i )
  {
    if ( m_items[i].state == ItemState::Pending )
    {
      m_items[i].state = ItemState::Queued;
      m_fifo.append( i );
    }
  }

  writeState();
  emit batchStarted( m_batchId, m_items.size() );
  emitProgress();
  pump();

  if ( error )
    error->clear();
  return true;
}

void BatchQueue::pump()
{
  if ( !m_running )
    return;

  if ( m_trippedFatal && m_stopOnFatal )
  {
    // 熔断：待跑项作废，在跑项协作停止。
    markAllQueuedPending();
    for ( int s = 0; s < static_cast<int>( m_slots.size() ); ++s )
    {
      if ( m_slots[s]->itemIndex >= 0 && m_slots[s]->runner )
        m_slots[s]->runner->requestCancel();
    }
    writeState();
    emitProgress();
    finishIfDone();
    return;
  }

  const int poolCap = m_tasks ? m_tasks->maxWorkerThreads() : 1;
  int running = runningCount();
  bool yieldedToInteractive = false;

  for ( int s = 0; s < static_cast<int>( m_slots.size() ); ++s )
  {
    if ( m_slotItem[s] >= 0 )
      continue; // 槽位占用
    if ( m_fifo.isEmpty() )
      break;
    // 不与用户交互作业抢线程：池已满则本轮不出队，等下一轮 pump。
    // 批次让位而非抢占（ledger D4）。
    if ( m_tasks && m_tasks->runningCount() >= poolCap )
    {
      yieldedToInteractive = true;
      break;
    }

    const int index = m_fifo.takeFirst();
    if ( index < 0 || index >= m_items.size() )
      continue;
    if ( m_items[index].state != ItemState::Queued )
      continue; // 已被取消/判重
    startOnSlot( s, index );
    ++running;
  }

  // 让位后必须自己再排一轮 pump：否则当批次无在飞项、池却被交互作业占满时
  // （本槽位空、FIFO 非空），没有任何事件会驱动下一次 pump → 队列永久停滞。
  // 交互作业一完成，runningCount 降下来，这里就补位。
  if ( yieldedToInteractive && !m_fifo.isEmpty() )
  {
    pumpLater();
    return;
  }
  // 无可推进项（全部判重跳过 / 全部已终态）时收尾，否则 m_running 永不复位，
  // 批次永远停在 busy——续跑一个全跳过的批次就会命中这条。
  if ( m_fifo.isEmpty() && runningCount() == 0 )
    finishIfDone();
}

void BatchQueue::pumpLater()
{
  QMetaObject::invokeMethod( this, [this] { pump(); }, Qt::QueuedConnection );
}

void BatchQueue::startOnSlot( int slotIndex, int itemIndex )
{
  if ( slotIndex < 0 || slotIndex >= static_cast<int>( m_slots.size() ) )
    return;
  if ( itemIndex < 0 || itemIndex >= m_items.size() )
    return;

  Slot *slot = m_slots[slotIndex].get();
  if ( !slot || !slot->runner )
    return;
  if ( slot->runner->busy() )
  {
    // 槽位还没真正空出来（框架 clearTask 未执行）。把项放回队首——绝不丢项，
    // 丢了它会永远停在 Queued，批次无法收尾。
    m_items[itemIndex].state = ItemState::Queued;
    if ( !m_fifo.contains( itemIndex ) )
      m_fifo.prepend( itemIndex );
    pumpLater();
    return;
  }

  BatchItem &item = m_items[itemIndex];
  item.state = ItemState::Running;
  item.percent = 0;
  item.error.clear();
  slot->itemIndex = itemIndex;
  m_slotItem[slotIndex] = itemIndex;
  m_startedAtMs[slotIndex] = QDateTime::currentMSecsSinceEpoch();

  BatchQueue *self = this;
  const bool stopOnFatal = m_stopOnFatal;
  // 索引与槽位按值捕获：BatchItem 本体走 shared_ptr（框架要求可拷贝共享），
  // 回调只带下标，回 owner 线程后索引依然有效（m_items 布局在批内不变）。
  const int idx = itemIndex;
  const int slotNo = slotIndex;
  auto shared = std::make_shared<BatchItem>( item );
  const ItemExecutor exec = m_executor;

  paleo::jobs::JobRunner<BatchItem>::Callbacks cb;

  // prepare 已在出队前完成（展开/判重/入队都在 owner 线程做完），这里只做
  // 一次空转确认——与 startConstraintJob 同形。
  cb.prepare = []( BatchItem &, QString * ) { return true; };

  // compute 跑在 worker：只做纯计算（不碰活 catalog / 不碰 UI）。
  cb.compute = [exec, shared]( BatchItem &j, const CancelFn &canceled,
                               const ProgressFn &progress ) -> bool {
    if ( !exec )
      return false;
    return exec( j, canceled, progress );
  };

  // commit 回 owner 线程：框架在失败终态**仍会**执行 commit（Job 带失败态），
  // 故这里按 job.error 分流成功/失败，而不是无脑写 Succeeded。
  cb.commit = [self, idx, slotNo, stopOnFatal]( BatchItem &j, QString * ) {
    if ( !j.error.isEmpty() )
    {
      self->finalizeItem( idx, ItemState::Failed, j.error );
      if ( stopOnFatal && self->m_trippedFatal )
      {
        // 熔断：立刻作废待跑项并停在跑项，不给后面每项白烧一次的机会。
        self->markAllQueuedPending();
        self->pump();
      }
    }
    else
    {
      self->finalizeItem( idx, ItemState::Succeeded, QString() );
    }
    self->onSlotFinished( slotNo, idx );
    return true;
  };

  // 取消/陈旧：不进 commit，协作停止 + 如实记 Cancelled（不谎报成功）。
  cb.onDropped = [self, idx, slotNo]( BatchItem &,
                                       paleo::jobs::DropReason reason ) {
    self->finalizeItem( idx, ItemState::Cancelled,
                        paleo::jobs::dropReasonText( reason ) );
    self->onSlotFinished( slotNo, idx );
  };

  const QString title =
      QObject::tr( "批次作业 %1 · %2" ).arg( item.horizon, item.methodId );
  // layerId 传层位：store 的 busy 门据此挡同层并发起算。
  PaleoTask *task =
      slot->runner->start( title, shared, cb, item.horizon, /*quiet=*/false );
  if ( !task )
  {
    // 忙则拒绝（槽位刚空但上一任务尚未清理）——该项回队首重试。
    slot->itemIndex = -1;
    m_slotItem[slotIndex] = -1;
    m_items[itemIndex].state = ItemState::Queued;
    m_fifo.prepend( itemIndex );
    return;
  }

  // 进度回写：worker 侧 reportStage 经 PaleoTask 排队回 owner 线程，编排层
  // 只读任务面（不绕进度面——与既有任务页同一条数据来源）。
  QPointer<PaleoTask> guard( task );
  const int captured = idx;
  QObject::connect(
      task, &PaleoTask::changed, m_ctx, [self, captured, guard]() {
        if ( guard.isNull() )
          return;
        if ( captured < 0 || captured >= self->m_items.size() )
          return;
        self->m_items[captured].percent = guard->percent();
        self->m_items[captured].stage = guard->stage();
        emit self->itemChanged( self->m_items[captured].itemId );
      } );
}

void BatchQueue::onSlotFinished( int slot, int itemIndex )
{
  if ( slot < 0 || slot >= m_slotItem.size() )
    return;
  // 耗时只在此项仍为 Running 时结算（commit/drop 已先改终态，则沿用已记值）。
  if ( itemIndex >= 0 && itemIndex < m_items.size() )
  {
    BatchItem &it = m_items[itemIndex];
    if ( it.state == ItemState::Running )
    {
      it.state = ItemState::Cancelled;
      if ( it.error.isEmpty() )
        it.error = QObject::tr( "作业未收尾" );
    }
    if ( m_startedAtMs[slot] > 0 )
      it.elapsedMs = QDateTime::currentMSecsSinceEpoch() - m_startedAtMs[slot];
  }
  m_slotItem[slot] = -1;
  m_startedAtMs[slot] = 0;
  if ( slot < static_cast<int>( m_slots.size() ) && m_slots[slot] )
    m_slots[slot]->itemIndex = -1;
  // 排队下一轮再 pump：commit 体内框架尚未 clearTask，此刻直接 pump 会命中
  // busy 提前返回并丢掉已出队的项（见头文件 pumpLater 说明）。
  pumpLater();
  finishIfDone();
}

bool BatchQueue::cancelItem( const QString &itemId )
{
  const int index = m_indexById.value( itemId, -1 );
  if ( index < 0 || index >= m_items.size() )
    return false;
  BatchItem &it = m_items[index];
  if ( it.state == ItemState::Succeeded || it.state == ItemState::Skipped ||
       it.state == ItemState::Failed || it.state == ItemState::Cancelled )
    return false; // 终态不可取消

  if ( it.state == ItemState::Running )
  {
    // 在跑：协作停止（Claim cancel 标志 + PaleoTask::requestCancel）。
    for ( int s = 0; s < static_cast<int>( m_slots.size() ); ++s )
    {
      if ( m_slots[s]->itemIndex == index && m_slots[s]->runner )
      {
        m_slots[s]->runner->requestCancel();
        return true;
      }
    }
    return false;
  }
  it.state = ItemState::Cancelled;
  it.error = QObject::tr( "已取消" );
  m_fifo.removeAll( index );
  writeState();
  emit itemChanged( it.itemId );
  emitProgress();
  finishIfDone();
  return true;
}

bool BatchQueue::cancelBatch()
{
  if ( !m_running )
    return false;
  markAllQueuedPending();
  for ( int s = 0; s < static_cast<int>( m_slots.size() ); ++s )
  {
    if ( m_slots[s]->itemIndex >= 0 && m_slots[s]->runner )
      m_slots[s]->runner->requestCancel();
  }
  writeState();
  emitProgress();
  finishIfDone();
  return true;
}

bool BatchQueue::retryFailed()
{
  // 批后也要能重试：批刚跑完（m_running 已复位）时点「重试失败项」是最自然的
  // 用户动作，不能只支持「批还在跑时重试」这一种时机。
  int retried = 0;
  for ( BatchItem &it : m_items )
  {
    if ( it.state != ItemState::Failed )
      continue;
    it.state = ItemState::Pending;
    it.error.clear();
    it.percent = 0;
    it.elapsedMs = 0;
    ++retried;
  }
  if ( retried == 0 )
    return false;

  if ( !m_running )
  {
    // 重新起批：清熔断态（重试是「前置条件已恢复」的显式动作），起跑时刻重钉。
    m_trippedFatal = false;
    m_fatalReason.clear();
    m_running = true;
    m_batchStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_fifo.clear();
    for ( int i = 0; i < m_items.size(); ++i )
    {
      if ( m_items[i].state == ItemState::Pending )
      {
        m_items[i].state = ItemState::Queued;
        m_fifo.append( i );
      }
    }
    writeState();
    emit batchStarted( m_batchId, m_items.size() );
    emitProgress();
    pump();
    return true;
  }

  for ( int i = 0; i < m_items.size(); ++i )
  {
    if ( m_items[i].state == ItemState::Pending )
    {
      m_items[i].state = ItemState::Queued;
      m_fifo.append( i );
    }
  }
  writeState();
  pump();
  return true;
}

void BatchQueue::finishIfDone()
{
  if ( !m_running )
    return;
  if ( pendingCount() > 0 || runningCount() > 0 )
    return;

  m_running = false;
  writeState();
  const BatchReport r = report();
  emit batchFinished( m_batchId, r.failed == 0 && !r.trippedFatal );
}

BatchReport BatchQueue::report() const
{
  BatchReport r;
  r.batchId = m_batchId;
  r.total = m_items.size();
  r.trippedFatal = m_trippedFatal;
  r.fatalReason = m_fatalReason;
  r.items = m_items;
  for ( const BatchItem &it : m_items )
  {
    switch ( it.state )
    {
      case ItemState::Succeeded:
        ++r.succeeded;
        break;
      case ItemState::Failed:
        ++r.failed;
        break;
      case ItemState::Skipped:
        ++r.skipped;
        break;
      case ItemState::Cancelled:
        ++r.cancelled;
        break;
      default:
        break;
    }
  }
  if ( m_batchStartedAtMs > 0 )
    r.totalElapsedMs = QDateTime::currentMSecsSinceEpoch() - m_batchStartedAtMs;
  return r;
}

} // namespace PaleoBatchQueue
