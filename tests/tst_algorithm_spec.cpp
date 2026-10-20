// algo-atom P0/P0-fill：AlgorithmSpec 与 initAlgorithm / AlgorithmParamSchema 对齐。
// 故意不引导 QgsApplication——冻结参数名集源自各算法 initAlgorithm。
#include <QtTest/QtTest>

#include "services/algorithmspec.h"
#include "services/algoparamschema.h"

namespace
{

const QString kConstraintIdw = QStringLiteral( "paleo:paleo_constraint_idw" );

// 各算法 addParameter 名（不含 addOutput 诊断串、不含隐藏 LOCAL_GRID_WKT）
const QHash<QString, QStringList> kFrozenInitParams = {
  { QStringLiteral( "paleo:paleo_constraint_idw" ),
    { QStringLiteral( "INPUT" ), QStringLiteral( "FIELD" ), QStringLiteral( "CONSTRAINTS" ),
      QStringLiteral( "FACIES_CODE" ), QStringLiteral( "CELL_SIZE" ),
      QStringLiteral( "ANISO_RATIO" ), QStringLiteral( "OUTPUT" ) } },
  { QStringLiteral( "paleo:paleo_facies_fusion" ),
    { QStringLiteral( "INPUTS" ), QStringLiteral( "OUTPUT" ) } },
  { QStringLiteral( "paleo:paleo_geological_smoothing" ),
    { QStringLiteral( "INPUT" ), QStringLiteral( "PASSES" ), QStringLiteral( "OUTPUT" ) } },
  { QStringLiteral( "paleo:paleo_facies_polygonize" ),
    { QStringLiteral( "INPUT" ), QStringLiteral( "SMOOTH" ), QStringLiteral( "MIN_CELLS" ),
      QStringLiteral( "MIN_FRACTION" ), QStringLiteral( "MIN_AREA" ),
      QStringLiteral( "SIMPLIFY" ), QStringLiteral( "BOUNDARY_SMOOTH" ),
      QStringLiteral( "SNAP_TOLERANCE" ), QStringLiteral( "ANGLE_TOLERANCE" ),
      QStringLiteral( "VALUE_RANGES" ), QStringLiteral( "CONSTRAINTS" ),
      QStringLiteral( "OUTPUT" ) } },
  { QStringLiteral( "paleo:paleo_isopach" ),
    { QStringLiteral( "INPUT_TOP" ), QStringLiteral( "INPUT_BASE" ),
      QStringLiteral( "NEGATIVE_TO_NODATA" ), QStringLiteral( "OUTPUT" ) } },
  { QStringLiteral( "paleo:paleo_welldist" ),
    { QStringLiteral( "INPUT" ), QStringLiteral( "CELL_SIZE" ), QStringLiteral( "OUTPUT" ) } },
  { QStringLiteral( "paleo:paleo_distance_transform" ),
    { QStringLiteral( "INPUT" ), QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "CELL_SIZE" ),
      QStringLiteral( "OUTPUT" ) } },
  { QStringLiteral( "paleo:paleo_min_curvature" ),
    { QStringLiteral( "INPUT" ), QStringLiteral( "FIELD" ), QStringLiteral( "CONSTRAINTS" ),
      QStringLiteral( "TENSION" ), QStringLiteral( "MAX_SWEEPS" ), QStringLiteral( "CELL_SIZE" ),
      QStringLiteral( "OUTPUT" ) } },
  { QStringLiteral( "paleo:paleo_local_direction_idw" ),
    { QStringLiteral( "INPUT" ), QStringLiteral( "FIELD" ), QStringLiteral( "CONSTRAINTS" ),
      QStringLiteral( "CELL_SIZE" ), QStringLiteral( "POWER" ), QStringLiteral( "COVERAGE" ),
      QStringLiteral( "CLUSTER" ), QStringLiteral( "LOCAL_GRID" ),
      QStringLiteral( "REQUIRE_FULL_COVERAGE" ), QStringLiteral( "PERCENT_TO_FRACTION" ),
      QStringLiteral( "MIN_POINTS" ), QStringLiteral( "MAX_POINTS" ),
      QStringLiteral( "SEARCH_RADIUS" ), QStringLiteral( "METHOD" ),
      QStringLiteral( "VARIAGRAM_MODEL" ), QStringLiteral( "NUGGET" ), QStringLiteral( "SILL" ),
      QStringLiteral( "RANGE" ), QStringLiteral( "VARIAGRAM_AZIMUTH" ),
      QStringLiteral( "KRIGING_MAX_POINTS" ), QStringLiteral( "VALUE_UNIT" ),
      QStringLiteral( "EXTENT" ), QStringLiteral( "OUTPUT" ) } },
  { QStringLiteral( "paleo:paleo_surfer_idw" ),
    { QStringLiteral( "INPUT" ), QStringLiteral( "FIELD" ), QStringLiteral( "CONSTRAINTS" ),
      QStringLiteral( "CELL_SIZE" ), QStringLiteral( "POWER" ), QStringLiteral( "COVERAGE" ),
      QStringLiteral( "LOCAL_GRID" ), QStringLiteral( "PERCENT_TO_FRACTION" ),
      QStringLiteral( "MIN_POINTS" ), QStringLiteral( "MAX_POINTS" ),
      QStringLiteral( "SEARCH_RADIUS" ), QStringLiteral( "ANISOTROPY_RATIO" ),
      QStringLiteral( "ANISOTROPY_ANGLE" ), QStringLiteral( "VALUE_UNIT" ),
      QStringLiteral( "EXTENT" ), QStringLiteral( "OUTPUT" ) } },
  { QStringLiteral( "paleo:paleo_cartographic_work" ),
    { QStringLiteral( "INPUT" ), QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "LEVELS" ),
      QStringLiteral( "TRANSITION" ), QStringLiteral( "OUTPUT" ) } },
  { QStringLiteral( "paleo:paleo_structural_idw" ),
    { QStringLiteral( "WELLS" ), QStringLiteral( "WELL_ID_FIELD" ),
      QStringLiteral( "VALUE_FIELD" ), QStringLiteral( "FACTOR_NAME" ),
      QStringLiteral( "FACTOR_MODE" ), QStringLiteral( "NUMERATOR_FIELD" ),
      QStringLiteral( "DENOMINATOR_FIELD" ), QStringLiteral( "VALUE_RANGE" ),
      QStringLiteral( "BOUNDARY" ), QStringLiteral( "INTERPOLATION_AREA" ),
      QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "GRID_RESOLUTION" ),
      QStringLiteral( "POWER" ), QStringLiteral( "MIN_POINTS" ), QStringLiteral( "MAX_POINTS" ),
      QStringLiteral( "SEARCH_RADIUS" ), QStringLiteral( "EXTEND_TO_BOUNDARY" ),
      QStringLiteral( "ENABLE_BARRIERS" ), QStringLiteral( "ENABLE_DIRECTIONS" ),
      QStringLiteral( "INTERPRETIVE_STRENGTH" ), QStringLiteral( "BARRIER_BUFFER" ),
      QStringLiteral( "BARRIER_BUFFER_AUTO" ), QStringLiteral( "BARRIER_SHAPE_RADIUS" ),
      QStringLiteral( "BARRIER_SHAPE_STRENGTH" ), QStringLiteral( "BARRIER_EXTEND" ),
      QStringLiteral( "BARRIER_EXTENSION_LIMIT" ), QStringLiteral( "CONTOUR_STOP_BUFFER" ),
      QStringLiteral( "CLUSTER" ), QStringLiteral( "LOCAL_GRID" ),
      QStringLiteral( "OUTPUT" ) } },
};

} // namespace

class AlgorithmSpecTests : public QObject
{
  Q_OBJECT
private slots:
  void allTwelveProcessingComplete();
  void nativeCppP4SpecsRegistered();
  void everyCompleteSpecMatchesFrozenInitAlgorithm();
  void schemaFieldsSubsetOfSpecParamsWhenKnown();
  void constraintIdwPortsAndAnisoInSchema();
  void unknownIdReturnsEmpty();
};

void AlgorithmSpecTests::allTwelveProcessingComplete()
{
  QCOMPARE( kFrozenInitParams.size(), 12 );
  for ( const QString &id : kFrozenInitParams.keys() )
  {
    QVERIFY2( AlgorithmSpecRegistry::knows( id ), qPrintable( id ) );
    const AlgorithmSpec s = AlgorithmSpecRegistry::spec( id );
    QVERIFY2( s.complete, qPrintable( id ) );
    QCOMPARE( s.engine, QStringLiteral( "processing" ) );
  }
}

void AlgorithmSpecTests::nativeCppP4SpecsRegistered()
{
  const QStringList nativeIds = {
    QStringLiteral( "geostat:ordinary_kriging" ),
    QStringLiteral( "hztrack:track_section" ),
    QStringLiteral( "inversion:bandlimited" ),
    QStringLiteral( "wellsiting:diagnose_coverage" ),
  };
  for ( const QString &id : nativeIds )
  {
    QVERIFY2( AlgorithmSpecRegistry::knows( id ), qPrintable( id ) );
    const AlgorithmSpec s = AlgorithmSpecRegistry::spec( id );
    QVERIFY2( s.complete, qPrintable( id ) );
    QCOMPARE( s.engine, QStringLiteral( "native_cpp" ) );
    QVERIFY2( !s.inputs.isEmpty() || !s.outputs.isEmpty(), qPrintable( id ) );
  }
  QVERIFY( AlgorithmSpecRegistry::knownIds().size() >= 16 );
  QVERIFY( AlgorithmSpecRegistry::completeIds().size() >= 16 );
}

void AlgorithmSpecTests::everyCompleteSpecMatchesFrozenInitAlgorithm()
{
  for ( auto it = kFrozenInitParams.constBegin(); it != kFrozenInitParams.constEnd(); ++it )
  {
    const AlgorithmSpec s = AlgorithmSpecRegistry::spec( it.key() );
    QVERIFY2( s.complete, qPrintable( it.key() ) );
    const QStringList names = s.processingParameterNames();
    for ( const QString &n : names )
    {
      QVERIFY2( it.value().contains( n ),
                qPrintable( QStringLiteral( "%1: Spec param '%2' not in frozen initAlgorithm" )
                                .arg( it.key(), n ) ) );
    }
    for ( const QString &n : it.value() )
    {
      QVERIFY2( names.contains( n ),
                qPrintable( QStringLiteral( "%1: initAlgorithm '%2' missing from Spec" )
                                .arg( it.key(), n ) ) );
    }
  }
}

void AlgorithmSpecTests::schemaFieldsSubsetOfSpecParamsWhenKnown()
{
  for ( const QString &id : AlgorithmParamSchema::registeredAlgorithms() )
  {
    if ( !AlgorithmSpecRegistry::knows( id ) )
      continue;
    const QStringList specKeys = AlgorithmSpecRegistry::spec( id ).paramKeys();
    for ( const AlgorithmParamField &f : AlgorithmParamSchema::fieldsFor( id ) )
    {
      QVERIFY2( specKeys.contains( f.key ),
                qPrintable( QStringLiteral( "%1 schema field '%2' not in Spec.params" )
                                .arg( id, f.key ) ) );
    }
  }
}

void AlgorithmSpecTests::constraintIdwPortsAndAnisoInSchema()
{
  const AlgorithmSpec s = AlgorithmSpecRegistry::spec( kConstraintIdw );
  QVERIFY( s.hiddenParameterNames().contains( QStringLiteral( "LOCAL_GRID_WKT" ) ) );
  QVERIFY( !s.processingParameterNames().contains( QStringLiteral( "LOCAL_GRID_WKT" ) ) );
  QVERIFY( s.paramKeys().contains( QStringLiteral( "ANISO_RATIO" ) ) );
  bool schemaHasAniso = false;
  for ( const AlgorithmParamField &f : AlgorithmParamSchema::fieldsFor( kConstraintIdw ) )
  {
    if ( f.key == QStringLiteral( "ANISO_RATIO" ) )
      schemaHasAniso = true;
  }
  QVERIFY( schemaHasAniso );
}

void AlgorithmSpecTests::unknownIdReturnsEmpty()
{
  QVERIFY( !AlgorithmSpecRegistry::knows( QStringLiteral( "paleo:does_not_exist" ) ) );
  QVERIFY( AlgorithmSpecRegistry::spec( QStringLiteral( "paleo:does_not_exist" ) ).id.isEmpty() );
}

QTEST_MAIN( AlgorithmSpecTests )
#include "tst_algorithm_spec.moc"
