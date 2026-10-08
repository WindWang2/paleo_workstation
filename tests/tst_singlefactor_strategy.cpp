// 层：数据（测试壳位于 tests/，被测对象为数据层词表）
#include <QtTest/QtTest>

#include "domain/singlefactorstrategy.h"

using namespace paleo::singlefactor;

class SingleFactorStrategyTests : public QObject
{
  Q_OBJECT
  private slots:
    void surfacePacksMapToRealEngines();
    void unknownIdsDoNotFallBack();
    void contourPacksResolveToAbsoluteParameters();
    void contourPacksListUnimplementedParameters();
    void cartographicWorkPacksAreExplicit();
};

// 参数包与真实算法严格一致：已实现包的 processingId 必须指向本仓真实注册的
// Processing 入口（局部方向克里金与 IDW 共用入口，血缘走独立 algorithmId）。
void SingleFactorStrategyTests::surfacePacksMapToRealEngines()
{
  const QVector<SurfaceMethodPack> &packs = surfaceMethodPacks();
  QVERIFY( packs.size() >= 5 );
  // paleoalgorithms.cpp 里真实 addAlgorithm 过的 Processing 入口（局部方向族必须
  // 落在其中，否则工作流 prepare 的 algorithmIds() 检查会直接拒绝）。
  const QStringList registeredProcessing{ QStringLiteral( "paleo:paleo_local_direction_idw" ),
                                          QStringLiteral( "paleo:paleo_surfer_idw" ),
                                          QStringLiteral( "paleo:paleo_structural_idw" ),
                                          QStringLiteral( "paleo:paleo_constraint_idw" ) };
  for ( const SurfaceMethodPack &pack : packs )
  {
    QVERIFY2( !pack.id.isEmpty(), "参数包 id 不能为空" );
    QVERIFY2( !pack.label.isEmpty(), qPrintable( pack.id ) );
    if ( pack.implemented )
    {
      QVERIFY2( pack.processingId.startsWith( QStringLiteral( "paleo:" ) ), qPrintable( pack.id ) );
      QVERIFY2( pack.algorithmId.startsWith( QStringLiteral( "paleo:" ) ), qPrintable( pack.id ) );
    }
    else
    {
      QVERIFY2( pack.processingId.isEmpty(), qPrintable( pack.id ) );
      QVERIFY2( pack.algorithmId.isEmpty(), qPrintable( pack.id ) );
    }
  }
  const SurfaceMethodPack *local = surfaceMethodPack( QStringLiteral( "local_direction_idw" ) );
  QVERIFY( local != nullptr );
  QCOMPARE( local->processingId, QStringLiteral( "paleo:paleo_local_direction_idw" ) );
  QCOMPARE( local->algorithmId, QStringLiteral( "paleo:paleo_local_direction_idw" ) );
  QVERIFY( registeredProcessing.contains( local->processingId ) );
  const SurfaceMethodPack *localKriging =
      surfaceMethodPack( QStringLiteral( "local_direction_kriging" ) );
  QVERIFY( localKriging != nullptr );
  // 共用 Processing 入口（METHOD=kriging 切引擎），血缘 id 独立；
  // 入口必须是已注册的那个，否则 UI 任务池路径会在 prepare 阶段被拒。
  QCOMPARE( localKriging->processingId, QStringLiteral( "paleo:paleo_local_direction_idw" ) );
  QCOMPARE( localKriging->algorithmId, QStringLiteral( "paleo:paleo_local_direction_kriging" ) );
  QVERIFY( registeredProcessing.contains( localKriging->processingId ) );
  QVERIFY( localKriging->supportsConstraints );
  // 全局克里金不消费约束线（方向18 的 v1 语义），包表也要如实。
  const SurfaceMethodPack *kriging = surfaceMethodPack( QStringLiteral( "kriging" ) );
  QVERIFY( kriging != nullptr );
  QVERIFY( !kriging->supportsConstraints );
  // 方向84：协克里金包——共用局部方向 Processing 入口（METHOD=cokriging 切引擎），
  // 血缘 id 独立；支持约束（硬屏障分量隔离 + 方向线/软边界如实记 v1 不消费）。
  const SurfaceMethodPack *cokriging = surfaceMethodPack( QStringLiteral( "cokriging" ) );
  QVERIFY( cokriging != nullptr );
  QCOMPARE( cokriging->processingId, QStringLiteral( "paleo:paleo_local_direction_idw" ) );
  QCOMPARE( cokriging->algorithmId, QStringLiteral( "paleo:paleo_local_direction_cokriging" ) );
  QVERIFY( registeredProcessing.contains( cokriging->processingId ) );
  QVERIFY( cokriging->supportsConstraints );
  QVERIFY( cokriging->implemented );
  QVERIFY( !cokriging->geologicalNote.isEmpty() );
}

// 未知 id 不静默回退（标签与算法必须一致）。
void SingleFactorStrategyTests::unknownIdsDoNotFallBack()
{
  QVERIFY( surfaceMethodPack( QStringLiteral( "magic_wand" ) ) == nullptr );
  QVERIFY( surfaceMethodPack( QString() ) == nullptr );
  QVERIFY( contourExtractPack( QStringLiteral( "no_such_contour" ) ) == nullptr );
  QVERIFY( cartographicWorkPack( QStringLiteral( "no_such_work" ) ) == nullptr );
}

void SingleFactorStrategyTests::contourPacksResolveToAbsoluteParameters()
{
  const ContourExtractPack *cartographic =
      contourExtractPack( QStringLiteral( "marching_squares_cartographic" ) );
  QVERIFY( cartographic != nullptr );
  const ContourExtractParameters resolved = resolveContourExtract( *cartographic, 25.0 );
  QCOMPARE( resolved.methodId, QStringLiteral( "marching_squares_cartographic" ) );
  QCOMPARE( resolved.smoothingIterations, 3 );
  QVERIFY( std::fabs( resolved.simplifyTolerance - 2.5 ) <= 1e-12 );
  QVERIFY( std::fabs( resolved.bridgeGap - 50.0 ) <= 1e-12 );
  QVERIFY( std::fabs( resolved.minContourLength - 350.0 ) <= 1e-12 );

  const ContourExtractPack *raw = contourExtractPack( QStringLiteral( "marching_squares_raw" ) );
  QVERIFY( raw != nullptr );
  const ContourExtractParameters rawResolved = resolveContourExtract( *raw, 25.0 );
  QCOMPARE( rawResolved.smoothingIterations, 0 );
  QCOMPARE( rawResolved.simplifyTolerance, 0.0 );
  QCOMPARE( rawResolved.upsampleFactor, 1 );
  // gridStep 非正时按 1e-9 兜底，不产生负参数。
  const ContourExtractParameters guarded = resolveContourExtract( *cartographic, 0.0 );
  QVERIFY( guarded.simplifyTolerance >= 0.0 );
  QVERIFY( guarded.bridgeGap >= 0.0 );
}

// 本仓没实现的上游参数必须逐条列出，不得假装消费。
void SingleFactorStrategyTests::contourPacksListUnimplementedParameters()
{
  for ( const ContourExtractPack &pack : contourExtractPacks() )
  {
    QVERIFY( !pack.id.isEmpty() );
    QVERIFY( !pack.label.isEmpty() );
    QVERIFY( pack.upsampleFactor >= 1 );
    QVERIFY( pack.smoothingIterations >= 0 );
    for ( const QString &name : pack.notImplemented )
      QVERIFY2( !pack.consumedParameters.contains( name ), qPrintable( pack.id + "/" + name ) );
  }
  const ContourExtractPack *cartographic =
      contourExtractPack( QStringLiteral( "marching_squares_cartographic" ) );
  QVERIFY( cartographic != nullptr );
  QVERIFY( cartographic->consumedParameters.contains( QStringLiteral( "smoothing_iterations" ) ) );
  QVERIFY( cartographic->notImplemented.contains( QStringLiteral( "upsample_factor" ) ) );
}

void SingleFactorStrategyTests::cartographicWorkPacksAreExplicit()
{
  const CartographicWorkPack *detour =
      cartographicWorkPack( QStringLiteral( "v17_local_interpretive_detour" ) );
  QVERIFY( detour != nullptr );
  QCOMPARE( detour->partitionVersion, QStringLiteral( "17" ) );
  QCOMPARE( detour->geometryPolicy, QStringLiteral( "local_interpretive_detour" ) );
  QVERIFY( detour->implemented );
  QVERIFY( detour->requiresAnalysisField );

  const CartographicWorkPack *plain = cartographicWorkPack( QStringLiteral( "analysis_numeric_only" ) );
  QVERIFY( plain != nullptr );
  QVERIFY( !plain->requiresAnalysisField );
  QVERIFY( plain->partitionVersion.isEmpty() );
}

QTEST_MAIN( SingleFactorStrategyTests )
#include "tst_singlefactor_strategy.moc"
