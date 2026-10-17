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

#include "../src/algorithms/constraintbarriers.h"
#include "../src/algorithms/paleoalgorithms.h"
#include "../src/workflow/constraintuseplan.h"

using namespace paleo::singlefactor;
using paleo::algorithms::UnifiedBarrierRead;
using paleo::algorithms::rasterizeBarrierParts;

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

  // P1（pr457-closure）：栅格化单元契约——格网遍历（supercover）不得漏格。
  // 网格 [0,100]×[0,100]，cellSize=10，10×10；行主序 mask，originYTop=100。
  static UnifiedBarrierRead singlePartRead( const QString &wkt, const QString &id = QStringLiteral( "b1" ) )
  {
    UnifiedBarrierRead read;
    read.parts << QgsGeometry::fromWkt( wkt );
    QVariantMap receipt;
    receipt.insert( QStringLiteral( "id" ), id );
    receipt.insert( QStringLiteral( "consumed" ), true );
    receipt.insert( QStringLiteral( "role" ), QStringLiteral( "hard_barrier" ) );
    read.receipts << receipt;
    read.partReceipt << 0;
    return read;
  }

  void rasterizationIsFourConnectedAlongDiagonals()
  {
    // 45° 斜线：标记格必须 4-连通（半格采样只落对角链时距离场会被对角漏格
    // 错误穿透）。
    {
      UnifiedBarrierRead read = singlePartRead( QStringLiteral( "LineString (5 95, 95 5)" ) );
      std::vector<std::uint8_t> mask;
      QCOMPARE( rasterizeBarrierParts( read, 0, 100, 10, 10, 10, mask, {} ), 1 );
      int first = -1, count = 0;
      for ( int i = 0; i < 100; ++i )
        if ( mask[size_t( i )] )
        {
          if ( first < 0 )
            first = i;
          ++count;
        }
      QVERIFY( count >= 10 );
      // BFS 4-连通：从首个标记格可达全部标记格。
      std::vector<std::uint8_t> seen( 100, 0 );
      std::vector<int> queue{ first };
      seen[size_t( first )] = 1;
      int reached = 0;
      while ( !queue.empty() )
      {
        const int cell = queue.back();
        queue.pop_back();
        ++reached;
        const int c = cell % 10, r = cell / 10;
        for ( const int d : { -1, 1 } )
        {
          if ( c + d >= 0 && c + d < 10 && mask[size_t( r * 10 + c + d )] && !seen[size_t( r * 10 + c + d )] )
          {
            seen[size_t( r * 10 + c + d )] = 1;
            queue.push_back( r * 10 + c + d );
          }
          if ( r + d >= 0 && r + d < 10 && mask[size_t( ( r + d ) * 10 + c )] && !seen[size_t( ( r + d ) * 10 + c )] )
          {
            seen[size_t( ( r + d ) * 10 + c )] = 1;
            queue.push_back( ( r + d ) * 10 + c );
          }
        }
      }
      QCOMPARE( reached, count );
    }
    // 极陡斜线（近垂直）同样 4-连通。
    {
      UnifiedBarrierRead read = singlePartRead( QStringLiteral( "LineString (52 5, 58 95)" ) );
      std::vector<std::uint8_t> mask;
      QCOMPARE( rasterizeBarrierParts( read, 0, 100, 10, 10, 10, mask, {} ), 1 );
      int count = 0;
      for ( auto v : mask )
        count += v;
      QVERIFY( count >= 10 );
      // 每行至少一格（跨行不允许断行）。
      for ( int r = 0; r < 10; ++r )
      {
        int rowMarks = 0;
        for ( int c = 0; c < 10; ++c )
          rowMarks += mask[size_t( r * 10 + c )];
        QVERIFY2( rowMarks > 0, qPrintable( QStringLiteral( "row %1 empty" ).arg( r ) ) );
      }
    }
  }

  void rasterizationBoundaryAndCornerTouchStayInDomain()
  {
    // 恰在网格边界上的屏障（上边/右边）：与计划侧闭矩形触域同口径——仍属计算
    // 域、落到边缘格，回执不翻 outside_compute_domain。
    {
      UnifiedBarrierRead read = singlePartRead( QStringLiteral( "LineString (0 100, 100 100)" ) );
      std::vector<std::uint8_t> mask;
      QCOMPARE( rasterizeBarrierParts( read, 0, 100, 10, 10, 10, mask, {} ), 1 );
      for ( int c = 0; c < 10; ++c )
        QVERIFY2( mask[size_t( c )], qPrintable( QStringLiteral( "top edge col %1" ).arg( c ) ) );
      const QVariantMap receipt = read.receipts.first().toMap();
      QVERIFY( receipt.value( QStringLiteral( "consumed" ) ).toBool() );
      QVERIFY( receipt.value( QStringLiteral( "reason" ) ).toString() != QStringLiteral( "outside_compute_domain" ) );
    }
    {
      UnifiedBarrierRead read = singlePartRead( QStringLiteral( "LineString (100 0, 100 100)" ) );
      std::vector<std::uint8_t> mask;
      QCOMPARE( rasterizeBarrierParts( read, 0, 100, 10, 10, 10, mask, {} ), 1 );
      for ( int r = 0; r < 10; ++r )
        QVERIFY2( mask[size_t( r * 10 + 9 )], qPrintable( QStringLiteral( "right edge row %1" ).arg( r ) ) );
    }
    // 仅触及网格角点（单点在闭域上）：入域、恰一格，回执一致（计划侧
    // polylineTouchesRect 闭矩形也判触域）。
    {
      UnifiedBarrierRead read = singlePartRead( QStringLiteral( "LineString (95 105, 105 95)" ) );
      std::vector<std::uint8_t> mask;
      QCOMPARE( rasterizeBarrierParts( read, 0, 100, 10, 10, 10, mask, {} ), 1 );
      int count = 0;
      for ( auto v : mask )
        count += v;
      QCOMPARE( count, 1 );
      QVERIFY( mask[9] ); // 右上角格（col 9, row 0）
      QVERIFY( read.receipts.first().toMap().value( QStringLiteral( "consumed" ) ).toBool() );
    }
    // 网格外屏障：零格、回执翻 outside_compute_domain（与计划一致）。
    {
      UnifiedBarrierRead read = singlePartRead( QStringLiteral( "LineString (200 200, 300 300)" ) );
      std::vector<std::uint8_t> mask;
      QCOMPARE( rasterizeBarrierParts( read, 0, 100, 10, 10, 10, mask, {} ), 0 );
      const QVariantMap receipt = read.receipts.first().toMap();
      QVERIFY( !receipt.value( QStringLiteral( "consumed" ) ).toBool() );
      QCOMPARE( receipt.value( QStringLiteral( "reason" ) ).toString(), QStringLiteral( "outside_compute_domain" ) );
    }
    // 一端在格外、中间穿域：裁剪到域内段，仍落格。
    {
      UnifiedBarrierRead read = singlePartRead( QStringLiteral( "LineString (-50 50, 150 50)" ) );
      std::vector<std::uint8_t> mask;
      QCOMPARE( rasterizeBarrierParts( read, 0, 100, 10, 10, 10, mask, {} ), 1 );
      int count = 0;
      for ( auto v : mask )
        count += v;
      QCOMPARE( count, 10 );
    }
  }

  void rasterizationReceiptsCarryCellCounts()
  {
    // 回执带 rasterized_cells / new_cells；完全被既有屏障遮蔽（new_cells=0）
    // 的部件如实标注，不再与「真产生计算效果」混同。
    UnifiedBarrierRead read;
    read.parts << QgsGeometry::fromWkt( QStringLiteral( "LineString (5 95, 95 5)" ) )
               << QgsGeometry::fromWkt( QStringLiteral( "LineString (5 95, 95 5)" ) );
    for ( const char *id : { "b1", "b2" } )
    {
      QVariantMap receipt;
      receipt.insert( QStringLiteral( "id" ), QString::fromLatin1( id ) );
      receipt.insert( QStringLiteral( "consumed" ), true );
      receipt.insert( QStringLiteral( "role" ), QStringLiteral( "hard_barrier" ) );
      read.receipts << receipt;
    }
    read.partReceipt << 0 << 1;
    std::vector<std::uint8_t> mask;
    QCOMPARE( rasterizeBarrierParts( read, 0, 100, 10, 10, 10, mask, {} ), 2 );
    const QVariantMap first = read.receipts.at( 0 ).toMap();
    const QVariantMap second = read.receipts.at( 1 ).toMap();
    QVERIFY( first.value( QStringLiteral( "rasterized" ) ).toBool() );
    QCOMPARE( first.value( QStringLiteral( "rasterized_cells" ) ).toInt(),
              first.value( QStringLiteral( "new_cells" ) ).toInt() );
    QVERIFY( first.value( QStringLiteral( "new_cells" ) ).toInt() >= 10 );
    QVERIFY( second.value( QStringLiteral( "rasterized" ) ).toBool() );
    QCOMPARE( second.value( QStringLiteral( "new_cells" ) ).toInt(), 0 );
    QVERIFY( second.value( QStringLiteral( "warnings" ) ).toStringList()
                 .contains( QStringLiteral( "fully_shadowed_by_other_barriers" ) ) );
    // 短屏障（单格内）：1 格。
    UnifiedBarrierRead tiny = singlePartRead( QStringLiteral( "LineString (45 55, 46 55)" ) );
    std::vector<std::uint8_t> tinyMask;
    QCOMPARE( rasterizeBarrierParts( tiny, 0, 100, 10, 10, 10, tinyMask, {} ), 1 );
    QCOMPARE( tiny.receipts.first().toMap().value( QStringLiteral( "rasterized_cells" ) ).toInt(), 1 );
  }

  void rasterizationCancelStops()
  {
    UnifiedBarrierRead read = singlePartRead( QStringLiteral( "LineString (5 95, 95 5)" ) );
    std::vector<std::uint8_t> mask;
    QCOMPARE( rasterizeBarrierParts( read, 0, 100, 10, 10, 10, mask, [] { return true; } ), -1 );
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
    // R3 后计划与引擎同口径：域外屏障两边都不消费（域=井点范围外扩 10%）。
    QCOMPARE( engine, planConsumed( QStringLiteral( "legacy" ), paddedWellExtent() ) );
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

  // P2 任务D：IDW 逐点取样回执——参与井带坐标/值，跳过井带原因码。
  void idwSampleReceipts()
  {
    QgsVectorLayer wells( QStringLiteral( "Point?crs=EPSG:3857&field=z:double&field=well_id:string" ), QStringLiteral( "w" ),
                          QStringLiteral( "memory" ) );
    QList<QgsFeature> features;
    const auto add = [&]( const QString &wkt, const QVariant &z, const QString &id ) {
      QgsFeature f( wells.fields() );
      if ( !wkt.isEmpty() )
        f.setGeometry( QgsGeometry::fromWkt( wkt ) );
      f.setAttribute( 0, z );
      f.setAttribute( 1, id );
      features << f;
    };
    add( QStringLiteral( "POINT(0 0)" ), 1.0, QStringLiteral( "A" ) );
    add( QStringLiteral( "POINT(10 0)" ), 2.0, QStringLiteral( "B" ) );
    add( QStringLiteral( "POINT(0 10)" ), 3.0, QStringLiteral( "C" ) );
    add( QStringLiteral( "POINT(10 10)" ), QVariant( QMetaType( QMetaType::Double ) ), QStringLiteral( "NULLZ" ) );
    add( QString(), 5.0, QStringLiteral( "NOGEOM" ) );
    QVERIFY( wells.dataProvider()->addFeatures( features ) );
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( static_cast<QgsMapLayer *>( &wells ) ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    params.insert( QStringLiteral( "OUTPUT" ), mDir.filePath( QStringLiteral( "idw-samples.tif" ) ) );
    QgsProcessingContext context;
    QgsProcessingFeedback feedback;
    const QVariantMap result = QgsApplication::processingRegistry()
                                   ->algorithmById( QStringLiteral( "paleo:paleo_constraint_idw" ) )
                                   ->run( params, context, &feedback );
    QVERIFY2( !result.isEmpty(), qPrintable( feedback.textLog() ) );
    const QJsonArray receipts =
        QJsonDocument::fromJson( result.value( QStringLiteral( "SAMPLE_RECEIPTS" ) ).toString().toUtf8() ).array();
    QCOMPARE( receipts.size(), 5 );
    QMap<QString, QString> reasons;
    QStringList used;
    for ( const QJsonValue &v : receipts )
    {
      const QJsonObject o = v.toObject();
      if ( o.value( QStringLiteral( "participating" ) ).toBool() )
        used << o.value( QStringLiteral( "well_id" ) ).toString();
      else
        reasons.insert( o.value( QStringLiteral( "well_id" ) ).toString(), o.value( QStringLiteral( "reason" ) ).toString() );
    }
    used.sort();
    QCOMPARE( used, QStringList( { QStringLiteral( "A" ), QStringLiteral( "B" ), QStringLiteral( "C" ) } ) );
    QCOMPARE( reasons.value( QStringLiteral( "NULLZ" ) ), QStringLiteral( "field_missing_or_non_finite" ) );
    QCOMPARE( reasons.value( QStringLiteral( "NOGEOM" ) ), QStringLiteral( "missing_geometry" ) );
  }

  // P2：距离变换 / 最小曲率按统一语义取约束——回执 == 计划。
  void griddingReceiptsMatchPlan_data()
  {
    QTest::addColumn<QString>( "algorithm" );
    QTest::addColumn<QString>( "planMethod" );
    QTest::newRow( "distance_transform" ) << QStringLiteral( "paleo:paleo_distance_transform" )
                                          << QStringLiteral( "distance_transform" );
    QTest::newRow( "min_curvature" ) << QStringLiteral( "paleo:paleo_min_curvature" ) << QStringLiteral( "min_curvature" );
  }
  void griddingReceiptsMatchPlan()
  {
    QFETCH( QString, algorithm );
    QFETCH( QString, planMethod );
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( mWells ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( mConstraints ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 2.0 );
    params.insert( QStringLiteral( "OUTPUT" ), mDir.filePath( planMethod + QStringLiteral( ".tif" ) ) );
    QgsProcessingContext context;
    QgsProcessingFeedback feedback;
    const QVariantMap result = QgsApplication::processingRegistry()->algorithmById( algorithm )->run( params, context, &feedback );
    QVERIFY2( !result.isEmpty(), qPrintable( feedback.textLog() ) );
    const QJsonArray receipts =
        QJsonDocument::fromJson( result.value( QStringLiteral( "CONSTRAINT_RECEIPTS" ) ).toString().toUtf8() ).array();
    QMap<QString, QString> roles;
    const QStringList engine = receiptConsumed( receipts, &roles );
    // P3：引擎逐部件记落格，计划按成图域（井点范围外扩 10%）判域——两者一致。
    QCOMPARE( engine, planConsumed( planMethod, paddedWellExtent() ) );
    QCOMPARE( roles.value( QStringLiteral( "b_full" ) ), QStringLiteral( "hard_barrier" ) );
    for ( const char *id : { "b_display", "b_none", "b_disabled", "b_zero", "b_bowtie", "d_main", "s_soft", "c_stop",
                             "b_outside" } )
      QVERIFY2( !engine.contains( QString::fromLatin1( id ) ), id );
    QString outsideReason;
    for ( const QJsonValue &v : receipts )
      if ( v.toObject().value( QStringLiteral( "id" ) ).toString() == QLatin1String( "b_outside" ) )
        outsideReason = v.toObject().value( QStringLiteral( "reason" ) ).toString();
    QCOMPARE( outsideReason, QStringLiteral( "outside_compute_domain" ) );
    // 元数据计数 == 回执里的硬屏障数（栅格外屏障不计）。
    GDALDatasetH ds = GDALOpen( mDir.filePath( planMethod + QStringLiteral( ".tif" ) ).toUtf8().constData(), GA_ReadOnly );
    QVERIFY( ds );
    int barriers = 0;
    for ( const QString &role : roles )
      barriers += role == QLatin1String( "hard_barrier" ) ? 1 : 0;
    QCOMPARE( QByteArray( GDALGetMetadataItem( ds, "PALEO_BREAK_LINES", nullptr ) ), QByteArray::number( barriers ) );
    GDALClose( ds );
  }

  // PR457-closure R1（闭契约版）：恰好压在网格边界上的屏障。井点 0..100 +
  // 10% pad → 成图域 [-10,110]，CELL_SIZE 2 → 边界恰为 ±110/-10（整除）。
  // #488 统一闭域契约：supercover 把贴边点 clamp 进最后一列/行格——贴边
  // 屏障影响边缘格、不是域外；计划侧 polylineTouchesRect 同为闭矩形。三个
  // 引擎（旧 IDW / 距离变换 / 最小曲率）回执一致：四条贴边线全部消费。
  void edgeAlignedBarrierIsConsumedForAllGridEngines()
  {
    QgsVectorLayer lines( QStringLiteral( "MultiLineString?crs=EPSG:3857&field=id:string&field=horizon:string&field=type:string"
                                          "&field=params_json:string&field=schema_version:integer" ),
                          QStringLiteral( "edge" ), QStringLiteral( "memory" ) );
    QVERIFY( lines.isValid() );
    QList<QgsFeature> features;
    const auto addLine = [&]( const QString &id, const QString &wkt ) {
      QgsFeature f( lines.fields() );
      QgsGeometry g = QgsGeometry::fromWkt( wkt );
      g.convertToMultiType();
      f.setGeometry( g );
      QgsAttributes attributes;
      attributes << id << QStringLiteral( "H1" ) << QStringLiteral( "break_line" )
                 << QVariant( QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"full_block"})" ) ) << 1;
      f.setAttributes( attributes );
      features << f;
    };
    addLine( QStringLiteral( "edge_right" ), QStringLiteral( "LINESTRING(110 0, 110 100)" ) );
    addLine( QStringLiteral( "edge_inside" ), QStringLiteral( "LINESTRING(109 0, 109 100)" ) );
    // 闭契约：成图域 [-10,110]（井点 0..100 + 10% pad），四条贴边线
    // （x==110 / x==-10 / y==110 / y==-10）clamp 进边缘格，全部消费
    // （full-audit F15：补左边缘 x==-10——原测试声称四边缘但缺测这条）。
    addLine( QStringLiteral( "edge_left" ), QStringLiteral( "LINESTRING(-10 0, -10 100)" ) );
    addLine( QStringLiteral( "edge_top" ), QStringLiteral( "LINESTRING(20 110, 80 110)" ) );
    addLine( QStringLiteral( "edge_bottom" ), QStringLiteral( "LINESTRING(20 -10, 80 -10)" ) );
    QVERIFY( lines.dataProvider()->addFeatures( features ) );

    for ( const QString &algorithm : { QStringLiteral( "paleo:paleo_constraint_idw" ),
                                       QStringLiteral( "paleo:paleo_distance_transform" ),
                                       QStringLiteral( "paleo:paleo_min_curvature" ) } )
    {
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( mWells ) );
      params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( &lines ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 2.0 );
      params.insert( QStringLiteral( "OUTPUT" ),
                     mDir.filePath( QStringLiteral( "edge-%1.tif" ).arg( algorithm.section( QLatin1Char( ':' ), 1 ) ) ) );
      QgsProcessingContext context;
      QgsProcessingFeedback feedback;
      const QVariantMap result =
          QgsApplication::processingRegistry()->algorithmById( algorithm )->run( params, context, &feedback );
      QVERIFY2( !result.isEmpty(), qPrintable( algorithm + ": " + feedback.textLog() ) );
      const QJsonArray receipts =
          QJsonDocument::fromJson( result.value( QStringLiteral( "CONSTRAINT_RECEIPTS" ) ).toString().toUtf8() ).array();
      bool sawEdge = false, sawInside = false, sawTop = false, sawBottom = false;
      bool sawLeft = false;
      QString insideRole, topRole;
      for ( const QJsonValue &v : receipts )
      {
        const QJsonObject receipt = v.toObject();
        const QString id = receipt.value( QStringLiteral( "id" ) ).toString();
        if ( id == QLatin1String( "edge_right" ) )
        {
          sawEdge = true;
          QVERIFY2( receipt.value( QStringLiteral( "consumed" ) ).toBool(),
                    qPrintable( algorithm + ": closed contract — edge line clamps into edge cell" ) );
        }
        if ( id == QLatin1String( "edge_left" ) )
        {
          sawLeft = true;
          QVERIFY2( receipt.value( QStringLiteral( "consumed" ) ).toBool(),
                    qPrintable( algorithm + ": closed contract — left edge clamps into first column" ) );
        }
        if ( id == QLatin1String( "edge_inside" ) )
        {
          sawInside = true;
          insideRole = receipt.value( QStringLiteral( "role" ) ).toString();
          QVERIFY2( receipt.value( QStringLiteral( "consumed" ) ).toBool(), qPrintable( algorithm ) );
        }
        if ( id == QLatin1String( "edge_top" ) )
        {
          sawTop = true;
          topRole = receipt.value( QStringLiteral( "role" ) ).toString();
          QVERIFY2( receipt.value( QStringLiteral( "consumed" ) ).toBool(),
                    qPrintable( algorithm + ": y==yMax must be inside (row 0)" ) );
        }
        if ( id == QLatin1String( "edge_bottom" ) )
        {
          sawBottom = true;
          QVERIFY2( receipt.value( QStringLiteral( "consumed" ) ).toBool(),
                    qPrintable( algorithm + ": closed contract — edge line clamps into edge cell" ) );
        }
      }
      QVERIFY2( sawEdge && sawLeft && sawInside && sawTop && sawBottom, qPrintable( algorithm ) );
      QCOMPARE( insideRole, QStringLiteral( "hard_barrier" ) );
      QCOMPARE( topRole, QStringLiteral( "hard_barrier" ) );
    }
  }

  // 数值层面：display_only 打断线不隔断（与无约束逐位一致），同一条线改成
  // 完全阻断则改变结果（对照，保证用例不空转）。
  void griddingDisplayOnlyDoesNotCut_data() { griddingReceiptsMatchPlan_data(); }
  void griddingDisplayOnlyDoesNotCut()
  {
    QFETCH( QString, algorithm );
    QFETCH( QString, planMethod );
    const auto run = [&]( const QString &json, const QString &name,
                          const QString &wkt = QStringLiteral( "LINESTRING(50 -20, 50 120)" ) ) -> QByteArray {
      QgsVectorLayer lines( QStringLiteral( "LineString?crs=EPSG:3857&field=id:string&field=horizon:string&field=type:string"
                                            "&field=params_json:string&field=schema_version:integer" ),
                            QStringLiteral( "c" ), QStringLiteral( "memory" ) );
      if ( !json.isNull() )
      {
        QgsFeature f( lines.fields() );
        f.setGeometry( QgsGeometry::fromWkt( wkt ) );
        f.setAttributes( QgsAttributes() << QStringLiteral( "wall" ) << QStringLiteral( "H1" )
                                         << QStringLiteral( "break_line" ) << json << 1 );
        lines.dataProvider()->addFeature( f );
      }
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( mWells ) );
      params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( static_cast<QgsMapLayer *>( &lines ) ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 4.0 );
      const QString out = mDir.filePath( name );
      params.insert( QStringLiteral( "OUTPUT" ), out );
      QgsProcessingContext context;
      QgsProcessingFeedback feedback;
      if ( QgsApplication::processingRegistry()->algorithmById( algorithm )->run( params, context, &feedback ).isEmpty() )
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
    const QByteArray none = run( QString(), planMethod + QStringLiteral( "-none.tif" ) );
    const QByteArray shown = run( QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"display_only"})" ),
                                  planMethod + QStringLiteral( "-display.tif" ) );
    const QByteArray disabled = run( QStringLiteral( R"({"semantic":"hard_barrier","enabled":false})" ),
                                     planMethod + QStringLiteral( "-disabled.tif" ) );
    const QByteArray full = run( QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"full_block"})" ),
                                 planMethod + QStringLiteral( "-full.tif" ) );
    QVERIFY( !none.isEmpty() );
    QVERIFY( !full.isEmpty() );
    QVERIFY2( full != none, "对照失效：完全阻断线应改变结果" );
    QCOMPARE( shown, none );
    QCOMPARE( disabled, none );
    // P3：完全落在栅格外的完全阻断线不隔断——与无约束逐位一致
    //（距离变换此前会因它从精确欧氏切到 8 邻接近似）。
    const QByteArray outside = run( QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"full_block"})" ),
                                    planMethod + QStringLiteral( "-outside.tif" ), QStringLiteral( "LINESTRING(1000 0, 1100 0)" ) );
    QCOMPARE( outside, none );
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
