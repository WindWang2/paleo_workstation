// 层：功能
#include "remotepredictrouter.h"

#include "onnxpredictionservice.h"
#include "tileinference.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <cmath>

// ai/ — 远端推理路由实现。

RemoteTransport::~RemoteTransport() = default;

// ---------------------------------------------------------------------------
// HttpRemoteTransport
// ---------------------------------------------------------------------------
HttpRemoteTransport::HttpRemoteTransport( const QUrl &base, QObject *parent )
  : QObject( parent )
  , m_base( base )
  , m_nam( new QNetworkAccessManager( this ) )
{
}

HttpRemoteTransport::~HttpRemoteTransport() = default;

QNetworkReply *HttpRemoteTransport::waitFinished( QNetworkReply *reply, int timeoutMs,
                                                   QString *error )
{
  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot( true );
  QObject::connect( reply, &QNetworkReply::finished, &loop, &QEventLoop::quit );
  QObject::connect( &timer, &QTimer::timeout, &loop, &QEventLoop::quit );
  timer.start( timeoutMs );
  loop.exec();
  if ( !timer.isActive() )
  {
    reply->abort();
    reply->deleteLater();
    if ( error )
      *error = QObject::tr( "远端请求超时（>%1ms）: %2" ).arg( timeoutMs ).arg( m_base.toString() );
    return nullptr;
  }
  return reply;
}

bool HttpRemoteTransport::healthCheck( int timeoutMs, QString *error )
{
  QNetworkRequest req( m_base.resolved( QUrl( QStringLiteral( "/health" ) ) ) );
  req.setTransferTimeout( 0 ); // 超时由 waitFinished 统一判
  QNetworkReply *reply = m_nam->get( req );
  reply = waitFinished( reply, timeoutMs, error );
  if ( !reply )
    return false;
  const bool ok = reply->error() == QNetworkReply::NoError;
  const int status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
  const QByteArray body = reply->readAll();
  const QString errDetail = reply->errorString();
  reply->deleteLater();
  if ( !ok || status != 200 )
  {
    if ( error )
      *error = QObject::tr( "健康检查失败: HTTP %1 (%2)" ).arg( status ).arg( errDetail );
    return false;
  }
  if ( !body.contains( "ok" ) )
  {
    if ( error )
      *error = QObject::tr( "健康检查应答异常: %1" ).arg( QString::fromUtf8( body ) );
    return false;
  }
  return true;
}

bool HttpRemoteTransport::predict( const RemotePredictionRequest &request, int timeoutMs,
                                   QVector<int> &cells, QString *error )
{
  QJsonObject body;
  body.insert( QStringLiteral( "id" ), request.id );
  body.insert( QStringLiteral( "horizon" ), request.horizon );
  body.insert( QStringLiteral( "kind" ), request.kind );
  body.insert( QStringLiteral( "columns" ), request.columns );
  body.insert( QStringLiteral( "rows" ), request.rows );
  QJsonArray samples;
  for ( float v : request.samples )
    samples.append( std::isnan( v ) ? QJsonValue() : QJsonValue( double( v ) ) );
  body.insert( QStringLiteral( "samples" ), samples );

  QNetworkRequest req( m_base.resolved( QUrl( QStringLiteral( "/predict" ) ) ) );
  req.setHeader( QNetworkRequest::ContentTypeHeader, QStringLiteral( "application/json" ) );
  req.setTransferTimeout( 0 );
  QNetworkReply *reply =
    m_nam->post( req, QJsonDocument( body ).toJson( QJsonDocument::Compact ) );
  reply = waitFinished( reply, timeoutMs, error );
  if ( !reply )
    return false;
  const bool ok = reply->error() == QNetworkReply::NoError;
  const int status = reply->attribute( QNetworkRequest::HttpStatusCodeAttribute ).toInt();
  const QByteArray payload = reply->readAll();
  const QString errDetail = reply->errorString();
  reply->deleteLater();
  if ( !ok || status != 200 )
  {
    if ( error )
      *error = QObject::tr( "远端预测失败: HTTP %1 (%2)" ).arg( status ).arg( errDetail );
    return false;
  }
  const QJsonObject doc = QJsonDocument::fromJson( payload ).object();
  const QJsonArray got = doc.value( QStringLiteral( "cells" ) ).toArray();
  if ( got.size() != request.columns * request.rows )
  {
    if ( error )
      *error = QObject::tr( "远端预测应答格数 %1 与请求网格 %2×%3 不符" )
                 .arg( got.size() )
                 .arg( request.columns )
                 .arg( request.rows );
    return false;
  }
  cells.clear();
  cells.reserve( got.size() );
  for ( const QJsonValue &v : got )
    cells.append( v.toInt() );
  return true;
}

// ---------------------------------------------------------------------------
// RemotePredictionRouter
// ---------------------------------------------------------------------------
RemotePredictionRouter::RemotePredictionRouter( RemoteTransport *transport, QObject *parent )
  : RemotePredictionService( parent )
  , m_transport( transport )
{
}

RemotePredictionRouter::~RemotePredictionRouter() = default;

void RemotePredictionRouter::setLocalFallback( PaleoOnnxService *onnx, const QString &model )
{
  m_onnx = onnx;
  m_localModel = model;
}

void RemotePredictionRouter::setFallbackEnabled( bool enabled )
{
  m_fallbackEnabled = enabled;
}

void RemotePredictionRouter::cancel()
{
  m_cancelled = true;
}

bool RemotePredictionRouter::runLocalFallback( const RemotePredictionRequest &request,
                                               QVector<int> &cells, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return false;
  };
  if ( !m_onnx || m_localModel.isEmpty() )
    return fail( QObject::tr( "无本地 ORT 降级（未绑定模型）" ) );
  if ( request.samples.size() != qsizetype( request.columns ) * request.rows )
    return fail( QObject::tr( "请求未携带网格数据——本地降级不造假（samples=%1，网格 %2×%3）" )
                   .arg( request.samples.size() )
                   .arg( request.columns )
                   .arg( request.rows ) );
  OnnxModelMeta meta; // #144：meta 随加载取回，推理绑定模型名
  if ( m_onnx->loadModelMeta( m_localModel, &meta, error ) != OnnxLoadStatus::Ok )
    return fail( error && !error->isEmpty() ? *error : QObject::tr( "本地模型加载失败" ) );

  if ( meta.inputShape.size() != 4 || meta.inputShape[0] > 1 || meta.inputShape[1] > 1 )
    return fail( QObject::tr( "本地模型输入须为 [1,1,H,W]，实际 %1" ).arg( meta.inputSignature ) );

  QVector<float> samples = request.samples;
  QVector<bool> valid;
  sanitizeModelInput( samples, &valid ); // #143：非有限样置 0 喂模型，输出端置 255

  QString runErr;
  const OnnxTensor out = m_onnx->runTensorOn( m_localModel, meta.inputName, samples,
                                            { 1, 1, request.rows, request.columns }, &runErr );
  if ( !runErr.isEmpty() )
    return fail( runErr );
  if ( out.shape.size() != 4 || out.shape[0] != 1 ||
       out.shape[2] != request.rows || out.shape[3] != request.columns )
    return fail( QObject::tr( "本地模型输出形状与请求网格不符" ) );
  if ( out.shape[1] < 1 || out.shape[1] > kMaxTileClasses )
    return fail( QObject::tr( "本地模型输出类数 %1 越界 [1, %2]" ).arg( out.shape[1] ).arg( kMaxTileClasses ) );

  TileClassGrid grid;
  softmaxGrid( out.values, int( out.shape[1] ), request.rows, request.columns, valid, &grid );
  cells = QVector<int>( grid.argmax.size(), 0 );
  for ( qsizetype i = 0; i < grid.argmax.size(); ++i )
    cells[i] = int( grid.argmax[i] ); // 255=nodata 语义与 tile 产品一致
  return true;
}

void RemotePredictionRouter::start( const RemotePredictionRequest &request )
{
  m_cancelled = false;
  m_lastRoute.clear();

  // 异步链（接口契约：start 立即返回，结果经信号回）。取消在阶段间检查。
  QTimer::singleShot( 0, this, [this, request]() {
    const auto giveUp = [this, &request]( const QString &reason ) {
      emit failed( request.id, reason );
    };
    if ( m_cancelled )
      return giveUp( QObject::tr( "已取消" ) );
    if ( !m_transport )
      return giveUp( QObject::tr( "未绑定远端传输" ) );

    emit progress( request.id, 5 );
    QString healthErr;
    const bool healthy = m_transport->healthCheck( m_healthTimeoutMs, &healthErr );
    if ( m_cancelled )
      return giveUp( QObject::tr( "已取消" ) );

    const auto degradeOrDie = [this, &request, &healthErr]( const QString &stageErr ) -> bool {
      // 远端不可用：先试本地降级，再如实失败（错误链两段都保留）。
      if ( !m_fallbackEnabled || !m_onnx || m_localModel.isEmpty() )
      {
        emit failed( request.id,
                     QObject::tr( "远端不可用且降级未配置: %1 / %2" ).arg( healthErr, stageErr ) );
        return true; // 已终态
      }
      emit progress( request.id, 50 );
      QVector<int> cells;
      QString localErr;
      if ( !runLocalFallback( request, cells, &localErr ) )
      {
        emit failed( request.id, QObject::tr( "远端失败 (%1)，本地降级失败 (%2)" )
                                    .arg( stageErr, localErr ) );
        return true;
      }
      RemotePredictionResult result;
      result.request = request;
      result.cells = cells;
      result.method = QStringLiteral( "local-ort:%1" ).arg( m_localModel );
      result.mock = false;
      m_lastRoute = result.method;
      emit completed( result );
      return true;
    };

    if ( !healthy )
      return (void)degradeOrDie( healthErr );

    emit progress( request.id, 30 );
    QVector<int> cells;
    QString predictErr;
    if ( m_cancelled )
      return giveUp( QObject::tr( "已取消" ) );
    if ( !m_transport->predict( request, m_predictTimeoutMs, cells, &predictErr ) )
      return (void)degradeOrDie( predictErr );

    RemotePredictionResult result;
    result.request = request;
    result.cells = cells;
    result.method = QStringLiteral( "remote" );
    result.mock = false;
    m_lastRoute = result.method;
    emit progress( request.id, 100 );
    emit completed( result );
  } );
}
