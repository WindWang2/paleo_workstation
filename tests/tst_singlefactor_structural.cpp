// 层：测试壳（被测对象：paleo:paleo_structural_idw 加工算法 + 约束导入链）
#include <QtTest/QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QUuid>

#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsfeature.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsprocessingregistry.h>
#include <qgsvectorlayer.h>

#include <gdal.h>

#include <cmath>
#include <limits>

#include "../src/algorithms/paleoalgorithms.h"
#include "../src/workflow/constraintimport.h"

namespace
{

QString fixturePath( const QString &name )
{
  const QString fromSource = QFileInfo( QString::fromUtf8( __FILE__ ) ).dir().filePath(
      QStringLiteral( "fixtures/singlefactor/" ) + name );
  if ( QFileInfo::exists( fromSource ) )
    return fromSource;
  return QStringLiteral( "tests/fixtures/singlefactor/" ) + name;
}

QJsonObject readJson( const QString &path )
{
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) )
    return {};
  return QJsonDocument::fromJson( file.readAll() ).object();
}

// 真实导入路径（readConstraintImportFeatures）→ 内存约束图层
//（id/type/params_json/schema_version 列，与 ConstraintStore 快照同构）。
std::unique_ptr<QgsVectorLayer> constraintLayer( const QString &directionsPath,
                                               const QString &barriersPath,
                                               QString *error )
{
  auto layer = std::make_unique<QgsVectorLayer>( QStringLiteral( "LineString" ),
                                                 QStringLiteral( "constraints" ),
                                                 QStringLiteral( "memory" ) );
  if ( !layer->isValid() )
  {
    if ( error )
      *error = QStringLiteral( "memory constraint layer invalid" );
    return nullptr;
  }
  layer->startEditing();
  layer->addAttribute( QgsField( QStringLiteral( "id" ), QMetaType::Type::QString ) );
  layer->addAttribute( QgsField( QStringLiteral( "type" ), QMetaType::Type::QString ) );
  layer->addAttribute( QgsField( QStringLiteral( "params_json" ), QMetaType::Type::QString ) );
  layer->addAttribute( QgsField( QStringLiteral( "schema_version" ), QMetaType::Type::Int ) );
  layer->updateFields();

  const auto append = [&layer, error]( const QString &path, const QString &role ) {
    QString resolved;
    const QVector<paleo::ImportedConstraintRecord> records =
        paleo::readConstraintImportFeatures( path, role, &resolved, error );
    if ( records.isEmpty() )
      return false;
    for ( const paleo::ImportedConstraintRecord &record : records )
    {
      QgsFeature feature( layer->fields() );
      feature.setGeometry( QgsGeometry::fromWkt( record.wkt ) );
      feature.setAttribute( QStringLiteral( "id" ),
                            record.params.value( QStringLiteral( "sourceId" ) ).toString() );
      feature.setAttribute( QStringLiteral( "type" ), record.type );
      QVariantMap params = record.params;
      params.insert( QStringLiteral( "schemaVersion" ), 1 );
      feature.setAttribute( QStringLiteral( "params_json" ),
                            QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( params ) )
                                                   .toJson( QJsonDocument::Compact ) ) );
      feature.setAttribute( QStringLiteral( "schema_version" ), 1 );
      if ( !layer->addFeature( feature ) )
      {
        if ( error )
          *error = QStringLiteral( "memory layer addFeature failed" );
        return false;
      }
    }
    return true;
  };
  bool ok = true;
  if ( !directionsPath.isEmpty() )
    ok = append( directionsPath, QStringLiteral( "direction" ) ) && ok;
  if ( ok && !barriersPath.isEmpty() )
    ok = append( barriersPath, QStringLiteral( "barrier" ) ) && ok;
  if ( !ok )
    return nullptr;
  layer->commitChanges();
  return layer;
}

double jsonNumber( const QJsonValue &value )
{
  return value.isDouble() ? value.toDouble() : std::numeric_limits<double>::quiet_NaN();
}

QVector<double> jsonDoubleVector( const QJsonValue &value )
{
  QVector<double> out;
  for ( const QJsonValue &entry : value.toArray() )
    out << jsonNumber( entry );
  return out;
}

// <case>.json：golden 同构 dump（lead 比对脚本 compare_structural.py 的输入）。
QJsonObject makeDump( const QJsonObject &qc, const QJsonObject &model )
{
  QJsonObject grid = model.value( QStringLiteral( "grid" ) ).toObject();
  grid.insert( QStringLiteral( "valid_mask" ),
               model.value( QStringLiteral( "valid_mask" ) ) );
  QJsonObject dump;
  dump.insert( QStringLiteral( "wells" ), qc.value( QStringLiteral( "wells" ) ) );
  dump.insert( QStringLiteral( "skipped" ), qc.value( QStringLiteral( "skipped" ) ) );
  dump.insert( QStringLiteral( "grid" ), grid );
  dump.insert( QStringLiteral( "field_model" ), model.value( QStringLiteral( "field_model" ) ) );
  dump.insert( QStringLiteral( "barrier_buffer_distance" ),
               model.value( QStringLiteral( "barrier_buffer_distance" ) ) );
  dump.insert( QStringLiteral( "contour_stop_buffer_distance" ),
               model.value( QStringLiteral( "contour_stop_buffer_distance" ) ) );
  dump.insert( QStringLiteral( "value_min" ), model.value( QStringLiteral( "value_min" ) ) );
  dump.insert( QStringLiteral( "value_max" ), model.value( QStringLiteral( "value_max" ) ) );
  return dump;
}

void writeDump( const QString &caseName, const QJsonObject &dump )
{
  const QString dir = qEnvironmentVariable( "PALEO_STRUCTURAL_DUMP_DIR" );
  if ( dir.isEmpty() )
    return;
  QDir().mkpath( dir );
  QFile file( QDir( dir ).filePath( caseName + QStringLiteral( ".json" ) ) );
  if ( file.open( QIODevice::WriteOnly ) )
    file.write( QJsonDocument( dump ).toJson() );
}

struct CaseMetrics
{
  double maxAbs = -1;
  double rmse = -1;
};

} // namespace

class tst_singlefactor_structural : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir mDir;

  void ensurePaleo()
  {
    if ( !QgsApplication::processingRegistry()->providerById( QStringLiteral( "paleo" ) ) )
      QgsApplication::processingRegistry()->addProvider( new PaleoProvider() );
    QVERIFY( QgsApplication::processingRegistry()->algorithmById(
        QStringLiteral( "paleo:paleo_structural_idw" ) ) );
  }

  // 跑加工算法；返回 {qc, structural}（JSON 对象）。失败返回空 pair。
  // extraParams：附加运行参数（如 POWER/SEARCH_RADIUS/EXTEND_TO_BOUNDARY 覆盖）。
  QPair<QJsonObject, QJsonObject> runStructural( const QString &wellsPath,
                                               const QString &boundaryPath,
                                               const QString &directionsPath,
                                               const QString &barriersPath,
                                               int resolution, qint64 *elapsedMs,
                                               const QVariantMap &extraParams = QVariantMap() )
  {
    auto wells = std::make_unique<QgsVectorLayer>( wellsPath, QStringLiteral( "wells" ),
                                                   QStringLiteral( "ogr" ) );
    auto boundary = std::make_unique<QgsVectorLayer>( boundaryPath, QStringLiteral( "boundary" ),
                                                      QStringLiteral( "ogr" ) );
    if ( !wells->isValid() || !boundary->isValid() )
      return {};
    QString importErr;
    std::unique_ptr<QgsVectorLayer> constraints =
        constraintLayer( directionsPath, barriersPath, &importErr );
    if ( !constraints )
    {
      qWarning() << "constraint import failed:" << importErr;
      return {};
    }

    const QString outPath = mDir.filePath(
        QStringLiteral( "structural-%1.tif" ).arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
    QVariantMap params;
    params.insert( QStringLiteral( "WELLS" ),
                   QVariant::fromValue( static_cast<QgsMapLayer *>( wells.get() ) ) );
    params.insert( QStringLiteral( "WELL_ID_FIELD" ), QStringLiteral( "well_id" ) );
    params.insert( QStringLiteral( "VALUE_FIELD" ), QStringLiteral( "sand_ratio" ) );
    params.insert( QStringLiteral( "FACTOR_MODE" ), QStringLiteral( "direct" ) );
    params.insert( QStringLiteral( "VALUE_RANGE" ), QStringLiteral( "ratio_0_1" ) );
    params.insert( QStringLiteral( "BOUNDARY" ),
                   QVariant::fromValue( static_cast<QgsMapLayer *>( boundary.get() ) ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ),
                   QVariant::fromValue( static_cast<QgsMapLayer *>( constraints.get() ) ) );
    params.insert( QStringLiteral( "GRID_RESOLUTION" ), resolution );
    for ( auto it = extraParams.constBegin(); it != extraParams.constEnd(); ++it )
      params.insert( it.key(), it.value() );
    params.insert( QStringLiteral( "OUTPUT" ), outPath );

    QgsProcessingContext ctx;
    QgsProcessingFeedback fb;
    const QgsProcessingAlgorithm *alg = QgsApplication::processingRegistry()->algorithmById(
        QStringLiteral( "paleo:paleo_structural_idw" ) );
    if ( !alg )
      return {};
    QElapsedTimer timer;
    timer.start();
    const QVariantMap results = alg->run( params, ctx, &fb );
    if ( elapsedMs )
      *elapsedMs = timer.elapsed();
    if ( results.isEmpty() )
    {
      qWarning() << "structural run failed:" << fb.textLog();
      return {};
    }
    const QString stem = results.value( QStringLiteral( "OUTPUT" ) ).toString();
    const QString base = stem.left( stem.lastIndexOf( QLatin1Char( '.' ) ) );
    return { readJson( base + QStringLiteral( ".qc.json" ) ),
             readJson( base + QStringLiteral( ".structural.json" ) ) };
  }

  void compareCase( const QJsonObject &golden, const QJsonObject &qc,
                    const QJsonObject &model, CaseMetrics &metrics )
  {
    // ---- wells：id/顺序/x/y/value/control 全等 ----
    const QJsonArray expectedWells = golden.value( QStringLiteral( "wells" ) ).toArray();
    const QJsonArray actualWells = qc.value( QStringLiteral( "wells" ) ).toArray();
    QCOMPARE( actualWells.size(), expectedWells.size() );
    for ( int i = 0; i < expectedWells.size() && i < actualWells.size(); ++i )
    {
      const QJsonObject expected = expectedWells.at( i ).toObject();
      const QJsonObject actual = actualWells.at( i ).toObject();
      QCOMPARE( actual.value( QStringLiteral( "well_id" ) ).toString(),
                expected.value( QStringLiteral( "well_id" ) ).toString() );
      QCOMPARE( jsonNumber( actual.value( QStringLiteral( "x" ) ) ),
                jsonNumber( expected.value( QStringLiteral( "x" ) ) ) );
      QCOMPARE( jsonNumber( actual.value( QStringLiteral( "y" ) ) ),
                jsonNumber( expected.value( QStringLiteral( "y" ) ) ) );
      QCOMPARE( jsonNumber( actual.value( QStringLiteral( "value" ) ) ),
                jsonNumber( expected.value( QStringLiteral( "value" ) ) ) );
      QCOMPARE( actual.value( QStringLiteral( "is_control_point" ) ).toBool(),
                expected.value( QStringLiteral( "is_control_point" ) ).toBool() );
    }

    // ---- skipped：井 id 前缀 + 原因类别 ----
    const QJsonArray expectedSkipped = golden.value( QStringLiteral( "skipped" ) ).toArray();
    const QJsonArray actualSkipped = qc.value( QStringLiteral( "skipped" ) ).toArray();
    QCOMPARE( actualSkipped.size(), expectedSkipped.size() );
    for ( int i = 0; i < expectedSkipped.size() && i < actualSkipped.size(); ++i )
    {
      const QString expected = expectedSkipped.at( i ).toString();
      const QString actual = actualSkipped.at( i ).toString();
      const QString expectedId = expected.section( QLatin1Char( ':' ), 0, 0 );
      const QString actualId = actual.section( QLatin1Char( ':' ), 0, 0 );
      QCOMPARE( actualId, expectedId );
      const QStringList categories = { QStringLiteral( "边界外" ),
                                       QStringLiteral( "指标值无效" ),
                                       QStringLiteral( "软纳入" ) };
      for ( const QString &category : categories )
      {
        if ( expected.contains( category ) )
          QVERIFY2( actual.contains( category ),
                    qPrintable( QStringLiteral( "skip[%1]: expected category %2 in «%3»" )
                                    .arg( i )
                                    .arg( category, actual ) ) );
      }
    }

    // ---- 网格轴 / 掩膜 / 值 ----
    const QJsonObject expectedGrid = golden.value( QStringLiteral( "grid" ) ).toObject();
    const QJsonObject actualGrid = model.value( QStringLiteral( "grid" ) ).toObject();
    const QVector<double> expectedX = jsonDoubleVector( expectedGrid.value( QStringLiteral( "x" ) ) );
    const QVector<double> expectedY = jsonDoubleVector( expectedGrid.value( QStringLiteral( "y" ) ) );
    const QVector<double> actualX = jsonDoubleVector( actualGrid.value( QStringLiteral( "x" ) ) );
    const QVector<double> actualY = jsonDoubleVector( actualGrid.value( QStringLiteral( "y" ) ) );
    QCOMPARE( actualX.size(), expectedX.size() );
    QCOMPARE( actualY.size(), expectedY.size() );
    const double spanX = expectedX.isEmpty() ? 1.0
                                             : expectedX.last() - expectedX.first();
    const double spanY = expectedY.isEmpty() ? 1.0
                                             : expectedY.last() - expectedY.first();
    const double axisTolX = 1e-9 * std::max( 1.0, std::abs( spanX ) );
    const double axisTolY = 1e-9 * std::max( 1.0, std::abs( spanY ) );
    for ( int i = 0; i < expectedX.size() && i < actualX.size(); ++i )
      QVERIFY2( std::abs( actualX[i] - expectedX[i] ) <= axisTolX,
                qPrintable( QStringLiteral( "x[%1] %2 vs %3" )
                                .arg( i )
                                .arg( actualX[i] )
                                .arg( expectedX[i] ) ) );
    for ( int i = 0; i < expectedY.size() && i < actualY.size(); ++i )
      QVERIFY2( std::abs( actualY[i] - expectedY[i] ) <= axisTolY,
                qPrintable( QStringLiteral( "y[%1] %2 vs %3" )
                                .arg( i )
                                .arg( actualY[i] )
                                .arg( expectedY[i] ) ) );

    const QJsonArray expectedMask = expectedGrid.value( QStringLiteral( "valid_mask" ) ).toArray();
    const QJsonArray actualMask = model.value( QStringLiteral( "valid_mask" ) ).toArray();
    const QJsonArray expectedZ = expectedGrid.value( QStringLiteral( "z" ) ).toArray();
    const QJsonArray actualZ = actualGrid.value( QStringLiteral( "z" ) ).toArray();
    QCOMPARE( actualMask.size(), expectedMask.size() );
    QCOMPARE( actualZ.size(), expectedZ.size() );
    double scale = 1.0;
    double sumSq = 0.0;
    int finiteCount = 0;
    for ( int r = 0; r < expectedZ.size() && r < actualZ.size(); ++r )
    {
      const QJsonArray expectedRow = expectedZ.at( r ).toArray();
      const QJsonArray actualRow = actualZ.at( r ).toArray();
      QCOMPARE( actualRow.size(), expectedRow.size() );
      for ( int c = 0; c < expectedRow.size() && c < actualRow.size(); ++c )
      {
        const bool expectedFinite = expectedRow.at( c ).isDouble();
        const bool actualFinite = actualRow.at( c ).isDouble();
        QCOMPARE( actualFinite, expectedFinite );
        if ( r < expectedMask.size() && r < actualMask.size() )
        {
          const QJsonArray expectedMaskRow = expectedMask.at( r ).toArray();
          const QJsonArray actualMaskRow = actualMask.at( r ).toArray();
          if ( c < expectedMaskRow.size() && c < actualMaskRow.size() )
          {
            // golden 存 int 0/1，C++ 存 bool——统一转 int 比。
            const int expectedCell = expectedMaskRow.at( c ).toVariant().toInt();
            const int actualCell = actualMaskRow.at( c ).toVariant().toInt();
            QCOMPARE( actualCell, expectedCell );
          }
        }
        if ( expectedFinite && actualFinite )
        {
          const double expected = expectedRow.at( c ).toDouble();
          const double actual = actualRow.at( c ).toDouble();
          scale = std::max( scale, std::abs( expected ) );
          metrics.maxAbs = std::max( metrics.maxAbs, std::abs( actual - expected ) );
          sumSq += ( actual - expected ) * ( actual - expected );
          ++finiteCount;
        }
      }
    }
    if ( finiteCount )
      metrics.rmse = std::sqrt( sumSq / finiteCount );
    QVERIFY2( metrics.maxAbs <= 1e-9 * scale,
              qPrintable( QStringLiteral( "z maxAbs %1 > 1e-9*%2" )
                              .arg( metrics.maxAbs )
                              .arg( scale ) ) );
    QVERIFY2( metrics.rmse <= 1e-10 * scale,
              qPrintable( QStringLiteral( "z RMSE %1 > 1e-10*%2" )
                              .arg( metrics.rmse )
                              .arg( scale ) ) );

    // ---- field_model ----
    const QJsonObject expectedFm = golden.value( QStringLiteral( "field_model" ) ).toObject();
    const QJsonObject actualFm = model.value( QStringLiteral( "field_model" ) ).toObject();
    QVERIFY( qAbs( jsonNumber( actualFm.value( QStringLiteral( "cluster_span" ) ) ) -
                   jsonNumber( expectedFm.value( QStringLiteral( "cluster_span" ) ) ) ) <
             1e-9 * std::max( 1.0, std::abs(
                                     jsonNumber( expectedFm.value( QStringLiteral( "cluster_span" ) ) ) ) ) );
    QCOMPARE( actualFm.value( QStringLiteral( "min_points" ) ).toInt(),
              expectedFm.value( QStringLiteral( "min_points" ) ).toInt() );
    QCOMPARE( actualFm.value( QStringLiteral( "max_points" ) ).toInt(),
              expectedFm.value( QStringLiteral( "max_points" ) ).toInt() );
    const bool expectedSearchNull = expectedFm.value( QStringLiteral( "search_radius" ) ).isNull();
    QCOMPARE( actualFm.value( QStringLiteral( "search_radius" ) ).isNull(), expectedSearchNull );

    const QJsonArray expectedDirs = expectedFm.value( QStringLiteral( "directions" ) ).toArray();
    const QJsonArray actualDirs = actualFm.value( QStringLiteral( "directions" ) ).toArray();
    QCOMPARE( actualDirs.size(), expectedDirs.size() );
    for ( int i = 0; i < expectedDirs.size() && i < actualDirs.size(); ++i )
    {
      const QJsonObject expected = expectedDirs.at( i ).toObject();
      const QJsonObject actual = actualDirs.at( i ).toObject();
      QCOMPARE( actual.value( QStringLiteral( "line_id" ) ).toString(),
                expected.value( QStringLiteral( "line_id" ) ).toString() );
      QCOMPARE( jsonNumber( actual.value( QStringLiteral( "ratio" ) ) ),
                jsonNumber( expected.value( QStringLiteral( "ratio" ) ) ) );
      for ( const QString &key : { QStringLiteral( "influence_radius" ),
                                   QStringLiteral( "core_radius" ) } )
      {
        const double expectedValue = jsonNumber( expected.value( key ) );
        const double actualValue = jsonNumber( actual.value( key ) );
        QVERIFY2( std::abs( actualValue - expectedValue ) <=
                      1e-9 * std::max( 1.0, std::abs( expectedValue ) ),
                  qPrintable( QStringLiteral( "dir %1 %2: %3 vs %4" )
                                  .arg( actual.value( QStringLiteral( "line_id" ) ).toString(),
                                        key )
                                  .arg( actualValue )
                                  .arg( expectedValue ) ) );
      }
      const QJsonArray expectedPieces = expected.value( QStringLiteral( "pieces" ) ).toArray();
      const QJsonArray actualPieces = actual.value( QStringLiteral( "pieces" ) ).toArray();
      QCOMPARE( actualPieces.size(), expectedPieces.size() );
      for ( int p = 0; p < expectedPieces.size() && p < actualPieces.size(); ++p )
      {
        const QJsonArray expectedPts = expectedPieces.at( p ).toArray();
        const QJsonArray actualPts = actualPieces.at( p ).toArray();
        QCOMPARE( actualPts.size(), expectedPts.size() );
        for ( int k = 0; k < expectedPts.size() && k < actualPts.size(); ++k )
        {
          const QJsonArray ep = expectedPts.at( k ).toArray();
          const QJsonArray ap = actualPts.at( k ).toArray();
          QCOMPARE( ap.size(), ep.size() );
          for ( int d = 0; d < ep.size() && d < ap.size(); ++d )
            QVERIFY2( std::abs( jsonNumber( ap.at( d ) ) - jsonNumber( ep.at( d ) ) ) <=
                          1e-6,
                      qPrintable( QStringLiteral( "piece pt mismatch" ) ) );
        }
      }
    }

    const QJsonArray expectedSoft =
        expectedFm.value( QStringLiteral( "interpretive_boundaries" ) ).toArray();
    const QJsonArray actualSoft =
        actualFm.value( QStringLiteral( "interpretive_boundaries" ) ).toArray();
    QCOMPARE( actualSoft.size(), expectedSoft.size() );
    for ( int i = 0; i < expectedSoft.size() && i < actualSoft.size(); ++i )
    {
      const QJsonObject expected = expectedSoft.at( i ).toObject();
      const QJsonObject actual = actualSoft.at( i ).toObject();
      QVERIFY( qAbs( jsonNumber( actual.value( QStringLiteral( "radius" ) ) ) -
                     jsonNumber( expected.value( QStringLiteral( "radius" ) ) ) ) < 1e-6 );
      QVERIFY( qAbs( jsonNumber( actual.value( QStringLiteral( "strength" ) ) ) -
                     jsonNumber( expected.value( QStringLiteral( "strength" ) ) ) ) < 1e-9 );
      const QJsonArray expectedPts = expected.value( QStringLiteral( "points" ) ).toArray();
      const QJsonArray actualPts = actual.value( QStringLiteral( "points" ) ).toArray();
      QCOMPARE( actualPts.size(), expectedPts.size() );
    }

    // ---- 宽度 / 值域 ----
    QVERIFY( qAbs( jsonNumber( model.value( QStringLiteral( "barrier_buffer_distance" ) ) ) -
                   jsonNumber( golden.value( QStringLiteral( "barrier_buffer_distance" ) ) ) ) <
             1e-6 );
    QVERIFY( qAbs( jsonNumber( model.value( QStringLiteral( "contour_stop_buffer_distance" ) ) ) -
                   jsonNumber( golden.value( QStringLiteral( "contour_stop_buffer_distance" ) ) ) ) <
             1e-6 );
    QVERIFY( qAbs( jsonNumber( model.value( QStringLiteral( "value_min" ) ) ) -
                   jsonNumber( golden.value( QStringLiteral( "value_min" ) ) ) ) < 1e-9 );
    QVERIFY( qAbs( jsonNumber( model.value( QStringLiteral( "value_max" ) ) ) -
                   jsonNumber( golden.value( QStringLiteral( "value_max" ) ) ) ) < 1e-9 );

    // ---- 覆盖率 ----
    const QJsonObject expectedDiag = golden.value( QStringLiteral( "diagnostics" ) ).toObject();
    const QJsonObject actualDiag = qc.value( QStringLiteral( "diagnostics" ) ).toObject();
    if ( expectedDiag.contains( QStringLiteral( "direction_coverage_percent" ) ) )
    {
      QVERIFY( qAbs( jsonNumber( actualDiag.value( QStringLiteral( "direction_coverage_percent" ) ) ) -
                     jsonNumber( expectedDiag.value( QStringLiteral( "direction_coverage_percent" ) ) ) ) <
               1e-6 );
    }
  }

private slots:
  // 导入语义：D2 ratio 默认 8 / D4 enabled=false；B2 full_block / B3 soft。
  void importDefaults();
  void importAutoRoleDetect();
  void syntheticGolden();
  void enpingGolden();
  // U3：POWER 入口校验（有限且 >0，文案对齐姊妹引擎「POWER 必须为有限正数」）。
  void powerZeroRejectedAtEntry();
  // U4a：local_kriging K 钳制回执（QC parameters 请求值 + effective 键）。
  void localKrigingClampReceiptInQc();
  // U5a：QC parameters 覆盖数值生效参数（SEARCH_RADIUS 等改变指纹输入）。
  void qcParametersCoverEffectiveInputs();
  // U5b：QC counts 补 finite/nodata（血缘 finite_cells/nodata_cells 不再恒 null）。
  void qcCountsFiniteNodata();
};

void tst_singlefactor_structural::importDefaults()
{
  QString err;
  const QVector<paleo::ImportedConstraintRecord> directions =
      paleo::readConstraintImportFeatures(
          fixturePath( QStringLiteral( "structural_synthetic/directions.shp" ) ),
          QStringLiteral( "direction" ), nullptr, &err );
  QVERIFY2( !directions.isEmpty(), qPrintable( err ) );
  QCOMPARE( directions.size(), 4 );
  QCOMPARE( directions.at( 0 ).type, QStringLiteral( "direction_line" ) );
  // D1：显式 3.0；D2：RATIO 缺失 → 默认 8；D4：ACTIVE=0 → enabled=false。
  QCOMPARE( directions.at( 0 ).params.value( QStringLiteral( "ratio" ) ).toDouble(), 3.0 );
  QCOMPARE( directions.at( 1 ).params.value( QStringLiteral( "ratio" ) ).toDouble(), 8.0 );
  QCOMPARE( directions.at( 1 ).params.value( QStringLiteral( "influenceRadius" ) ).toDouble(),
            1500.0 );
  QCOMPARE( directions.at( 1 ).params.value( QStringLiteral( "coreRadius" ) ).toDouble(), 400.0 );
  QVERIFY( !directions.at( 3 ).params.value( QStringLiteral( "enabled" ) ).toBool() );
  QVERIFY( directions.at( 0 ).params.value( QStringLiteral( "enabled" ) ).toBool() );

  const QVector<paleo::ImportedConstraintRecord> barriers =
      paleo::readConstraintImportFeatures(
          fixturePath( QStringLiteral( "structural_synthetic/barriers.shp" ) ),
          QStringLiteral( "barrier" ), nullptr, &err );
  QVERIFY2( !barriers.isEmpty(), qPrintable( err ) );
  QCOMPARE( barriers.size(), 4 );
  QCOMPARE( barriers.at( 0 ).type, QStringLiteral( "break_line" ) );
  QCOMPARE( barriers.at( 0 ).params.value( QStringLiteral( "blockMode" ) ).toString(),
            QStringLiteral( "display_only" ) );
  // B2：BLK_MODE 缺失 → full_block；B3：soft；B4：display_only + enabled=false。
  QCOMPARE( barriers.at( 1 ).params.value( QStringLiteral( "blockMode" ) ).toString(),
            QStringLiteral( "full_block" ) );
  QCOMPARE( barriers.at( 2 ).params.value( QStringLiteral( "blockMode" ) ).toString(),
            QStringLiteral( "soft" ) );
  QCOMPARE( barriers.at( 3 ).params.value( QStringLiteral( "blockMode" ) ).toString(),
            QStringLiteral( "display_only" ) );
  QVERIFY( !barriers.at( 3 ).params.value( QStringLiteral( "enabled" ) ).toBool() );
}

void tst_singlefactor_structural::importAutoRoleDetect()
{
  QString resolved;
  QString err;
  const QVector<paleo::ImportedConstraintRecord> directions =
      paleo::readConstraintImportFeatures(
          fixturePath( QStringLiteral( "structural_synthetic/directions.shp" ) ),
          QStringLiteral( "auto" ), &resolved, &err );
  QVERIFY2( !directions.isEmpty(), qPrintable( err ) );
  QCOMPARE( resolved, QStringLiteral( "direction" ) );
  const QVector<paleo::ImportedConstraintRecord> barriers =
      paleo::readConstraintImportFeatures(
          fixturePath( QStringLiteral( "structural_synthetic/barriers.shp" ) ),
          QStringLiteral( "auto" ), &resolved, &err );
  QVERIFY2( !barriers.isEmpty(), qPrintable( err ) );
  QCOMPARE( resolved, QStringLiteral( "barrier" ) );
}

void tst_singlefactor_structural::syntheticGolden()
{
  ensurePaleo();
  const QJsonObject golden = readJson(
      fixturePath( QStringLiteral( "structural_synthetic_golden.json" ) ) );
  QVERIFY( !golden.isEmpty() );
  const QString base = fixturePath( QStringLiteral( "structural_synthetic" ) );
  qint64 elapsed = 0;
  const auto [qc, model] =
      runStructural( base + QStringLiteral( "/wells.shp" ),
                     base + QStringLiteral( "/boundary.shp" ),
                     base + QStringLiteral( "/directions.shp" ),
                     base + QStringLiteral( "/barriers.shp" ), 120, &elapsed );
  QVERIFY2( !qc.isEmpty() && !model.isEmpty(), "structural algorithm produced no sidecars" );
  writeDump( QStringLiteral( "synthetic" ), makeDump( qc, model ) );
  CaseMetrics metrics;
  compareCase( golden, qc, model, metrics );
  qInfo() << "synthetic: maxAbs" << metrics.maxAbs << "RMSE" << metrics.rmse << "elapsed"
          << elapsed << "ms";
}

void tst_singlefactor_structural::enpingGolden()
{
  const QString packageEnv = qEnvironmentVariable( "PALEO_ENPING_PACKAGE" );
  const QString goldenEnv = qEnvironmentVariable( "PALEO_ENPING_GOLDEN" );
  if ( packageEnv.isEmpty() || goldenEnv.isEmpty() )
    QSKIP( "PALEO_ENPING_PACKAGE/PALEO_ENPING_GOLDEN not set" );
  ensurePaleo();
  QString d03 = packageEnv;
  if ( QDir( packageEnv ).exists( QStringLiteral( "03_点位与成图边界" ) ) )
    d03 = QDir( packageEnv ).filePath( QStringLiteral( "03_点位与成图边界" ) );
  const QString wells = QDir( d03 ).filePath( QStringLiteral( "外委恩平一二段_井位标注点.shp" ) );
  const QString boundary = QDir( d03 ).filePath( QStringLiteral( "恩平一二段_成图范围.shp" ) );
  const QString directions = QDir( d03 ).filePath( QStringLiteral( "方向线_解释草案.shp" ) );
  const QString barriers =
      QDir( d03 ).filePath( QStringLiteral( "打断线_解释性分区草案.shp" ) );
  for ( const QString &path : { wells, boundary, directions, barriers } )
    QVERIFY2( QFileInfo::exists( path ), qPrintable( path ) );
  const QJsonObject golden = readJson( goldenEnv );
  QVERIFY( !golden.isEmpty() );
  qint64 elapsed = 0;
  const auto [qc, model] = runStructural( wells, boundary, directions, barriers, 339, &elapsed );
  QVERIFY2( !qc.isEmpty() && !model.isEmpty(), "structural algorithm produced no sidecars" );
  writeDump( QStringLiteral( "enping" ), makeDump( qc, model ) );
  CaseMetrics metrics;
  compareCase( golden, qc, model, metrics );
  qInfo() << "enping: maxAbs" << metrics.maxAbs << "RMSE" << metrics.rmse << "elapsed" << elapsed
          << "ms";
}

// U3 Processing 级：POWER=0 / -1 在入口被拒，异常文案含 "POWER"——
// 此前无校验，0/-1 幂次一路进引擎，extend 模式下被误报成缺井。
void tst_singlefactor_structural::powerZeroRejectedAtEntry()
{
  ensurePaleo();
  const QString base = fixturePath( QStringLiteral( "structural_synthetic" ) );
  for ( const double badPower : { 0.0, -1.0 } )
  {
    qint64 elapsed = 0;
    const auto [qc, model] =
        runStructural( base + QStringLiteral( "/wells.shp" ),
                       base + QStringLiteral( "/boundary.shp" ), QString(), QString(), 60,
                       &elapsed, QVariantMap{ { QStringLiteral( "POWER" ), badPower } } );
    QVERIFY2( qc.isEmpty() && model.isEmpty(),
              qPrintable( QStringLiteral( "POWER=%1 must be rejected" ).arg( badPower ) ) );
  }
  // 直接断言算法抛出的文案（run() 吞异常进 feedback 日志）。
  auto wells = std::make_unique<QgsVectorLayer>( base + QStringLiteral( "/wells.shp" ),
                                                 QStringLiteral( "wells" ), QStringLiteral( "ogr" ) );
  auto boundary = std::make_unique<QgsVectorLayer>( base + QStringLiteral( "/boundary.shp" ),
                                                    QStringLiteral( "boundary" ),
                                                    QStringLiteral( "ogr" ) );
  QVERIFY( wells->isValid() && boundary->isValid() );
  const QgsProcessingAlgorithm *alg = QgsApplication::processingRegistry()->algorithmById(
      QStringLiteral( "paleo:paleo_structural_idw" ) );
  QVERIFY( alg );
  QVariantMap params;
  params.insert( QStringLiteral( "WELLS" ),
                 QVariant::fromValue( static_cast<QgsMapLayer *>( wells.get() ) ) );
  params.insert( QStringLiteral( "WELL_ID_FIELD" ), QStringLiteral( "well_id" ) );
  params.insert( QStringLiteral( "VALUE_FIELD" ), QStringLiteral( "sand_ratio" ) );
  params.insert( QStringLiteral( "BOUNDARY" ),
                 QVariant::fromValue( static_cast<QgsMapLayer *>( boundary.get() ) ) );
  params.insert( QStringLiteral( "POWER" ), 0.0 );
  params.insert( QStringLiteral( "GRID_RESOLUTION" ), 60 );
  params.insert( QStringLiteral( "OUTPUT" ),
                 mDir.filePath( QStringLiteral( "structural-power0.tif" ) ) );
  QgsProcessingContext ctx;
  QgsProcessingFeedback fb;
  const QVariantMap results = alg->run( params, ctx, &fb );
  QVERIFY( results.isEmpty() );
  QVERIFY2( fb.textLog().contains( QStringLiteral( "POWER" ) ),
            qPrintable( fb.textLog() ) );
}

// U4a Processing 级：METHOD=kriging + KRIGING_MAX_POINTS=100（引擎内钳到 64）→
// QC parameters.kriging_max_points 仍记请求值 100、另记 kriging_max_points_effective=64，
// issues 含 kriging_neighbourhood_clamped；K=16（<=64）无 effective 键（回归）。
void tst_singlefactor_structural::localKrigingClampReceiptInQc()
{
  ensurePaleo();
  auto wells = std::make_unique<QgsVectorLayer>( QStringLiteral( "Point" ),
                                                 QStringLiteral( "kriging-wells" ),
                                                 QStringLiteral( "memory" ) );
  QVERIFY( wells->isValid() );
  // 空 CRS + LOCAL_GRID=true → local_engineering（内存层默认 4326 会被拒）。
  wells->setCrs( QgsCoordinateReferenceSystem() );
  wells->startEditing();
  wells->addAttribute( QgsField( QStringLiteral( "well_id" ), QMetaType::Type::QString ) );
  wells->addAttribute( QgsField( QStringLiteral( "v" ), QMetaType::Type::Double ) );
  wells->updateFields();
  for ( int row = 0; row < 5; ++row )
  {
    for ( int column = 0; column < 5; ++column )
    {
      QgsFeature feature( wells->fields() );
      feature.setGeometry( QgsGeometry::fromPointXY(
          QgsPointXY( 100.0 + 40.0 * column, 100.0 + 40.0 * row ) ) );
      feature.setAttribute( QStringLiteral( "well_id" ),
                            QStringLiteral( "W%1" ).arg( row * 5 + column ) );
      feature.setAttribute( QStringLiteral( "v" ), 10.0 + std::sin( 0.7 * column ) + 0.3 * row );
      QVERIFY( wells->addFeature( feature ) );
    }
  }
  wells->commitChanges();

  const QgsProcessingAlgorithm *alg = QgsApplication::processingRegistry()->algorithmById(
      QStringLiteral( "paleo:paleo_local_direction_idw" ) );
  QVERIFY( alg );

  const auto run = [&]( int k ) -> QJsonObject {
    const QString outPath = mDir.filePath(
        QStringLiteral( "ldk-%1-%2.tif" ).arg( k ).arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ),
                   QVariant::fromValue( static_cast<QgsMapLayer *>( wells.get() ) ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "v" ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 40.0 );
    params.insert( QStringLiteral( "POWER" ), 2.0 );
    params.insert( QStringLiteral( "METHOD" ), QStringLiteral( "kriging" ) );
    params.insert( QStringLiteral( "NUGGET" ), 0.2 );
    params.insert( QStringLiteral( "SILL" ), 1.0 );
    params.insert( QStringLiteral( "RANGE" ), 150.0 );
    params.insert( QStringLiteral( "KRIGING_MAX_POINTS" ), k );
    params.insert( QStringLiteral( "LOCAL_GRID" ), true );
    params.insert( QStringLiteral( "OUTPUT" ), outPath );
    QgsProcessingContext ctx;
    QgsProcessingFeedback fb;
    const QVariantMap results = alg->run( params, ctx, &fb );
    if ( results.isEmpty() )
    {
      qWarning() << "local_direction_idw kriging run failed:" << fb.textLog();
      return {};
    }
    return readJson( results.value( QStringLiteral( "QC" ) ).toString() );
  };

  const QJsonObject qcBig = run( 100 );
  QVERIFY2( !qcBig.isEmpty(), "kriging K=100 run failed" );
  QCOMPARE( qcBig.value( QStringLiteral( "method_actual" ) ).toString(), QStringLiteral( "kriging" ) );
  const QJsonObject parameters = qcBig.value( QStringLiteral( "parameters" ) ).toObject();
  QCOMPARE( parameters.value( QStringLiteral( "kriging_max_points" ) ).toInt(), 100 );
  QCOMPARE( parameters.value( QStringLiteral( "kriging_max_points_effective" ) ).toInt(), 64 );
  bool issueSeen = false;
  for ( const QJsonValue &issue : qcBig.value( QStringLiteral( "issues" ) ).toArray() )
    issueSeen = issueSeen || issue.toString().contains( QStringLiteral( "kriging_neighbourhood_clamped" ) );
  QVERIFY2( issueSeen, "QC issues must carry the clamp receipt" );

  // 回归：K=16（<=64）无 effective 键、无回执。
  const QJsonObject qcSmall = run( 16 );
  const QJsonObject smallParameters = qcSmall.value( QStringLiteral( "parameters" ) ).toObject();
  QCOMPARE( smallParameters.value( QStringLiteral( "kriging_max_points" ) ).toInt(), 16 );
  QVERIFY( !smallParameters.contains( QStringLiteral( "kriging_max_points_effective" ) ) );
  for ( const QJsonValue &issue : qcSmall.value( QStringLiteral( "issues" ) ).toArray() )
    QVERIFY2( !issue.toString().contains( QStringLiteral( "kriging_neighbourhood_clamped" ) ),
              qPrintable( issue.toString() ) );
}

// U5a：QC parameters（= 参数指纹输入）必须覆盖实际数值生效的参数——
// SEARCH_RADIUS/MIN/MAX_POINTS/解释强度/屏障参数只改栅格不改 hash 的洞。
// 记 resolve 后生效值：extend=true 下 search_radius 为 null、min/max=1/0。
void tst_singlefactor_structural::qcParametersCoverEffectiveInputs()
{
  ensurePaleo();
  const QString base = fixturePath( QStringLiteral( "structural_synthetic" ) );
  const QString wellsPath = base + QStringLiteral( "/wells.shp" );
  const QString boundaryPath = base + QStringLiteral( "/boundary.shp" );
  const QString directionsPath = base + QStringLiteral( "/directions.shp" );
  const QString barriersPath = base + QStringLiteral( "/barriers.shp" );

  qint64 elapsed = 0;
  const auto [qcDefault, modelDefault] =
      runStructural( wellsPath, boundaryPath, directionsPath, barriersPath, 120, &elapsed );
  QVERIFY2( !qcDefault.isEmpty() && !modelDefault.isEmpty(), "default run failed" );
  const QJsonObject paramsDefault = qcDefault.value( QStringLiteral( "parameters" ) ).toObject();
  for ( const QString &key : { QStringLiteral( "search_radius" ),
                               QStringLiteral( "min_points" ),
                               QStringLiteral( "max_points" ),
                               QStringLiteral( "interpretive_strength" ),
                               QStringLiteral( "barrier_buffer" ),
                               QStringLiteral( "barrier_shape_radius" ),
                               QStringLiteral( "barrier_shape_strength" ),
                               QStringLiteral( "enable_barriers" ),
                               QStringLiteral( "enable_directions" ) } )
    QVERIFY2( paramsDefault.contains( key ),
              qPrintable( QStringLiteral( "parameters lacks %1" ).arg( key ) ) );
  // extend=true：搜索半径未生效（null）、min/max 解析为 1/0。
  QVERIFY( paramsDefault.value( QStringLiteral( "search_radius" ) ).isNull() );
  QCOMPARE( paramsDefault.value( QStringLiteral( "min_points" ) ).toInt(), 1 );
  QCOMPARE( paramsDefault.value( QStringLiteral( "max_points" ) ).toInt(), 0 );

  // extend=false + SEARCH_RADIUS=6000：resolve 后 search_radius=6000 进指纹输入，
  // 与默认跑的 parameters 序列化不同（不同参数不再共享同一指纹）。
  const auto [qcRadius, modelRadius] =
      runStructural( wellsPath, boundaryPath, directionsPath, barriersPath, 120, &elapsed,
                     QVariantMap{ { QStringLiteral( "EXTEND_TO_BOUNDARY" ), false },
                                  { QStringLiteral( "SEARCH_RADIUS" ), 6000.0 } } );
  QVERIFY2( !qcRadius.isEmpty(), "extend=false run failed" );
  const QJsonObject paramsRadius = qcRadius.value( QStringLiteral( "parameters" ) ).toObject();
  QCOMPARE( paramsRadius.value( QStringLiteral( "search_radius" ) ).toDouble(), 6000.0 );
  QVERIFY( QJsonDocument( paramsDefault ).toJson( QJsonDocument::Compact ) !=
           QJsonDocument( paramsRadius ).toJson( QJsonDocument::Compact ) );

  // 同 extend=false、不同 SEARCH_RADIUS（10 → 解析为 spacing*2，不为 6000）：
  // 两个 parameters 必须不同。
  const auto [qcSmall, modelSmall] =
      runStructural( wellsPath, boundaryPath, directionsPath, barriersPath, 120, &elapsed,
                     QVariantMap{ { QStringLiteral( "EXTEND_TO_BOUNDARY" ), false },
                                  { QStringLiteral( "SEARCH_RADIUS" ), 10.0 } } );
  QVERIFY2( !qcSmall.isEmpty(), "extend=false small-radius run failed" );
  const QJsonObject paramsSmall = qcSmall.value( QStringLiteral( "parameters" ) ).toObject();
  QVERIFY2( paramsSmall.value( QStringLiteral( "search_radius" ) ).toDouble() != 6000.0,
            "resolved spacing*2 radius must not equal 6000" );
  QVERIFY( QJsonDocument( paramsSmall ).toJson( QJsonDocument::Compact ) !=
           QJsonDocument( paramsRadius ).toJson( QJsonDocument::Compact ) );
}

// U5b：QC counts 补 finite/nodata——workflow 血缘（finite_cells/nodata_cells）
// 读 counts.finite/nodata，此前 structural 成果恒 null。独立核对：
// finite == structural.json grid.z 的非空格数、nodata == 空格数、和 == 网格格数；
// extend=false + 小搜索半径场景 nodata > 0。
void tst_singlefactor_structural::qcCountsFiniteNodata()
{
  ensurePaleo();
  const QString base = fixturePath( QStringLiteral( "structural_synthetic" ) );
  const QString wellsPath = base + QStringLiteral( "/wells.shp" );
  const QString boundaryPath = base + QStringLiteral( "/boundary.shp" );
  const QString directionsPath = base + QStringLiteral( "/directions.shp" );
  const QString barriersPath = base + QStringLiteral( "/barriers.shp" );

  qint64 elapsed = 0;
  const auto [qcDefault, modelDefault] =
      runStructural( wellsPath, boundaryPath, directionsPath, barriersPath, 120, &elapsed );
  QVERIFY2( !qcDefault.isEmpty() && !modelDefault.isEmpty(), "default run failed" );
  const QJsonObject counts = qcDefault.value( QStringLiteral( "counts" ) ).toObject();
  QVERIFY( counts.contains( QStringLiteral( "accepted" ) ) );
  QVERIFY( counts.contains( QStringLiteral( "skipped" ) ) );
  QVERIFY( counts.contains( QStringLiteral( "soft_included" ) ) );
  QVERIFY2( counts.contains( QStringLiteral( "finite" ) ), "counts lacks finite" );
  QVERIFY2( counts.contains( QStringLiteral( "nodata" ) ), "counts lacks nodata" );

  const QJsonArray zRows =
      modelDefault.value( QStringLiteral( "grid" ) ).toObject().value( QStringLiteral( "z" ) ).toArray();
  int finiteFromModel = 0;
  int cells = 0;
  for ( const QJsonValue &rowValue : zRows )
  {
    const QJsonArray row = rowValue.toArray();
    for ( const QJsonValue &cell : row )
    {
      ++cells;
      if ( cell.isDouble() )
        ++finiteFromModel;
    }
  }
  QVERIFY( cells > 0 );
  QCOMPARE( counts.value( QStringLiteral( "finite" ) ).toInt(), finiteFromModel );
  QCOMPARE( counts.value( QStringLiteral( "nodata" ) ).toInt(), cells - finiteFromModel );
  QCOMPARE( counts.value( QStringLiteral( "finite" ) ).toInt() +
                counts.value( QStringLiteral( "nodata" ) ).toInt(),
            cells );
  QVERIFY( finiteFromModel > 0 );

  // extend=false + 小搜索半径：有效域内支撑不足的格保持 NaN → nodata > 0。
  const auto [qcSparse, modelSparse] =
      runStructural( wellsPath, boundaryPath, directionsPath, barriersPath, 120, &elapsed,
                     QVariantMap{ { QStringLiteral( "EXTEND_TO_BOUNDARY" ), false },
                                  { QStringLiteral( "SEARCH_RADIUS" ), 10.0 } } );
  QVERIFY2( !qcSparse.isEmpty(), "sparse run failed" );
  const QJsonObject sparseCounts = qcSparse.value( QStringLiteral( "counts" ) ).toObject();
  QVERIFY2( sparseCounts.value( QStringLiteral( "nodata" ) ).toInt() > 0,
            "extend=false + small search radius must leave nodata cells" );
  QVERIFY( sparseCounts.value( QStringLiteral( "finite" ) ).toInt() <
           counts.value( QStringLiteral( "finite" ) ).toInt() );
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true );
  app.initQgis();
  GDALAllRegister();
  tst_singlefactor_structural tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_singlefactor_structural.moc"
