#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include <qgsapplication.h>
#include <qgsproject.h>

#include <gdal.h>
#include <cpl_conv.h>

#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/aiassistworkflow.h"

// goal/ai-geological-assist 轮3+5：AI 辅助工作流落地面。
// · tile 分类产品三件套（相/掩膜/置信度）——低置信像素被处理是硬断言；
//   产物按 T26 落 artifacts/derived + DERIVED + 只读纪律。
// · 追踪建议：建议对象不自动写——suggest/accept/veto 全程 catalog 字节
//   零变更，commitAccepted 才落盘（解释员裁决语义）。
class TestAiAssistWorkflow : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanup();
  void classifyTilesProducesThreeProducts();
  void classificationFailureIsHonestAndWritesNothing();
  void suggestionsFollowSyntheticHorizon();
  void suggestionsNeverAutoWrite();
  void commitAcceptedWritesArtifact();
  void commitWithoutAcceptedFails();
  void asyncClassificationCompletesAndDeclares();
  void asyncClassificationCancelledWritesNothing();

private:
  struct Fixture
  {
    QTemporaryDir dir;
    DataCatalog catalog;
    QgisProjectService projectSvc;
    LayerManifest manifest{ dir.filePath( QStringLiteral( "project.sqlite" ) ) };
    QgisLayerService layers{ &projectSvc, &manifest };
    PaleoOnnxService onnx;
    AiAssistWorkflow wf{ &layers };
  };
  bool initFixture( Fixture &f )
  {
    if ( !f.dir.isValid() || !f.catalog.open( f.dir.path() ) )
      return false;
    if ( !f.projectSvc.createProject( f.dir.filePath( QStringLiteral( "proj.qgz" ) ) ) )
      return false;
    if ( !f.manifest.open() )
      return false;
    const QString models = f.dir.filePath( QStringLiteral( "models" ) );
    if ( !QDir().mkpath( models ) )
      return false;
    if ( !OnnxFixtureWriter::writeSeg( models + QStringLiteral( "/seg3.onnx" ),
                                       { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } ) )
      return false;
    if ( !OnnxFixtureWriter::writeTraceScorer( models + QStringLiteral( "/scorer.onnx" ),
                                               8.0f, 0.0f ) )
      return false;
    f.onnx.setModelRoot( models );
    f.wf.setOnnxService( &f.onnx );
    f.wf.setCatalog( &f.catalog, f.dir.path() );
    return true;
  }
  static float rampValue( int row, int col )
  {
    if ( row < 3 && col < 4 )
      return std::numeric_limits<float>::quiet_NaN();
    return float( ( col - 25 ) * 0.05 + ( row - 30 ) * 0.01 );
  }
  static bool rampFetch( int row0, int col0, int rows, int cols, QVector<float> &out, QString & )
  {
    out.resize( rows * cols );
    for ( int r = 0; r < rows; ++r )
      for ( int c = 0; c < cols; ++c )
        out[r * cols + c] = rampValue( row0 + r, col0 + c );
    return true;
  }
  static int analyticArgmax( float x )
  {
    const float l0 = x, l1 = -x, l2 = 0.1f;
    if ( l0 >= l1 && l0 >= l2 )
      return 0;
    if ( l1 >= l0 && l1 >= l2 )
      return 1;
    return 2;
  }
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
  // 合成层位：h(il,xl) = 100 + 2*sin(il/3) + round((xl%5)/2)，拾取窗内唯一峰。
  static int syntheticHorizon( int il, int xl )
  {
    return 100 + int( 2.0 * std::sin( double( il ) / 3.0 ) ) + ( xl % 5 ) / 2;
  }
  static bool syntheticTraceFetch( int il, int xl, int begin, int count, QVector<float> &out,
                                   QString & )
  {
    out = QVector<float>( count, 0.0f );
    const int h = syntheticHorizon( il, xl );
    if ( h >= begin && h < begin + count )
      out[h - begin] = 1.0f; // 尖峰：scorer sigmoid(8·1)≈0.9997，其余≈0
    return true;
  }
  static QByteArray catalogBytes( const Fixture &f )
  {
    const QString dir = QFileInfo( f.catalog.catalogPath() ).absolutePath();
    QFile sqlite( dir + QStringLiteral( "/catalog.sqlite" ) );
    if ( !sqlite.open( QIODevice::ReadOnly ) )
      return QByteArray( "catalog.sqlite unreadable" );
    QByteArray wal;
    QFile walFile( dir + QStringLiteral( "/catalog.sqlite-wal" ) );
    if ( walFile.open( QIODevice::ReadOnly ) )
      wal = walFile.readAll();
    return sqlite.readAll() + QByteArray( "\n--wal--\n" ) + wal;
  }
  static const LayerDeclaration *findDecl( QgisLayerService &layers, const QString &layerId )
  {
    static LayerDeclaration holder;
    for ( const LayerDeclaration &d : layers.declared() )
      if ( d.layerId == layerId )
      {
        holder = d;
        return &holder;
      }
    return nullptr;
  }
  // 读单波段栅格（GDAL）；dt 断言由调用方做。
  static bool readRaster( const QString &path, int &xSize, int &ySize, QVector<float> &values )
  {
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
    if ( !ds )
      return false;
    xSize = GDALGetRasterXSize( ds );
    ySize = GDALGetRasterYSize( ds );
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    values.resize( xSize * ySize );
    const bool ok = GDALRasterIO( band, GF_Read, 0, 0, xSize, ySize, values.data(), xSize, ySize,
                                  GDT_Float32, 0, 0 ) == CE_None;
    GDALClose( ds );
    return ok;
  }
};

void TestAiAssistWorkflow::initTestCase()
{
  QVERIFY( QgisRuntime::isInitialized() );
  QVERIFY( GDALGetDriverByName( "GTiff" ) != nullptr );
}

void TestAiAssistWorkflow::cleanup()
{
  QgsProject::instance()->removeAllMapLayers();
}

void TestAiAssistWorkflow::classifyTilesProducesThreeProducts()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  const int R = 60, C = 50;
  QString err;
  QVERIFY2( f.wf.classifyTiles( QStringLiteral( "T1" ), QStringLiteral( "seg3" ), R, C, 20, 20, 5,
                                &TestAiAssistWorkflow::rampFetch, 0.35, &err ),
            qPrintable( err ) );
  const QStringList ids = f.wf.lastProductLayerIds();
  QCOMPARE( ids.size(), 3 );
  QCOMPARE( ids,
            QStringList() << QStringLiteral( "aifacies.T1.seg3" )
                          << QStringLiteral( "aifacies.T1.seg3.masked" )
                          << QStringLiteral( "confidence.T1.seg3" ) );

  int maskedLowConf = 0, nodata = 0;
  for ( const QString &id : ids )
  {
    const LayerDeclaration *d = findDecl( f.layers, id );
    QVERIFY2( d != nullptr, qPrintable( id ) );
    QCOMPARE( d->type, QStringLiteral( "raster" ) );
    QCOMPARE( d->group, QStringLiteral( "03_Predict" ) );
    QCOMPARE( d->horizon, QStringLiteral( "T1" ) );
    QVERIFY( QFile::exists( d->source ) );
    // T26：DERIVED 版本只读纪律。
    QVERIFY2( !QFileInfo( d->source ).isWritable(),
              qPrintable( QStringLiteral( "%1 must be read-only" ).arg( d->source ) ) );

    int x = 0, y = 0;
    QVector<float> v;
    QVERIFY2( readRaster( d->source, x, y, v ), qPrintable( d->source ) );
    QCOMPARE( x, C );
    QCOMPARE( y, R );
    for ( int r = 0; r < R; ++r )
      for ( int c = 0; c < C; ++c )
      {
        const float px = v[r * C + c];
        const int idx = r * C + c;
        const float xv = rampValue( r, c );
        if ( std::isnan( xv ) )
        {
          ++nodata;
          if ( id.endsWith( QLatin1String( ".masked" ) ) || id.startsWith( QLatin1String( "aifacies" ) ) )
            QCOMPARE( px, 255.0f );
          else
            QCOMPARE( px, -9999.0f );
          continue;
        }
        if ( id == QStringLiteral( "aifacies.T1.seg3" ) )
        {
          QCOMPARE( px, float( analyticArgmax( xv ) ) );
        }
        else if ( id == QStringLiteral( "confidence.T1.seg3" ) )
        {
          QVERIFY2( qAbs( double( px ) - analyticConfidence( xv ) ) < 1e-4,
                    qPrintable( QStringLiteral( "conf(%1,%2) %3" ).arg( r ).arg( c ).arg( px ) ) );
        }
        else
        {
          // 掩膜：低置信或无数据 → 255；否则保留相号。
          const double conf = analyticConfidence( xv );
          if ( conf < 0.35 )
          {
            ++maskedLowConf;
            QCOMPARE( px, 255.0f );
          }
          else
          {
            QCOMPARE( px, float( analyticArgmax( xv ) ) );
          }
        }
      }
  }
  QVERIFY2( nodata >= 3 * 12, "NaN 像素在三件产品里都被处理为 nodata" );
  QVERIFY2( maskedLowConf > 0, "低置信区像素被掩膜处理（阈值 0.35 下确有格点）" );
}

void TestAiAssistWorkflow::classificationFailureIsHonestAndWritesNothing()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  const QByteArray before = catalogBytes( f );
  QVERIFY( before.startsWith( "SQLite format 3" ) );
  const int declaredBefore = f.layers.declared().size();
  auto badFetch = []( int, int, int, int, QVector<float> &, QString &err ) {
    err = QStringLiteral( "合成取数故障" );
    return false;
  };
  QString err;
  QVERIFY( !f.wf.classifyTiles( QStringLiteral( "T1" ), QStringLiteral( "seg3" ), 32, 32, 16, 16, 4,
                                badFetch, 0.35, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "合成取数故障" ) ), qPrintable( err ) );
  QCOMPARE( f.layers.declared().size(), declaredBefore );
  QCOMPARE( catalogBytes( f ), before ); // 失败不登记任何产物
}

void TestAiAssistWorkflow::suggestionsFollowSyntheticHorizon()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  const int seedIl = 10, seedXl = 10;
  TrackingSeed seed { seedIl, seedXl, syntheticHorizon( seedIl, seedXl ) };
  QVector<TrackingSuggestion> suggestions;
  QString err;
  QVERIFY2( f.wf.suggestTracking( QStringLiteral( "T1" ), QStringLiteral( "scorer" ), { seed }, 32,
                                  4, &TestAiAssistWorkflow::syntheticTraceFetch, &suggestions,
                                  &err ),
            qPrintable( err ) );
  QCOMPARE( suggestions.size(), 81 ); // 1 种子 + (2·4+1)²−1 邻道
  for ( const TrackingSuggestion &s : suggestions )
  {
    QCOMPARE( s.sampleIndex, syntheticHorizon( s.inlineNo, s.xlineNo ) );
    if ( !s.isSeed )
    {
      QVERIFY( s.score > 0.99f );   // 尖峰处 sigmoid(8)≈0.9997
      QVERIFY( s.confidence > 0.9f ); // Bernoulli 熵归一近 1
    }
  }
  // 建议参考链：锚道必在建议集内（或即种子），且局部（切比雪夫距离≤2——
  // 同环先拾取的道在平手时可赢过种子，这是最近已拾取语义的正确行为）。
  QSet<QPair<int, int>> picked;
  picked.insert( { seedIl, seedXl } );
  for ( const TrackingSuggestion &s : suggestions )
    picked.insert( { s.inlineNo, s.xlineNo } );
  int referencingSeed = 0;
  for ( const TrackingSuggestion &s : suggestions )
  {
    if ( s.isSeed )
      continue;
    QVERIFY2( picked.contains( { s.sourceInline, s.sourceXline } ),
              "锚道必是已拾取集成员" );
    const int dIl = std::abs( s.inlineNo - s.sourceInline );
    const int dXl = std::abs( s.xlineNo - s.sourceXline );
    QVERIFY2( std::max( dIl, dXl ) <= 2,
              qPrintable( QStringLiteral( "锚道局部性 (%1,%2)->(%3,%4)" )
                            .arg( s.inlineNo )
                            .arg( s.xlineNo )
                            .arg( s.sourceInline )
                            .arg( s.sourceXline ) ) );
    if ( s.sourceInline == seedIl && s.sourceXline == seedXl )
      ++referencingSeed;
  }
  QVERIFY( referencingSeed >= 1 ); // 至少紧邻道直接参考种子
}

void TestAiAssistWorkflow::suggestionsNeverAutoWrite()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  const QByteArray before = catalogBytes( f );
  QVERIFY( before.startsWith( "SQLite format 3" ) );
  const int declaredBefore = f.layers.declared().size();

  const int seedIl = 5, seedXl = 5;
  TrackingSeed seed { seedIl, seedXl, syntheticHorizon( seedIl, seedXl ) };
  QVector<TrackingSuggestion> suggestions;
  QString err;
  QVERIFY( f.wf.suggestTracking( QStringLiteral( "H2" ), QStringLiteral( "scorer" ), { seed }, 32,
                                 3, &TestAiAssistWorkflow::syntheticTraceFetch, &suggestions,
                                 &err ) );
  QCOMPARE( f.wf.pendingCount( QStringLiteral( "H2" ) ), 48 );
  QCOMPARE( f.wf.acceptedCount( QStringLiteral( "H2" ) ), 1 ); // 种子锚点
  QCOMPARE( catalogBytes( f ), before ); // 生成建议零变更
  QCOMPARE( f.layers.declared().size(), declaredBefore );

  // 接受 2 条、否决 1 条——全部内存状态转移，catalog 仍零变更。
  QVERIFY( f.wf.acceptSuggestion( QStringLiteral( "H2" ), seedIl + 1, seedXl ) );
  QVERIFY( f.wf.acceptSuggestion( QStringLiteral( "H2" ), seedIl, seedXl + 1 ) );
  QVERIFY( f.wf.vetoSuggestion( QStringLiteral( "H2" ), seedIl - 1, seedXl ) );
  QCOMPARE( f.wf.pendingCount( QStringLiteral( "H2" ) ), 45 );
  QCOMPARE( f.wf.acceptedCount( QStringLiteral( "H2" ) ), 3 );
  QCOMPARE( f.wf.vetoSuggestion( QStringLiteral( "H2" ), seedIl, seedXl ), false ); // 种子不可否决
  QCOMPARE( catalogBytes( f ), before ); // 裁决仍零变更（Oracle：接受前 catalog 无变更）
  QCOMPARE( f.layers.declared().size(), declaredBefore );
}

void TestAiAssistWorkflow::commitAcceptedWritesArtifact()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  const int seedIl = 6, seedXl = 7;
  TrackingSeed seed { seedIl, seedXl, syntheticHorizon( seedIl, seedXl ) };
  QVector<TrackingSuggestion> suggestions;
  QString err;
  QVERIFY( f.wf.suggestTracking( QStringLiteral( "H3" ), QStringLiteral( "scorer" ), { seed }, 32,
                                 2, &TestAiAssistWorkflow::syntheticTraceFetch, &suggestions,
                                 &err ) );
  QVERIFY( f.wf.acceptSuggestion( QStringLiteral( "H3" ), seedIl + 1, seedXl + 1 ) );
  const QByteArray before = catalogBytes( f );
  QVERIFY( before.startsWith( "SQLite format 3" ) );

  QSignalSpy committed( &f.wf, &AiAssistWorkflow::acceptedCommitted );
  QVERIFY2( f.wf.commitAccepted( QStringLiteral( "H3" ), &err ), qPrintable( err ) );
  QCOMPARE( committed.size(), 1 );
  QVERIFY( catalogBytes( f ) != before ); // 提交才写 catalog

  const QString rel = committed.at( 0 ).at( 1 ).toString();
  const QString abs = f.dir.filePath( rel );
  QVERIFY2( QFile::exists( abs ), qPrintable( abs ) );
  QVERIFY( !QFileInfo( abs ).isWritable() ); // DERIVED 只读
  QFile jf( abs );
  QVERIFY( jf.open( QIODevice::ReadOnly ) );
  const QJsonObject doc = QJsonDocument::fromJson( jf.readAll() ).object();
  QCOMPARE( doc.value( QStringLiteral( "kind" ) ).toString(), QStringLiteral( "ai_tracking_picks" ) );
  const QJsonArray picks = doc.value( QStringLiteral( "picks" ) ).toArray();
  QCOMPARE( picks.size(), 2 ); // 种子 + 1 接受
  QCOMPARE( picks.at( 0 ).toObject().value( QStringLiteral( "seed" ) ).toBool( false ), true );
}

void TestAiAssistWorkflow::commitWithoutAcceptedFails()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  const QByteArray before = catalogBytes( f );
  QVERIFY( before.startsWith( "SQLite format 3" ) );
  QString err;
  QVERIFY( !f.wf.commitAccepted( QStringLiteral( "Nope" ), &err ) );
  QVERIFY2( !err.isEmpty(), "honest error required" );
  QCOMPARE( catalogBytes( f ), before );
  // error 缺省为 nullptr：失败路径不得解引用空指针
  QVERIFY( !f.wf.commitAccepted( QStringLiteral( "Nope" ) ) );
  QCOMPARE( catalogBytes( f ), before );
}

void TestAiAssistWorkflow::asyncClassificationCompletesAndDeclares()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  PaleoTaskService tasks;
  f.wf.setTaskService( &tasks );
  QSignalSpy done( &f.wf, &AiAssistWorkflow::tileClassificationDone );

  PaleoTask *task = f.wf.startClassification( QStringLiteral( "T9" ), QStringLiteral( "seg3" ),
                                              40, 40, 16, 16, 4,
                                              &TestAiAssistWorkflow::rampFetch, 0.35 );
  QVERIFY( task != nullptr );
  QEventLoop loop;
  connect( task, &PaleoTask::finished, &loop, &QEventLoop::quit );
  loop.exec();
  QCOMPARE( task->state(), PaleoTask::State::Succeeded );
  // 产品收尾在主线程 finished 之后同步跑——再等一拍事件循环。
  QTest::qWait( 0 );
  QCOMPARE( done.size(), 1 );
  const QStringList ids = done.at( 0 ).at( 1 ).toStringList();
  QCOMPARE( ids.size(), 3 );
  for ( const QString &id : ids )
  {
    const LayerDeclaration *d = findDecl( f.layers, id );
    QVERIFY2( d != nullptr, qPrintable( id ) );
    QVERIFY( QFile::exists( d->source ) );
  }
}

void TestAiAssistWorkflow::asyncClassificationCancelledWritesNothing()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  PaleoTaskService tasks;
  f.wf.setTaskService( &tasks );
  const QByteArray before = catalogBytes( f );
  QVERIFY( before.startsWith( "SQLite format 3" ) );
  QSignalSpy done( &f.wf, &AiAssistWorkflow::tileClassificationDone );

  auto slowFetch = []( int, int, int rows, int cols, QVector<float> &out, QString & ) {
    QThread::msleep( 2 );
    out = QVector<float>( rows * cols, 0.2f );
    return true;
  };
  PaleoTask *task = f.wf.startClassification( QStringLiteral( "TC" ), QStringLiteral( "seg3" ),
                                              160, 160, 8, 8, 2, slowFetch, 0.35 );
  QVERIFY( task != nullptr );
  QTimer::singleShot( 40, task, [task]() { task->requestCancel(); } );
  QEventLoop loop;
  connect( task, &PaleoTask::finished, &loop, &QEventLoop::quit );
  loop.exec();
  QCOMPARE( task->state(), PaleoTask::State::Cancelled );
  QTest::qWait( 0 );
  QCOMPARE( done.size(), 0 ); // 取消：无产品、无成功信号
  QCOMPARE( catalogBytes( f ), before );
}

int main( int argc, char *argv[] )
{
  if ( !QgisRuntime::isInitialized() )
    QgisRuntime::initialize( QStringLiteral( "/usr" ) );
  QgsApplication::processingRegistry();
  GDALAllRegister();
  TestAiAssistWorkflow tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_aiassistworkflow.moc"
