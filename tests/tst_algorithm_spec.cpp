// algo-atom P0：AlgorithmSpec 与 initAlgorithm / AlgorithmParamSchema 对齐。
// 故意不引导 QgsApplication——冻结参数名集源自 paleoalgorithms.cpp
// ConstraintIDWAlgorithm::initAlgorithm（与 schema 注释同源），避免为契约
// 单测拉起整套 QGIS。
#include <QtTest/QtTest>

#include "services/algorithmspec.h"
#include "services/algoparamschema.h"

namespace
{

// 冻结自 src/algorithms/paleoalgorithms.cpp ConstraintIDWAlgorithm::initAlgorithm
// （addParameter 名；不含 addOutput 的 CONSTRAINT_RECEIPTS / SAMPLE_RECEIPTS，
// 也不含隐藏约定 LOCAL_GRID_WKT）。
const QStringList kConstraintIdwInitAlgorithmParams = {
  QStringLiteral( "INPUT" ),
  QStringLiteral( "FIELD" ),
  QStringLiteral( "CONSTRAINTS" ),
  QStringLiteral( "FACIES_CODE" ),
  QStringLiteral( "CELL_SIZE" ),
  QStringLiteral( "ANISO_RATIO" ),
  QStringLiteral( "OUTPUT" ),
};

const QString kConstraintIdw = QStringLiteral( "paleo:paleo_constraint_idw" );

} // namespace

class AlgorithmSpecTests : public QObject
{
  Q_OBJECT
private slots:
  void constraintIdwSpecExistsAndComplete();
  void constraintIdwPortsMatchContract();
  void constraintIdwParamsSubsetOfInitAlgorithm();
  void constraintIdwSchemaFieldsSubsetOfSpecParams();
  void constraintIdwHiddenLocalGridWkt();
  void registryListsTwelveProcessingIds();
  void unknownIdReturnsEmpty();
};

void AlgorithmSpecTests::constraintIdwSpecExistsAndComplete()
{
  QVERIFY( AlgorithmSpecRegistry::knows( kConstraintIdw ) );
  QVERIFY( AlgorithmSpecRegistry::completeIds().contains( kConstraintIdw ) );
  const AlgorithmSpec s = AlgorithmSpecRegistry::spec( kConstraintIdw );
  QCOMPARE( s.id, kConstraintIdw );
  QVERIFY( s.complete );
  QCOMPARE( s.engine, QStringLiteral( "processing" ) );
  QVERIFY( !s.displayName.isEmpty() );
}

void AlgorithmSpecTests::constraintIdwPortsMatchContract()
{
  const AlgorithmSpec s = AlgorithmSpecRegistry::spec( kConstraintIdw );

  QHash<QString, AlgoPort> inById;
  for ( const AlgoPort &p : s.inputs )
    inById.insert( p.id, p );
  QVERIFY( inById.contains( QStringLiteral( "INPUT" ) ) );
  QCOMPARE( inById.value( QStringLiteral( "INPUT" ) ).kind, QStringLiteral( "vector" ) );
  QCOMPARE( inById.value( QStringLiteral( "INPUT" ) ).geometryHint, QStringLiteral( "point" ) );
  QVERIFY( inById.value( QStringLiteral( "INPUT" ) ).required );

  QVERIFY( inById.contains( QStringLiteral( "CONSTRAINTS" ) ) );
  QCOMPARE( inById.value( QStringLiteral( "CONSTRAINTS" ) ).kind, QStringLiteral( "vector" ) );
  QCOMPARE( inById.value( QStringLiteral( "CONSTRAINTS" ) ).geometryHint, QStringLiteral( "line" ) );
  QVERIFY( !inById.value( QStringLiteral( "CONSTRAINTS" ) ).required );

  QHash<QString, AlgoPort> outById;
  for ( const AlgoPort &p : s.outputs )
    outById.insert( p.id, p );
  QVERIFY( outById.contains( QStringLiteral( "OUTPUT" ) ) );
  QCOMPARE( outById.value( QStringLiteral( "OUTPUT" ) ).kind, QStringLiteral( "raster" ) );
  QVERIFY( outById.contains( QStringLiteral( "CONSTRAINT_RECEIPTS" ) ) );
  QVERIFY( outById.contains( QStringLiteral( "SAMPLE_RECEIPTS" ) ) );

  const QStringList keys = s.paramKeys();
  QVERIFY( keys.contains( QStringLiteral( "FIELD" ) ) );
  QVERIFY( keys.contains( QStringLiteral( "FACIES_CODE" ) ) );
  QVERIFY( keys.contains( QStringLiteral( "CELL_SIZE" ) ) );
  QVERIFY( keys.contains( QStringLiteral( "ANISO_RATIO" ) ) );
}

void AlgorithmSpecTests::constraintIdwParamsSubsetOfInitAlgorithm()
{
  const AlgorithmSpec s = AlgorithmSpecRegistry::spec( kConstraintIdw );
  const QStringList names = s.processingParameterNames();
  QVERIFY( !names.isEmpty() );
  for ( const QString &n : names )
  {
    QVERIFY2( kConstraintIdwInitAlgorithmParams.contains( n ),
              qPrintable( QStringLiteral( "Spec processing param '%1' not in frozen "
                                          "initAlgorithm set" )
                              .arg( n ) ) );
  }
  // 冻结集里的标量 + 图层名也应被 Spec 覆盖（完整契约，不仅 ⊆）
  for ( const QString &n : kConstraintIdwInitAlgorithmParams )
  {
    QVERIFY2( names.contains( n ),
              qPrintable( QStringLiteral( "initAlgorithm param '%1' missing from Spec "
                                          "processingParameterNames" )
                              .arg( n ) ) );
  }
}

void AlgorithmSpecTests::constraintIdwSchemaFieldsSubsetOfSpecParams()
{
  QVERIFY( AlgorithmParamSchema::knows( kConstraintIdw ) );
  const AlgorithmSpec s = AlgorithmSpecRegistry::spec( kConstraintIdw );
  const QStringList specKeys = s.paramKeys();
  const auto schemaFields = AlgorithmParamSchema::fieldsFor( kConstraintIdw );
  QVERIFY( !schemaFields.isEmpty() );
  for ( const AlgorithmParamField &f : schemaFields )
  {
    QVERIFY2( specKeys.contains( f.key ),
              qPrintable( QStringLiteral( "schema field '%1' not in Spec.params" ).arg( f.key ) ) );
  }
  // P3：ANISO_RATIO 已进 AlgorithmParamSchema，与 Spec / initAlgorithm 对齐
  QVERIFY( specKeys.contains( QStringLiteral( "ANISO_RATIO" ) ) );
  bool schemaHasAniso = false;
  for ( const AlgorithmParamField &f : schemaFields )
  {
    if ( f.key == QStringLiteral( "ANISO_RATIO" ) )
      schemaHasAniso = true;
  }
  QVERIFY2( schemaHasAniso, "ANISO_RATIO must be in AlgorithmParamSchema (algo-atom P3)" );
}

void AlgorithmSpecTests::constraintIdwHiddenLocalGridWkt()
{
  const AlgorithmSpec s = AlgorithmSpecRegistry::spec( kConstraintIdw );
  const QStringList hidden = s.hiddenParameterNames();
  QCOMPARE( hidden, QStringList{ QStringLiteral( "LOCAL_GRID_WKT" ) } );
  QVERIFY( !s.processingParameterNames().contains( QStringLiteral( "LOCAL_GRID_WKT" ) ) );
  QVERIFY( !kConstraintIdwInitAlgorithmParams.contains( QStringLiteral( "LOCAL_GRID_WKT" ) ) );
}

void AlgorithmSpecTests::registryListsTwelveProcessingIds()
{
  const QStringList ids = AlgorithmSpecRegistry::knownIds();
  QCOMPARE( ids.size(), 12 );
  QVERIFY( ids.contains( kConstraintIdw ) );
  // 桩条目存在但不 complete
  QVERIFY( AlgorithmSpecRegistry::knows( QStringLiteral( "paleo:paleo_facies_fusion" ) ) );
  QVERIFY( !AlgorithmSpecRegistry::spec( QStringLiteral( "paleo:paleo_facies_fusion" ) ).complete );
  QCOMPARE( AlgorithmSpecRegistry::completeIds().size(), 1 );
}

void AlgorithmSpecTests::unknownIdReturnsEmpty()
{
  QVERIFY( !AlgorithmSpecRegistry::knows( QStringLiteral( "paleo:does_not_exist" ) ) );
  const AlgorithmSpec s = AlgorithmSpecRegistry::spec( QStringLiteral( "paleo:does_not_exist" ) );
  QVERIFY( s.id.isEmpty() );
  QVERIFY( !s.complete );
}

QTEST_MAIN( AlgorithmSpecTests )
#include "tst_algorithm_spec.moc"
