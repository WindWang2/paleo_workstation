// 层：测试壳（被测对象：paleo_workflow FaciesMappingWorkflow 真实栈——
// catalog 版本/SHA/declare/organizer 落组/权重持久化/JobRunner 三段式；
// goal/facies-automapping oracle 4）
#include "helpers/workflowfixture.h"

#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerorganizer.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/services/jobrunner.h"
#include "../src/services/paleotaskservice.h"
#include "../src/workflow/faciesmappingworkflow.h"

#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

#include <qgsapplication.h>
#include <qgslayertree.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

#include <gdal.h>
#include <ogr_api.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <functional>

// 真实栈模式（tst_factorworkflow 同款）：临时 manifest+catalog+QGIS 引导。
// 覆盖 oracle 4：草稿相图资产 catalog 版本/SHA 正常、organizer 落正确层位组；
// 以及阶段3 权重持久化进版本 extra（job params）、QA 报告资产、失败路径
// 不产游离图层。

using paleo::tests::initFixture;
using paleo::tests::derivedVersionRegistered;

namespace
{

paleo::singlefactor::Polygon rect( double x0, double y0, double x1, double y1 )
{
  paleo::singlefactor::Polygon poly;
  poly.exterior.points = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
  return poly;
}

// 一口优势相明确的井（覆盖率 1.0，code 唯一）。
paleo::faciesmapping::WellFaciesColumn wellColumn( const char *id, double x, double y,
                                                   int code )
{
  paleo::faciesmapping::WellFaciesColumn column;
  column.wellId = id;
  column.x = x;
  column.y = y;
  column.top = 1000;
  column.bottom = 1100;
  column.intervals = { { 1000, 1100, code, 1.0 } };
  return column;
}

// 标准请求：域 10×10，中缝等值线 x=5，东西两约束区，两口井分列两侧。
FaciesMappingWorkflow::DraftFaciesRequest standardRequest()
{
  using Request = FaciesMappingWorkflow::DraftFaciesRequest;
  Request request;
  request.horizon = QStringLiteral( "T1" );

  request.domain = { rect( 0, 0, 10, 10 ) };
  paleo::singlefactor::ContourLevelLines level;
  level.level = 5.0;
  level.lines = { { { { 5, 0 }, { 5, 10 } } } };
  request.contours = { level };

  FaciesMappingWorkflow::ZoneInput west;
  west.id = QStringLiteral( "zone_west" );
  west.faciesCode = 10;
  west.polygons = { rect( 0, 0, 5, 10 ) };
  FaciesMappingWorkflow::ZoneInput east;
  east.id = QStringLiteral( "zone_east" );
  east.faciesCode = 20;
  east.polygons = { rect( 5, 0, 10, 10 ) };
  request.zones = { west, east };

  request.wellColumns = { wellColumn( "W1", 2, 5, 10 ), wellColumn( "W2", 8, 5, 20 ) };

  FaciesMappingWorkflow::HardLineInput fault;
  fault.id = QStringLiteral( "fault_1" );
  fault.points = { { 50, -50 }, { 50, 50 } }; // 区外平行线：不构成冲突
  request.hardLines = { fault };

  request.wellWeight = 1.0;
  request.factorWeight = 1.0;
  request.predictionWeight = 1.0;
  request.assignThreshold = 0.5;
  return request;
}

// 按值取资产最新版本（避免共享 scratch 的指针混叠）；找不到返回 false。
bool versionOfAsset( DataCatalog &catalog, const QString &projectDir, const QString &assetType,
                     CatalogVersion *outVersion = nullptr, QString *outPath = nullptr )
{
  for ( const CatalogAsset &a : catalog.assets() )
  {
    if ( a.type != assetType )
      continue;
    const QVector<CatalogVersion> versions = catalog.versionsForAsset( a.id );
    if ( versions.isEmpty() )
      continue;
    if ( outVersion )
      *outVersion = versions.back();
    if ( outPath )
      *outPath = DataCatalog::resolvedVersionPath( projectDir, versions.back() );
    return true;
  }
  return false;
}

// OGR 读 GPKG 要素的 facies_code 列（按 region_id 序）。
QVector<int> faciesCodesInGpkg( const QString &path )
{
  QVector<int> codes;
  GDALDatasetH ds = GDALOpenEx( path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr,
                                nullptr, nullptr );
  if ( !ds )
    return codes;
  OGRLayerH layer = GDALDatasetGetLayerByName( ds, "facies_draft" );
  if ( layer )
  {
    OGR_L_ResetReading( layer );
    while ( true )
    {
      OGRFeatureH feature = OGR_L_GetNextFeature( layer );
      if ( !feature )
        break;
      codes.append( OGR_F_GetFieldAsInteger(
        feature, OGR_F_GetFieldIndex( feature, "facies_code" ) ) );
      OGR_F_Destroy( feature );
    }
  }
  GDALClose( ds );
  return codes;
}

bool pumpUntil( const std::function<bool()> &pred, int timeoutMs = 30000 )
{
  QElapsedTimer clock;
  clock.start();
  while ( !pred() )
  {
    if ( clock.elapsed() > timeoutMs )
      return false;
    QCoreApplication::processEvents( QEventLoop::AllEvents, 10 );
    QThread::msleep( 2 );
  }
  return true;
}

} // namespace

class TestFaciesMappingWorkflow : public QObject
{
  Q_OBJECT

  private:
    using Fixture = paleo::tests::WorkflowFixture;

  private slots:
    void initTestCase()
    {
      QVERIFY( QgsApplication::instance() != nullptr );
    }

    void cleanup()
    {
      QgsProject::instance()->removeAllMapLayers();
    }

    void draftRegistersCatalogVersionsAndDeclaresLayer();
    void weightsPersistedInVersionExtra();
    void jobRunnerThreeStageCommits();
    void qaReportJsonReadable();
    void failurePathProducesNoSideEffects();

  private:
    const LayerDeclaration *findDecl( QgisLayerService &layers, const QString &layerId ) const
    {
      const QVector<LayerDeclaration> decls = layers.declared();
      for ( const LayerDeclaration &d : decls )
        if ( d.layerId == layerId )
          return new LayerDeclaration( d );
      return nullptr;
    }
};

// oracle 4 主链：catalog 版本/SHA + declare + organizer 落层位组。
void TestFaciesMappingWorkflow::draftRegistersCatalogVersionsAndDeclaresLayer()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  QgisLayerOrganizer organizer( &f.projectSvc, &f.layers );

  FaciesMappingWorkflow wf( &f.catalog, f.projectDir() );
  wf.attachLayerService( &f.layers );
  QSignalSpy draftSpy( &wf, &FaciesMappingWorkflow::draftReady );
  QSignalSpy qaSpy( &wf, &FaciesMappingWorkflow::qaReportReady );

  QString err;
  QVERIFY2( wf.run( standardRequest(), &err ), qPrintable( err ) );
  QCOMPARE( draftSpy.count(), 1 );
  QCOMPARE( qaSpy.count(), 1 );
  QCOMPARE( draftSpy.at( 0 ).at( 0 ).toString(), QStringLiteral( "T1" ) );
  QCOMPARE( draftSpy.at( 0 ).at( 1 ).toString(), QStringLiteral( "facies_draft.T1" ) );

  // catalog 双资产：版本行 + 非空 SHA + 受管路径。
  QString draftPath;
  CatalogVersion draftVersion;
  QVERIFY2( versionOfAsset( f.catalog, f.projectDir(), QStringLiteral( "facies_draft_map" ),
                            &draftVersion, &draftPath ),
            "草稿相图资产未登记" );
  QVERIFY2( derivedVersionRegistered( f.catalog, QStringLiteral( "facies_draft_map" ),
                                      draftPath ),
            "草稿相图 DERIVED 版本/SHA 不完整" );
  QString reportPath;
  CatalogVersion reportVersion;
  QVERIFY2( versionOfAsset( f.catalog, f.projectDir(), QStringLiteral( "facies_qa_report" ),
                            &reportVersion, &reportPath ),
            "QA 报告资产未登记" );
  QVERIFY( QFile::exists( reportPath ) );
  QVERIFY2( derivedVersionRegistered( f.catalog, QStringLiteral( "facies_qa_report" ),
                                      reportPath ),
            "QA 报告 DERIVED 版本/SHA 不完整" );
  // 报告父版本 = 草稿版本（provenance 链）。
  QVERIFY2( reportVersion.parentVersionIds.contains( draftVersion.id ),
            "QA 报告未挂草稿版本为父版本" );

  // declare：正常图层管线，05_PaleoMap 组 + 层位归属。
  const LayerDeclaration *decl = findDecl( f.layers, QStringLiteral( "facies_draft.T1" ) );
  QVERIFY2( decl, "facies_draft.T1 声明缺失" );
  QCOMPARE( decl->type, QStringLiteral( "vector" ) );
  QCOMPARE( decl->group, QStringLiteral( "05_PaleoMap" ) );
  QCOMPARE( decl->horizon, QStringLiteral( "T1" ) );
  QCOMPARE( decl->source.section( QLatin1Char( '|' ), 0, 0 ), draftPath );
  delete decl;

  // organizer：实例化后图层落 paleoHorizon=T1 组。
  QString instErr;
  QgsMapLayer *layer = f.layers.instantiate( QStringLiteral( "facies_draft.T1" ), &instErr );
  QVERIFY2( layer, qPrintable( instErr ) );
  QgsLayerTreeGroup *horizonGroup = nullptr;
  for ( QgsLayerTreeNode *node : f.projectSvc.project()->layerTreeRoot()->children() )
  {
    if ( node->customProperty( QStringLiteral( "paleoHorizon" ) ).toString() ==
           QStringLiteral( "T1" ) )
    {
      horizonGroup = qobject_cast<QgsLayerTreeGroup *>( node );
      break;
    }
  }
  QVERIFY2( horizonGroup, "未找到 paleoHorizon=T1 层位组" );
  bool inGroup = false;
  for ( QgsLayerTreeNode *child : horizonGroup->children() )
  {
    if ( auto *leaf = qobject_cast<QgsLayerTreeLayer *>( child );
         leaf && leaf->layer() == layer )
      inGroup = true;
  }
  QVERIFY2( inGroup, "草稿相图层未落层位组" );

  // GPKG 内容对账：东西两单元，合成后相码与井一致。
  const QVector<int> codes = faciesCodesInGpkg( draftPath );
  QCOMPARE( codes.size(), 2 );
  QVERIFY( codes.contains( 10 ) );
  QVERIFY( codes.contains( 20 ) );
}

// 阶段3：权重/阈值持久化进 catalog 版本 extra（job params）；同资产重复
// 运行产生递增版本（find-or-create 语义），extra 反映当次参数。
void TestFaciesMappingWorkflow::weightsPersistedInVersionExtra()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  FaciesMappingWorkflow wf( &f.catalog, f.projectDir() );
  wf.attachLayerService( &f.layers );

  // 附加因素证据源：东区两点投 code 30（得分 2.0）压过井票 20（1.0）→
  // 份额 2/3 过阈值，30 胜出；factorWeight=0.2 时 0.4 < 1.0 → 20 回归。
  FaciesMappingWorkflow::DraftFaciesRequest request = standardRequest();
  FaciesMappingWorkflow::SampleSourceInput factor;
  factor.id = QStringLiteral( "factor:sand" );
  factor.kind = QStringLiteral( "factor" );
  factor.weight = 1.0;
  factor.points = { { 8, 6, 30, 1.0 }, { 9, 7, 30, 1.0 } };
  request.extraSources = { factor };

  QString err;
  QVERIFY2( wf.run( request, &err ), qPrintable( err ) );
  QString draftPath;
  CatalogVersion v1;
  QVERIFY( versionOfAsset( f.catalog, f.projectDir(), QStringLiteral( "facies_draft_map" ),
                           &v1, &draftPath ) );
  QCOMPARE( v1.extra.value( QStringLiteral( "well_weight" ) ).toDouble(), 1.0 );
  QCOMPARE( v1.extra.value( QStringLiteral( "factor_weight" ) ).toDouble(), 1.0 );
  QCOMPARE( v1.extra.value( QStringLiteral( "assign_threshold" ) ).toDouble(), 0.5 );
  QVERIFY( !v1.extra.value( QStringLiteral( "param_hash" ) ).toString().isEmpty() );
  QCOMPARE( v1.extra.value( QStringLiteral( "semantic_profile" ) ).toString(),
            QStringLiteral( "facies_draft_mapping_v1" ) );
  QVector<int> codes = faciesCodesInGpkg( draftPath );
  QVERIFY( codes.contains( 30 ) ); // 因素票取胜

  // 权重翻转：factorWeight=0.2 → 因素票 0.2 < 井票 1.0 → 20 回归。
  request.factorWeight = 0.2;
  QVERIFY2( wf.run( request, &err ), qPrintable( err ) );
  for ( const CatalogAsset &a : f.catalog.assets() )
  {
    if ( a.type != QStringLiteral( "facies_draft_map" ) )
      continue;
    const QVector<CatalogVersion> versions = f.catalog.versionsForAsset( a.id );
    QCOMPARE( versions.size(), 2 ); // 同资产递增版本，不是新资产
    QCOMPARE( versions.back().extra.value( QStringLiteral( "factor_weight" ) ).toDouble(),
              0.2 );
  }
  const QString secondPath = [&f]() {
    QString path;
    versionOfAsset( f.catalog, f.projectDir(), QStringLiteral( "facies_draft_map" ), nullptr,
                    &path );
    return path;
  }();
  codes = faciesCodesInGpkg( secondPath );
  QVERIFY( codes.contains( 20 ) );
  QVERIFY( !codes.contains( 30 ) ); // 权重翻转改变了结论并如实落盘
}

// JobRunner 三段式：worker compute + owner commit，draftReady 经信号回壳。
void TestFaciesMappingWorkflow::jobRunnerThreeStageCommits()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  FaciesMappingWorkflow wf( &f.catalog, f.projectDir() );
  wf.attachLayerService( &f.layers );

  QObject dispatcher;
  PaleoTaskService svc( nullptr, &dispatcher );
  paleo::jobs::JobRunner<FaciesMappingWorkflow::DraftFaciesJob> runner( &dispatcher );
  runner.setTaskService( &svc );
  QSignalSpy draftSpy( &wf, &FaciesMappingWorkflow::draftReady );

  std::shared_ptr<FaciesMappingWorkflow::DraftFaciesJob> started;
  PaleoTask *task = wf.startJob( runner, standardRequest(), nullptr, &started );
  QVERIFY2( task, "startJob 应受理任务" );
  QVERIFY( started );
  QVERIFY2( pumpUntil( [&] { return draftSpy.count() > 0; } ), "三段式未走到 commit" );
  QVERIFY( !runner.busy() );
  QVERIFY( started->computed.ok );
  QVERIFY2( versionOfAsset( f.catalog, f.projectDir(), QStringLiteral( "facies_draft_map" ) ),
            "commit 段未登记资产" );
  svc.shutdown( 3000, false );
}

// QA 报告 JSON：结构可解析、诊断/参数指纹在场（报告可复现）。
void TestFaciesMappingWorkflow::qaReportJsonReadable()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  FaciesMappingWorkflow wf( &f.catalog, f.projectDir() );
  wf.attachLayerService( &f.layers );

  // 构造一个已知缺陷：孤岛阈值 25 → 东/西 50 面积不触发，但把最小面积
  // 抬高不会产生孤岛；改为直接给孤岛阈值 60 → 两个 50 单元都报孤岛。
  FaciesMappingWorkflow::DraftFaciesRequest request = standardRequest();
  request.minIslandArea = 60.0;

  QSignalSpy qaSpy( &wf, &FaciesMappingWorkflow::qaReportReady );
  QString err;
  QVERIFY2( wf.run( request, &err ), qPrintable( err ) );
  QCOMPARE( qaSpy.count(), 1 );
  QCOMPARE( qaSpy.at( 0 ).at( 1 ).toInt(), 2 ); // 两个单元各一条孤岛

  QString reportPath = qaSpy.at( 0 ).at( 0 ).toString();
  QFile file( reportPath );
  QVERIFY( file.open( QIODevice::ReadOnly ) );
  const QJsonObject report =
      QJsonDocument::fromJson( file.readAll() ).object();
  file.close();
  QCOMPARE( report.value( QStringLiteral( "horizon" ) ).toString(),
            QStringLiteral( "T1" ) );
  QVERIFY( report.contains( QStringLiteral( "param_hash" ) ) ||
           report.value( QStringLiteral( "params" ) )
               .toObject()
               .contains( QStringLiteral( "param_hash" ) ) );
  QCOMPARE( report.value( QStringLiteral( "issues" ) ).toArray().size(), 2 );
  const QJsonObject issue =
      report.value( QStringLiteral( "issues" ) ).toArray().at( 0 ).toObject();
  QCOMPARE( issue.value( QStringLiteral( "type" ) ).toString(),
            QStringLiteral( "small_island" ) );
  QVERIFY( issue.value( QStringLiteral( "region_ids" ) ).toArray().size() >= 1 );
  QVERIFY( issue.contains( QStringLiteral( "location" ) ) );
}

// 失败路径：缺层位 → 拒绝；不登记任何版本/声明（不产游离图层）。
void TestFaciesMappingWorkflow::failurePathProducesNoSideEffects()
{
  Fixture f;
  QVERIFY( initFixture( f ) );
  FaciesMappingWorkflow wf( &f.catalog, f.projectDir() );
  wf.attachLayerService( &f.layers );

  FaciesMappingWorkflow::DraftFaciesRequest request = standardRequest();
  request.horizon.clear();
  QString err;
  QVERIFY( !wf.run( request, &err ) );
  QVERIFY( !err.isEmpty() );

  FaciesMappingWorkflow::DraftFaciesRequest noGeometry = standardRequest();
  noGeometry.domain.clear();
  noGeometry.contours.clear();
  QVERIFY( !wf.run( noGeometry, &err ) );

  QVERIFY( f.catalog.assets().isEmpty() ); // 未登记任何资产
  QVERIFY( !findDecl( f.layers, QStringLiteral( "facies_draft.T1" ) ) );
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath(
    qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true );
  app.initQgis();
  QgsApplication::processingRegistry();
  GDALAllRegister();
  TestFaciesMappingWorkflow tc;
  QByteArray logPath = QByteArray( QT_TESTCASE_BUILDDIR ) + "/tst_faciesmapping_workflow-result.txt";
  QList<QByteArray> forwarded;
  forwarded << QByteArray( argv[0] );
  for ( int i = 1; i < argc; ++i )
    forwarded << QByteArray( argv[i] );
  forwarded << QByteArray( "-o" ) << logPath + ",txt";
  QList<char *> cargv;
  cargv.reserve( forwarded.size() );
  for ( QByteArray &a : forwarded )
    cargv << a.data();
  const int rc = QTest::qExec( &tc, cargv.size(), cargv.data() );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_faciesmapping_workflow.moc"
