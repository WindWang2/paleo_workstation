#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>
#include <limits>

#include "Data/Sgy/SgyVolume.h"

#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"
#include "../src/ai/tileinference.h"

// goal/ai-geological-assist 轮7：AI 辅助推理性能档案。
// 纪律：禁绝对毫秒断言（TEST-02 教训）——比率门（批式单 tile 均耗 ≤ 单发
// 均耗×4，防每-tile 泄漏式回退）+ 不变量（inferenceMs ≤ elapsedMs）；
// 真机 966MB 体数据走 env 门控（PALEO_SEISMIC_REAL_SGY /
// PALEO_REAL_PROJECT_AREA），输出 `BASELINE <metric> = <value>` 行供手工
// 誊入 docs/progress/ai-assist.md，未设置环境整套跳过。
class TestAiAssistPerf : public QObject
{
  Q_OBJECT
  private slots:
    void initTestCase();
    void fixtureThroughputRatioGate();
    void realAreaTileThroughput();

  private:
    static QString resolveRealSgy()
    {
      const QString direct = qEnvironmentVariable( "PALEO_SEISMIC_REAL_SGY" );
      if ( !direct.isEmpty() )
        return direct;
      const QString area = qEnvironmentVariable( "PALEO_REAL_PROJECT_AREA" );
      if ( area.isEmpty() )
        return QString();
      return area + QStringLiteral( "/地震体/200P_seismic.sgy" );
    }
    QString fixtureDir() const { return m_fixtureDir.path(); }
    // 方向 81：显式成员 + initTestCase 校验——旧的函数内 static QTemporaryDir
    // 从不检查 isValid()，临时目录不可写时只表现为「0.29s 即红」的
    // loadModel 失败，看不出是夹具环境而非推理回归。
    QTemporaryDir m_fixtureDir;
    PaleoOnnxService m_onnx;

    struct RunStats
    {
      double batchPerTileMs = 0.0;
      double singleTileMs = 0.0;
      qint64 inferenceMs = 0;
      qint64 elapsedMs = 0;
      int tiles = 0;
      int rows = 0, cols = 0;
      QVector<double> singleSamples, batchSamples; // 各轮原始值（诊断）
    };
    static constexpr int kRounds = 3;
    static QString samplesText( const RunStats &st )
    {
      auto join = []( const QVector<double> &v ) {
        QStringList parts;
        for ( const double x : v )
          parts << QString::number( x, 'f', 3 );
        return parts.join( QLatin1Char( '/' ) );
      };
      return QStringLiteral( "batch %1 vs single %2 (ms/tile, min of %3; rounds batch=%4 single=%5)" )
        .arg( st.batchPerTileMs, 0, 'f', 3 )
        .arg( st.singleTileMs, 0, 'f', 3 )
        .arg( kRounds )
        .arg( join( st.batchSamples ), join( st.singleSamples ) );
    }
    // 纯合成取数（常幅）+ seg3；measureSingle 控制是否先测单发均耗。
    bool runBatch( int rows, int cols, int tileRows, int tileCols, int halo,
                   const QVector<float> *sliceValues, RunStats *out, QString *error )
    {
      TileInferenceRequest req;
      req.model = QStringLiteral( "seg3" );
      req.gridRows = rows;
      req.gridCols = cols;
      req.tileRows = tileRows;
      req.tileCols = tileCols;
      req.halo = halo;
      req.fetch = [rows, cols, sliceValues]( int row0, int col0, int r, int c,
                                             QVector<float> &buf, QString & ) {
        buf.resize( r * c );
        for ( int y = 0; y < r; ++y )
          for ( int x = 0; x < c; ++x )
          {
            const int gy = row0 + y, gx = col0 + x;
            if ( sliceValues )
              buf[y * c + x] = ( gy < rows && gx < cols )
                                 ? ( *sliceValues )[size_t( gy ) * cols + gx]
                                 : std::numeric_limits<float>::quiet_NaN();
            else
              buf[y * c + x] = 0.3f;
          }
        return true;
      };

      // 单发均耗：与批式逐 tile 同构的工作量（推理 + softmax 后处理），首跑
      // 热身丢弃。纳秒级计时——微型 fixture 模型单次亚毫秒，整数 ms 全成 0。
      // 方向 81 自洽化：①同尺寸比——批式每块读 (tile+2·halo)²（内部块
      // 80×80 = 64×64 的 1.56 倍），旧版单发只量 64×64，比率门里白白垫了
      // 1.56 倍偏置；②两侧各取 3 轮最小值——串行全量负载下单轮亚毫秒计时
      // 受调度抖动支配（方向 72：ctest 红、直跑绿），最小值是本机能力的
      // 稳定估计。4 倍门本身不动。
      const int sr = tileRows + 2 * halo, sc = tileCols + 2 * halo;
      {
        const QVector<float> tile( sr * sc, 0.3f );
        QString err;
        TileClassGrid warm;
        const OnnxTensor t0 =
          m_onnx.runTensor( QStringLiteral( "x" ), tile, { 1, 1, sr, sc }, &err );
        if ( t0.shape.size() < 2 )
        {
          *error = QStringLiteral( "single-tile warmup failed: %1" ).arg( err );
          return false;
        }
        softmaxGrid( t0.values, int( t0.shape[1] ), sr, sc, {}, &warm );
        double best = std::numeric_limits<double>::max();
        for ( int round = 0; round < kRounds; ++round )
        {
          QElapsedTimer clock;
          clock.start();
          const int n = 50;
          for ( int i = 0; i < n; ++i )
          {
            const OnnxTensor t =
              m_onnx.runTensor( QStringLiteral( "x" ), tile, { 1, 1, sr, sc }, &err );
            softmaxGrid( t.values, int( t.shape[1] ), sr, sc, {}, &warm );
          }
          const double ms = double( clock.nsecsElapsed() ) / 1e6 / n;
          out->singleSamples.append( ms );
          best = qMin( best, ms );
        }
        out->singleTileMs = best;
      }

      TileInferenceResult result;
      qint64 batchNs = std::numeric_limits<qint64>::max();
      for ( int round = 0; round < kRounds; ++round )
      {
        QElapsedTimer batchClock;
        batchClock.start();
        TileInferenceResult r;
        if ( !runTileInference( &m_onnx, req, &r, {}, nullptr, error ) )
          return false;
        const qint64 ns = batchClock.nsecsElapsed();
        if ( r.tilesDone > 0 )
          out->batchSamples.append( double( ns ) / 1e6 / r.tilesDone );
        if ( ns < batchNs )
        {
          batchNs = ns;
          result = r;
        }
      }
      out->tiles = result.tilesDone;
      out->inferenceMs = result.inferenceMs;
      out->elapsedMs = result.elapsedMs;
      out->batchPerTileMs =
        result.tilesDone > 0 ? double( batchNs ) / 1e6 / result.tilesDone : 0.0;
      out->rows = result.grid.rows;
      out->cols = result.grid.cols;
      return true;
    }
};

void TestAiAssistPerf::initTestCase()
{
  QVERIFY2( m_fixtureDir.isValid(),
            qPrintable( QStringLiteral( "fixture temp dir: %1 (TEMP/TMP 是否可写？)" )
                          .arg( m_fixtureDir.errorString() ) ) );
  QVERIFY2( OnnxFixtureWriter::writeSeg( fixtureDir() + QStringLiteral( "/seg3.onnx" ),
                                         { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } ),
            qPrintable( fixtureDir() ) );
  m_onnx.setModelRoot( fixtureDir() );
  QString err;
  QVERIFY2( m_onnx.loadModel( QStringLiteral( "seg3" ), &err ), qPrintable( err ) );
}

void TestAiAssistPerf::fixtureThroughputRatioGate()
{
  // 工区形状（411×641）合成网格：管线形状与真实切片一致，数据合成。
  RunStats st;
  QString err;
  QVERIFY2( runBatch( 411, 641, 64, 64, 8, nullptr, &st, &err ), qPrintable( err ) );
  QCOMPARE( st.tiles, 77 ); // ceil(411/64)*ceil(641/64) = 7*11
  QCOMPARE( st.rows, 411 );
  QCOMPARE( st.cols, 641 );
  // 不变量：纯推理时间 ≤ 全程（比率门的地基）。
  QVERIFY2( st.inferenceMs <= st.elapsedMs, "inferenceMs must be a part of elapsedMs" );
  // 比率门：批式单 tile 均耗不劣化到单发的 4 倍以上（防逐 tile 泄漏回退）。
  QVERIFY2( st.singleTileMs <= 0.0 || st.batchPerTileMs <= st.singleTileMs * 4.0,
            qPrintable( samplesText( st ) ) );
  qInfo( "%s", qPrintable( samplesText( st ) ) );
  qInfo( "BASELINE aiassist_fixture_tiles = %d", st.tiles );
  qInfo( "BASELINE aiassist_fixture_single_tile_ms = %.3f", st.singleTileMs );
  qInfo( "BASELINE aiassist_fixture_batch_pertile_ms = %.3f", st.batchPerTileMs );
  qInfo( "BASELINE aiassist_fixture_inference_ms_total = %lld", st.inferenceMs );
}

void TestAiAssistPerf::realAreaTileThroughput()
{
  const QString realSgy = resolveRealSgy();
  if ( realSgy.isEmpty() || !QFile::exists( realSgy ) )
    QSKIP( "PALEO_SEISMIC_REAL_SGY / PALEO_REAL_PROJECT_AREA 未设置——966MB 实测跳过" );

  seismic::SgyVolume volume;
  std::string err;
  QVERIFY2( volume.Load( realSgy.toStdString(), err ), err.c_str() );
  const auto &inlines = volume.InlineValues();
  QVERIFY( !inlines.empty() );
  const int midIl = inlines[inlines.size() / 2];
  seismic::SgySliceImage img;
  QVERIFY2( volume.ExtractSlice( seismic::SgySliceType::Inline, midIl, img, err ), err.c_str() );
  QVERIFY( img.width > 0 && img.height > 0 );

  // 切片行主序（height=inline 行 × width=xline 列）直喂 tile fetch。
  QVector<float> slice( img.values.begin(), img.values.end() );
  const qint64 volumeMiB = QFileInfo( realSgy ).size() / ( 1024 * 1024 );

  RunStats st;
  QString runErr;
  QVERIFY2( runBatch( img.height, img.width, 64, 64, 8, &slice, &st, &runErr ),
            qPrintable( runErr ) );
  QVERIFY( st.inferenceMs <= st.elapsedMs );
  QVERIFY2( st.singleTileMs <= 0.0 || st.batchPerTileMs <= st.singleTileMs * 4.0,
            qPrintable( samplesText( st ) ) );

  const double samples = double( st.rows ) * st.cols;
  const double batchSec = st.batchPerTileMs * st.tiles / 1000.0;
  const double mps = batchSec > 0 ? samples / batchSec : 0.0;
  qInfo( "BASELINE aiassist_volume_mib = %lld", volumeMiB );
  qInfo( "BASELINE aiassist_slice_grid = %dx%d", st.rows, st.cols );
  qInfo( "BASELINE aiassist_tiles = %d", st.tiles );
  qInfo( "BASELINE aiassist_inference_ms_total = %lld", st.inferenceMs );
  qInfo( "BASELINE aiassist_single_tile_ms = %.3f", st.singleTileMs );
  qInfo( "BASELINE aiassist_batch_pertile_ms = %.3f", st.batchPerTileMs );
  qInfo( "BASELINE aiassist_tile_throughput_samples_per_sec = %.0f", mps );
}

int main( int argc, char *argv[] )
{
  QCoreApplication app( argc, argv );
  TestAiAssistPerf tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "tst_aiassist_perf.moc"
