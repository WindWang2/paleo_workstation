// algo-atom P2：ProcessingAlgorithmAdapter / NativeCppAdapter 薄契约测试。
// 不引导 QgsApplication——null proc / Spec 查找不触达 Processing 运行时。
#include <QtTest/QtTest>

#include "services/algorithmspec.h"
#include "workflow/algorithmadapters.h"
#include "workflow/algorithmpublish.h"

class AlgorithmAdapterTests : public QObject
{
  Q_OBJECT
private slots:
  void processingAdapterRejectsNullProc();
  void processingAdapterRejectsNonProcessingEngine();
  void processingAdapterRunByIdFallbackShell();
  void nativeCppAdapterIsStub();
  void ordinaryKrigingTypedRejectsEmptyPath();
  void runResultHelpersShapeArtifacts();
};

void AlgorithmAdapterTests::processingAdapterRejectsNullProc()
{
  paleo::ProcessingAlgorithmAdapter adapter( nullptr );
  const AlgorithmSpec spec =
      AlgorithmSpecRegistry::spec( QStringLiteral( "paleo:paleo_facies_fusion" ) );
  QVERIFY( spec.complete );
  QString err;
  const AlgorithmRunResult r = adapter.run( spec, {}, &err );
  QVERIFY( !r.ok );
  QVERIFY( err.contains( QStringLiteral( "未绑定" ) ) );
}

void AlgorithmAdapterTests::processingAdapterRejectsNonProcessingEngine()
{
  paleo::ProcessingAlgorithmAdapter adapter( nullptr );
  AlgorithmSpec spec;
  spec.id = QStringLiteral( "geostat:ordinary_kriging" );
  spec.engine = QStringLiteral( "native_cpp" );
  QString err;
  // null proc fails first — use a fake by setting engine check: call runById path
  // with empty id on a non-null would need service. Here directly test engine gate
  // by constructing adapter and ensuring native engine message when proc were set.
  // Without a live proc, null check wins — still assert NativeCpp stub separately.
  const AlgorithmRunResult r = adapter.run( spec, {}, &err );
  QVERIFY( !r.ok );
  QVERIFY( err.contains( QStringLiteral( "不是 processing" ) ) );
}

void AlgorithmAdapterTests::processingAdapterRunByIdFallbackShell()
{
  paleo::ProcessingAlgorithmAdapter adapter( nullptr );
  QString err;
  const AlgorithmRunResult r =
      adapter.runById( QStringLiteral( "paleo:does_not_exist_yet" ), {}, &err );
  QVERIFY( !r.ok ); // null proc
  QVERIFY( err.contains( QStringLiteral( "未绑定" ) ) );
}

void AlgorithmAdapterTests::nativeCppAdapterIsStub()
{
  paleo::NativeCppAdapter adapter;
  AlgorithmSpec okSpec;
  okSpec.id = QStringLiteral( "geostat:ordinary_kriging" );
  okSpec.engine = QStringLiteral( "native_cpp" );
  QString err;
  const AlgorithmRunResult viaMap = adapter.run( okSpec, {}, &err );
  QVERIFY( !viaMap.ok );
  QVERIFY( err.contains( QStringLiteral( "runOrdinaryKriging" ) ) );

  AlgorithmSpec other;
  other.id = QStringLiteral( "hztrack:track_section" );
  other.engine = QStringLiteral( "native_cpp" );
  err.clear();
  const AlgorithmRunResult todo = adapter.run( other, {}, &err );
  QVERIFY( !todo.ok );
  QVERIFY( err.contains( QStringLiteral( "尚未实现" ) ) );
}

void AlgorithmAdapterTests::ordinaryKrigingTypedRejectsEmptyPath()
{
  paleo::NativeCppAdapter adapter;
  paleo::OrdinaryKrigingInputs in;
  // grid invalid + empty path
  QString err;
  const AlgorithmRunResult r = adapter.runOrdinaryKriging( in, &err );
  QVERIFY( !r.ok );
  QVERIFY( err.contains( QStringLiteral( "无效" ) ) );
}

void AlgorithmAdapterTests::runResultHelpersShapeArtifacts()
{
  QVariantMap results;
  results.insert( QStringLiteral( "OUTPUT" ), QStringLiteral( "/tmp/out.tif" ) );
  results.insert( QStringLiteral( "CONSTRAINT_RECEIPTS" ), QStringLiteral( "[]" ) );
  const AlgorithmRunResult r = paleo::runResultFromProcessingOutput( results );
  QVERIFY( r.ok );
  QCOMPARE( r.artifacts.size(), 1 );
  QCOMPARE( r.artifacts.first().absolutePath, QStringLiteral( "/tmp/out.tif" ) );

  const AlgorithmRunResult empty = paleo::runResultFromOutputPath( QString() );
  QVERIFY( !empty.ok );

  const QVariantMap rebuilt = paleo::processingResultsFromRunResult( r );
  QCOMPARE( rebuilt.value( QStringLiteral( "OUTPUT" ) ).toString(),
            QStringLiteral( "/tmp/out.tif" ) );
}

QTEST_MAIN( AlgorithmAdapterTests )
#include "tst_algorithm_adapters.moc"
