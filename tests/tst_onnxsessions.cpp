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
#include "../src/ai/onnxlocalpredictor.h"
#include "../src/ai/remotepredictionservice.h"

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
  // #479 锁分离：会话构建锁（m_loadMutex）与推理锁（m_mutex 短临界区）分离
  // 的行为锚定——模型加载期间推理不停摆、tile 并行不退化单线程。
  void concurrentLoadBuildsSingleSession();   // 并发首载不重复建会话
  void inferenceSurvivesPoolChurnUnderLoad(); // 加载/LRU 淘汰期间推理不悬垂
  // #144：runTensorOn 绑定模型名——别的任务换了活动模型也不受影响；
  // 未加载的模型明确失败，不落到活动模型。
  void runTensorOnBindsToNamedModel();
  // OnnxLocalPredictor（远端失败降级引擎）错误分支直测：此前只经
  // tst_remotepredictrouter 的 happy path 间接覆盖。
  void localPredictorRefusesMissingSamples();
  void localPredictorRejectsNonGridInputModel();
  void localPredictorRejectsOversizedClassCount();
  void localPredictorHappyPathArgmax();
  void warmupMultiChannelDynamicShapeSucceeds();

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

void TestOnnxSessions::concurrentLoadBuildsSingleSession()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "m40.onnx" ) ), 40.0f ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );

  // 4 线程同时首载同一模型：构建锁串行化后只应建一个会话（后来者池命中
  // 直接复用），不能出现 4 份并发构建的重复会话。
  std::atomic_int oks{ 0 };
  auto loader = [ &svc, &oks ]() {
    for ( int i = 0; i < 8; ++i )
    {
      QString e;
      OnnxModelMeta meta;
      if ( svc.loadModelMeta( QStringLiteral( "m40" ), &meta, &e ) == OnnxLoadStatus::Ok )
        oks.fetch_add( 1 );
    }
  };
  std::thread threads[4];
  for ( auto &t : threads )
    t = std::thread( loader );
  for ( auto &t : threads )
    t.join();
  QCOMPARE( oks.load(), 32 );
  QCOMPARE( svc.sessionPoolSize(), 1 );
}

void TestOnnxSessions::inferenceSurvivesPoolChurnUnderLoad()
{
  // 6 个 addScalar 模型（池容量 4）——加载线程循环加载必触发 LRU 淘汰/重建；
  // 同时推理线程持续 runTensorOn。#479 retire/release 协议的行为锚定：
  //   · 推理拿到的会话即使被淘汰也活到 Run 结束（不悬垂、不崩溃）；
  //   · 成功的推理结果必须正确（y = x + k）；
  //   · 失败只允许「会话不在池中」（淘汰竞态的诚实错误），不得是别的错。
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  constexpr int kModels = 6;
  const float gains[kModels] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f };
  for ( int i = 0; i < kModels; ++i )
    QVERIFY( OnnxFixtureWriter::writeAddScalar(
      dir.filePath( QStringLiteral( "m%1.onnx" ).arg( i ) ), gains[i] ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );

  std::atomic<bool> stop{ false };
  std::atomic_int wrongResults{ 0 };
  std::atomic_int unexpectedErrors{ 0 };
  std::atomic_int successes{ 0 };

  std::atomic_int loads{ 0 };
  std::thread churn( [ & ]() {
    QString e;
    while ( !stop.load() )
      for ( int i = 0; i < kModels; ++i )
      {
        svc.loadModelMeta( QStringLiteral( "m%1" ).arg( i ), nullptr, &e );
        loads.fetch_add( 1 );
        QThread::msleep( 1 ); // 高速淘汰/重建，但留可见窗口（对抗性满速
                              // 循环会把断言退化为调度竞走）
      }
  } );

  const int kRounds = 60;
  for ( int r = 0; r < kRounds; ++r )
  {
    for ( int i = 0; i < kModels; ++i )
    {
      QString e;
      const OnnxTensor t = svc.runTensorOn(
        QStringLiteral( "m%1" ).arg( i ), QStringLiteral( "x" ), { 2.0f }, { 1 }, &e );
      if ( e.isEmpty() )
      {
        successes.fetch_add( 1 );
        if ( qAbs( t.values.value( 0 ) - ( 2.0f + gains[i] ) ) > 1e-5f )
          wrongResults.fetch_add( 1 );
      }
      else if ( !e.contains( QStringLiteral( "不在池中" ) ) )
        unexpectedErrors.fetch_add( 1 );
    }
  }
  stop = true;
  churn.join();

  // 契约锚定：搅动期不错果、失败只有「会话不在池中」的诚实错误（淘汰
  // 竞态）；是否恰好抢到成功属调度，不做断言。搅动确实发生了。
  QCOMPARE( wrongResults.load(), 0 );
  QCOMPARE( unexpectedErrors.load(), 0 );
  QVERIFY( loads.load() >= kModels );
  // 确定性成功路径：停止搅动后补一次装载，推理必须成功且正确
  //（y = x + k；retire/release 协议收尾后池路径可正常命中）。
  QString loadErr;
  QVERIFY( svc.loadModelMeta( QStringLiteral( "m0" ), nullptr, &loadErr ) ==
           OnnxLoadStatus::Ok );
  QString runErr;
  const OnnxTensor after = svc.runTensorOn(
    QStringLiteral( "m0" ), QStringLiteral( "x" ), { 2.0f }, { 1 }, &runErr );
  QVERIFY2( runErr.isEmpty(), qPrintable( runErr ) );
  QVERIFY( qAbs( after.values.value( 0 ) - 3.0f ) <= 1e-5f );
  Q_UNUSED( successes );
}

void TestOnnxSessions::runTensorOnBindsToNamedModel()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "m40.onnx" ) ), 40.0f ) );
  QVERIFY( OnnxFixtureWriter::writeAddScalar( dir.filePath( QStringLiteral( "m80.onnx" ) ), 80.0f ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  QString err;
  OnnxModelMeta meta40;
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "m40" ), &meta40, &err ), OnnxLoadStatus::Ok );
  QVERIFY( svc.loadModel( QStringLiteral( "m80" ), &err ) ); // 活动模型被换成 m80

  const OnnxTensor bound =
    svc.runTensorOn( QStringLiteral( "m40" ), meta40.inputName, { 2.0f }, { 1 }, &err );
  QVERIFY2( err.isEmpty(), qPrintable( err ) );
  QVERIFY2( qAbs( bound.values.value( 0 ) - 42.0f ) < 1e-5f, "绑定 m40 的推理不能跑到活动模型 m80 上" );
  const QVector<float> active = svc.run( QStringLiteral( "x" ), { 2.0f }, { 1 }, &err );
  QVERIFY( qAbs( active.value( 0 ) - 82.0f ) < 1e-5f ); // 旧接口语义不变

  const OnnxTensor missing =
    svc.runTensorOn( QStringLiteral( "nope" ), meta40.inputName, { 2.0f }, { 1 }, &err );
  QVERIFY( missing.values.isEmpty() );
  QVERIFY( !err.isEmpty() );

  // 并发：一个线程反复切换活动模型，另一个线程绑定 m40 推理——结果恒为 42。
  std::atomic<bool> stop { false };
  std::thread switcher( [&]() {
    QString e;
    while ( !stop.load() )
    {
      svc.loadModel( QStringLiteral( "m80" ), &e );
      svc.loadModel( QStringLiteral( "m40" ), &e );
    }
  } );
  int wrong = 0;
  for ( int i = 0; i < 200; ++i )
  {
    QString e;
    const OnnxTensor t =
      svc.runTensorOn( QStringLiteral( "m40" ), meta40.inputName, { 2.0f }, { 1 }, &e );
    if ( !e.isEmpty() || qAbs( t.values.value( 0 ) - 42.0f ) > 1e-5f )
      ++wrong;
  }
  stop = true;
  switcher.join();
  QCOMPARE( wrong, 0 );
}

// ---- OnnxLocalPredictor（远端失败降级引擎）错误分支直测 ----
// 此前只经 tst_remotepredictrouter 的 seg3 happy path 间接覆盖（NaN→255），
// 错误分支零直测。

void TestOnnxSessions::localPredictorRefusesMissingSamples()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  writeSeg3( dir.path() );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  OnnxLocalPredictor pred( &svc, QStringLiteral( "seg3" ) );

  RemotePredictionRequest req; // samples 空——红线：没有数据就不造假
  req.columns = 4;
  req.rows = 3;
  QVector<int> cells;
  QString err;
  QVERIFY( !pred.predict( req, cells, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "不造假" ) ), qPrintable( err ) );
}

void TestOnnxSessions::localPredictorRejectsNonGridInputModel()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  // addScalar：输入 float[1]（非 [1,1,H,W] 网格模型）
  QVERIFY( OnnxFixtureWriter::writeAddScalar(
      dir.filePath( QStringLiteral( "scalar.onnx" ) ), 40.0f ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  OnnxLocalPredictor pred( &svc, QStringLiteral( "scalar" ) );

  RemotePredictionRequest req;
  req.columns = 4;
  req.rows = 3;
  req.samples = QVector<float>( 12, 1.0f );
  QVector<int> cells;
  QString err;
  QVERIFY( !pred.predict( req, cells, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "输入" ) ), qPrintable( err ) );
}

void TestOnnxSessions::localPredictorRejectsOversizedClassCount()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  // 256 类 > kMaxTileClasses(255)：tileinference 的 255=nodata 保留语义
  QVector<float> gains( 256, 1.0f ), biases( 256, 0.0f );
  QVERIFY( OnnxFixtureWriter::writeSeg(
      dir.filePath( QStringLiteral( "wide.onnx" ) ), gains, biases ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  OnnxLocalPredictor pred( &svc, QStringLiteral( "wide" ) );

  RemotePredictionRequest req;
  req.columns = 4;
  req.rows = 3;
  req.samples = QVector<float>( 12, 0.5f );
  QVector<int> cells;
  QString err;
  QVERIFY( !pred.predict( req, cells, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "越界" ) ), qPrintable( err ) );
}

void TestOnnxSessions::localPredictorHappyPathArgmax()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  writeSeg3( dir.path() ); // y[c] = gains[c]*x + biases[c]，gains {1,-1,0}
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  OnnxLocalPredictor pred( &svc, QStringLiteral( "seg3" ) );

  RemotePredictionRequest req;
  req.columns = 4;
  req.rows = 2;
  req.samples = QVector<float>{ 2.0f, -2.0f, 0.0f, 0.05f,   // 行 0
                                -1.5f, 3.0f, -0.02f, 1.0f }; // 行 1
  QVector<int> cells;
  QString err;
  QVERIFY2( pred.predict( req, cells, &err ), qPrintable( err ) );
  QCOMPARE( cells.size(), 8 );
  // argmax 解析解：x>0.05 → 类 0；x<-0.05 → 类 1；其间 → 类 2（bias 0.1）
  QCOMPARE( cells, ( QVector<int>{ 0, 1, 2, 2, 1, 0, 2, 0 } ) );
}

void TestOnnxSessions::warmupMultiChannelDynamicShapeSucceeds()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  // Input: [1, 3, -1, -1] with dynamic H, W and 3 channels
  const QString modelPath = dir.filePath( QStringLiteral( "multi_dynamic.onnx" ) );
  QVERIFY( OnnxFixtureWriter::writeSeg( modelPath, { 1.0f, 2.0f, 0.5f }, { 0.1f, -0.1f, 0.0f },
                                        8, nullptr, -1, -1, 3 ) );
  PaleoOnnxService svc;
  svc.setModelRoot( dir.path() );
  OnnxModelMeta meta;
  QString err;
  QCOMPARE( svc.loadModelMeta( QStringLiteral( "multi_dynamic" ), &meta, &err ), OnnxLoadStatus::Ok );
  QVERIFY2( err.isEmpty(), qPrintable( err ) );
  QCOMPARE( meta.inputShape, QVector<int64_t>( { 1, 3, -1, -1 } ) );
  QVERIFY2( meta.warmupMs >= 0, "Warmup should succeed for multi-channel dynamic model" );
  QVERIFY2( meta.warmupError.isEmpty(), qPrintable( meta.warmupError ) );
}

int main( int argc, char *argv[] )
{
  // vendorRuntimeDir() walks up from applicationDirPath().
  QCoreApplication app( argc, argv );
  TestOnnxSessions tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "tst_onnxsessions.moc"
