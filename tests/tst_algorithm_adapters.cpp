// algo-atom P2：ProcessingAlgorithmAdapter / NativeCppAdapter 薄契约测试。
// 不引导 QgsApplication——null proc / Spec 查找不触达 Processing 运行时。
#include <QtTest/QtTest>

#include "services/algorithmspec.h"
#include "workflow/algorithmadapters.h"
#include "workflow/nativecppadapter.h"
#include "workflow/nativeinputs_geostat.h"
#include "workflow/nativeinputs_hztrack.h"
#include "workflow/nativeinputs_inversion.h"
#include "workflow/nativeinputs_facies.h"
#include "workflow/nativeinputs_wellsiting.h"
#include "workflow/algorithmpublish.h"

class AlgorithmAdapterTests : public QObject
{
  Q_OBJECT
private slots:
  void processingAdapterRejectsNullProc();
  void processingAdapterRejectsNonProcessingEngine();
  void processingAdapterRunByIdFallbackShell();
  void nativeCppAdapterMapEntryPointsToTyped();
  void ordinaryKrigingTypedRejectsEmptyPath();
  void sgsAndCokrigingTypedRejectEmptyPath();
  void trackSectionRejectsInvalidSeed();
  void bandlimitedRejectsNullTrace();
  void sparseSpikeRejectsNullTrace();
  void synthesizeRejectsNullPointers();
  void diagnoseCoverageRejectsNullPointers();
  void propagateRejectsInvalidGeometry();
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

void AlgorithmAdapterTests::nativeCppAdapterMapEntryPointsToTyped()
{
  paleo::NativeCppAdapter adapter;
  const QList<QPair<QString, QString>> cases = {
    { QStringLiteral( "geostat:ordinary_kriging" ), QStringLiteral( "runOrdinaryKriging" ) },
    { QStringLiteral( "geostat:ordinary_cokriging" ), QStringLiteral( "runOrdinaryCoKriging" ) },
    { QStringLiteral( "geostat:sgs" ), QStringLiteral( "runSgs" ) },
    { QStringLiteral( "hztrack:track_section" ), QStringLiteral( "runTrackSection" ) },
    { QStringLiteral( "inversion:bandlimited" ), QStringLiteral( "runBandlimited" ) },
    { QStringLiteral( "faciesmapping:synthesize" ), QStringLiteral( "runSynthesizeFacies" ) },
    { QStringLiteral( "inversion:sparse_spike" ), QStringLiteral( "runSparseSpike" ) },
    { QStringLiteral( "wellsiting:diagnose_coverage" ), QStringLiteral( "runDiagnoseCoverage" ) },
    { QStringLiteral( "hztrack:propagate_volume" ), QStringLiteral( "runPropagateVolume" ) },
  };
  for ( const auto &c : cases )
  {
    AlgorithmSpec spec;
    spec.id = c.first;
    spec.engine = QStringLiteral( "native_cpp" );
    QString err;
    const AlgorithmRunResult viaMap = adapter.run( spec, {}, &err );
    QVERIFY2( !viaMap.ok, qPrintable( c.first ) );
    QVERIFY2( err.contains( c.second ), qPrintable( c.first + QLatin1Char( ':' ) + err ) );
  }

  AlgorithmSpec unknown;
  unknown.id = QStringLiteral( "native:not_a_real_alg" );
  unknown.engine = QStringLiteral( "native_cpp" );
  QString err;
  const AlgorithmRunResult todo = adapter.run( unknown, {}, &err );
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


void AlgorithmAdapterTests::sgsAndCokrigingTypedRejectEmptyPath()
{
  paleo::NativeCppAdapter adapter;
  QString err;
  paleo::SgsInputs sgs;
  QVERIFY( !adapter.runSgs( sgs, &err ).ok );
  QVERIFY( err.contains( QStringLiteral( "无效" ) ) );
  err.clear();
  paleo::OrdinaryCoKrigingInputs co;
  QVERIFY( !adapter.runOrdinaryCoKriging( co, &err ).ok );
  QVERIFY( err.contains( QStringLiteral( "无效" ) ) );
}

void AlgorithmAdapterTests::trackSectionRejectsInvalidSeed()
{
  paleo::NativeCppAdapter adapter;
  std::vector<float> sec( 20, 0.f );
  paleo::TrackSectionInputs in;
  in.section = sec.data();
  in.nTraces = 4;
  in.nSamples = 5;
  in.seed = { 99, 0 }; // out of range
  QString err;
  paleo::hztrack::TrackResult out;
  const AlgorithmRunResult r = adapter.runTrackSection( in, &out, &err );
  QVERIFY( !r.ok );
  QVERIFY( err.contains( QStringLiteral( "非法" ) ) );
}

void AlgorithmAdapterTests::bandlimitedRejectsNullTrace()
{
  paleo::NativeCppAdapter adapter;
  paleo::BandlimitedInputs in;
  QString err;
  QVERIFY( !adapter.runBandlimited( in, nullptr, &err ).ok );
  QVERIFY( err.contains( QStringLiteral( "trace" ) ) );
}

void AlgorithmAdapterTests::synthesizeRejectsNullPointers()
{
  paleo::NativeCppAdapter adapter;
  paleo::SynthesizeFaciesInputs in;
  QString err;
  QVERIFY( !adapter.runSynthesizeFacies( in, nullptr, &err ).ok );
  QVERIFY( err.contains( QStringLiteral( "指针" ) ) );
}


void AlgorithmAdapterTests::sparseSpikeRejectsNullTrace()
{
  paleo::NativeCppAdapter adapter;
  paleo::SparseSpikeInputs in;
  QString err;
  QVERIFY( !adapter.runSparseSpike( in, nullptr, &err ).ok );
  QVERIFY( err.contains( QStringLiteral( "trace" ) ) || err.contains( QStringLiteral( "wavelet" ) ) );
}

void AlgorithmAdapterTests::diagnoseCoverageRejectsNullPointers()
{
  paleo::NativeCppAdapter adapter;
  paleo::DiagnoseCoverageInputs in;
  QString err;
  QVERIFY( !adapter.runDiagnoseCoverage( in, nullptr, nullptr, &err ).ok );
  QVERIFY( err.contains( QStringLiteral( "指针" ) ) );
}


void AlgorithmAdapterTests::propagateRejectsInvalidGeometry()
{
  paleo::NativeCppAdapter adapter;
  paleo::PropagateVolumeInputs in;
  // nIl=0 → Invalid
  QString err;
  paleo::hztrack::PropagateResult out;
  const AlgorithmRunResult r = adapter.runPropagateVolume( in, &out, &err );
  QVERIFY( !r.ok );
  QVERIFY( err.contains( QStringLiteral( "非法" ) ) );
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
