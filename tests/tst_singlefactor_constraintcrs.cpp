// 层：测试壳
// P1-C：约束库几何类型 / CRS 契约。库层 "constraints" 建库即 wkbUnknown 且
// 无 SRS；绘制入库必须从画布 CRS 显式变换到本地工程网格，读出图层必须带
// 本地工程网格 CRS（配准工程经显式坐标操作显示），编辑会话换源不丢 CRS。
#include <QtTest>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <qgscategorizedsymbolrenderer.h>
#include <qgscoordinatetransform.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

#include "../src/catalog/datacatalog.h"
#include "../src/io/constraintstore.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectfile.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/constraintcrs.h"
#include "../src/qgis/projectmapreference.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgiseditingservice.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgisstyleservice.h"
#include "../src/ui/typedconstraintdrawcontroller.h"
#include "../src/workflow/workflows.h"

namespace
{

PaleoGeoreference georeference()
{
  // 与 tst_projectsvc::mapreferenceConfigureRegistersPipeline 同一组实测参数。
  PaleoGeoreference g;
  g.kind = QStringLiteral( "similarity2d" );
  g.targetCrs = QStringLiteral( "EPSG:4326" );
  g.anchorLonDeg = 108.05;
  g.anchorLatDeg = 36.10;
  g.metersPerDegLon = 90049.687955;
  g.metersPerDegLat = 110960.830261;
  g.a = 1.000896238;
  g.b = 0.030275703;
  g.tE = -6182.244744;
  g.tN = -9871.264523;
  return g;
}

struct Stack
{
  PaleoProjectStore store; // 真 GPKG 约束库（不是内存回退）
  QgisProjectService projectSvc;
  std::unique_ptr<LayerManifest> manifest;
  std::unique_ptr<QgisLayerService> layerSvc;
  std::unique_ptr<ConstraintWorkflow> wf;
  std::unique_ptr<QgisEditingService> edits; // P3 (a)：绘制自动开会话（产品同此装配）
  QgisCanvasController canvasCtl;
  std::unique_ptr<TypedConstraintDrawController> ctl;
};

std::unique_ptr<Stack> makeStack( const QString &dir )
{
  QDir().mkpath( dir );
  auto s = std::make_unique<Stack>();
  if ( !s->projectSvc.createProject( QDir( dir ).filePath( QStringLiteral( "proj.qgz" ) ) ) )
    return nullptr;
  s->manifest = std::make_unique<LayerManifest>( QDir( dir ).filePath( QStringLiteral( "m.sqlite" ) ) );
  if ( !s->manifest->open() )
    return nullptr;
  s->layerSvc = std::make_unique<QgisLayerService>( &s->projectSvc, s->manifest.get() );
  s->store.setProjectPaths( QDir( dir ).filePath( QStringLiteral( "proj.qgz" ) ),
                            QDir( dir ).filePath( QStringLiteral( "project.gpkg" ) ),
                            QDir( dir ).filePath( QStringLiteral( "meta.sqlite" ) ) );
  s->wf = std::make_unique<ConstraintWorkflow>( nullptr, s->layerSvc.get() );
  s->wf->setStore( &s->store );
  s->edits = std::make_unique<QgisEditingService>( &s->store );
  s->wf->setEditingService( s->edits.get() );
  s->ctl = std::make_unique<TypedConstraintDrawController>( &s->canvasCtl, s->wf.get() );
  return s;
}

QgsPointXY firstVertex( const QString &wkt )
{
  const QgsGeometry g = QgsGeometry::fromWkt( wkt );
  return QgsPointXY( g.vertexAt( 0 ) );
}

} // namespace

class TestConstraintCrs : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase() { QVERIFY( QgisRuntime::isInitialized() ); }

  void toStoreWktContract()
  {
    QString out, error;
    // 已是库 CRS（同一 WKT）：identity，坐标逐字不动。
    QVERIFY( paleo::constraintcrs::toStoreWkt( QStringLiteral( "LineString (1 2, 3 4)" ),
                                               paleo::constraintcrs::storeCrs(), {}, &out, &error ) );
    QCOMPARE( out, QStringLiteral( "LineString (1 2, 3 4)" ) );
    // 另一写法构造的同一本地网格（同 WKT 字符串）仍是 identity。
    const QgsCoordinateReferenceSystem sameGrid = QgsCoordinateReferenceSystem::fromWkt(
        DataCatalog::localGridCrsWkt() );
    QVERIFY( paleo::constraintcrs::toStoreWkt( QStringLiteral( "LineString (1 2, 3 4)" ), sameGrid, {}, &out,
                                               &error ) );
    QCOMPARE( out, QStringLiteral( "LineString (1 2, 3 4)" ) );
  }

  // P0-1：无关工程 CRS（米制、Engineering）不再是库网格等价——原点不同即拒绝，
  // 不得按「单位都是米」静默 identity 写入。
  void unrelatedEngineeringCrsIsNotStoreEquivalent()
  {
    const QgsCoordinateReferenceSystem shiftedOrigin = QgsCoordinateReferenceSystem::fromWkt( QStringLiteral(
        "ENGCRS[\"shifted local grid\",EDATUM[\"Other datum\"],CS[Cartesian,2],"
        "AXIS[\"Easting (E)\",east,ORDER[1],LENGTHUNIT[\"metre\",1]],"
        "AXIS[\"Northing (N)\",north,ORDER[2],LENGTHUNIT[\"metre\",1]]]" ) );
    QVERIFY( shiftedOrigin.isValid() );
    QCOMPARE( shiftedOrigin.type(), Qgis::CrsType::Engineering );
    QCOMPARE( shiftedOrigin.mapUnits(), Qgis::DistanceUnit::Meters );
    QVERIFY( shiftedOrigin != paleo::constraintcrs::storeCrs() );
    QString out, error;
    QVERIFY( !paleo::constraintcrs::toStoreWkt( QStringLiteral( "LineString (5 6, 7 8)" ), shiftedOrigin, {},
                                                &out, &error ) );
    QVERIFY2( !error.isEmpty(), "refusal must carry a reason" );

    // 轴向不同（N/E 互换）：同为米制 Engineering 仍拒绝。
    const QgsCoordinateReferenceSystem swappedAxes = QgsCoordinateReferenceSystem::fromWkt( QStringLiteral(
        "ENGCRS[\"swapped axes grid\",EDATUM[\"Local engineering datum\"],CS[Cartesian,2],"
        "AXIS[\"Northing (N)\",north,ORDER[1],LENGTHUNIT[\"metre\",1]],"
        "AXIS[\"Easting (E)\",east,ORDER[2],LENGTHUNIT[\"metre\",1]]]" ) );
    QVERIFY( swappedAxes.isValid() );
    QVERIFY( !paleo::constraintcrs::toStoreWkt( QStringLiteral( "LineString (5 6, 7 8)" ), swappedAxes, {}, &out,
                                                &error ) );

    // 旧式 LOCAL_CS（米制）同样不再视为同一网格。
    const QgsCoordinateReferenceSystem localCs = QgsCoordinateReferenceSystem::fromWkt( QStringLiteral(
        "LOCAL_CS[\"other local grid\",LOCAL_DATUM[\"Arbitrary\",0],UNIT[\"metre\",1],"
        "AXIS[\"Easting\",EAST],AXIS[\"Northing\",NORTH]]" ) );
    QVERIFY( localCs.isValid() );
    QVERIFY( !paleo::constraintcrs::toStoreWkt( QStringLiteral( "LineString (5 6, 7 8)" ), localCs, {}, &out,
                                                &error ) );
  }

  // 来源 CRS 无效：坐标含义无法核实 → 拒绝（不静默指定本地 CRS）。
  void unknownSourceCrsRequiresVerifiedContext()
  {
    QString out, error;
    QVERIFY( !paleo::constraintcrs::toStoreWkt( QStringLiteral( "LineString (1 2, 3 4)" ),
                                                 QgsCoordinateReferenceSystem(), {}, &out, &error ) );
    QVERIFY2( !error.isEmpty(), "invalid source CRS must be refused with a reason" );
  }

  // 有显式坐标操作（配准工程）：EPSG:3857 → 本地网格真实变换。
  void explicitTransformIsApplied()
  {
    QgsProject reference;
    QString error;
    QVERIFY2( paleo::mapreference::configure( &reference, georeference(), QStringLiteral( "EPSG:3857" ), &error ),
              qPrintable( error ) );
    const QgsCoordinateReferenceSystem web( QStringLiteral( "EPSG:3857" ) );
    const QgsPointXY local( 3720.83, 3899.60 );
    const QgsCoordinateTransform toWeb( paleo::constraintcrs::storeCrs(), web, reference.transformContext() );
    QVERIFY( toWeb.isValid() );
    const QgsPointXY w = toWeb.transform( local );
    QString out;
    QVERIFY( paleo::constraintcrs::toStoreWkt(
        QStringLiteral( "LineString (%1 %2, %3 %4)" )
            .arg( QString::number( w.x(), 'f', 6 ) )
            .arg( QString::number( w.y(), 'f', 6 ) )
            .arg( QString::number( w.x() + 1000, 'f', 6 ) )
            .arg( QString::number( w.y() + 500, 'f', 6 ) ),
        web, reference.transformContext(), &out, &error ) );
    const QgsPointXY stored = firstVertex( out );
    QVERIFY2( stored.distance( local ) < 1e-3,
              qPrintable( QStringLiteral( "stored %1,%2 vs local %3,%4" ).arg( stored.x() ).arg( stored.y() ).arg(
                              local.x() ).arg( local.y() ) ) );

    // 往返：库坐标 → 3857 → 回库，坐标在亚毫米内复原。
    const QgsCoordinateTransform back( web, paleo::constraintcrs::storeCrs(), reference.transformContext() );
    QString roundWkt;
    QVERIFY( paleo::constraintcrs::toStoreWkt( out, paleo::constraintcrs::storeCrs(), {}, &roundWkt, &error ) );
    const QgsPointXY roundTrip = back.transform( firstVertex( roundWkt ) );
    // 先把库里存的本地点变到 web 再回本地，验证数值往返稳定。
    const QgsPointXY viaWeb = toWeb.transform( QgsPointXY( firstVertex( roundWkt ) ) );
    const QgsPointXY backAgain = back.transform( viaWeb );
    QVERIFY2( backAgain.distance( local ) < 1e-6, "coordinate round trip must be numerically stable" );
  }

  // 未配准工程：EPSG:3857 与 ENGCRS 之间没有坐标操作 → 如实拒绝，不按原坐标写入。
  void missingTransformRefusesWrite()
  {
    QString out, error;
    QVERIFY( !paleo::constraintcrs::toStoreWkt( QStringLiteral( "LineString (1 2, 3 4)" ),
                                                 QgsCoordinateReferenceSystem( QStringLiteral( "EPSG:3857" ) ), {},
                                                 &out, &error ) );
    QVERIFY2( !error.isEmpty(), "missing operation must refuse with a reason" );
  }

  // 库层几何类型与 CRS 的实测口径 + 实例化补 CRS + 样式。
  void storeLayerGetsLocalGridCrs()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    auto s = makeStack( dir.filePath( QStringLiteral( "p" ) ) );
    QVERIFY( s );
    s->ctl->startCapture( QStringLiteral( "T1" ), QStringLiteral( "line" ), QStringLiteral( "direction_line" ), 0 );
    s->ctl->onDrawn( QStringLiteral( "LineString (0 0, 10 10)" ) );
    ConstraintStore *store = s->wf->constraintStore();
    QVERIFY( store );

    // 原始 OGR 打开：库层无 SRS（实测记录，不依赖它做判断）。
    QgsVectorLayer raw( store->gpkgPath() + QStringLiteral( "|layername=constraints" ), QStringLiteral( "raw" ),
                        QStringLiteral( "ogr" ) );
    QVERIFY( raw.isValid() );
    qInfo() << "raw store layer crs valid:" << raw.crs().isValid()
            << "wkbType:" << QgsWkbTypes::displayString( raw.wkbType() )
            << "geometryType:" << static_cast<int>( raw.geometryType() );

    QString err;
    auto *layer = qobject_cast<QgsVectorLayer *>( s->layerSvc->instantiate( QStringLiteral( "constraints.T1" ), &err ) );
    QVERIFY2( layer, qPrintable( err ) );
    QVERIFY( layer->crs().isValid() );
    QCOMPARE( layer->crs(), paleo::constraintcrs::storeCrs() );
    // 已有有效 CRS 不覆盖。
    QVERIFY( !paleo::constraintcrs::assignStoreCrsIfMissing( layer ) );
  }

  // 配准工程：画布 = EPSG:3857 + 显式坐标操作。绘制入库存本地坐标，重开后
  // 图层按同一操作显示回原画布位置。
  void georeferencedCanvasDrawStoresLocalCoordinates()
  {
    QgsProject reference;
    QString error;
    QVERIFY2( paleo::mapreference::configure( &reference, georeference(), QStringLiteral( "EPSG:3857" ), &error ),
              qPrintable( error ) );
    const QgsCoordinateReferenceSystem local = paleo::constraintcrs::storeCrs();
    const QgsCoordinateReferenceSystem web( QStringLiteral( "EPSG:3857" ) );
    const QgsCoordinateTransform toWeb( local, web, reference.transformContext() );
    QVERIFY( toWeb.isValid() );
    const QgsPointXY a( 3720.83, 3899.60 ), b( 4720.83, 4399.60 );
    const QgsPointXY wa = toWeb.transform( a ), wb = toWeb.transform( b );

    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    auto s = makeStack( dir.filePath( QStringLiteral( "p" ) ) );
    QVERIFY( s );
    QgsMapCanvas *canvas = s->canvasCtl.canvas();
    canvas->mapSettings().setTransformContext( reference.transformContext() );
    canvas->setDestinationCrs( web );
    QSignalSpy failed( s->ctl.get(), &TypedConstraintDrawController::captureFailed );
    s->ctl->startCapture( QStringLiteral( "T1" ), QStringLiteral( "line" ), QStringLiteral( "break_line" ), 0 );
    s->ctl->onDrawn( QStringLiteral( "LineString (%1 %2, %3 %4)" )
                         .arg( wa.x(), 0, 'f', 6 )
                         .arg( wa.y(), 0, 'f', 6 )
                         .arg( wb.x(), 0, 'f', 6 )
                         .arg( wb.y(), 0, 'f', 6 ) );
    QVERIFY2( failed.isEmpty(), failed.isEmpty() ? "" : qPrintable( failed.first().first().toString() ) );
    const QVector<QVariantMap> rows = s->wf->loadConstraints( QStringLiteral( "T1" ) );
    QCOMPARE( rows.size(), 1 );
    const QgsPointXY stored = firstVertex( rows.first().value( QStringLiteral( "wkt" ) ).toString() );
    QVERIFY2( stored.distance( a ) < 1e-3,
              qPrintable( QStringLiteral( "stored %1,%2 vs local %3,%4" )
                              .arg( stored.x() ).arg( stored.y() ).arg( a.x() ).arg( a.y() ) ) );

    // 重开：图层 CRS = 本地网格，按画布上下文回到 3857 原位。
    QString err;
    auto *layer = qobject_cast<QgsVectorLayer *>( s->layerSvc->instantiate( QStringLiteral( "constraints.T1" ), &err ) );
    QVERIFY2( layer, qPrintable( err ) );
    QCOMPARE( layer->crs(), local );
    QgsFeature feature;
    QVERIFY( layer->getFeatures().nextFeature( feature ) );
    const QgsCoordinateTransform toCanvas( layer->crs(), canvas->mapSettings().destinationCrs(),
                                           canvas->mapSettings().transformContext() );
    const QgsPointXY shown = toCanvas.transform( QgsPointXY( feature.geometry().vertexAt( 0 ) ) );
    QVERIFY2( shown.distance( wa ) < 1e-2, qPrintable( QStringLiteral( "%1,%2" ).arg( shown.x() ).arg( shown.y() ) ) );
  }

  // 非配准工程却给了外部 CRS 画布：拒绝入库（不静默混 CRS）。
  void foreignCanvasWithoutOperationRefuses()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    auto s = makeStack( dir.filePath( QStringLiteral( "p" ) ) );
    QVERIFY( s );
    QgsMapCanvas *canvas = s->canvasCtl.canvas();
    canvas->mapSettings().setTransformContext( QgsCoordinateTransformContext() );
    canvas->setDestinationCrs( QgsCoordinateReferenceSystem( QStringLiteral( "EPSG:3857" ) ) );
    QSignalSpy failed( s->ctl.get(), &TypedConstraintDrawController::captureFailed );
    s->ctl->startCapture( QStringLiteral( "T1" ), QStringLiteral( "line" ), QStringLiteral( "break_line" ), 0 );
    s->ctl->onDrawn( QStringLiteral( "LineString (0 0, 10 10)" ) );
    QCOMPARE( failed.count(), 1 );
    QVERIFY( !s->ctl->active() );
    QVERIFY( s->wf->loadConstraints( QStringLiteral( "T1" ) ).isEmpty() );
  }

  // 编辑会话换到工作副本再换回：CRS 不丢。
  void editSessionKeepsCrs()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    PaleoProjectStore project;
    ConstraintStore store( dir.filePath( QStringLiteral( "project.gpkg" ) ), &project );
    QgisEditingService edits( &project );
    project.setProjectPaths( dir.filePath( QStringLiteral( "p.qgz" ) ), store.gpkgPath(),
                             dir.filePath( QStringLiteral( "meta.sqlite" ) ) );
    QString error;
    QVERIFY2( store.appendExtended( QStringLiteral( "T1" ), QStringLiteral( "c-1" ),
                                    QStringLiteral( "LINESTRING(0 0, 10 0, 20 0)" ), QStringLiteral( "direction_line" ),
                                    7, QStringLiteral( "{\"semantic\":\"direction_guide\",\"ratio\":8}" ), 1, &error ),
              qPrintable( error ) );
    QgsVectorLayer layer( store.gpkgPath() + QStringLiteral( "|layername=constraints" ), QStringLiteral( "constraints" ),
                          QStringLiteral( "ogr" ) );
    layer.setSubsetString( QStringLiteral( "horizon='T1'" ) );
    layer.setCustomProperty( QStringLiteral( "paleoLayerId" ), QStringLiteral( "constraints.T1" ) );
    QVERIFY( layer.isValid() );
    paleo::constraintcrs::assignStoreCrsIfMissing( &layer );
    QCOMPARE( layer.crs(), paleo::constraintcrs::storeCrs() );
    QVERIFY2( edits.beginEdit( &layer, &error ), qPrintable( error ) );
    QCOMPARE( layer.crs(), paleo::constraintcrs::storeCrs() );
    QVERIFY( edits.rollbackEdit( &layer ) );
    QCOMPARE( layer.crs(), paleo::constraintcrs::storeCrs() );
  }
};

int main( int argc, char *argv[] )
{
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
  {
    qFatal( "QgisRuntime::initialize failed" );
    return 1;
  }
  TestConstraintCrs tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_singlefactor_constraintcrs.moc"
