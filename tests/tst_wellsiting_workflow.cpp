// 层：测试壳
#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "helpers/workflowfixture.h"

#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/wellsitingstore.h"
#include "../src/services/projectdata.h"
#include "../src/workflow/wellsitingworkflow.h"

#include <qgsapplication.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

// 方向34 井网辅助编排测试（Oracle 2/4 + 端到端面）：
//   · planned 隔离红线——含 planned 井场景下，诊断/评估的实井输入集
//     （realWellsForSiting / catalog.entities("well") / ProjectDataFacade）
//     断言不含 planned；
//   · planned CRUD 生命周期（改名/移动=弃旧建新/删除=retired，catalog 实体
//     保留审计痕迹）；
//   · 诊断端到端——空洞报告 + wellsiting_holes 声明（07_Validation）落图；
//   · 评估改善方向（workflow 层）+ 贡献分与场景差同栅格口径一致；
//   · 方案保存-重开一致（点位/指标快照/可见性面，Oracle 4）；
//   · 导出（CSV 点位表 + 对比图 PNG）。

using paleo::tests::WorkflowFixture;
using paleo::tests::initFixture;

namespace
{

// 实井 8 口（3×3 缺东北角，同核测试夹具）；R=3500 时只有东北角一个空洞。
void addRealWells( DataCatalog &catalog, QString *error = nullptr )
{
  for ( int cx = 1000; cx <= 9000; cx += 4000 )
    for ( int cy = 1000; cy <= 9000; cy += 4000 )
    {
      if ( cx == 9000 && cy == 9000 )
        continue;
      CatalogEntity e;
      e.entityType = QStringLiteral( "well" );
      e.name = QStringLiteral( "W%1%2" ).arg( cx ).arg( cy );
      e.hasSurface = true;
      e.surfaceX = double( cx );
      e.surfaceY = double( cy );
      e.coordinateStatus = QStringLiteral( "untransformed" );
      e.id = catalog.nextEntityId( QStringLiteral( "well" ) );
      QVERIFY2( catalog.addEntity( e, error ), "real well add failed" );
    }
}

// 10km 方形工区边界约束（与核测试夹具同域）。
QList<SitingConstraintGeometry> boundaryConstraints()
{
  SitingConstraintGeometry boundary;
  boundary.type = QStringLiteral( "polygon" );
  boundary.wkt = QStringLiteral( "POLYGON((0 0, 10000 0, 10000 10000, 0 10000))" );
  return { boundary };
}

WellSitingParams holeParams()
{
  WellSitingParams params;
  params.controlRadius = 3500;
  params.cellSize = 100;
  params.gridSpacing = 500;
  return params;
}

} // namespace

class TestWellSitingWorkflow : public QObject
{
  Q_OBJECT
  private slots:
    void initTestCase();
    void plannedIsolation();
    void plannedCrudLifecycle();
    void diagnosisEndToEnd();
    void domainFallbackNote();
    void candidatesEndToEnd();
    void evaluationImprovement();
    void scenarioPersistenceReopen();
    void exports();
    void cleanupTestCase();
};

void TestWellSitingWorkflow::initTestCase()
{
  QVERIFY( QgsApplication::instance() != nullptr );
}

namespace
{

struct SitingStack
{
  WorkflowFixture f;
  WellSitingStore store;
  WellSitingWorkflow wf;
  ProjectDataFacade facade;

  explicit SitingStack( bool withBoundary = true )
      : store( f.dir.filePath( QStringLiteral( "project.sqlite" ) ) ), wf( &f.layers, &f.store )
  {
    QVERIFY( initFixture( f ) );
    QString error;
    QVERIFY( store.open( &error ) );
    facade.setCatalog( &f.catalog, f.dir.path() );
    wf.setCatalog( &f.catalog, f.dir.path() );
    wf.setProjectData( &facade );
    wf.setSitingStore( &store );
    addRealWells( f.catalog );
    // 默认装 10km 方形工区边界约束（与核测试夹具同域）；无边界时的
    // 井位包围盒回退在 domainFallbackNote 单测。
    if ( withBoundary )
    {
      QList<SitingConstraintGeometry> constraints;
      SitingConstraintGeometry boundary;
      boundary.type = QStringLiteral( "polygon" );
      boundary.wkt = QStringLiteral( "POLYGON((0 0, 10000 0, 10000 10000, 0 10000))" );
      constraints.append( boundary );
      wf.setConstraintProvider( [constraints] { return constraints; } );
    }
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Oracle 2：planned 隔离红线
// ---------------------------------------------------------------------------
void TestWellSitingWorkflow::plannedIsolation()
{
  SitingStack s;
  QString error;
  const QString plannedId = s.wf.addPlannedWell( QStringLiteral( "P1" ), 9000, 9000, &error );
  QVERIFY( !plannedId.isEmpty() );

  // 实井集（诊断/评估输入）不含 planned——红线本体。
  const QList<QVariantMap> realWells = s.wf.realWellsForSiting();
  QCOMPARE( realWells.size(), 8 );
  for ( const QVariantMap &w : realWells )
    QVERIFY2( w.value( QStringLiteral( "id" ) ).toString() != plannedId,
              "planned 井混入实井输入集" );

  // catalog 按类型查询（单因素/编图链的取井口径）不含 planned。
  for ( const CatalogEntity &e : s.f.catalog.entities( QStringLiteral( "well" ) ) )
    QVERIFY( e.id != plannedId );
  QCOMPARE( s.f.catalog.entities( QStringLiteral( "planned" ) ).size(), 1 );

  // ProjectDataFacade（编图链读侧唯一入口）同样不含 planned。
  const QVector<ProjectWell> facadeWells = s.facade.wells();
  QCOMPARE( facadeWells.size(), 8 );
  for ( const ProjectWell &w : facadeWells )
    QVERIFY( w.id != plannedId );

  // 诊断与评估都在场：well_count 只数实井，候选井走显式 planned 通道。
  QVERIFY( s.wf.runDiagnosis( holeParams(), &error ) );
  QCOMPARE( s.wf.lastDiagnosisSummary().value( QStringLiteral( "well_count" ) ).toInt(), 8 );
  const QVariantMap evaluation = s.wf.evaluateScenario( holeParams(), { plannedId }, &error );
  QVERIFY( !evaluation.isEmpty() );
  QCOMPARE( evaluation.value( QStringLiteral( "well_count" ) ).toInt(), 8 );
  QCOMPARE( evaluation.value( QStringLiteral( "candidate_count" ) ).toInt(), 1 );
}

void TestWellSitingWorkflow::plannedCrudLifecycle()
{
  SitingStack s;
  QString error;
  const QString id1 = s.wf.addPlannedWell( QStringLiteral( "P1" ), 9000, 9000, &error );
  QVERIFY( !id1.isEmpty() );
  QCOMPARE( s.wf.plannedWells().size(), 1 );
  QVERIFY( QFileInfo::exists( s.f.dir.filePath( QStringLiteral( "artifacts/layers/planned_wells.geojson" ) ) ) );

  // 实体类型与 id 前缀（catalog 词表登记面）。
  const CatalogEntity entity = s.f.catalog.entityById( id1 );
  QCOMPARE( entity.entityType, QStringLiteral( "planned" ) );
  QVERIFY( entity.id.startsWith( QStringLiteral( "planned-" ) ) );
  QCOMPARE( entity.coordinateStatus, QStringLiteral( "untransformed" ) );

  // 改名：显示名走 siting 文档，catalog 实体名保持审计痕迹。
  QVERIFY( s.wf.renamePlannedWell( id1, QStringLiteral( "替补井A" ), &error ) );
  QCOMPARE( s.wf.plannedWells().first().value( QStringLiteral( "name" ) ).toString(),
            QStringLiteral( "替补井A" ) );
  QCOMPARE( s.f.catalog.entityById( id1 ).name, QStringLiteral( "P1" ) );

  // 移动 = 弃旧建新（catalog 实体不可变）：新 id 新坐标，旧 id retired。
  QVERIFY( s.wf.movePlannedWell( id1, 9500, 9500, &error ) );
  const QList<QVariantMap> moved = s.wf.plannedWells();
  QCOMPARE( moved.size(), 1 );
  const QString id2 = moved.first().value( QStringLiteral( "id" ) ).toString();
  QVERIFY( id2 != id1 );
  QCOMPARE( moved.first().value( QStringLiteral( "x" ) ).toDouble(), 9500.0 );
  QCOMPARE( s.f.catalog.entityById( id1 ).id, id1 ); // 审计痕迹仍在

  // 删除 = retired：列表清空，catalog 实体保留。
  QVERIFY( s.wf.removePlannedWell( id2, &error ) );
  QCOMPARE( s.wf.plannedWells().size(), 0 );
  QCOMPARE( s.f.catalog.entities( QStringLiteral( "planned" ) ).size(), 2 ); // 审计痕迹

  // 坏输入如实失败。
  QVERIFY( s.wf.addPlannedWell( QStringLiteral( "  " ), 0, 0, &error ).isEmpty() );
  QVERIFY( !s.wf.renamePlannedWell( id2, QString(), &error ) );
  QVERIFY( !s.wf.removePlannedWell( QStringLiteral( "planned-999" ), &error ) );
}

void TestWellSitingWorkflow::diagnosisEndToEnd()
{
  SitingStack s;
  QString error;
  QSignalSpy doneSpy( &s.wf, &WellSitingWorkflow::diagnosisDone );
  QVERIFY( s.wf.runDiagnosis( holeParams(), &error ) );
  QCOMPARE( doneSpy.size(), 1 );

  const QVariantMap summary = s.wf.lastDiagnosisSummary();
  QCOMPARE( summary.value( QStringLiteral( "hole_count" ) ).toInt(), 1 );
  // 合成夹具解析值：≈2.33 km²（核测试同口径）。
  const double areaKm2 = summary.value( QStringLiteral( "hole_area_km2" ) ).toDouble();
  QVERIFY2( areaKm2 > 2.0 && areaKm2 < 2.7,
            qPrintable( QStringLiteral( "hole_area_km2=%1" ).arg( areaKm2 ) ) );
  // 域来源注记如实（本夹具带工区边界约束）。
  QVERIFY( summary.value( QStringLiteral( "domain_source" ) ).toString()
               .contains( QStringLiteral( "工区边界约束" ) ) );
  QVERIFY( !summary.value( QStringLiteral( "note" ) ).toString().isEmpty() );

  // 空洞行带 WKT（东北角）与最深点。
  const QList<QVariantMap> holes = s.wf.lastHoles();
  QCOMPARE( holes.size(), 1 );
  QVERIFY( holes.first().value( QStringLiteral( "wkt" ) ).toString()
               .startsWith( QStringLiteral( "POLYGON" ) ) );
  QVERIFY( holes.first().value( QStringLiteral( "deepest_x" ) ).toDouble() > 9800 );

  // 诊断图层：文件落地 + 声明（07_Validation 组，horizon 无关）。
  const QString geojson =
      s.f.dir.filePath( QStringLiteral( "artifacts/layers/wellsiting_holes.geojson" ) );
  QVERIFY( QFileInfo::exists( geojson ) );
  QVector<LayerDeclaration> decls;
  QVERIFY( s.f.layers.tryDeclared( &decls, &error ) );
  const LayerDeclaration *holeDecl = nullptr;
  for ( const LayerDeclaration &d : decls )
    if ( d.layerId == QLatin1String( "wellsiting_holes" ) )
      holeDecl = &d;
  QVERIFY( holeDecl != nullptr );
  QCOMPARE( holeDecl->group, QStringLiteral( "07_Validation" ) );
  QCOMPARE( holeDecl->horizon, QString() );
  QCOMPARE( holeDecl->source, geojson );

  // 无实井时如实失败，不造域。
  WorkflowFixture empty;
  QVERIFY( initFixture( empty ) );
  WellSitingStore emptyStore( empty.dir.filePath( QStringLiteral( "project.sqlite" ) ) );
  WellSitingWorkflow emptyWf( &empty.layers, &empty.store );
  emptyWf.setCatalog( &empty.catalog, empty.dir.path() );
  emptyWf.setSitingStore( &emptyStore );
  QVERIFY( !emptyWf.runDiagnosis( holeParams(), &error ) );
  QVERIFY( !error.isEmpty() );
}

void TestWellSitingWorkflow::domainFallbackNote()
{
  // 无边界约束/无地震角点 → 域回退为井位包围盒外扩 controlRadius，且
  // 包围盒外的环带空洞如实报出来（不假装全域受控）。
  SitingStack s( false );
  QString error;
  QVERIFY( s.wf.runDiagnosis( holeParams(), &error ) );
  const QVariantMap summary = s.wf.lastDiagnosisSummary();
  QVERIFY( summary.value( QStringLiteral( "domain_source" ) ).toString()
               .contains( QStringLiteral( "井位包围盒" ) ) );
  QVERIFY( summary.value( QStringLiteral( "hole_count" ) ).toInt() >= 1 );
  QVERIFY( summary.value( QStringLiteral( "domain_area_km2" ) ).toDouble() > 200.0 );
}

void TestWellSitingWorkflow::candidatesEndToEnd()
{
  SitingStack s;
  QString error;
  const WellSitingParams params = holeParams();
  QVERIFY( s.wf.generateCandidates( params, &error ) );
  const QList<QVariantMap> candidates = s.wf.lastCandidates();
  QVERIFY( candidates.size() >= 2 );
  for ( const QVariantMap &c : candidates )
  {
    QVERIFY( c.value( QStringLiteral( "strategy" ) ).toString() == QLatin1String( "deepest" ) ||
             c.value( QStringLiteral( "strategy" ) ).toString() == QLatin1String( "grid" ) );
    QVERIFY( c.value( QStringLiteral( "contribution_hole_area_m2" ) ).toDouble() >= 0 );
  }

  // 避让线沿对角穿空洞腹地（缓冲 1500m 否掉全部候选——核测试同口径）。
  // provider 同时带工区边界（避免落回包围盒回退域）与避让线。
  WellSitingWorkflow wf2( &s.f.layers, &s.f.store );
  wf2.setCatalog( &s.f.catalog, s.f.dir.path() );
  wf2.setSitingStore( &s.store );
  QList<SitingConstraintGeometry> constraints;
  SitingConstraintGeometry boundary;
  boundary.type = QStringLiteral( "polygon" );
  boundary.wkt = QStringLiteral( "POLYGON((0 0, 10000 0, 10000 10000, 0 10000))" );
  constraints.append( boundary );
  SitingConstraintGeometry line;
  line.type = QStringLiteral( "line" );
  line.wkt = QStringLiteral( "LINESTRING(8000 10000, 10000 8000)" );
  constraints.append( line );
  wf2.setConstraintProvider( [constraints] { return constraints; } );
  WellSitingParams tight = params;
  tight.lineBuffer = 1500;
  QVERIFY( wf2.generateCandidates( tight, &error ) );
  QCOMPARE( wf2.lastCandidates().size(), 0 );

  // SitingStack 默认带工区边界约束：诊断域来源如实注记。
  QVERIFY( s.wf.runDiagnosis( params, &error ) );
  QCOMPARE( s.wf.lastDiagnosisSummary().value( QStringLiteral( "domain_source" ) ).toString(),
            QStringLiteral( "工区边界约束" ) );
}

void TestWellSitingWorkflow::evaluationImprovement()
{
  SitingStack s;
  QString error;
  const QString plannedId = s.wf.addPlannedWell( QStringLiteral( "P1" ), 9000, 9000, &error );
  QVERIFY( !plannedId.isEmpty() );
  const WellSitingParams params = holeParams();

  const QVariantMap evaluation = s.wf.evaluateScenario( params, { plannedId }, &error );
  QVERIFY( !evaluation.isEmpty() );
  const QVariantMap before = evaluation.value( QStringLiteral( "before" ) ).toMap();
  const QVariantMap after = evaluation.value( QStringLiteral( "after" ) ).toMap();

  // 方向：补井后空洞清零、覆盖率升（合成夹具解析断言）。
  QCOMPARE( before.value( QStringLiteral( "hole_count" ) ).toInt(), 1 );
  QVERIFY( before.value( QStringLiteral( "hole_area_total" ) ).toDouble() > 2.0e6 );
  QCOMPARE( after.value( QStringLiteral( "hole_count" ) ).toInt(), 0 );
  QCOMPARE( after.value( QStringLiteral( "hole_area_total" ) ).toDouble(), 0.0 );
  QVERIFY( after.value( QStringLiteral( "coverage_ratio" ) ).toDouble() >
           before.value( QStringLiteral( "coverage_ratio" ) ).toDouble() );

  // 幅度：贡献分与「基线 − 方案」同栅格口径一致（逐井贡献字段在）。
  const QVariantMap contributions = evaluation.value( QStringLiteral( "contributions" ) ).toMap();
  QVERIFY( contributions.contains( plannedId ) );
  QCOMPARE( contributions.value( plannedId ).toMap()
                .value( QStringLiteral( "hole_area_m2" ) )
                .toDouble(),
            before.value( QStringLiteral( "hole_area_total" ) ).toDouble() -
                after.value( QStringLiteral( "hole_area_total" ) ).toDouble() );
  // 部署假设如实入注记。
  QVERIFY( after.value( QStringLiteral( "note" ) ).toString().contains( QStringLiteral( "候选井" ) ) );

  // 悬空 id 如实记录，不虚造点位。
  const QVariantMap dangling = s.wf.evaluateScenario( params, { QStringLiteral( "planned-777" ) },
                                                     &error );
  QCOMPARE( dangling.value( QStringLiteral( "missing_planned_ids" ) ).toStringList(),
            QStringList{ QStringLiteral( "planned-777" ) } );
}

// ---------------------------------------------------------------------------
// Oracle 4：方案保存-重开一致
// ---------------------------------------------------------------------------
void TestWellSitingWorkflow::scenarioPersistenceReopen()
{
  // 目录活到函数尾（QTemporaryDir 析构即删——首段作用域结束不能带走它）；
  // catalog 用 new/delete 模拟「保存-关工程-重开」的进程边界。
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString metaPath = dir.filePath( QStringLiteral( "project.sqlite" ) );
  QString p1, p2;
  {
    auto *catalog = new DataCatalog();
    QVERIFY( catalog->open( dir.path() ) );
    WellSitingStore store( metaPath );
    QString error;
    QVERIFY( store.open( &error ) );
    WellSitingWorkflow wf( nullptr, nullptr ); // 无图层面：评估/方案不落图
    wf.setCatalog( catalog, dir.path() );
    wf.setSitingStore( &store );
    wf.setConstraintProvider( [] { return boundaryConstraints(); } );
    addRealWells( *catalog );
    p1 = wf.addPlannedWell( QStringLiteral( "替补A" ), 9000, 9000, &error );
    p2 = wf.addPlannedWell( QStringLiteral( "替补B" ), 9500, 8800, &error );
    QVERIFY( !p1.isEmpty() && !p2.isEmpty() );
    QVERIFY( wf.renamePlannedWell( p2, QStringLiteral( "替补B改" ), &error ) );
    const WellSitingParams params = holeParams();
    QVERIFY( wf.saveScenario( QStringLiteral( "加密东北" ), { p1, p2 }, params, &error ) );
    QVERIFY( wf.saveScenario( QStringLiteral( "只加一口" ), { p1 }, params, &error ) );
    QCOMPARE( wf.scenarios().size(), 2 );
    delete catalog; // 关工程：落盘并释放 sqlite 句柄
  }

  // 重开：同一目录上的新 catalog（从 sqlite 装载）+ 新 store + 新 wf。
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );
  WellSitingStore store( metaPath );
  QString error;
  QVERIFY( store.open( &error ) );
  WellSitingWorkflow wf( nullptr, nullptr );
  wf.setCatalog( &catalog, dir.path() );
  wf.setSitingStore( &store );
  wf.setConstraintProvider( [] { return boundaryConstraints(); } );

  const QList<QVariantMap> scenarios = wf.scenarios();
  QCOMPARE( scenarios.size(), 2 );
  const QVariantMap first = scenarios.at( 0 );
  QCOMPARE( first.value( QStringLiteral( "name" ) ).toString(), QStringLiteral( "加密东北" ) );
  QCOMPARE( first.value( QStringLiteral( "well_count" ) ).toInt(), 2 );
  QCOMPARE( first.value( QStringLiteral( "planned_well_ids" ) ).toStringList().size(), 2 );

  // 指标快照重开一致（点位坐标从 catalog 实体重开装载，指标从快照读回）。
  const QVariantMap before = first.value( QStringLiteral( "metrics_before" ) ).toMap();
  const QVariantMap after = first.value( QStringLiteral( "metrics_after" ) ).toMap();
  QCOMPARE( before.value( QStringLiteral( "hole_count" ) ).toInt(), 1 );
  QVERIFY( before.value( QStringLiteral( "hole_area_total" ) ).toDouble() > 2.0e6 );
  QCOMPARE( after.value( QStringLiteral( "hole_count" ) ).toInt(), 0 );
  QCOMPARE( after.value( QStringLiteral( "hole_area_total" ) ).toDouble(), 0.0 );
  QCOMPARE( first.value( QStringLiteral( "contributions" ) ).toMap().size(), 2 );

  // 计划井可见性面重开一致：改名仍在，坐标重开可解析。
  const QList<QVariantMap> planned = wf.plannedWells();
  QCOMPARE( planned.size(), 2 );
  QSet<QString> names;
  for ( const QVariantMap &w : planned )
    names.insert( w.value( QStringLiteral( "name" ) ).toString() );
  QVERIFY( names.contains( QStringLiteral( "替补A" ) ) );
  QVERIFY( names.contains( QStringLiteral( "替补B改" ) ) );

  // 删除方案后清单落库（再重开仍一致）。
  QVERIFY( wf.deleteScenario( first.value( QStringLiteral( "id" ) ).toString(), &error ) );
  QCOMPARE( wf.scenarios().size(), 1 );
  WellSitingStore reopen( metaPath );
  QVERIFY( reopen.open( &error ) );
  paleo::siting::ScenarioSet set;
  QVERIFY( reopen.load( set, &error ) );
  QCOMPARE( set.scenarios.size(), 1 );
}

void TestWellSitingWorkflow::exports()
{
  SitingStack s;
  QString error;
  // CSV：表头 + 点位行（名称/坐标/贡献）；方案名/井名带逗号 → RFC4180
  // 引号转义。
  const QString trickyName = QStringLiteral( "加密, 东北（v2）" );
  const QString trickyId =
      s.wf.addPlannedWell( QStringLiteral( "替补, 带逗号" ), 9000, 9000, &error );
  QVERIFY( !trickyId.isEmpty() );
  QVERIFY( s.wf.saveScenario( trickyName, { trickyId }, holeParams(), &error ) );
  QString scenarioId;
  for ( const QVariantMap &sc : s.wf.scenarios() )
    if ( sc.value( QStringLiteral( "name" ) ).toString() == trickyName )
      scenarioId = sc.value( QStringLiteral( "id" ) ).toString();
  QVERIFY( !scenarioId.isEmpty() );

  QVERIFY( QDir().mkpath( s.f.dir.filePath( QStringLiteral( "export" ) ) ) );
  const QString csvPath = s.f.dir.filePath( QStringLiteral( "export/scenario.csv" ) );
  QVERIFY( s.wf.exportScenarioCsv( scenarioId, csvPath, &error ) );
  QFile csv( csvPath );
  QVERIFY( csv.open( QIODevice::ReadOnly ) );
  const QString content = QString::fromUtf8( csv.readAll() );
  QVERIFY( content.startsWith( QStringLiteral( "scenario_name,well_id,well_name" ) ) );
  QVERIFY( content.contains( QStringLiteral( "\"%1\"" ).arg( trickyName ) ) );
  QVERIFY( content.contains( QStringLiteral( "\"替补, 带逗号\"" ) ) );
  // 内嵌引号翻倍：井名带英文引号 → "…""quoted""…"。
  const QString quotedName = QStringLiteral( "计划井 \"Q1\"" );
  const QString quotedId = s.wf.addPlannedWell( quotedName, 9100, 9100, &error );
  QVERIFY( !quotedId.isEmpty() );
  QVERIFY( s.wf.saveScenario( QStringLiteral( "引号方案" ), { quotedId }, holeParams(),
                              &error ) );
  QString quotedScenarioId;
  for ( const QVariantMap &sc : s.wf.scenarios() )
    if ( sc.value( QStringLiteral( "name" ) ).toString() == QStringLiteral( "引号方案" ) )
      quotedScenarioId = sc.value( QStringLiteral( "id" ) ).toString();
  const QString quotedPath = s.f.dir.filePath( QStringLiteral( "export/quoted.csv" ) );
  QVERIFY( s.wf.exportScenarioCsv( quotedScenarioId, quotedPath, &error ) );
  QFile quoted( quotedPath );
  QVERIFY( quoted.open( QIODevice::ReadOnly ) );
  const QString quotedContent = QString::fromUtf8( quoted.readAll() );
  QVERIFY( quotedContent.contains( QStringLiteral( "计划井 \"\"Q1\"\"" ) ) );
  quoted.close();
  QVERIFY( content.contains( QStringLiteral( "9000.0" ) ) );
  csv.close();

  // 对比图 PNG：文件存在 + PNG 魔数。
  const QString chartPath = s.f.dir.filePath( QStringLiteral( "export/compare.png" ) );
  QVERIFY( s.wf.exportComparisonChart( chartPath, &error ) );
  QFile chart( chartPath );
  QVERIFY( chart.open( QIODevice::ReadOnly ) );
  const QByteArray magic = chart.read( 8 );
  QCOMPARE( magic.left( 4 ), QByteArray::fromHex( "89504e47" ) );
  chart.close();

  // 无方案时图表如实拒绝。
  WellSitingWorkflow emptyWf( nullptr, nullptr );
  QVERIFY( !emptyWf.exportComparisonChart( chartPath, &error ) );
}

void TestWellSitingWorkflow::cleanupTestCase()
{
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH",
                                           QStringLiteral( "/usr" ) ),
                     true );
  app.initQgis();
  QgsApplication::processingRegistry();
  TestWellSitingWorkflow tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_wellsiting_workflow.moc"
