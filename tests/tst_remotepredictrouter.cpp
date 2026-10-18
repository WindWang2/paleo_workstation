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
#include <QThread>
#include <QTimer>

#include <cmath>
#include <limits>
#include <memory>

#include <atomic>

#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxlocalpredictor.h"
#include "../src/ai/onnxpredictionservice.h"
#include "../src/ai/remotepredictrouter.h"

// goal/ai-geological-assist 轮6：远端推理路由——健康检查/超时/断线降级到
// 本地 ORT。全部走 127.0.0.1 loopback fixture server（无外网端点）。
// 不断言墙钟时长（性能纪律）：超时行为只断言「结果正确走了降级」。
class LoopbackServer : public QObject
{
  Q_OBJECT
  public:
    enum class Mode { Normal, DeadOnAccept, Slow, Garbage, ReturnNullCells, OversizeDeclared, OversizeUndeclared };
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

    QJsonObject lastRequest;
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
      if ( ( mode == Mode::OversizeDeclared || mode == Mode::OversizeUndeclared ) &&
           requestLine.startsWith( QStringLiteral( "GET /health" ) ) )
      {
        // #469：先发头与 256 KiB，再挂住连接不收尾——旧实现只在 finished
        // 时检查大小，此处永远等不到 finished，只会拖到超时。
        ++healthHits;
        if ( mode == Mode::OversizeDeclared )
          sock->write( QByteArrayLiteral( "HTTP/1.1 200 OK\r\nContent-Length: 104857600\r\n\r\n" ) );
        else
          sock->write( QByteArrayLiteral( "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" ) );
        sock->write( QByteArray( 256 * 1024, 'o' ) );
        return;
      }
      if ( requestLine.startsWith( QStringLiteral( "GET /health" ) ) )
      {
        ++healthHits;
        payload = QByteArrayLiteral( "ok" );
      }
      else if ( requestLine.startsWith( QStringLiteral( "POST /predict" ) ) )
      {
        ++predictHits;
        const QJsonObject req = QJsonDocument::fromJson( body ).object();
        lastRequest = req;
        const int columns = req.value( QStringLiteral( "columns" ) ).toInt();
        const int rows = req.value( QStringLiteral( "rows" ) ).toInt();
        QJsonArray cells;
        for ( int i = 0; i < columns * rows; ++i )
        {
          if ( mode == Mode::ReturnNullCells && i == 1 )
            cells.append( QJsonValue() ); // null
          else
            cells.append( 3 ); // 远端固定答 3——与本地模型的可区分断言
        }
        if (req.value("kind") == "wells") {
          QJsonArray points;
          for (const auto &v : req.value("wells").toArray()) {
            const auto well = v.toObject();
            points << QJsonObject{{"id", well.value("id")}, {"x", well.value("x")}, {"y", well.value("y")},
              {"facies_code", 3}, {"facies_intervals", QJsonArray{QJsonObject{{"top", 0}, {"bottom", 10}, {"code", 3}}}}};
          }
          payload = QJsonDocument(QJsonObject{{"points", points}}).toJson(QJsonDocument::Compact);
        } else
          payload = QJsonDocument( QJsonObject { { QStringLiteral( "cells" ), cells } } ).toJson( QJsonDocument::Compact );
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

// 方向51：手动传输——不发网络请求，由测试决定何时「回包」。用来确定性地
// 断言世代号守卫（取消/新一轮之后到达的应答必须被丢弃）。
class ManualTransport : public RemoteTransport
{
  Q_OBJECT
  public:
    int healthStarts = 0, predictStarts = 0, aborts = 0;
    quint64 lastRunId = 0;
    static constexpr quint64 kStaleRunId = 999999;
    void healthCheck( int, quint64 runId ) override
    {
      ++healthStarts;
      lastRunId = runId;
    }
    void predict( const RemotePredictionRequest &, int, quint64 runId ) override
    {
      ++predictStarts;
      lastRunId = runId;
    }
    void abort() override { ++aborts; }
    // 正常回包：原样带回调用方的轮次号。
    void settleHealth( bool ok, const QString &error )
    {
      emit healthDone( lastRunId, ok, error );
    }
    void settlePredict( const QVector<int> &cells, const QString &error )
    {
      emit predictDone( lastRunId, cells, error );
    }
    // 迟到回包：带着一个**不是当前在飞轮次**的号（router 已换新号或已取消，
    // 其 m_runId=0）。用一个大哨兵而不是 lastRunId-1：0 与 cancel() 后的
    // 失效号撞车会掩盖真实语义。
    void settleStaleHealth( bool ok, const QString &error )
    {
      emit healthDone( kStaleRunId, ok, error );
    }
    void settleStalePredict( const QVector<int> &cells, const QString &error )
    {
      emit predictDone( kStaleRunId, cells, error );
    }
};

// 方向51：本地降级桩——没有 ONNX 运行库的构建里也能测「远端不可用 → 本地
// 降级」这条语义（router 只认 LocalPredictor 接口，不再直接调 ORT）。
class StubLocalPredictor : public LocalPredictor
{
  public:
    bool configured = true;
    bool failAnyway = false;
    QVector<int> cells;
    bool isConfigured() const override { return configured; }
    QString engineId() const override { return QStringLiteral( "stub-engine" ); }
    bool predict( const RemotePredictionRequest &request, QVector<int> &out,
                  QString *error ) override
    {
      if ( !configured )
      {
        if ( error ) *error = QObject::tr( "未绑定本地引擎" );
        return false;
      }
      if ( request.samples.size() != qsizetype( request.columns ) * request.rows )
      {
        if ( error ) *error = QObject::tr( "请求未携带网格数据——本地降级不造假" );
        return false;
      }
      if ( failAnyway )
      {
        if ( error ) *error = QObject::tr( "桩引擎失败" );
        return false;
      }
      out = cells;
      return true;
    }
};

class TestRemotePredictionRouter : public QObject
{
  Q_OBJECT
  private slots:
    void initTestCase();
    void startReturnsBeforeAnySignalAndNeverBlocks();
    void stalePredictReplyAfterCancelIsDropped();
    void cancelDuringFlightReportsCancelledOnceAndDropsLateReply();
    void stubLocalPredictorDegradesWithoutOnnxRuntime();
    void unconfiguredLocalPredictorFailsHonestly();
    void healthyRemoteServesPredictions();
    void wellBatchSendsAttributesAndReceivesPoints();
    void deadRemoteFallsBackToLocalOrt();
    void fallbackWithoutDataFailsHonestly();
    void remoteDownWithoutFallbackFailsHonestly();
    void slowServerDegradesToOurt();
    void garbageServerDegradesToOurt();
    void cancelBeforeChainStartsFails();
    void remoteJsonNullDeserializedToNodataNegative9999();
    void localFallbackTranslatesFaciesCodesAndNodata();
    void oversizeResponseAbortsDuringReceive();
    // #479：本地 ORT 降级不得在 GUI 线程槽内同步执行（SHA 全读+会话构建+
    // warmup+全网格推理是秒~分钟级重活）——断言 predict 落在非 GUI 线程且
    // completed/failed 信号语义不变；取消后迟到的降级结果按过期丢弃。
    void localFallbackRunsOffMainThread();
    void cancelledLocalFallbackResultIsDropped();

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

void TestRemotePredictionRouter::startReturnsBeforeAnySignalAndNeverBlocks()
{
  // 方向51 硬约束：start() 立即返回，链在后续事件循环里跑（不得嵌套事件
  // 循环把调用方卡住）。同一轮里同步观察不到任何终态信号。
  ManualTransport transport;
  RemotePredictionRouter router( &transport );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QCOMPARE( completed.size(), 0 );
  QCOMPARE( failed.size(), 0 );
  QCOMPARE( transport.healthStarts, 0 ); // 健康检查也还没发起
  // 链跑起来后（一次事件循环）才发起健康检查。
  QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  QCOMPARE( transport.healthStarts, 1 );
  router.cancel();
}

void TestRemotePredictionRouter::stalePredictReplyAfterCancelIsDropped()
{
  ManualTransport transport;
  StubLocalPredictor local;
  local.cells = QVector<int>( 16, 7 );
  RemotePredictionRouter router( &transport );
  router.setLocalPredictor( &local );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  QCOMPARE( transport.healthStarts, 1 );
  transport.settleHealth( true, {} );       // 健康 → 发起预测
  QCOMPARE( transport.predictStarts, 1 );
  router.cancel();                          // 取消作废本轮
  QVERIFY( transport.aborts >= 1 );
  // 迟到的预测应答（取消后才到、且带的是上一轮号）必须被丢弃：既不完成也
  // 不再报错——取消后补一个 completed 就是"幽灵结果"。
  transport.settleStalePredict( QVector<int>( 16, 3 ), {} );
  QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  QCOMPARE( completed.size(), 0 );
  QCOMPARE( failed.size(), 1 ); // 只有取消那一条
  QVERIFY( failed.at( 0 ).at( 1 ).toString().contains( QStringLiteral( "已取消" ) ) );
  QVERIFY( router.lastRoute().isEmpty() );
  QVERIFY( !router.busy() );
}

void TestRemotePredictionRouter::cancelDuringFlightReportsCancelledOnceAndDropsLateReply()
{
  ManualTransport transport;
  RemotePredictionRouter router( &transport );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  // 还在等健康检查时取消：router 须报一次"已取消"，随后的健康应答不再产生
  // 第二个终态（双回会让 UI 出现"失败后又完成"的幽灵态）。
  router.cancel();
  QCOMPARE( failed.size(), 1 );
  transport.settleStaleHealth( true, {} );
  transport.settleStalePredict( QVector<int>( 16, 3 ), {} );
  QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  QCOMPARE( failed.size(), 1 );
  QCOMPARE( transport.predictStarts, 0 ); // 已取消的轮次不再往下走
}

void TestRemotePredictionRouter::stubLocalPredictorDegradesWithoutOnnxRuntime()
{
  // 没有 ORT 也能验证降级语义：远端健康检查失败 → 走 LocalPredictor 接口。
  ManualTransport transport;
  StubLocalPredictor local;
  local.cells = QVector<int>( 16, 7 );
  RemotePredictionRouter router( &transport );
  router.setLocalPredictor( &local );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  transport.settleHealth( false, QStringLiteral( "健康检查失败: 桩" ) );
  QVERIFY2( waitSpy( completed ), "远端不健康 → 走本地降级" );
  QCOMPARE( failed.size(), 0 );
  const RemotePredictionResult result =
    completed.at( 0 ).at( 0 ).value<RemotePredictionResult>();
  QCOMPARE( result.method, QStringLiteral( "stub-engine" ) );
  QVERIFY( !result.mock );
  QCOMPARE( result.cells.size(), 16 );
  for ( int cell : result.cells )
    QCOMPARE( cell, 7 );
  QCOMPARE( router.lastRoute(), QStringLiteral( "stub-engine" ) );
}

void TestRemotePredictionRouter::unconfiguredLocalPredictorFailsHonestly()
{
  ManualTransport transport;
  StubLocalPredictor local;
  local.configured = false; // 降级引擎缺失
  RemotePredictionRouter router( &transport );
  router.setLocalPredictor( &local );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  transport.settleHealth( false, QStringLiteral( "健康检查失败: 桩" ) );
  QVERIFY2( waitSpy( failed ), "降级不可用 → 如实失败" );
  QCOMPARE( completed.size(), 0 );
  QVERIFY2( failed.at( 0 ).at( 1 ).toString().contains( QStringLiteral( "降级未配置" ) ),
            qPrintable( failed.at( 0 ).at( 1 ).toString() ) );
}

void TestRemotePredictionRouter::wellBatchSendsAttributesAndReceivesPoints() {
  LoopbackServer server;
  QVERIFY(server.start(LoopbackServer::Mode::Normal));
  HttpRemoteTransport transport(server.baseUrl()); RemotePredictionRouter router(&transport);
  auto request = makeRequest(1, 1, false); request.kind = "wells";
  request.wells = {QVariantMap{{"id", "W1"}, {"x", 100.0}, {"y", 200.0},
    {"vector_intervals", QVariantList{QVariantMap{{"lithology", "砂岩"}}}},
    {"vector_factors", QVariantList{QVariantMap{{"sand_ratio", 0.4}}}}},
    QVariantMap{{"id", "W2"}, {"x", 110.0}, {"y", 210.0}}};
  request.facies = {QVariantMap{{"code", 3}, {"name", "河道"}}};
  QSignalSpy done(&router, &RemotePredictionService::completed);
  router.start(request); QVERIFY(waitSpy(done));
  const auto result = qvariant_cast<RemotePredictionResult>(done.first().first());
  QVERIFY(!result.mock); QCOMPARE(result.points.size(), 2); QVERIFY(result.cells.isEmpty());
  const auto sent = server.lastRequest.value("wells").toArray().first().toObject();
  QCOMPARE(sent.value("vector_intervals").toArray().first().toObject().value("lithology").toString(), QString("砂岩"));
  QCOMPARE(sent.value("vector_factors").toArray().first().toObject().value("sand_ratio").toDouble(), 0.4);
  QVERIFY(!result.points.first().toMap().value("facies_intervals").toString().isEmpty());
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
      QCOMPARE( result.cells[i], -9999 ); // NaN 采样位 → 无数据 (-9999)
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

void TestRemotePredictionRouter::remoteJsonNullDeserializedToNodataNegative9999()
{
  LoopbackServer server;
  QVERIFY( server.start( LoopbackServer::Mode::ReturnNullCells ) );
  HttpRemoteTransport transport( server.baseUrl() );
  RemotePredictionRouter router( &transport );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 2, 2, true ) );
  QVERIFY2( waitSpy( completed ), "server returning null cells must complete" );
  QCOMPARE( failed.size(), 0 );
  const RemotePredictionResult res = completed.at( 0 ).at( 0 ).value<RemotePredictionResult>();
  QCOMPARE( res.cells.size(), 4 );
  QCOMPARE( res.cells[0], 3 );
  QCOMPARE( res.cells[1], -9999 ); // P0-5: JSON null MUST be -9999, NOT 0!
}

void TestRemotePredictionRouter::localFallbackTranslatesFaciesCodesAndNodata()
{
  ensureSeg3();
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );
  OnnxLocalPredictor predictor( &onnx, QStringLiteral( "seg3" ) );
  RemotePredictionRequest req = makeRequest( 2, 2, true );
  req.facies = QVariantList{
    QVariantMap{ { QStringLiteral( "code" ), 101 }, { QStringLiteral( "name" ), QStringLiteral( "f1" ) } },
    QVariantMap{ { QStringLiteral( "code" ), 102 }, { QStringLiteral( "name" ), QStringLiteral( "f2" ) } },
    QVariantMap{ { QStringLiteral( "code" ), 103 }, { QStringLiteral( "name" ), QStringLiteral( "f3" ) } },
  };
  QVector<int> cells;
  QString err;
  QVERIFY2( predictor.predict( req, cells, &err ), qPrintable( err ) );
  QCOMPARE( cells.size(), 4 );
  QCOMPARE( cells[0], 101 ); // P0-6: 0-based argmax 0 translated to facies code 101!
  QCOMPARE( cells[1], -9999 ); // P0-4: NoData must be -9999, NOT 255!
}

int main( int argc, char *argv[] )
{
  QCoreApplication app( argc, argv );
  TestRemotePredictionRouter tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "tst_remotepredictrouter.moc"

// ---------------------------------------------------------------------------
// #479：本地降级异步化。慢桩 predictor：进入 predict 置 inFlight、150ms 后
// 置 finished——主线程泵事件能观察到 inFlight 且自身不被卡，即证明 predict
// 与 GUI 线程并发（同步执行时 settleHealth 根本不会先返回）。
// ---------------------------------------------------------------------------
namespace
{
class SlowLocalPredictor : public LocalPredictor
{
  public:
    std::atomic_bool insidePredict{ false };
    std::atomic_bool finishedPredict{ false };
    QThread *predictThread = nullptr;
    bool isConfigured() const override { return true; }
    QString engineId() const override { return QStringLiteral( "slow-stub" ); }
    bool predict( const RemotePredictionRequest &request, QVector<int> &out,
                  QString * ) override
    {
      predictThread = QThread::currentThread();
      insidePredict.store( true );
      QThread::msleep( 150 ); // 模拟 SHA 全读+会话构建+warmup+全网格推理
      out = QVector<int>( qsizetype( request.columns ) * request.rows, 7 );
      finishedPredict.store( true );
      return true;
    }
};
} // namespace

void TestRemotePredictionRouter::localFallbackRunsOffMainThread()
{
  ManualTransport transport;
  SlowLocalPredictor local;
  RemotePredictionRouter router( &transport );
  router.setLocalPredictor( &local );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  transport.settleHealth( false, QStringLiteral( "健康检查失败: 桩" ) );
  // settleHealth 已返回而 predict 仍在跑 = 降级不在 GUI 线程同步执行。
  QElapsedTimer clock;
  clock.start();
  while ( !local.insidePredict.load() && clock.elapsed() < 5000 )
    QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  QVERIFY2( local.insidePredict.load(), "降级推理必须已在工作线程开跑" );
  QVERIFY2( waitSpy( completed ), "异步降级须完成并发出 completed" );
  QCOMPARE( failed.size(), 0 );
  QVERIFY2( local.predictThread != nullptr, "predict 已执行" );
  QVERIFY2( local.predictThread != QThread::currentThread(),
            "降级推理必须移出 GUI 线程（#479 P2）" );
  const RemotePredictionResult result =
    completed.at( 0 ).at( 0 ).value<RemotePredictionResult>();
  QCOMPARE( result.method, QStringLiteral( "slow-stub" ) );
  QCOMPARE( result.cells.size(), 16 );
  QCOMPARE( router.lastRoute(), QStringLiteral( "slow-stub" ) );
  QVERIFY( !router.busy() );
}

void TestRemotePredictionRouter::cancelledLocalFallbackResultIsDropped()
{
  ManualTransport transport;
  SlowLocalPredictor local;
  RemotePredictionRouter router( &transport );
  router.setLocalPredictor( &local );
  QSignalSpy completed( &router, &RemotePredictionService::completed );
  QSignalSpy failed( &router, &RemotePredictionService::failed );
  router.start( makeRequest( 4, 4, true ) );
  QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  transport.settleHealth( false, QStringLiteral( "健康检查失败: 桩" ) );
  QElapsedTimer clock;
  clock.start();
  while ( !local.insidePredict.load() && clock.elapsed() < 5000 )
    QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  QVERIFY( local.insidePredict.load() );
  // 降级推理在飞时取消：终态是「已取消」，迟到的降级 completed 不补发。
  router.cancel();
  QCOMPARE( failed.size(), 1 );
  QVERIFY( failed.at( 0 ).at( 1 ).toString().contains( QStringLiteral( "已取消" ) ) );
  clock.restart();
  while ( !local.finishedPredict.load() && clock.elapsed() < 5000 )
    QCoreApplication::processEvents( QEventLoop::AllEvents, 20 );
  QVERIFY( local.finishedPredict.load() );
  QCoreApplication::processEvents( QEventLoop::AllEvents, 200 ); // 排队的结算回调落地
  QCOMPARE( completed.size(), 0 ); // 取消后的降级结果 = 幽灵，必须丢弃
  QCOMPARE( failed.size(), 1 );    // 不再补第二条终态
  QVERIFY( !router.busy() );
}


// #469：响应体上限必须在接收途中生效（声明超限 / 未声明长度但已收超限两路），
// 不得等整个响应缓冲进内存后才在 finished 里检查。旧实现：服务端挂住连接时
// 只能拖到 30s 超时，且报的是「超时」而非「超出上限」。
void TestRemotePredictionRouter::oversizeResponseAbortsDuringReceive()
{
  for ( const auto mode : { LoopbackServer::Mode::OversizeDeclared, LoopbackServer::Mode::OversizeUndeclared } )
  {
    LoopbackServer server;
    QVERIFY( server.start( mode ) );
    HttpRemoteTransport transport( server.baseUrl() );
    QCOMPARE( transport.maxResponseBytes(), HttpRemoteTransport::kDefaultMaxResponseBytes );
    transport.setMaxResponseBytes( 64 * 1024 );
    QSignalSpy health( &transport, &RemoteTransport::healthDone );
    transport.healthCheck( 30000, 7 );
    QVERIFY2( waitSpy( health, 1, 10000 ), "超限响应必须在接收途中被中止，而不是拖到超时" );
    QCOMPARE( health.at( 0 ).at( 0 ).toULongLong(), quint64( 7 ) );
    QCOMPARE( health.at( 0 ).at( 1 ).toBool(), false );
    const QString err = health.at( 0 ).at( 2 ).toString();
    QVERIFY2( err.contains( QStringLiteral( "上限" ) ), qPrintable( err ) );
    // abort 同步触发的 finished 判为过期：不得重复回包。
    QCoreApplication::processEvents( QEventLoop::AllEvents, 200 );
    QCOMPARE( health.size(), 1 );
  }
}
