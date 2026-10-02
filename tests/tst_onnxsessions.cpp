#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <atomic>
#include <cmath>
#include <thread>

#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"

// goal/ai-geological-assist 轮1：微型 fixture 生成器（手写 protobuf，零网络）
// + 会话管理（池/指纹/warmup/失败分类）。全部模型在 QTemporaryDir 内程序化
// 生成——不再依赖 spikes/ 布局，测试完全封闭。
class TestOnnxSessions : public QObject
{
  Q_OBJECT
private slots:
  void fixtureWriterIsDeterministic();
  void sha256MatchesKnownDigest();
  void addScalarModelRuns();
  void segModelLogitsMatchAnalytic();
  void traceScorerMatchesSigmoid();
  void loadMetaCarriesFingerprintAndSignature();
  void warmupTimedOnStaticInput();
  void poolHitOnSameModelReload();
  void poolEvictsLruBeyondCapacity();
  void fingerprintChangeForcesRebuild();
  void missingModelClassifiedNotFound();
  void garbageBytesClassifiedBadFormat();
  void futureIrVersionClassifiedUnsupported();
  void failedLoadKeepsPoolAndSession();
  void inputDimMismatchHonestError();
  void concurrentRunsAllValid();

private:
  // seg3: y[0,c] = gain[c]*x + bias[c]，gains {1,-1,0}、biases {0,0,0.1}——
  // |x| 大 → 类 0/1 高置信；x≈0 → 三类接近均匀（熵≈ln3，置信度测试的靶）。
  QString writeSeg3( const QString &dir ) const
  {
    const QString p = dir + QStringLiteral( "/seg3.onnx" );
    OnnxFixtureWriter::writeSeg( p, { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } );
    return p;
  }
};

void TestOnnxSessions::fixtureWriterIsDeterministic()
{
  QTemporaryDir dir;
  QVERIFY2( dir.isValid(), "temp dir" );
  const QString a = dir.filePath( QStringLiteral( "a.onnx" ) );
  const QString b = dir.filePath( QStringLiteral( "b.onnx" ) );
  QVERIFY( OnnxFixtureWriter::writeSeg( a, { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } ) );
  QVERIFY( OnnxFixtureWriter::writeSeg( b, { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } ) );
  QCOMPARE( PaleoOnnxService::sha256OfFile( a ), PaleoOnnxService::sha256OfFile( b ) );
  QVERIFY( QFileInfo( a ).size() > 0 );
}

void TestOnnxSessions::sha256MatchesKnownDigest()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString p = dir.filePath( QStringLiteral( "known.bin" ) );
  QFile f( p );
  QVERIFY( f.open( QIODevice::WriteOnly ) );
  f.write( "abc" );
  f.close();
  QCOMPARE( PaleoOnnxService::sha256OfFile( p ),
            QStringLiteral( "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ) );
}

void TestOnnxSessions::addScalarModelRuns()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "add40.onnx" ) ), 40.0f ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  QVERIFY2( svc.loadModel( QStringLiteral( "add40" ), &err ), qPrintable( err ) );
  const QVector<float> out = svc.run( QStringLiteral( "x" ), { 2.0f }, { 1 }, &err );
  QVERIFY2( err.isEmpty(), qPrintable( err ) );
  QCOMPARE( out.size(), qsizetype( 1 ) );
  QVERIFY( qAbs( out[0] - 42.0f ) < 1e-5f );
}

void TestOnnxSessions::segModelLogitsMatchAnalytic()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  writeSeg3( dir.path() );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  QVERIFY2( svc.loadModel( QStringLiteral( "seg3" ), &err ), qPrintable( err ) );

  // x[0,0] = [[2, -2], [0, 0.5]]；期望 NCHW logits：
  // c0=x, c1=-x, c2=0.1（逐元素）。
  const QVector<float> in = { 2.0f, -2.0f, 0.0f, 0.5f };
  const OnnxTensor out =
    svc.runTensor( QStringLiteral( "x" ), in, { 1, 1, 2, 2 }, &err );
  QVERIFY2( err.isEmpty(), qPrintable( err ) );
  QCOMPARE( out.shape, QVector<int64_t>( { 1, 3, 2, 2 } ) );
  QCOMPARE( out.values.size(), qsizetype( 12 ) );
  for ( int h = 0; h < 2; ++h )
    for ( int w = 0; w < 2; ++w )
    {
      const float x = in[h * 2 + w];
      const int base = ( h * 2 + w );
      QVERIFY2( qAbs( out.values[base] - x ) < 1e-6f, "class0 logit = x" );
      QVERIFY2( qAbs( out.values[4 + base] + x ) < 1e-6f, "class1 logit = -x" );
      QVERIFY2( qAbs( out.values[8 + base] - 0.1f ) < 1e-6f, "class2 logit = 0.1" );
    }
}

void TestOnnxSessions::traceScorerMatchesSigmoid()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeTraceScorer(
    dir.filePath( QStringLiteral( "scorer.onnx" ) ), 8.0f, 0.0f ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  QVERIFY2( svc.loadModel( QStringLiteral( "scorer" ), &err ), qPrintable( err ) );

  const QVector<float> in = { 0.0f, 0.5f, 1.0f, -1.0f };
  const OnnxTensor out =
    svc.runTensor( QStringLiteral( "t" ), in, { 1, 1, 4 }, &err );
  QVERIFY2( err.isEmpty(), qPrintable( err ) );
  QCOMPARE( out.shape, QVector<int64_t>( { 1, 1, 4 } ) );
  for ( int i = 0; i < 4; ++i )
  {
    const double expect = 1.0 / ( 1.0 + std::exp( -8.0 * double( in[i] ) ) );
    QVERIFY2( qAbs( double( out.values[i] ) - expect ) < 1e-6,
              qPrintable( QStringLiteral( "%1 vs %2" ).arg( out.values[i] ).arg( expect ) ) );
  }
  // 值域契约：概率 ∈ (0,1)。
  for ( float v : out.values )
  {
    QVERIFY( v > 0.0f && v < 1.0f );
  }
}

void TestOnnxSessions::loadMetaCarriesFingerprintAndSignature()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString p = writeSeg3( dir.path() );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  OnnxModelMeta meta;
  QString err;
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "seg3" ), &meta, &err ), OnnxLoadStatus::Ok );
  QVERIFY2( err.isEmpty(), qPrintable( err ) );
  QCOMPARE( meta.name, QStringLiteral( "seg3" ) );
  QCOMPARE( meta.path, QDir::cleanPath( QFileInfo( p ).absoluteFilePath() ) );
  QCOMPARE( meta.sha256, PaleoOnnxService::sha256OfFile( p ) );
  QCOMPARE( meta.inputName, QStringLiteral( "x" ) );
  QCOMPARE( meta.inputShape, QVector<int64_t>( { 1, 1, -1, -1 } ) );
  QCOMPARE( meta.inputSignature, QStringLiteral( "x:float32[1,1,?,?]" ) );
  QCOMPARE( meta.inputElemType, 1 );
  QCOMPARE( svc.lastLoadStatus(), OnnxLoadStatus::Ok );
}

void TestOnnxSessions::warmupTimedOnStaticInput()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "add40.onnx" ) ) ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  OnnxModelMeta meta;
  QVERIFY( svc.loadModelMeta( QStringLiteral( "add40" ), &meta, nullptr ) == OnnxLoadStatus::Ok );
  QVERIFY2( meta.warmupMs >= 0, "warmup must have run on a static [1] input" );
  QVERIFY2( meta.warmupError.isEmpty(), qPrintable( meta.warmupError ) );
}

void TestOnnxSessions::poolHitOnSameModelReload()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "add40.onnx" ) ) ) );
  writeSeg3( dir.path() );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  QVERIFY( svc.loadModel( QStringLiteral( "add40" ), &err ) );
  QVERIFY( !svc.lastLoadPoolHit() );
  QCOMPARE( svc.sessionPoolSize(), 1 );
  QVERIFY( svc.loadModel( QStringLiteral( "seg3" ), &err ) );
  QCOMPARE( svc.sessionPoolSize(), 2 );
  QVERIFY( svc.loadModel( QStringLiteral( "add40" ), &err ) );
  QVERIFY2( svc.lastLoadPoolHit(), "second load of the same fingerprint must hit the pool" );
  QCOMPARE( svc.sessionPoolSize(), 2 );
  // 池命中后推理仍正确（活跃指针换了但会话健康）。
  const QVector<float> out = svc.run( QStringLiteral( "x" ), { 2.0f }, { 1 }, &err );
  QVERIFY2( err.isEmpty(), qPrintable( err ) );
  QVERIFY( qAbs( out[0] - 42.0f ) < 1e-5f );
}

void TestOnnxSessions::poolEvictsLruBeyondCapacity()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  for ( int i = 0; i < 5; ++i )
    QVERIFY( OnnxFixtureWriter::writeAddScalar(
      dir.filePath( QStringLiteral( "m%1.onnx" ).arg( i ) ), float( 40 + i ) ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  for ( int i = 0; i < 4; ++i )
    QVERIFY2( svc.loadModel( QStringLiteral( "m%1" ).arg( i ), &err ), qPrintable( err ) );
  QCOMPARE( svc.sessionPoolSize(), 4 );
  QVERIFY( svc.loadModel( QStringLiteral( "m4" ), &err ) );
  QCOMPARE( svc.sessionPoolSize(), 4 ); // m0（最久未用）被淘汰
  // m0 已被逐出 → 再加载必须重建（非池命中），且语义正确（+40）。
  QVERIFY2( svc.loadModel( QStringLiteral( "m0" ), &err ), qPrintable( err ) );
  QVERIFY( !svc.lastLoadPoolHit() );
  const QVector<float> out = svc.run( QStringLiteral( "x" ), { 2.0f }, { 1 }, &err );
  QVERIFY2( err.isEmpty(), qPrintable( err ) );
  QVERIFY( qAbs( out[0] - 42.0f ) < 1e-5f );
}

void TestOnnxSessions::fingerprintChangeForcesRebuild()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString p = dir.filePath( QStringLiteral( "flip.onnx" ) );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( p, 40.0f ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  QVERIFY( svc.loadModel( QStringLiteral( "flip" ), &err ) );
  QCOMPARE( svc.sessionPoolSize(), 1 );
  // 文件被替换（指纹变化）→ 旧会话淘汰、重建、语义更新，池大小不涨。
  QVERIFY( OnnxFixtureWriter::writeAddScalar( p, 80.0f ) );
  QVERIFY( svc.loadModel( QStringLiteral( "flip" ), &err ) );
  QVERIFY( !svc.lastLoadPoolHit() );
  QCOMPARE( svc.sessionPoolSize(), 1 );
  const QVector<float> out = svc.run( QStringLiteral( "x" ), { 2.0f }, { 1 }, &err );
  QVERIFY2( err.isEmpty(), qPrintable( err ) );
  QVERIFY2( qAbs( out[0] - 82.0f ) < 1e-5f, "reloaded session must reflect the new file" );
}

void TestOnnxSessions::missingModelClassifiedNotFound()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "missing" ), nullptr, &err ),
            OnnxLoadStatus::NotFound );
  QVERIFY2( !err.isEmpty(), "honest error string required" );
  QVERIFY( err.contains( QStringLiteral( "missing.onnx" ) ) );
  QCOMPARE( svc.lastLoadStatus(), OnnxLoadStatus::NotFound );
}

void TestOnnxSessions::garbageBytesClassifiedBadFormat()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString p = dir.filePath( QStringLiteral( "garbage.onnx" ) );
  QFile f( p );
  QVERIFY( f.open( QIODevice::WriteOnly ) );
  f.write( QByteArray::fromHex( "deadbeefcafebabe0f0f0f0f" ) );
  f.close();
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  const OnnxLoadStatus st = svc.loadModelMeta( QStringLiteral( "garbage" ), nullptr, &err );
  QVERIFY2( st == OnnxLoadStatus::BadFormat || st == OnnxLoadStatus::CreateFailed,
            qPrintable( QStringLiteral( "status=%1 err=%2" ).arg( int( st ) ).arg( err ) ) );
  QVERIFY2( !err.isEmpty(), "ORT raw message must survive in the error chain" );
  QVERIFY( err.contains( QStringLiteral( "garbage.onnx" ) ) );
}

void TestOnnxSessions::futureIrVersionClassifiedUnsupported()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeAddScalar(
    dir.filePath( QStringLiteral( "future.onnx" ) ), 40.0f, 9999 ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  const OnnxLoadStatus st = svc.loadModelMeta( QStringLiteral( "future" ), nullptr, &err );
  QCOMPARE( st, OnnxLoadStatus::UnsupportedIr );
  QVERIFY2( err.contains( QStringLiteral( "future.onnx" ) ), "error chain carries the path" );
}

void TestOnnxSessions::failedLoadKeepsPoolAndSession()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  writeSeg3( dir.path() );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  QVERIFY( svc.loadModel( QStringLiteral( "seg3" ), &err ) );
  QCOMPARE( svc.sessionPoolSize(), 1 );
  QVERIFY( !svc.loadModel( QStringLiteral( "missing" ), &err ) );
  QCOMPARE( svc.sessionPoolSize(), 1 ); // 池不被失败加载污染
  QCOMPARE( svc.loadedModel(), QStringLiteral( "seg3" ) );
  err.clear();
  const QVector<float> out = svc.run( QStringLiteral( "x" ), { 1.0f }, { 1, 1, 1, 1 }, &err );
  QVERIFY2( err.isEmpty(), qPrintable( err ) );
  QCOMPARE( out.size(), qsizetype( 3 ) );
}

void TestOnnxSessions::inputDimMismatchHonestError()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  writeSeg3( dir.path() );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  QVERIFY( svc.loadModel( QStringLiteral( "seg3" ), &err ) );
  // 元素数与 shape 不符 → ORT 拒绝，错误链不吞原始信息。
  const QVector<float> out = svc.run( QStringLiteral( "x" ), { 1.0f, 2.0f }, { 1, 1, 4, 4 }, &err );
  QVERIFY( out.isEmpty() );
  QVERIFY2( !err.isEmpty(), "dimension mismatch must fail honestly" );
}

void TestOnnxSessions::concurrentRunsAllValid()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  writeSeg3( dir.path() );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  QVERIFY( svc.loadModel( QStringLiteral( "seg3" ), &err ) );

  std::atomic_int failures{ 0 };
  auto worker = [ &svc, &failures ]( float x ) {
    for ( int i = 0; i < 20; ++i )
    {
      QString e;
      const OnnxTensor t =
        svc.runTensor( QStringLiteral( "x" ), { x }, { 1, 1, 1, 1 }, &e );
      if ( !e.isEmpty() || t.values.size() != 3 ||
           qAbs( t.values[0] - x ) > 1e-5f || qAbs( t.values[1] + x ) > 1e-5f )
        failures.fetch_add( 1 );
    }
  };
  const float xs[4] = { 0.25f, -0.25f, 2.0f, -2.0f };
  std::thread threads[4];
  for ( int i = 0; i < 4; ++i )
    threads[i] = std::thread( worker, xs[i] );
  for ( int i = 0; i < 4; ++i )
    threads[i].join();
  QCOMPARE( failures.load(), 0 );
}

int main( int argc, char *argv[] )
{
  // vendorRuntimeDir() walks up from applicationDirPath().
  QCoreApplication app( argc, argv );
  TestOnnxSessions tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "tst_onnxsessions.moc"
