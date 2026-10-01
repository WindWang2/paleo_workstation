#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <cmath>
#include <limits>
#include <memory>

#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"
#include "../src/ai/remotepredictrouter.h"

// goal/ai-geological-assist 轮6：远端推理路由——健康检查/超时/断线降级到
// 本地 ORT。全部走 127.0.0.1 loopback fixture server（无外网端点）。
// 不断言墙钟时长（性能纪律）：超时行为只断言「结果正确走了降级」。
class LoopbackServer : public QObject
{
  Q_OBJECT
  public:
    enum class Mode { Normal, DeadOnAccept, Slow, Garbage };
    Mode mode = Mode::Normal;
    int healthHits = 0, predictHits = 0;

    bool start( Mode m )
    {
      mode = m;
      connect( &m_server, &QTcpServer::newConnection, this,
               [this] { serve( m_server.nextPendingConnection() ); } );
      return m_server.listen( QHostAddress::LocalHost, 0 );
    }
    QUrl baseUrl() const
    {
      return QUrl( QStringLiteral( "http://127.0.0.1:%1" ).arg( m_server.serverPort() ) );
    }

  private:
    void serve( QTcpSocket *sock )
    {
      if ( !sock )
        return;
      if ( mode == Mode::DeadOnAccept )
      {
        sock->abort();
        sock->deleteLater();
        return;
      }
      if ( mode == Mode::Garbage )
      {
        sock->write( QByteArrayLiteral( "not-http garbage\r\n\r\n" ) );
        sock->disconnectFromHost();
        sock->deleteLater();
        return;
      }
      if ( mode == Mode::Slow )
        return; // 挂起不答（socket 是 server 子对象，随其析构关闭）——逼超时路径
      // 连接生命周期全部挂在 socket 自身上：缓冲是 socket 子对象（随其释放），
      // 信号 receiver 也是 socket（销毁自动断连）——不维护手工指针列表，
      // 崩溃面（悬垂 removeOne）从结构上移除。
      const auto buf = std::make_shared<QByteArray>();
      connect( sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater );
      connect( sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
        buf->append( sock->readAll() );
        const int headerEnd = buf->indexOf( "\r\n\r\n" );
        if ( headerEnd < 0 )
          return;
        const QByteArray header = buf->left( headerEnd );
        const QByteArray body = buf->mid( headerEnd + 4 );
        const QString headerStr = QString::fromUtf8( header );
        const int contentLength =
          headerStr.section( "Content-Length:", 1, 1 ).section( "\r", 0, 0 ).trimmed().toInt();
        if ( body.size() < contentLength )
          return;
        const QString requestLine = headerStr.section( "\r\n", 0, 0 );
        respond( sock, requestLine, body );
        // 消费已处理字节——连接复用（keep-alive）时下一请求会接在同一
        // socket 上，残留会把 POST 误读成上一条 GET。
        *buf = buf->mid( headerEnd + 4 + contentLength );
      } );
    }
    void respond( QTcpSocket *sock, const QString &requestLine, const QByteArray &body )
    {
      QByteArray payload;
      if ( requestLine.startsWith( QStringLiteral( "GET /health" ) ) )
      {
        ++healthHits;
        payload = QByteArrayLiteral( "ok" );
      }
      else if ( requestLine.startsWith( QStringLiteral( "POST /predict" ) ) )
      {
        ++predictHits;
        const QJsonObject req = QJsonDocument::fromJson( body ).object();
        const int columns = req.value( QStringLiteral( "columns" ) ).toInt();
        const int rows = req.value( QStringLiteral( "rows" ) ).toInt();
        QJsonArray cells;
        for ( int i = 0; i < columns * rows; ++i )
          cells.append( 3 ); // 远端固定答 3——与本地模型的可区分断言
        payload = QJsonDocument( QJsonObject { { QStringLiteral( "cells" ), cells } } )
                    .toJson( QJsonDocument::Compact );
      }
      else
      {
        sock->write( QByteArrayLiteral( "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n" ) );
        return;
      }
      sock->write( QStringLiteral( "HTTP/1.1 200 OK\r\nContent-Length: %1\r\n\r\n" )
                     .arg( payload.size() )
                     .toUtf8() );
      sock->write( payload );
    }

    QTcpServer m_server;
};

class TestRemotePredictionRouter : public QObject
{
  Q_OBJECT
  private slots:
    void initTestCase();
    void healthyRemoteServesPredictions();
    void deadRemoteFallsBackToLocalOrt();
    void fallbackWithoutDataFailsHonestly();
    void remoteDownWithoutFallbackFailsHonestly();
    void slowServerDegradesToOurt();
    void garbageServerDegradesToOurt();
    void cancelBeforeChainStartsFails();

  private:
    static bool waitSpy( QSignalSpy &spy, int count = 1, int timeoutMs = 15000 )
    {
      QElapsedTimer clock;
      clock.start();
      while ( spy.size() < count && clock.elapsed() < timeoutMs )
        QCoreApplication::processEvents( QEventLoop::AllEvents, 50 );
      return spy.size() >= count;
    }
    static RemotePredictionRequest makeRequest( int columns, int rows, bool withSamples )
    {
      RemotePredictionRequest req;
      req.id = QStringLiteral( "r1" );
      req.horizon = QStringLiteral( "T1" );
      req.kind = QStringLiteral( "seismic" );
      req.columns = columns;
      req.rows = rows;
      if ( withSamples )
      {
        req.samples = QVector<float>( qsizetype( columns ) * rows, 2.0f ); // 类 0 明显领先
        if ( req.samples.size() > 1 )
          req.samples[1] = std::numeric_limits<float>::quiet_NaN(); // 降级路径的 nodata 位
      }
      return req;
    }
    QString fixtureDir()
    {
      static QTemporaryDir dir;
      return dir.path();
    }
    void ensureSeg3()
    {
      static bool done = false;
      if ( !done )
      {
        OnnxFixtureWriter::writeSeg( fixtureDir() + QStringLiteral( "/seg3.onnx" ),
                                     { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } );
        done = true;
      }
    }
};

void TestRemotePredictionRouter::initTestCase()
{
  qRegisterMetaType<RemotePredictionResult>( "RemotePredictionResult" );
  qRegisterMetaType<RemotePredictionRequest>( "RemotePredictionRequest" );
}

void TestRemotePredictionRouter::healthyRemoteServesPredictions()
{
  LoopbackServer server;
  QVERIFY( server.start( LoopbackServer::Mode::Normal ) );
  HttpRemoteTransport transport( server.baseUrl() );
  RemotePredictionRouter router( &transport );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QVERIFY2( waitSpy( completed ), "remote prediction must complete" );
  QCOMPARE( failed.size(), 0 );
  const RemotePredictionResult result =
    completed.at( 0 ).at( 0 ).value<RemotePredictionResult>();
  QCOMPARE( result.method, QStringLiteral( "remote" ) );
  QVERIFY( !result.mock );
  QCOMPARE( result.cells.size(), 16 );
  for ( int cell : result.cells )
    QCOMPARE( cell, 3 );
  QCOMPARE( router.lastRoute(), QStringLiteral( "remote" ) );
  QCOMPARE( server.predictHits, 1 );
}

void TestRemotePredictionRouter::deadRemoteFallsBackToLocalOrt()
{
  LoopbackServer server;
  QVERIFY( server.start( LoopbackServer::Mode::DeadOnAccept ) );
  HttpRemoteTransport transport( server.baseUrl() );
  ensureSeg3();
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );
  RemotePredictionRouter router( &transport );
  router.setLocalFallback( &onnx, QStringLiteral( "seg3" ) );
  router.setHealthTimeoutMs( 800 );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QVERIFY2( waitSpy( completed ), "dead remote must degrade to local ORT" );
  QCOMPARE( failed.size(), 0 );
  const RemotePredictionResult result =
    completed.at( 0 ).at( 0 ).value<RemotePredictionResult>();
  QCOMPARE( result.method, QStringLiteral( "local-ort:seg3" ) );
  QCOMPARE( result.cells.size(), 16 );
  for ( int i = 0; i < result.cells.size(); ++i )
  {
    if ( i == 1 )
      QCOMPARE( result.cells[i], 255 ); // NaN 采样位 → 无数据
    else
      QCOMPARE( result.cells[i], 0 );   // x=2 → 类 0
  }
  QCOMPARE( router.lastRoute(), QStringLiteral( "local-ort:seg3" ) );
}

void TestRemotePredictionRouter::fallbackWithoutDataFailsHonestly()
{
  LoopbackServer server;
  QVERIFY( server.start( LoopbackServer::Mode::DeadOnAccept ) );
  HttpRemoteTransport transport( server.baseUrl() );
  ensureSeg3();
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );
  RemotePredictionRouter router( &transport );
  router.setLocalFallback( &onnx, QStringLiteral( "seg3" ) );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, false ) ); // 无网格数据
  QVERIFY2( waitSpy( failed ), "no data → honest failure" );
  QCOMPARE( completed.size(), 0 );
  const QString reason = failed.at( 0 ).at( 1 ).toString();
  QVERIFY2( reason.contains( QStringLiteral( "不造假" ) ), qPrintable( reason ) );
  QVERIFY2( reason.contains( QStringLiteral( "远端失败" ) ), "错误链保留远端段" );
}

void TestRemotePredictionRouter::remoteDownWithoutFallbackFailsHonestly()
{
  LoopbackServer server;
  QVERIFY( server.start( LoopbackServer::Mode::DeadOnAccept ) );
  HttpRemoteTransport transport( server.baseUrl() );
  RemotePredictionRouter router( &transport ); // 未配本地降级
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QVERIFY2( waitSpy( failed ), "no fallback → honest failure" );
  QCOMPARE( completed.size(), 0 );
  const QString reason = failed.at( 0 ).at( 1 ).toString();
  QVERIFY2( reason.contains( QStringLiteral( "降级未配置" ) ), qPrintable( reason ) );
}

void TestRemotePredictionRouter::slowServerDegradesToOurt()
{
  LoopbackServer server;
  QVERIFY( server.start( LoopbackServer::Mode::Slow ) );
  HttpRemoteTransport transport( server.baseUrl() );
  ensureSeg3();
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );
  RemotePredictionRouter router( &transport );
  router.setLocalFallback( &onnx, QStringLiteral( "seg3" ) );
  router.setHealthTimeoutMs( 250 ); // 慢服务不答 → 超时判死 → 降级
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QVERIFY2( waitSpy( completed ), "timeout must degrade, not hang" );
  QCOMPARE( failed.size(), 0 );
  QCOMPARE( router.lastRoute(), QStringLiteral( "local-ort:seg3" ) );
}

void TestRemotePredictionRouter::garbageServerDegradesToOurt()
{
  LoopbackServer server;
  QVERIFY( server.start( LoopbackServer::Mode::Garbage ) );
  HttpRemoteTransport transport( server.baseUrl() );
  ensureSeg3();
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );
  RemotePredictionRouter router( &transport );
  router.setLocalFallback( &onnx, QStringLiteral( "seg3" ) );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QVERIFY2( waitSpy( completed, 1, 20000 ), "garbage HTTP must degrade" );
  QCOMPARE( failed.size(), 0 );
  QCOMPARE( router.lastRoute(), QStringLiteral( "local-ort:seg3" ) );
}

void TestRemotePredictionRouter::cancelBeforeChainStartsFails()
{
  LoopbackServer server;
  QVERIFY( server.start( LoopbackServer::Mode::Normal ) );
  HttpRemoteTransport transport( server.baseUrl() );
  RemotePredictionRouter router( &transport );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  router.cancel(); // 链在 singleShot(0) 才起跑——取消先到
  QVERIFY2( waitSpy( failed ), "cancelled start must report" );
  QCOMPARE( completed.size(), 0 );
  QVERIFY( failed.at( 0 ).at( 1 ).toString().contains( QStringLiteral( "已取消" ) ) );
}

int main( int argc, char *argv[] )
{
  QCoreApplication app( argc, argv );
  TestRemotePredictionRouter tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "tst_remotepredictrouter.moc"

