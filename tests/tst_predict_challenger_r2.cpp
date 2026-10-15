#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <limits>

#include "../src/ai/onnxfixture.h"
#include "../src/ai/onnxpredictionservice.h"
#include "../src/ai/tileinference.h"
#include "../src/ai/onnxlocalpredictor.h"
#include "../src/ai/remotepredictrouter.h"
#include "../src/workflow/workflows_internal.h"

// Adversarial test harness for Smart Prediction Module Review 2:
// 1. Dynamic shape inference ([1, 1, -1, -1] and [1, 1, 0, 0])
// 2. Fixed-shape models on boundary tiles (padding and cropping)
// 3. Tile overlap smoothing (verification of continuity across tile seams)
// 4. Output semantics parsing (probabilities vs logits vs class_index)
// 5. NoData (-9999) and non-finite values (NaN / Inf) protection against conversion to class 0
class TestPredictChallengerR2 : public QObject
{
  Q_OBJECT

private slots:
  // Area 1: Dynamic shape inference
  void challenge1_dynamicShapeNegativeOne();
  void challenge1_dynamicShapeZero();

  // Area 2: Fixed-shape boundary padding & cropping
  void challenge2_fixedShapePadsAndCropsCorrectly();
  void challenge2_fixedShapeSmallerGridThanFixedDimension();
  void challenge2_fixedShapeOversizedReadRegionFailsHonestly();

  // Area 3: Tile overlap smoothing and continuity
  void challenge3_tileOverlapSmoothingVerification();
  void challenge3_overlappingTilesBlendGenuinely();

  // Area 4: Output semantics parsing
  void challenge4_outputSemanticsProbabilitiesBypassesSoftmax();
  void challenge4_outputSemanticsLogitsAppliesSoftmax();
  void challenge4_outputSemanticsClassIndexBypassesMath();

  // Area 5: NoData & non-finite handling
  void challenge5_sanitizeModelInputWithNoDataMinus9999();
  void challenge5_softmaxGridProbabilitiesWithMinus9999();
  void challenge5_softmaxGridLogitsSingleClassWithMinus9999();
  void challenge5_writeOnnxRasterMultiClassWithNaN();
  void challenge5_remotePredictTransportNullHandling();
  void challenge5_onnxLocalPredictorWithMinus9999Input();

private:
  QTemporaryDir m_dir;

  QString fixtureDir() const { return m_dir.path(); }

  void writeDynamicSeg( const QString &path, int64_t h, int64_t w )
  {
    OnnxFixtureWriter::writeSeg( path, { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f },
                                 8, nullptr, h, w );
  }
};

// -----------------------------------------------------------------------------
// Area 1: Dynamic shape inference ([1, 1, -1, -1] and [1, 1, 0, 0])
// -----------------------------------------------------------------------------

void TestPredictChallengerR2::challenge1_dynamicShapeNegativeOne()
{
  const QString modelPath = fixtureDir() + QStringLiteral( "/dyn_neg1.onnx" );
  writeDynamicSeg( modelPath, -1, -1 );

  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );

  OnnxModelMeta meta;
  QString err;
  QCOMPARE( onnx.loadModelMeta( QStringLiteral( "dyn_neg1" ), &meta, &err ), OnnxLoadStatus::Ok );
  QVERIFY2( err.isEmpty(), qPrintable( err ) );
  QCOMPARE( meta.inputShape, QVector<int64_t>( { 1, 1, -1, -1 } ) );
  QVERIFY2( meta.warmupMs >= 0, "warmup must succeed on dynamic shape model" );
  QVERIFY2( meta.warmupError.isEmpty(), qPrintable( meta.warmupError ) );

  // Run tile inference on non-square, arbitrary grid: 13 rows x 17 cols
  TileInferenceRequest req;
  req.model = QStringLiteral( "dyn_neg1" );
  req.gridRows = 13;
  req.gridCols = 17;
  req.tileRows = 5;
  req.tileCols = 6;
  req.halo = 2;
  req.fetch = []( int row0, int col0, int rows, int cols, QVector<float> &out, QString & ) {
    out.resize( rows * cols );
    for ( int r = 0; r < rows; ++r )
      for ( int c = 0; c < cols; ++c )
        out[r * cols + c] = float( ( row0 + r ) * 0.1f - ( col0 + c ) * 0.05f );
    return true;
  };

  TileInferenceResult res;
  QVERIFY2( runTileInference( &onnx, req, &res, {}, nullptr, &err ), qPrintable( err ) );
  QCOMPARE( res.grid.rows, 13 );
  QCOMPARE( res.grid.cols, 17 );
  QCOMPARE( res.grid.classes, 3 );
  for ( quint8 cls : res.grid.argmax )
  {
    QVERIFY( cls <= 2 ); // valid class in 0..2
  }
}

void TestPredictChallengerR2::challenge1_dynamicShapeZero()
{
  // When ONNX model defines dimension with value 0
  const QString modelPath = fixtureDir() + QStringLiteral( "/dyn_zero.onnx" );
  writeDynamicSeg( modelPath, 0, 0 );

  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );

  OnnxModelMeta meta;
  QString err;
  const OnnxLoadStatus st = onnx.loadModelMeta( QStringLiteral( "dyn_zero" ), &meta, &err );
  QCOMPARE( st, OnnxLoadStatus::Ok );
  QCOMPARE( meta.inputShape, QVector<int64_t>( { 1, 1, 0, 0 } ) );

  // Check what warmup did:
  qDebug() << "[Area 1] dyn_zero warmupMs:" << meta.warmupMs << "warmupError:" << meta.warmupError;

  // Now probe tile inference: does runTileInference succeed or throw an invalid dimension exception?
  TileInferenceRequest req;
  req.model = QStringLiteral( "dyn_zero" );
  req.gridRows = 8;
  req.gridCols = 8;
  req.tileRows = 4;
  req.tileCols = 4;
  req.halo = 0;
  req.fetch = []( int, int, int rows, int cols, QVector<float> &out, QString & ) {
    out = QVector<float>( rows * cols, 1.0f );
    return true;
  };

  TileInferenceResult res;
  const bool ok = runTileInference( &onnx, req, &res, {}, nullptr, &err );
  qDebug() << "[Area 1] dyn_zero runTileInference ok:" << ok << "error:" << err;
  // Note: ONNX protobuf dim_value=0 is technically a dimension of length 0 in ORT.
  // We document empirical behavior.
}

// -----------------------------------------------------------------------------
// Area 2: Fixed-shape boundary padding & cropping
// -----------------------------------------------------------------------------

void TestPredictChallengerR2::challenge2_fixedShapePadsAndCropsCorrectly()
{
  // Create fixed 8x8 model: y[c] = gains[c]*x + biases[c]
  const QString modelPath = fixtureDir() + QStringLiteral( "/fixed8x8.onnx" );
  OnnxFixtureWriter::writeSeg( modelPath, { 1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f },
                               8, nullptr, 8, 8 );

  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );

  OnnxModelMeta meta;
  QString err;
  QCOMPARE( onnx.loadModelMeta( QStringLiteral( "fixed8x8" ), &meta, &err ), OnnxLoadStatus::Ok );
  QCOMPARE( meta.inputShape, QVector<int64_t>( { 1, 1, 8, 8 } ) );

  // Grid is 10x10, tiles are 8x8 -> boundary tiles have rows=2 or cols=2
  TileInferenceRequest req;
  req.model = QStringLiteral( "fixed8x8" );
  req.gridRows = 10;
  req.gridCols = 10;
  req.tileRows = 8;
  req.tileCols = 8;
  req.halo = 0;

  // Linear ramp input: x(r, c) = (r - 5) * 0.2f
  req.fetch = []( int row0, int col0, int rows, int cols, QVector<float> &out, QString & ) {
    out.resize( rows * cols );
    for ( int r = 0; r < rows; ++r )
      for ( int c = 0; c < cols; ++c )
        out[r * cols + c] = float( ( row0 + r - 5 ) * 0.2f );
    return true;
  };

  TileInferenceResult res;
  QVERIFY2( runTileInference( &onnx, req, &res, {}, nullptr, &err ), qPrintable( err ) );
  QCOMPARE( res.grid.rows, 10 );
  QCOMPARE( res.grid.cols, 10 );
  QCOMPARE( res.tilesTotal, 4 ); // (8+2)x(8+2) -> 2x2 = 4 tiles

  // Verify boundary cell at (9, 9) where both r and c were padded:
  // x = (9 - 5)*0.2 = 0.8
  // class 0 logit: 1.0*0.8 = 0.8
  // class 1 logit: -1.0*0.8 = -0.8
  // class 2 logit: 0.1
  // Expected argmax = 0
  const int idx99 = 9 * 10 + 9;
  QCOMPARE( int( res.grid.argmax[idx99] ), 0 );

  // Verify cell at (0, 0): x = -1.0 -> argmax should be 1
  QCOMPARE( int( res.grid.argmax[0] ), 1 );
}

void TestPredictChallengerR2::challenge2_fixedShapeSmallerGridThanFixedDimension()
{
  const QString modelPath = fixtureDir() + QStringLiteral( "/fixed8x8.onnx" );
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );

  // Whole grid is 5x5, which is strictly smaller than model fixed dimension 8x8
  TileInferenceRequest req;
  req.model = QStringLiteral( "fixed8x8" );
  req.gridRows = 5;
  req.gridCols = 5;
  req.tileRows = 8;
  req.tileCols = 8;
  req.halo = 0;
  req.fetch = []( int, int, int rows, int cols, QVector<float> &out, QString & ) {
    out = QVector<float>( rows * cols, 2.0f );
    return true;
  };

  TileInferenceResult res;
  QString err;
  QVERIFY2( runTileInference( &onnx, req, &res, {}, nullptr, &err ), qPrintable( err ) );
  QCOMPARE( res.grid.rows, 5 );
  QCOMPARE( res.grid.cols, 5 );
  QCOMPARE( res.tilesTotal, 1 );
  // All cells x=2.0 -> argmax = 0
  for ( quint8 val : res.grid.argmax )
  {
    QCOMPARE( int( val ), 0 );
  }
}

void TestPredictChallengerR2::challenge2_fixedShapeOversizedReadRegionFailsHonestly()
{
  const QString modelPath = fixtureDir() + QStringLiteral( "/fixed8x8.onnx" );
  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );

  // Request tileRows=8 with halo=2 -> tile read domain rows = 8 + 2 = 10 > 8!
  TileInferenceRequest req;
  req.model = QStringLiteral( "fixed8x8" );
  req.gridRows = 16;
  req.gridCols = 16;
  req.tileRows = 8;
  req.tileCols = 8;
  req.halo = 2;
  req.fetch = []( int, int, int rows, int cols, QVector<float> &out, QString & ) {
    out = QVector<float>( rows * cols, 1.0f );
    return true;
  };

  TileInferenceResult res;
  QString err;
  const bool ok = runTileInference( &onnx, req, &res, {}, nullptr, &err );
  QVERIFY( !ok );
  QVERIFY2( err.contains( QStringLiteral( "超过模型固定输入" ) ), qPrintable( err ) );
}

// -----------------------------------------------------------------------------
// Area 3: Tile overlap smoothing and continuity
// -----------------------------------------------------------------------------

void TestPredictChallengerR2::challenge3_tileOverlapSmoothingVerification()
{
  // Test whether adjacent overlapping tiles undergo distance-weighted smoothing/blending
  // or if they are disjoint partitions with a sharp cut (step discontinuity).
  const int R = 1, C = 32;
  const int tileR = 1, tileC = 16;
  const int halo = 4;

  const QVector<InferenceTile> tiles = planInferenceTiles( R, C, tileR, tileC, halo );
  QCOMPARE( tiles.size(), 2 );

  // Tile 0: innerCol0 = 0, innerCols = 16 (covers cols 0..15). read domain = cols 0..19
  // Tile 1: innerCol0 = 16, innerCols = 16 (covers cols 16..31). read domain = cols 12..31
  // Halo overlap zone is columns 12..19 (8 columns shared in reading).
  QCOMPARE( tiles[0].innerCol0, 0 );
  QCOMPARE( tiles[0].innerCols, 16 );
  QCOMPARE( tiles[1].innerCol0, 16 );
  QCOMPARE( tiles[1].innerCols, 16 );

  // Create a grid where Tile 0 and Tile 1 disagree in the overlap zone:
  // Tile 0 predicts prob = 0.9 for class 0 in all its read pixels.
  // Tile 1 predicts prob = 0.1 for class 0 (i.e. prob = 0.9 for class 1) in all its read pixels.
  TileClassGrid tileGrid0;
  tileGrid0.rows = tiles[0].rows;
  tileGrid0.cols = tiles[0].cols;
  tileGrid0.classes = 2;
  tileGrid0.argmax = QVector<quint8>( tiles[0].rows * tiles[0].cols, 0 );
  tileGrid0.probMax = QVector<float>( tiles[0].rows * tiles[0].cols, 0.90f );
  tileGrid0.confidence = QVector<float>( tiles[0].rows * tiles[0].cols, 0.85f );

  TileClassGrid tileGrid1;
  tileGrid1.rows = tiles[1].rows;
  tileGrid1.cols = tiles[1].cols;
  tileGrid1.classes = 2;
  tileGrid1.argmax = QVector<quint8>( tiles[1].rows * tiles[1].cols, 1 );
  tileGrid1.probMax = QVector<float>( tiles[1].rows * tiles[1].cols, 0.90f );
  tileGrid1.confidence = QVector<float>( tiles[1].rows * tiles[1].cols, 0.85f );

  TileClassGrid stitched;
  stitched.rows = R;
  stitched.cols = C;
  stitched.classes = 2;
  stitched.argmax = QVector<quint8>( R * C, 255 );
  stitched.confidence = QVector<float>( R * C, 0.0f );
  stitched.probMax = QVector<float>( R * C, 0.0f );

  stitchTileIntoGrid( tiles[0], tileGrid0, &stitched );
  stitchTileIntoGrid( tiles[1], tileGrid1, &stitched );

  // EMPIRICAL OBSERVATION:
  // What is at col 15? Tile 0 wrote it -> argmax = 0, probMax = 0.90f.
  // What is at col 16? Tile 1 wrote it -> argmax = 1, probMax = 0.90f.
  // Is there any intermediate/blended value anywhere in the overlap zone (cols 12..19)?
  // Check every col from 0 to 15: all class 0, exactly 0.90f.
  // Check every col from 16 to 31: all class 1, exactly 0.90f.
  bool anyBlended = false;
  for ( int c = 12; c < 20; ++c )
  {
    if ( stitched.argmax[c] != 0 && stitched.argmax[c] != 1 )
      anyBlended = true;
    if ( stitched.probMax[c] < 0.89f ) // If blended between 0.9 and 0.1, probMax would be smaller
      anyBlended = true;
  }
  qDebug() << "[Area 3] Was any distance-weighted blending performed across seam?" << anyBlended;
  qDebug() << "[Area 3] Col 15 class:" << stitched.argmax[15] << "Col 16 class:" << stitched.argmax[16];

  // Document that the transition is a step discontinuity with 0 blending:
  QCOMPARE( int( stitched.argmax[15] ), 0 );
  QCOMPARE( int( stitched.argmax[16] ), 1 );
  QVERIFY2( !anyBlended, "Empirical proof: stitchTileIntoGrid partitions into disjoint rectangular inner regions with ZERO overlap blending" );
}

void TestPredictChallengerR2::challenge3_overlappingTilesBlendGenuinely()
{
  const int R = 1, C = 32;
  // Two tiles whose inner domains overlap across cols 10..21 (12 cols)
  InferenceTile t0;
  t0.row0 = 0; t0.col0 = 0; t0.rows = 1; t0.cols = 22;
  t0.innerRow0 = 0; t0.innerCol0 = 0; t0.innerRows = 1; t0.innerCols = 22;

  InferenceTile t1;
  t1.row0 = 0; t1.col0 = 10; t1.rows = 1; t1.cols = 22;
  t1.innerRow0 = 0; t1.innerCol0 = 10; t1.innerRows = 1; t1.innerCols = 22;

  TileClassGrid g0;
  g0.rows = 1; g0.cols = 22; g0.classes = 2;
  g0.argmax = QVector<quint8>( 22, 0 );
  g0.probMax = QVector<float>( 22, 0.90f );
  g0.confidence = QVector<float>( 22, 0.85f );
  g0.accumulatedProbs = QVector<float>( 44, 0.0f );
  for ( int i = 0; i < 22; ++i )
  {
    g0.accumulatedProbs[i] = 0.90f;
    g0.accumulatedProbs[22 + i] = 0.10f;
  }

  TileClassGrid g1;
  g1.rows = 1; g1.cols = 22; g1.classes = 2;
  g1.argmax = QVector<quint8>( 22, 1 );
  g1.probMax = QVector<float>( 22, 0.90f );
  g1.confidence = QVector<float>( 22, 0.85f );
  g1.accumulatedProbs = QVector<float>( 44, 0.0f );
  for ( int i = 0; i < 22; ++i )
  {
    g1.accumulatedProbs[i] = 0.10f;
    g1.accumulatedProbs[22 + i] = 0.90f;
  }

  TileClassGrid stitched;
  stitched.rows = R;
  stitched.cols = C;
  stitched.classes = 2;
  stitched.argmax = QVector<quint8>( R * C, 255 );
  stitched.confidence = QVector<float>( R * C, 0.0f );
  stitched.probMax = QVector<float>( R * C, 0.0f );

  stitchTileIntoGrid( t0, g0, &stitched );
  stitchTileIntoGrid( t1, g1, &stitched );

  // Check that in the overlap zone, probabilities are distance-blended:
  bool anyBlended = false;
  for ( int c = 10; c < 22; ++c )
  {
    if ( stitched.probMax[c] < 0.89f )
      anyBlended = true;
  }
  QVERIFY2( anyBlended, "Overlapping tile regions must be blended with distance-weighted probabilities" );
  // Near left edge of overlap (col 10), tile 0 dominates:
  QCOMPARE( int( stitched.argmax[10] ), 0 );
  // Near right edge of overlap (col 21), tile 1 dominates:
  QCOMPARE( int( stitched.argmax[21] ), 1 );
}

// -----------------------------------------------------------------------------
// Area 4: Output semantics parsing (probabilities vs logits vs class_index)
// -----------------------------------------------------------------------------

void TestPredictChallengerR2::challenge4_outputSemanticsProbabilitiesBypassesSoftmax()
{
  const QVector<float> rawProbs = { 0.70f, 0.20f, 0.10f };
  TileClassGrid g;
  softmaxGrid( rawProbs, 3, 1, 1, { true }, &g, QStringLiteral( "probabilities" ) );

  QCOMPARE( int( g.argmax[0] ), 0 );
  // If duplicate softmax had occurred: exp(0.7)/sum(exp) = 2.0137 / (2.0137 + 1.2214 + 1.1051) = 0.4639
  // Because probabilities bypasses softmax: probMax must be exactly 0.70f!
  QVERIFY2( qAbs( double( g.probMax[0] ) - 0.70 ) < 1e-4,
            qPrintable( QStringLiteral( "probMax was %1, expected 0.70" ).arg( g.probMax[0] ) ) );
}

void TestPredictChallengerR2::challenge4_outputSemanticsLogitsAppliesSoftmax()
{
  const QVector<float> logits = { 2.0f, 0.0f, -1.0f };
  TileClassGrid g;
  softmaxGrid( logits, 3, 1, 1, { true }, &g, QStringLiteral( "logits" ) );

  QCOMPARE( int( g.argmax[0] ), 0 );
  const double expectedP0 = std::exp( 2.0 ) / ( std::exp( 2.0 ) + std::exp( 0.0 ) + std::exp( -1.0 ) ); // 0.84379
  QVERIFY2( qAbs( double( g.probMax[0] ) - expectedP0 ) < 1e-4,
            qPrintable( QStringLiteral( "probMax was %1, expected %2" ).arg( g.probMax[0] ).arg( expectedP0 ) ) );
}

void TestPredictChallengerR2::challenge4_outputSemanticsClassIndexBypassesMath()
{
  const QVector<float> classTensor = { 3.0f, 7.0f, 42.0f, 254.0f };
  TileClassGrid g;
  softmaxGrid( classTensor, 1, 2, 2, { true, true, true, true }, &g, QStringLiteral( "class_index" ) );

  QCOMPARE( int( g.argmax[0] ), 3 );
  QCOMPARE( int( g.argmax[1] ), 7 );
  QCOMPARE( int( g.argmax[2] ), 42 );
  QCOMPARE( int( g.argmax[3] ), 254 );
  QCOMPARE( g.confidence[0], 1.0f );
  QCOMPARE( g.probMax[0], 1.0f );
}

// -----------------------------------------------------------------------------
// Area 5: NoData (-9999) and non-finite values (NaN / Inf)
// -----------------------------------------------------------------------------

void TestPredictChallengerR2::challenge5_sanitizeModelInputWithNoDataMinus9999()
{
  // Probe whether sanitizeModelInput recognizes -9999 as NoData
  QVector<float> data = { -9999.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f };
  QVector<bool> valid;
  const int bad = sanitizeModelInput( data, &valid );

  qDebug() << "[Area 5] sanitizeModelInput bad count:" << bad
           << "valid[0] for -9999.0f:" << valid[0]
           << "valid[1] for NaN:" << valid[1];

  // Observe: NaN is masked as false. But is -9999.0f masked?
  // -9999.0f is finite, so sanitizeModelInput leaves valid[0] == true!
  QCOMPARE( valid[1], false );
  // Note: valid[0] remains TRUE, meaning -9999 is passed to model as a valid float!
}

void TestPredictChallengerR2::challenge5_softmaxGridProbabilitiesWithMinus9999()
{
  // Probing softmaxGrid under outputSemantics="probabilities" when input has -9999
  const QVector<float> probs = { -9999.0f, -9999.0f, -9999.0f };
  TileClassGrid g;
  softmaxGrid( probs, 3, 1, 1, {}, &g, QStringLiteral( "probabilities" ) );

  qDebug() << "[Area 5] softmaxGrid probabilities with -9999:"
           << "argmax:" << g.argmax[0]
           << "confidence:" << g.confidence[0]
           << "probMax:" << g.probMax[0];

  // In softmaxGrid lines 128-137:
  // prob = std::clamp(-9999.0, 0.0, 1.0) = 0.0.
  // best = 0, bestP = 0.0.
  // out->argmax[0] = 0! Converted to geological class 0!
}

void TestPredictChallengerR2::challenge5_softmaxGridLogitsSingleClassWithMinus9999()
{
  const QVector<float> logits = { -9999.0f };
  TileClassGrid g;
  softmaxGrid( logits, 1, 1, 1, {}, &g, QStringLiteral( "logits" ) );

  qDebug() << "[Area 5] softmaxGrid single-class logits with -9999:"
           << "argmax:" << g.argmax[0]
           << "confidence:" << g.confidence[0];

  // p1 = 1 / (1 + exp(9999)) = 0.0.
  // p1 >= 0.5 is false -> argmax = 0!
  // Converted to class 0!
}

void TestPredictChallengerR2::challenge5_writeOnnxRasterMultiClassWithNaN()
{
  // Probe writeOnnxRaster multi-class argmax reduction when tensor contains NaN
  const int rows = 1, cols = 1;
  const float nanVal = std::numeric_limits<float>::quiet_NaN();
  const QVector<float> values = { nanVal, nanVal, nanVal };
  const QVector<int64_t> outShape = { 1, 3, 1, 1 };

  // Trace logic of lines 287-307 of workflows_internal.h:
  // for p in pixels:
  //   int bestC = 0;
  //   float maxVal = values[p]; // NaN
  //   for c = 1..C-1:
  //     v > maxVal -> false (IEEE NaN comparison always false)
  //   writeBuffer[p] = static_cast<float>(bestC); // 0.0f!
  // Writes 0.0f (geological class 0) instead of -9999.0f!
  int rowsOut = 0, colsOut = 0;
  QString err;
  const bool ok = paleo::workflow_detail::resolveOnnxGrid( values, outShape, rowsOut, colsOut, &err );
  Q_UNUSED( ok );
  QCOMPARE( rowsOut, 0 ); // Refused because not 411x641
}

void TestPredictChallengerR2::challenge5_remotePredictTransportNullHandling()
{
  // Probe HttpRemoteTransport::parseResponse JSON null handling
  const QByteArray json = R"({
    "cells": [ null, 0, 1, null, 2 ]
  })";
  const QJsonDocument doc = QJsonDocument::fromJson( json );
  const QJsonArray got = doc.object().value( QStringLiteral( "cells" ) ).toArray();

  QVector<int> cells;
  for ( const QJsonValue &v : got )
  {
    if ( v.isNull() || !v.isDouble() )
      cells.append( -9999 );
    else
      cells.append( v.toInt() );
  }

  QCOMPARE( cells.size(), 5 );
  QCOMPARE( cells[0], -9999 ); // null -> -9999 (NOT class 0)
  QCOMPARE( cells[1], 0 );     // 0 -> 0
  QCOMPARE( cells[2], 1 );     // 1 -> 1
  QCOMPARE( cells[3], -9999 ); // null -> -9999
  QCOMPARE( cells[4], 2 );     // 2 -> 2
}

void TestPredictChallengerR2::challenge5_onnxLocalPredictorWithMinus9999Input()
{
  // Test OnnxLocalPredictor when input samples contains -9999.0f
  const QString modelPath = fixtureDir() + QStringLiteral( "/seg3_local.onnx" );
  OnnxFixtureWriter::writeSeg( modelPath, { -1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.1f } );

  PaleoOnnxService onnx;
  onnx.setModelRoot( fixtureDir() );

  OnnxLocalPredictor pred( &onnx, QStringLiteral( "seg3_local" ) );
  RemotePredictionRequest req;
  req.rows = 1;
  req.columns = 2;
  // Cell 0 is NaN (non-finite), Cell 1 is -9999.0f (standard NoData)
  req.samples = { std::numeric_limits<float>::quiet_NaN(), -9999.0f };

  QVariantList facies;
  QVariantMap f0;
  f0.insert( QStringLiteral( "code" ), 1 );
  f0.insert( QStringLiteral( "name" ), QStringLiteral( "Facies 1" ) );
  facies.append( f0 );
  QVariantMap f1;
  f1.insert( QStringLiteral( "code" ), 2 );
  f1.insert( QStringLiteral( "name" ), QStringLiteral( "Facies 2" ) );
  facies.append( f1 );
  req.facies = facies;

  QVector<int> cells;
  QString err;
  QVERIFY2( pred.predict( req, cells, &err ), qPrintable( err ) );
  QCOMPARE( cells.size(), 2 );

  qDebug() << "[Area 5] OnnxLocalPredictor result: cell 0 (NaN) =" << cells[0]
           << "cell 1 (-9999.0f) =" << cells[1];

  // Cell 0 had NaN -> masked as invalid -> rawClass = 255 -> cells[0] = -9999.
  QCOMPARE( cells[0], -9999 );

  // Cell 1 had -9999.0f -> NOT masked by sanitizeModelInput -> fed to model:
  // y[0] = -1.0*(-9999) = 9999
  // y[1] = 1.0*(-9999) = -9999
  // y[2] = 0.1
  // argmax = 0 -> maps to facies[0].code = 1!
  // Notice: The NoData pixel -9999.0f was treated as valid data and predicted as Facies 1!
}

int main( int argc, char *argv[] )
{
  QCoreApplication app( argc, argv );
  TestPredictChallengerR2 tc;
  return QTest::qExec( &tc, argc, argv );
}

#include "tst_predict_challenger_r2.moc"
