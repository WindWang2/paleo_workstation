#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"
#include "../src/ai/tileinference.h"
#include "../src/services/paleotaskservice.h"

// goal/ai-geological-assist 轮2：tile 批量推理。
// 纯函数断言（plan 覆盖恰好一次 / softmax 对测试内独立双精度实现 / 缝合
// 只写内区）+ 端到端（生成 seg3 模型 → 合成坡度网格 → 概率图/置信度/无数据
// 标记）+ 取消/诚实失败 + 异步任务（PaleoTaskService）取消后无泄漏（fd/RSS）。
class TestTileInference : public QObject
{
  Q_OBJECT
private slots:
  void planCoversEveryCellExactlyOnce();
  void softmaxMatchesIndependentImpl();
  void softmaxNodataStays255();
  // #143：非有限 logit（NaN 经感受野扩散）→ 无数据，不落到类 0/置信 1。
  void softmaxNonFiniteLogitsAreNodata();
  // #143：输入净化——NaN/Inf 置 0 并记无数据掩膜。
  void sanitizeModelInputMasksNonFinite();
  // #143：类数超过 255（类号与无数据 255 冲突）→ 全无数据。
  void softmaxRejectsTooManyClasses();
  void stitchWritesInnerRegionOnly();
  void endToEndProbabilityAndConfidence();
  void progressIsMonotonic();
  void cancelMidRunStopsHonestly();
  void fetchFailurePropagates();
  void badModelFailsHonestly();
  void nonTileModelRejectedBySignature();
  void asyncTaskCancelsWithoutLeak();

private:
  QString fixtureDir()
  {
    static QTemporaryDir dir; // 整个测试类共用一个模型目录
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
  // 合成数据：x = (col-45)*0.05 + (row-50)*0.01；左上角 3×4 NaN 模拟缺道。
  static float rampValue( int row, int col )
  {
    if ( row < 3 && col < 4 )
      return std::numeric_limits<float>::quiet_NaN();
    return float( ( col - 45 ) * 0.05 + ( row - 50 ) * 0.01 );
  }
  // seg3 logits (x, -x, 0.1) 的解析 argmax。
  static int analyticArgmax( float x )
  {
    const float l0 = x, l1 = -x, l2 = 0.1f;
    if ( l0 >= l1 && l0 >= l2 )
      return 0;
    if ( l1 >= l0 && l1 >= l2 )
      return 1;
    return 2;
  }
  // seg3 logits (x, -x, 0.1) 的解析置信度（1 − H/ln3，双精度）。
  static double analyticConfidence( float x )
  {
    const double l[3] = { double( x ), -double( x ), 0.1 };
    const double m = *std::max_element( l, l + 3 );
    double e[3], sum = 0;
    for ( int c = 0; c < 3; ++c )
    {
      e[c] = std::exp( l[c] - m );
      sum += e[c];
    }
    double entropy = 0;
    for ( int c = 0; c < 3; ++c )
    {
      const double p = e[c] / sum;
      entropy -= p > 0 ? p * std::log( p ) : 0;
    }
    return 1.0 - entropy / std::log( 3.0 );
  }
  static int countFds()
  {
    QDir fdDir( QStringLiteral( "/proc/self/fd" ) );
    const auto entries = fdDir.entryList( QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot );
    return entries.size();
  }
  static qint64 readRssKb()
  {
    QFile f( QStringLiteral( "/proc/self/status" ) );
    if ( !f.open( QIODevice::ReadOnly ) )
      return -1;
    const QByteArray all = f.readAll();
    const int idx = all.indexOf( "VmRSS:" );
    if ( idx < 0 )
      return -1;
    const QByteArray line = all.mid( idx, all.indexOf( '\n', idx ) - idx );
    const QList<QByteArray> parts = line.split( ' ' );
    for ( const QByteArray &p : parts )
      if ( !p.isEmpty() && p != "VmRSS:" )
        return p.toLongLong();
    return -1;
  }
  static bool rampFetch( int row0, int col0, int rows, int cols, QVector<float> &out,
                         QString & )
  {
    out.resize( rows * cols );
    for ( int r = 0; r < rows; ++r )
      for ( int c = 0; c < cols; ++c )
        out[r * cols + c] = rampValue( row0 + r, col0 + c );
    return true;
  }
};

void TestTileInference::planCoversEveryCellExactlyOnce()
{
  const QVector<InferenceTile> tiles = planInferenceTiles( 100, 80, 30, 25, 8 );
  QCOMPARE( tiles.size(), 16 ); // ceil(100/30) * ceil(80/25) = 4*4
  QVector<int> coverage( 100 * 80, 0 );
  for ( const InferenceTile &t : tiles )
  {
    QVERIFY( t.isValid() );
    QVERIFY( t.row0 <= t.innerRow0 && t.innerRow0 + t.innerRows <= t.row0 + t.rows );
    QVERIFY( t.col0 <= t.innerCol0 && t.innerCol0 + t.innerCols <= t.col0 + t.cols );
    QVERIFY( t.row0 >= 0 && t.col0 >= 0 && t.row0 + t.rows <= 100 && t.col0 + t.cols <= 80 );
    for ( int r = t.innerRow0; r < t.innerRow0 + t.innerRows; ++r )
      for ( int c = t.innerCol0; c < t.innerCol0 + t.innerCols; ++c )
        coverage[r * 80 + c] += 1;
  }
  for ( int cov : coverage )
    QCOMPARE( cov, 1 );
  QCOMPARE( tiles.first().row0, 0 );
  QCOMPARE( tiles.first().col0, 0 );
  QCOMPARE( tiles.last().row0 + tiles.last().rows, 100 );
  QCOMPARE( tiles.last().col0 + tiles.last().cols, 80 );
}

void TestTileInference::softmaxMatchesIndependentImpl()
{
  const int C = 3, R = 2, W = 3;
  // 类主序（NCHW 去掉 batch 维：logits[c*6+p]），6 像素 × 3 类：
  // p0 极化 / p1 均匀 / p2 近均匀 / p3 近 one-hot / p4 均匀大负 / p5 近均匀。
  const QVector<float> logits = {
    2.0f,    0.5f,  0.2f,  1000.0f, -5.0f, 7.5f, // class0
    -2.0f,   0.5f,  0.1f,  0.0f,    -5.1f, 7.4f, // class1
    0.0f,    0.5f,  0.15f, 0.0f,    -4.9f, 7.6f  // class2
  };
  QCOMPARE( logits.size(), C * R * W );
  TileClassGrid g;
  softmaxGrid( logits, C, R, W, {}, &g );
  QCOMPARE( g.argmax.size(), qsizetype( R * W ) );
  for ( int p = 0; p < R * W; ++p )
  {
    // 独立双精度复算（测试内第二实现，非产品代码复述）。
    double cmax = -1e30;
    for ( int c = 0; c < C; ++c )
      cmax = std::max( cmax, double( logits[c * R * W + p] ) );
    double sum = 0;
    double e[3];
    for ( int c = 0; c < C; ++c )
    {
      e[c] = std::exp( double( logits[c * R * W + p] ) - cmax );
      sum += e[c];
    }
    int best = 0;
    double entropy = 0, bestP = 0;
    for ( int c = 0; c < C; ++c )
    {
      const double prob = e[c] / sum;
      entropy -= prob > 0 ? prob * std::log( prob ) : 0;
      if ( prob > bestP )
      {
        bestP = prob;
        best = c;
      }
    }
    const double expectConf = 1.0 - entropy / std::log( 3.0 );
    QCOMPARE( int( g.argmax[p] ), best );
    QVERIFY2( qAbs( double( g.confidence[p] ) - expectConf ) < 1e-5,
              qPrintable( QStringLiteral( "p=%1 %2 vs %3" )
                            .arg( p )
                            .arg( g.confidence[p] )
                            .arg( expectConf ) ) );
    QVERIFY2( qAbs( double( g.probMax[p] ) - bestP ) < 1e-5, "probMax = argmax 类概率" );
    QVERIFY( g.confidence[p] >= 0.0f && g.confidence[p] <= 1.0f );
    QVERIFY( g.probMax[p] > 0.0f && g.probMax[p] <= 1.0f );
  }
  QVERIFY( g.confidence[3] > 0.999f ); // 近 one-hot → 高置信
  QVERIFY( g.confidence[4] < 0.1f );   // 接近均匀 → 低置信
  QVERIFY( g.confidence[5] < 0.1f );
}

void TestTileInference::softmaxNodataStays255()
{
  const QVector<float> logits = { 1.0f, 0.0f, 1.0f, 0.0f };
  const QVector<bool> valid = { true, false };
  TileClassGrid g;
  softmaxGrid( logits, 2, 1, 2, valid, &g );
  QCOMPARE( int( g.argmax[0] ), 0 );
  QCOMPARE( int( g.argmax[1] ), 255 ); // NaN 像素 → 无数据
}

void TestTileInference::softmaxNonFiniteLogitsAreNodata()
{
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  // 3 像素 × 3 类：p0 正常、p1 全 NaN、p2 一类 +Inf。
  const QVector<float> logits = { 0.1f, nan, 0.0f, 2.0f, nan, inf, 0.3f, nan, 0.0f };
  TileClassGrid g;
  softmaxGrid( logits, 3, 1, 3, QVector<bool>{ true, true, true }, &g );
  QCOMPARE( int( g.argmax[0] ), 1 );
  QVERIFY( g.confidence[0] > 0.0f && g.confidence[0] < 1.0f );
  for ( int p : { 1, 2 } )
  {
    QCOMPARE( int( g.argmax[p] ), 255 ); // 旧实现：类 0、置信 1.0
    QCOMPARE( g.confidence[p], 0.0f );
    QCOMPARE( g.probMax[p], 0.0f );
  }
}

void TestTileInference::sanitizeModelInputMasksNonFinite()
{
  QVector<float> data = { 1.0f, std::numeric_limits<float>::quiet_NaN(), -2.0f,
                          std::numeric_limits<float>::infinity() };
  QVector<bool> valid;
  QCOMPARE( sanitizeModelInput( data, &valid ), 2 );
  QCOMPARE( data, ( QVector<float>{ 1.0f, 0.0f, -2.0f, 0.0f } ) );
  QCOMPARE( valid, ( QVector<bool>{ true, false, true, false } ) );
}

void TestTileInference::softmaxRejectsTooManyClasses()
{
  const int C = 256;
  QVector<float> logits( C, 0.0f );
  logits[255] = 10.0f; // 类 255 最大——若不拦截会与无数据 255 混淆
  TileClassGrid g;
  softmaxGrid( logits, C, 1, 1, {}, &g );
  QCOMPARE( int( g.argmax[0] ), 255 );
  QCOMPARE( g.confidence[0], 0.0f );
  // 上限内（255 类，类号 0..254）正常
  QVector<float> ok( 255, 0.0f );
  ok[254] = 10.0f;
  softmaxGrid( ok, 255, 1, 1, {}, &g );
  QCOMPARE( int( g.argmax[0] ), 254 );
}

void TestTileInference::stitchWritesInnerRegionOnly()
{
  const int R = 40, C = 30;
  const QVector<InferenceTile> tiles = planInferenceTiles( R, C, 16, 12, 5 );
  QVERIFY( tiles.size() > 1 );
  TileClassGrid grid;
  grid.rows = R;
  grid.cols = C;
  grid.classes = 1;
  grid.argmax = QVector<quint8>( R * C, 255 );
  grid.confidence = QVector<float>( R * C, -1.0f );
  grid.probMax = QVector<float>( R * C, -1.0f );
  // 每 tile 结果按「读域坐标」编码位置；缝合后每格必须等于自己的整图坐标码
  //——若误写 halo 区，邻 tile 内区会被旧码覆盖，坐标对不上即暴露。
  for ( const InferenceTile &t : tiles )
  {
    TileClassGrid tg;
    tg.rows = t.rows;
    tg.cols = t.cols;
    tg.classes = 1;
    tg.argmax = QVector<quint8>( t.rows * t.cols, 0 );
    tg.confidence = QVector<float>( t.rows * t.cols, 0.0f );
    tg.probMax = QVector<float>( t.rows * t.cols, 0.0f );
    for ( int r = 0; r < t.rows; ++r )
      for ( int c = 0; c < t.cols; ++c )
      {
        const float code = float( ( t.row0 + r ) * 1000 + ( t.col0 + c ) );
        tg.confidence[r * t.cols + c] = code;
        tg.probMax[r * t.cols + c] = code;
      }
    stitchTileIntoGrid( t, tg, &grid );
  }
  for ( int r = 0; r < R; ++r )
    for ( int c = 0; c < C; ++c )
    {
      const float expect = float( r * 1000 + c );
      const int idx = r * C + c;
      QVERIFY2( grid.confidence[idx] == expect && grid.probMax[idx] == expect,
                qPrintable( QStringLiteral( "cell %1,%2 wrote %3 (halo bleed)" )
                              .arg( r )
                              .arg( c )
                              .arg( grid.confidence[idx] ) ) );
    }
}

void TestTileInference::endToEndProbabilityAndConfidence()
{
  ensureSeg3();
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );

  const int R = 100, W = 90;
  TileInferenceRequest req;
  req.model = QStringLiteral( "seg3" );
  req.gridRows = R;
  req.gridCols = W;
  req.tileRows = 30;
  req.tileCols = 26;
  req.halo = 7;
  req.fetch = &TestTileInference::rampFetch;
  TileInferenceResult res;
  QString err;
  QVERIFY2( runTileInference( &onnx, req, &res, {}, nullptr, &err ), qPrintable( err ) );
  QCOMPARE( res.tilesDone, res.tilesTotal );
  QCOMPARE( res.grid.rows, R );
  QCOMPARE( res.grid.cols, W );
  QCOMPARE( res.grid.classes, 3 );
  int nodata = 0;
  for ( int r = 0; r < R; ++r )
  {
    for ( int c = 0; c < W; ++c )
    {
      const int idx = r * W + c;
      const float x = rampValue( r, c );
      if ( std::isnan( x ) )
      {
        QCOMPARE( int( res.grid.argmax[idx] ), 255 );
        ++nodata;
        continue;
      }
      QCOMPARE( int( res.grid.argmax[idx] ), analyticArgmax( x ) );
      QVERIFY2( qAbs( double( res.grid.confidence[idx] ) - analyticConfidence( x ) ) < 1e-4,
                qPrintable( QStringLiteral( "conf(%1,%2 x=%3) %4 vs %5" )
                              .arg( r )
                              .arg( c )
                              .arg( x )
                              .arg( res.grid.confidence[idx] )
                              .arg( analyticConfidence( x ) ) ) );
      QVERIFY( res.grid.confidence[idx] >= 0.0f && res.grid.confidence[idx] <= 1.0f );
      QVERIFY( res.grid.probMax[idx] > 0.0f && res.grid.probMax[idx] <= 1.0f );
    }
  }
  QCOMPARE( nodata, 12 ); // 3×4 缺道块
  // |x| 大 → 高置信；x≈0.1 三类接近 → 低置信（熵真实反映，不造假）。
  const int confidentIdx = 10 * W + 88; // x ≈ 1.75（类0 明显领先）
  const int boundaryIdx = 50 * W + 47;  // x ≈ 0.1（类0/类2 并列，熵高）
  QVERIFY( res.grid.confidence[confidentIdx] > 0.45f );
  QVERIFY( res.grid.confidence[boundaryIdx] < 0.4f );
  QVERIFY( res.grid.confidence[boundaryIdx] < res.grid.confidence[confidentIdx] );
}

void TestTileInference::progressIsMonotonic()
{
  ensureSeg3();
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );
  TileInferenceRequest req;
  req.model = QStringLiteral( "seg3" );
  req.gridRows = 64;
  req.gridCols = 64;
  req.tileRows = 16;
  req.tileCols = 16;
  req.halo = 4;
  req.fetch = []( int, int, int rows, int cols, QVector<float> &out, QString & ) {
    out = QVector<float>( rows * cols, 0.3f );
    return true;
  };
  QVector<QPair<int, int>> seen;
  TileInferenceResult res;
  QString err;
  QVERIFY2( runTileInference( &onnx, req, &res,
                              [ &seen ]( int done, int total ) { seen.append( { done, total } ); },
                              nullptr, &err ),
            qPrintable( err ) );
  QCOMPARE( seen.size(), 16 );
  for ( int i = 0; i < seen.size(); ++i )
  {
    QCOMPARE( seen[i].second, 16 );
    QCOMPARE( seen[i].first, i + 1 ); // 严格单调 +1、总数恒定
  }
}

void TestTileInference::cancelMidRunStopsHonestly()
{
  ensureSeg3();
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );
  TileInferenceRequest req;
  req.model = QStringLiteral( "seg3" );
  req.gridRows = 128;
  req.gridCols = 128;
  req.tileRows = 8;
  req.tileCols = 8; // 256 tile
  req.halo = 2;
  std::atomic_int fetches{ 0 };
  req.fetch = [ &fetches ]( int, int, int rows, int cols, QVector<float> &out, QString & ) {
    fetches.fetch_add( 1 );
    out = QVector<float>( rows * cols, 0.2f );
    return true;
  };
  TileInferenceResult res;
  QString err;
  const bool ok = runTileInference(
    &onnx, req, &res, {}, [ &fetches ]() { return fetches.load() >= 5; }, &err );
  QVERIFY( !ok );
  QVERIFY2( err.contains( QStringLiteral( "已取消" ) ), qPrintable( err ) );
  QVERIFY2( err.contains( QStringLiteral( "5/256" ) ), qPrintable( err ) );
  QCOMPARE( res.tilesDone, 5 );
  QVERIFY( res.grid.isEmpty() ); // 中止不给部分产品
}

void TestTileInference::fetchFailurePropagates()
{
  ensureSeg3();
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );
  TileInferenceRequest req;
  req.model = QStringLiteral( "seg3" );
  req.gridRows = 32;
  req.gridCols = 32;
  req.tileRows = 32;
  req.tileCols = 32;
  req.fetch = []( int, int, int, int, QVector<float> &, QString &err ) {
    err = QStringLiteral( "模拟读体失败" );
    return false;
  };
  TileInferenceResult res;
  QString err;
  QVERIFY( !runTileInference( &onnx, req, &res, {}, nullptr, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "模拟读体失败" ) ), qPrintable( err ) );
  QVERIFY2( err.contains( QStringLiteral( "取数失败" ) ), "tile 序号与取数语义入错误链" );
}

void TestTileInference::badModelFailsHonestly()
{
  const QString garbage = fixtureDir() + QStringLiteral( "/garbage.onnx" );
  {
    QFile f( garbage );
    QVERIFY( f.open( QIODevice::WriteOnly ) );
    f.write( QByteArray::fromHex( "deadbeefcafebabe0f0f0f0f" ) );
    f.close();
  }
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );
  TileInferenceRequest req;
  req.model = QStringLiteral( "garbage" );
  req.gridRows = 16;
  req.gridCols = 16;
  req.fetch = []( int, int, int rows, int cols, QVector<float> &out, QString & ) {
    out = QVector<float>( rows * cols, 0.0f );
    return true;
  };
  TileInferenceResult res;
  QString err;
  QVERIFY( !runTileInference( &onnx, req, &res, {}, nullptr, &err ) );
  QVERIFY2( !err.isEmpty(), qPrintable( err ) );
  QVERIFY( res.grid.isEmpty() );
}

void TestTileInference::nonTileModelRejectedBySignature()
{
  QString werr;
  QVERIFY( OnnxFixtureWriter::writeAddScalar(
    fixtureDir() + QStringLiteral( "/scalar.onnx" ), 40.0f, 8, &werr ) );
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );
  TileInferenceRequest req;
  req.model = QStringLiteral( "scalar" );
  req.gridRows = 16;
  req.gridCols = 16;
  req.fetch = []( int, int, int rows, int cols, QVector<float> &out, QString & ) {
    out = QVector<float>( rows * cols, 0.0f );
    return true;
  };
  TileInferenceResult res;
  QString err;
  QVERIFY( !runTileInference( &onnx, req, &res, {}, nullptr, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "[1,1,H,W]" ) ),
            qPrintable( QStringLiteral( "signature gate: %1" ).arg( err ) ) );
}

void TestTileInference::asyncTaskCancelsWithoutLeak()
{
  ensureSeg3();
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );
  QVERIFY( onnx.loadModel( QStringLiteral( "seg3" ) ) );
  const int poolBefore = onnx.sessionPoolSize();
  const qint64 rssBefore = readRssKb();

  PaleoTaskService tasks;
  { // 热身：让任务池线程/事件基建先分配完——基线量的是「取消是否泄漏」，
    // 不是「系统首次运行是否分配」。
    QEventLoop warmLoop;
    PaleoTask *warm = tasks.start( QStringLiteral( "warmup" ),
                                   []( PaleoTask * ) -> QString { return QString(); },
                                   QString(), PaleoTask::Priority::Normal, true );
    connect( warm, &PaleoTask::finished, &warmLoop, &QEventLoop::quit );
    warmLoop.exec();
  }
  const int fdsBefore = countFds();
  TileInferenceRequest req;
  req.model = QStringLiteral( "seg3" );
  req.gridRows = 200;
  req.gridCols = 200;
  req.tileRows = 10;
  req.tileCols = 10; // 400 tile
  req.halo = 3;
  req.fetch = []( int, int, int rows, int cols, QVector<float> &out, QString & ) {
    QThread::msleep( 2 ); // 慢源：给取消留窗口
    out = QVector<float>( rows * cols, 0.25f );
    return true;
  };
  PaleoTask *task = tasks.start(
    QStringLiteral( "tile-cancel-test" ),
    [ &onnx, &req ]( PaleoTask *t ) -> QString {
      TileInferenceResult res;
      QString err;
      int lastPct = -1;
      const bool ok = runTileInference(
        &onnx, req, &res,
        [ t, &lastPct ]( int done, int total ) {
          const int pct = total > 0 ? done * 100 / total : 0;
          if ( pct != lastPct )
          {
            lastPct = pct;
            t->reportStage( QStringLiteral( "infer" ), pct );
          }
        },
        [ t ]() { return t->cancelRequested(); }, &err );
      return ok ? QString() : err;
    },
    QString(), PaleoTask::Priority::Normal, true );

  QVERIFY( task != nullptr );
  QEventLoop loop;
  QTimer::singleShot( 60, task, [ task ]() { task->requestCancel(); } );
  connect( task, &PaleoTask::finished, &loop, &QEventLoop::quit );
  loop.exec();

  QCOMPARE( task->state(), PaleoTask::State::Cancelled );
  // 取消后无泄漏：句柄数不涨、RSS 增长有界（粗上界抓会话/缓冲泄漏）、池不涨。
  QCOMPARE( countFds(), fdsBefore );
  QVERIFY2( readRssKb() - rssBefore < 200 * 1024,
            qPrintable( QStringLiteral( "rss %1 -> %2 kB" ).arg( rssBefore ).arg( readRssKb() ) ) );
  QCOMPARE( onnx.sessionPoolSize(), poolBefore );
}

int main( int argc, char *argv[] )
{
  QCoreApplication app( argc, argv );
  TestTileInference tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "tst_tileinference.moc"
