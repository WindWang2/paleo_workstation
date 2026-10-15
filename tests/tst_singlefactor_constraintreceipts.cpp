// 层：测试壳
// P1-A H1（引擎侧）：同一组约束经真实 Processing 引擎运行，QC/输出里的
// constraint_receipts（引擎按实际消费独立产出）必须与统一计划
// planConstraintUse（UI/工作流口径）逐条一致——不数字符串、不预设答案。
#include <QtTest>

#include <qgsapplication.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsprocessingregistry.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayer.h>

#include <gdal.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "../src/algorithms/paleoalgorithms.h"
#include "../src/workflow/constraintuseplan.h"

using namespace paleo::singlefactor;

namespace
{

struct Row
{
  QString id;
  QString type;
  QString json;
  QString wkt;
};

QVector<Row> constraintRows()
{
  return {
      { "b_full", "break_line", R"({"semantic":"hard_barrier","blockMode":"full_block"})", "LINESTRING(50 -20, 50 60)" },
      { "b_display", "break_line", R"({"semantic":"hard_barrier","blockMode":"display_only"})", "LINESTRING(12 20, 12 80)" },
      { "b_none", "break_line", R"({"semantic":"hard_barrier","blockMode":"none"})", "LINESTRING(88 20, 88 80)" },
      { "b_disabled", "break_line", R"({"semantic":"hard_barrier","enabled":false})", "LINESTRING(60 -20, 60 60)" },
      { "b_typecol", "break_line", QString(), "LINESTRING(30 70, 30 120)" },
      { "b_outside", "break_line", QString(), "LINESTRING(1000 0, 1100 0)" },
      { "b_zero", "break_line", QString(), "LINESTRING(40 40, 40 40)" },
      { "b_bowtie", "break_line", QString(), "LINESTRING(60 70, 70 80, 70 70, 60 80)" },
      { "d_main", "direction_line", R"({"semantic":"direction_guide","ratio":6})", "LINESTRING(0 10, 100 30)" },
      { "d_flat", "direction_line", R"({"semantic":"direction_guide","ratio":1})", "LINESTRING(0 90, 100 90)" },
      { "d_multi", "direction_line", QString(), "MULTILINESTRING((5 40, 20 45),(80 45, 95 50))" },
      { "d_disabled", "direction_line", R"({"semantic":"direction_guide","enabled":false})", "LINESTRING(0 60, 100 60)" },
      { "s_soft", "interpretive_boundary", R"({"semantic":"interpretive_boundary","softStrength":0.3})",
        "LINESTRING(20 0, 20 30)" },
      { "s_zero", "interpretive_boundary", R"({"semantic":"interpretive_boundary","softStrength":0})",
        "LINESTRING(80 0, 80 30)" },
      { "c_stop", "contour_stop", R"({"semantic":"contour_stop"})", "LINESTRING(40 85, 60 85)" },
  };
}

QVector<QVariantMap> planRows()
{
  QVector<QVariantMap> rows;
  for ( const Row &r : constraintRows() )
    rows << QVariantMap{ { "id", r.id }, { "horizon", "H1" }, { "type", r.type }, { "params_json", r.json },
                         { "schema_version", 1 }, { "wkt", r.wkt } };
  return rows;
}

QStringList planConsumed( const QString &method, std::optional<QRectF> domain )
{
  ConstraintMethodContext context;
  context.methodId = method;
  context.horizon = QStringLiteral( "H1" );
  context.domain = domain;
  const ConstraintUsePlan plan = planConstraintUse( planRows(), context );
  QStringList ids = plan.summary.value( QStringLiteral( "consumed" ) ).toStringList();
  ids.sort();
  return ids;
}

QStringList receiptConsumed( const QJsonArray &receipts, QMap<QString, QString> *roles = nullptr )
{
  QStringList ids;
  for ( const QJsonValue &value : receipts )
  {
    const QJsonObject item = value.toObject();
    if ( item.value( QStringLiteral( "consumed" ) ).toBool() )
    {
      ids << item.value( QStringLiteral( "id" ) ).toString();
      if ( roles )
        roles->insert( item.value( QStringLiteral( "id" ) ).toString(), item.value( QStringLiteral( "role" ) ).toString() );
    }
  }
  ids.sort();
  return ids;
}

} // namespace

class TestConstraintReceipts : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir mDir;
  QgsVectorLayer *mWells = nullptr;
  QgsVectorLayer *mConstraints = nullptr;

  QJsonObject runLocal( const QString &algorithm, const QString &method, const QString &name )
  {
    const QString out = mDir.filePath( name + QStringLiteral( ".tif" ) );
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( mWells ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( mConstraints ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 2.0 );
    if ( !method.isEmpty() )
      params.insert( QStringLiteral( "METHOD" ), method );
    params.insert( QStringLiteral( "OUTPUT" ), out );
    QgsProcessingContext context;
    QgsProcessingFeedback feedback;
    const QVariantMap result =
        QgsApplication::processingRegistry()->algorithmById( algorithm )->run( params, context, &feedback );
    if ( result.isEmpty() )
      qWarning() << feedback.textLog();
    QFile qc( result.value( QStringLiteral( "QC" ) ).toString() );
    if ( !qc.open( QIODevice::ReadOnly ) )
      return {};
    return QJsonDocument::fromJson( qc.readAll() ).object();
  }

  // 输出范围 = 井点范围外扩 10%（localdirectionalgorithm 缺省口径）。
  static QRectF paddedWellExtent() { return QRectF( QPointF( -10, -10 ), QPointF( 110, 110 ) ); }

private slots:
  void initTestCase()
  {
    QVERIFY( mDir.isValid() );
    if ( !QgsApplication::processingRegistry()->providerById( QStringLiteral( "paleo" ) ) )
      QgsApplication::processingRegistry()->addProvider( new PaleoProvider() );
    mWells = new QgsVectorLayer( QStringLiteral( "Point?crs=EPSG:3857&field=z:double&field=well_id:string" ), QStringLiteral( "wells" ),
                                 QStringLiteral( "memory" ) );
    QList<QgsFeature> wells;
    int k = 0;
    for ( const double x : { 0.0, 25.0, 75.0, 100.0 } )
      for ( const double y : { 0.0, 50.0, 100.0 } )
      {
        QgsFeature f( mWells->fields() );
        f.setGeometry( QgsGeometry::fromPointXY( QgsPointXY( x, y ) ) );
        f.setAttribute( 0, 10.0 + x * 0.1 + y * 0.05 + ( ++k % 3 ) );
        f.setAttribute( 1, QStringLiteral( "W%1" ).arg( k ) );
        wells << f;
      }
    QVERIFY( mWells->dataProvider()->addFeatures( wells ) );
    mWells->updateExtents();

    mConstraints = new QgsVectorLayer(
        QStringLiteral( "MultiLineString?crs=EPSG:3857&field=id:string&field=horizon:string&field=type:string"
                        "&field=params_json:string&field=schema_version:integer" ),
        QStringLiteral( "constraints" ), QStringLiteral( "memory" ) );
    QVERIFY( mConstraints->isValid() );
    QList<QgsFeature> lines;
    for ( const Row &r : constraintRows() )
    {
      QgsFeature f( mConstraints->fields() );
      QgsGeometry g = QgsGeometry::fromWkt( r.wkt );
      g.convertToMultiType(); // 单部件 multi 不加 #k 后缀，id 与计划一致
      f.setGeometry( g );
      QgsAttributes attributes;
      attributes << r.id << QStringLiteral( "H1" ) << r.type << ( r.json.isEmpty() ? QVariant() : QVariant( r.json ) ) << 1;
      f.setAttributes( attributes );
      lines << f;
    }
    QVERIFY( mConstraints->dataProvider()->addFeatures( lines ) );
    QCOMPARE( mConstraints->featureCount(), static_cast<long long>( constraintRows().size() ) );
  }

  void cleanupTestCase()
  {
    delete mConstraints;
    delete mWells;
  }

  // 本地方向 IDW / Kriging：方向线、硬屏障、软边界逐条与计划一致。
  void localDirectionReceiptsMatchPlan_data()
  {
    QTest::addColumn<QString>( "method" );
    QTest::addColumn<QString>( "planMethod" );
    QTest::newRow( "idw" ) << QString() << QStringLiteral( "local_direction_idw" );
    QTest::newRow( "kriging" ) << QStringLiteral( "kriging" ) << QStringLiteral( "local_direction_kriging" );
  }
  void localDirectionReceiptsMatchPlan()
  {
    QFETCH( QString, method );
    QFETCH( QString, planMethod );
    const QJsonObject qc =
        runLocal( QStringLiteral( "paleo:paleo_local_direction_idw" ), method, QStringLiteral( "ld_" ) + planMethod );
    QVERIFY2( !qc.isEmpty(), "QC sidecar" );
    QMap<QString, QString> roles;
    const QStringList engine = receiptConsumed( qc.value( QStringLiteral( "constraint_receipts" ) ).toArray(), &roles );
    const QStringList plan = planConsumed( planMethod, paddedWellExtent() );
    QCOMPARE( engine, plan );
    // 验收用例逐条：方向线消费，display_only 不是硬屏障，停用无作用，无效几何不参与。
    QCOMPARE( roles.value( QStringLiteral( "d_main" ) ), QStringLiteral( "direction_guide" ) );
    QCOMPARE( roles.value( QStringLiteral( "d_multi#0" ) ), QStringLiteral( "direction_guide" ) );
    QCOMPARE( roles.value( QStringLiteral( "b_full" ) ), QStringLiteral( "hard_barrier" ) );
    QCOMPARE( roles.value( QStringLiteral( "s_soft" ) ), QStringLiteral( "soft_boundary" ) );
    for ( const char *id : { "b_display", "b_none", "b_disabled", "d_disabled", "d_flat", "s_zero", "c_stop", "b_zero",
                             "b_bowtie", "b_outside" } )
      QVERIFY2( !engine.contains( QString::fromLatin1( id ) ), id );
  }

  void surferReceiptsMatchPlan()
  {
    const QJsonObject qc = runLocal( QStringLiteral( "paleo:paleo_surfer_idw" ), QString(), QStringLiteral( "surfer" ) );
    QVERIFY2( !qc.isEmpty(), "QC sidecar" );
    const QStringList engine = receiptConsumed( qc.value( QStringLiteral( "constraint_receipts" ) ).toArray() );
    // Surfer 的 FaultPathMetric 用有限线段，域外线也进入（只是不改距离）：计划不判域。
    QCOMPARE( engine, planConsumed( QStringLiteral( "surfer_idw" ), std::nullopt ) );
    QVERIFY( !engine.contains( QStringLiteral( "d_main" ) ) );
  }

  void legacyReceiptsMatchPlan()
  {
    const QString out = mDir.filePath( QStringLiteral( "legacy.tif" ) );
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( mWells ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( mConstraints ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 2.0 );
    params.insert( QStringLiteral( "OUTPUT" ), out );
    QgsProcessingContext context;
    QgsProcessingFeedback feedback;
    const QVariantMap result = QgsApplication::processingRegistry()
                                   ->algorithmById( QStringLiteral( "paleo:paleo_constraint_idw" ) )
                                   ->run( params, context, &feedback );
    QVERIFY2( !result.isEmpty(), qPrintable( feedback.textLog() ) );
    const QJsonArray receipts =
        QJsonDocument::fromJson( result.value( QStringLiteral( "CONSTRAINT_RECEIPTS" ) ).toString().toUtf8() ).array();
    QMap<QString, QString> roles;
    const QStringList engine = receiptConsumed( receipts, &roles );
    QCOMPARE( engine, planConsumed( QStringLiteral( "legacy" ), std::nullopt ) );
    // 旧 IDW 之前按 type 列把 display_only/none/停用打断线都当硬屏障——现在不再。
    for ( const char *id : { "b_display", "b_none", "b_disabled", "b_zero", "b_bowtie" } )
      QVERIFY2( !engine.contains( QString::fromLatin1( id ) ), id );
    QCOMPARE( roles.value( QStringLiteral( "d_main" ) ), QStringLiteral( "global_direction" ) );
    // 元数据计数与回执同源。
    GDALDatasetH ds = GDALOpen( out.toUtf8().constData(), GA_ReadOnly );
    QVERIFY( ds );
    int barriers = 0;
    for ( const QString &role : roles )
      barriers += role == QLatin1String( "hard_barrier" ) ? 1 : 0;
    QCOMPARE( QByteArray( GDALGetMetadataItem( ds, "PALEO_BREAK_LINES", nullptr ) ), QByteArray::number( barriers ) );
    GDALClose( ds );
  }

  // P2：结构面 IDW 回执 == 计划。contour_only 模型：完全阻断与停线都只是
  // 解释软边界，回执绝不出现 hard_barrier；停用的解释边界不进趋势权重。
  void structuralReceiptsMatchPlan()
  {
    QgsVectorLayer boundary( QStringLiteral( "Polygon?crs=EPSG:3857" ), QStringLiteral( "boundary" ),
                             QStringLiteral( "memory" ) );
    QgsFeature square;
    square.setGeometry( QgsGeometry::fromWkt( QStringLiteral( "POLYGON((0 0, 100 0, 100 100, 0 100, 0 0))" ) ) );
    QVERIFY( boundary.dataProvider()->addFeature( square ) );
    boundary.updateExtents();
    QVariantMap params;
    params.insert( QStringLiteral( "WELLS" ), QVariant::fromValue( static_cast<QgsMapLayer *>( mWells ) ) );
    params.insert( QStringLiteral( "WELL_ID_FIELD" ), QStringLiteral( "well_id" ) );
    params.insert( QStringLiteral( "VALUE_FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "FACTOR_MODE" ), QStringLiteral( "direct" ) );
    params.insert( QStringLiteral( "BOUNDARY" ), QVariant::fromValue( static_cast<QgsMapLayer *>( &boundary ) ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( static_cast<QgsMapLayer *>( mConstraints ) ) );
    params.insert( QStringLiteral( "GRID_RESOLUTION" ), 60 );
    params.insert( QStringLiteral( "OUTPUT" ), mDir.filePath( QStringLiteral( "structural.tif" ) ) );
    QgsProcessingContext context;
    QgsProcessingFeedback feedback;
    const QVariantMap result = QgsApplication::processingRegistry()
                                   ->algorithmById( QStringLiteral( "paleo:paleo_structural_idw" ) )
                                   ->run( params, context, &feedback );
    QVERIFY2( !result.isEmpty(), qPrintable( feedback.textLog() ) );
    QFile qcFile( result.value( QStringLiteral( "QC" ) ).toString() );
    QVERIFY( qcFile.open( QIODevice::ReadOnly ) );
    const QJsonObject qc = QJsonDocument::fromJson( qcFile.readAll() ).object();
    QMap<QString, QString> roles;
    const QStringList engine = receiptConsumed( qc.value( QStringLiteral( "constraint_receipts" ) ).toArray(), &roles );
    QCOMPARE( engine, planConsumed( QStringLiteral( "structural_idw" ), QRectF( 0, 0, 100, 100 ) ) );
    for ( const QString &role : roles )
      QVERIFY2( role != QLatin1String( "hard_barrier" ), "contour_only 模型不存在硬隔断" );
    QCOMPARE( roles.value( QStringLiteral( "b_full" ) ), QStringLiteral( "soft_boundary" ) );
    QCOMPARE( roles.value( QStringLiteral( "b_display" ) ), QStringLiteral( "soft_boundary" ) );
    QCOMPARE( roles.value( QStringLiteral( "d_main" ) ), QStringLiteral( "direction_guide" ) );
    QCOMPARE( roles.value( QStringLiteral( "s_soft" ) ), QStringLiteral( "soft_boundary" ) );
    for ( const char *id : { "b_none", "b_disabled", "d_disabled", "d_flat", "s_zero", "b_zero", "b_bowtie" } )
      QVERIFY2( !engine.contains( QString::fromLatin1( id ) ), id );
  }

  // 结构面引擎：停用的解释边界以前仍进趋势权重（keepDisabled 漏判）——
  // 启用/停用两次运行的栅格必须一致（停用行无数值作用）。
  void structuralDisabledSoftBoundaryHasNoEffect()
  {
    // mode: 0 = 无约束线；1 = 停用解释边界；2 = 启用解释边界（对照：确认该线确实会改面）。
    const auto run = [this]( int mode, const QString &name ) -> QByteArray {
      QgsVectorLayer boundary( QStringLiteral( "Polygon?crs=EPSG:3857" ), QStringLiteral( "boundary" ),
                               QStringLiteral( "memory" ) );
      QgsFeature square;
      square.setGeometry( QgsGeometry::fromWkt( QStringLiteral( "POLYGON((0 0, 100 0, 100 100, 0 100, 0 0))" ) ) );
      boundary.dataProvider()->addFeature( square );
      boundary.updateExtents();
      QgsVectorLayer lines( QStringLiteral( "LineString?crs=EPSG:3857&field=id:string&field=horizon:string&field=type:string"
                                            "&field=params_json:string&field=schema_version:integer" ),
                            QStringLiteral( "c" ), QStringLiteral( "memory" ) );
      if ( mode > 0 )
      {
        QgsFeature f( lines.fields() );
        f.setGeometry( QgsGeometry::fromWkt( QStringLiteral( "LINESTRING(50 0, 50 100)" ) ) );
        f.setAttributes( QgsAttributes() << QStringLiteral( "s_off" ) << QStringLiteral( "H1" )
                                         << QStringLiteral( "interpretive_boundary" )
                                         << ( mode == 1 ? QStringLiteral( R"({"semantic":"interpretive_boundary","softStrength":0.8,"enabled":false})" )
                                                       : QStringLiteral( R"({"semantic":"interpretive_boundary","softStrength":0.8})" ) )
                                         << 1 );
        lines.dataProvider()->addFeature( f );
      }
      QVariantMap params;
      params.insert( QStringLiteral( "WELLS" ), QVariant::fromValue( static_cast<QgsMapLayer *>( mWells ) ) );
      params.insert( QStringLiteral( "WELL_ID_FIELD" ), QStringLiteral( "well_id" ) );
      params.insert( QStringLiteral( "VALUE_FIELD" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "FACTOR_MODE" ), QStringLiteral( "direct" ) );
      params.insert( QStringLiteral( "BOUNDARY" ), QVariant::fromValue( static_cast<QgsMapLayer *>( &boundary ) ) );
      params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( static_cast<QgsMapLayer *>( &lines ) ) );
      params.insert( QStringLiteral( "GRID_RESOLUTION" ), 40 );
      const QString out = mDir.filePath( name );
      params.insert( QStringLiteral( "OUTPUT" ), out );
      QgsProcessingContext context;
      QgsProcessingFeedback feedback;
      const QVariantMap result = QgsApplication::processingRegistry()
                                     ->algorithmById( QStringLiteral( "paleo:paleo_structural_idw" ) )
                                     ->run( params, context, &feedback );
      if ( result.isEmpty() )
        return {};
      GDALDatasetH ds = GDALOpen( out.toUtf8().constData(), GA_ReadOnly );
      if ( !ds )
        return {};
      const int nx = GDALGetRasterXSize( ds ), ny = GDALGetRasterYSize( ds );
      QByteArray bytes( nx * ny * static_cast<int>( sizeof( double ) ), '\0' );
      const CPLErr err = GDALRasterIO( GDALGetRasterBand( ds, 1 ), GF_Read, 0, 0, nx, ny, bytes.data(), nx, ny,
                                       GDT_Float64, 0, 0 );
      GDALClose( ds );
      return err == CE_None ? bytes : QByteArray();
    };
    const QByteArray plain = run( 0, QStringLiteral( "soft-plain.tif" ) );
    const QByteArray disabled = run( 1, QStringLiteral( "soft-disabled.tif" ) );
    const QByteArray enabled = run( 2, QStringLiteral( "soft-enabled.tif" ) );
    QVERIFY( !plain.isEmpty() );
    QVERIFY( !enabled.isEmpty() );
    QVERIFY2( enabled != plain, "对照失效：启用的解释边界应改变结构面" );
    QCOMPARE( disabled, plain );
  }

  // 地统方法不读约束：计划逐条标未使用。
  void geostatPlanFlagsAllUnused()
  {
    ConstraintMethodContext context;
    context.methodId = QStringLiteral( "kriging" );
    const ConstraintUsePlan plan = planConstraintUse( planRows(), context );
    QCOMPARE( plan.consumed, 0 );
    QVERIFY( plan.availableBarriers > 0 );
    QVERIFY( plan.availableDirections > 0 );
    for ( const ConstraintUseDecision &d : plan.decisions )
      QVERIFY( !d.consumed );
  }

  void availabilityCounts()
  {
    ConstraintMethodContext context;
    context.methodId = QStringLiteral( "local_direction_idw" );
    context.horizon = QStringLiteral( "H1" );
    const ConstraintUsePlan plan = planConstraintUse( planRows(), context );
    // 完全阻断且几何有效：b_full、b_typecol、b_outside（可用≠参与：域外由方法判定）。
    QCOMPARE( plan.availableBarriers, 3 );
    // d_main、d_flat、d_multi#0、d_multi#1（比值 1 仍是可用方向线，方法判不消费）。
    QCOMPARE( plan.availableDirections, 4 );
    QCOMPARE( plan.availableSoft, 2 );
    QCOMPARE( plan.availableCartographic, 2 ); // b_display（重分类为停线）+ c_stop
    QCOMPARE( plan.disabled, 2 );
    QCOMPARE( plan.invalid, 2 ); // b_zero、b_bowtie
  }
};

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true );
  app.initQgis();
  GDALAllRegister();
  TestConstraintReceipts tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_singlefactor_constraintreceipts.moc"
