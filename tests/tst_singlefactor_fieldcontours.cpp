// 层：测试壳（被测对象：上游 field_contours/local_interpretive_detour 等值线移植
// + structural_idw 因素的 generateContours 工作流分支）
#include <QtTest/QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <qgsapplication.h>
#include <qgsfeature.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsprocessingregistry.h>
#include <qgsproject.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>

#include <gdal.h>

#include <cmath>
#include <limits>
#include <memory>

#include "../src/algorithms/paleoalgorithms.h"
#include "../src/algorithms/singlefactor/contourlevels.h"
#include "../src/algorithms/singlefactor/fieldcontours.h"
#include "../src/catalog/datacatalog.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/workflow/constraintimport.h"
#include "../src/workflow/workflows.h"

namespace
{

QString fixturePath( const QString &name )
{
  const QString fromSource = QFileInfo( QString::fromUtf8( __FILE__ ) ).dir().filePath(
      QStringLiteral( "fixtures/singlefactor/" ) + name );
  if ( QFileInfo::exists( fromSource ) )
    return fromSource;
  return QStringLiteral( "tests/fixtures/singlefactor/" ) + name;
}

QJsonObject readJson( const QString &path )
{
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) )
    return {};
  return QJsonDocument::fromJson( file.readAll() ).object();
}

// 与 tst_singlefactor_structural 同构：真实导入路径 → 内存约束图层。
std::unique_ptr<QgsVectorLayer> constraintLayer( const QString &directionsPath,
                                               const QString &barriersPath,
                                               QString *error )
{
  auto layer = std::make_unique<QgsVectorLayer>( QStringLiteral( "LineString" ),
                                                 QStringLiteral( "constraints" ),
                                                 QStringLiteral( "memory" ) );
  if ( !layer->isValid() )
  {
    if ( error )
      *error = QStringLiteral( "memory constraint layer invalid" );
    return nullptr;
  }
  layer->startEditing();
  layer->addAttribute( QgsField( QStringLiteral( "id" ), QMetaType::Type::QString ) );
  layer->addAttribute( QgsField( QStringLiteral( "type" ), QMetaType::Type::QString ) );
  layer->addAttribute( QgsField( QStringLiteral( "params_json" ), QMetaType::Type::QString ) );
  layer->addAttribute( QgsField( QStringLiteral( "schema_version" ), QMetaType::Type::Int ) );
  layer->updateFields();

  const auto append = [&layer, error]( const QString &path, const QString &role ) {
    QString resolved;
    const QVector<paleo::ImportedConstraintRecord> records =
        paleo::readConstraintImportFeatures( path, role, &resolved, error );
    if ( records.isEmpty() )
      return false;
    for ( const paleo::ImportedConstraintRecord &record : records )
    {
      QgsFeature feature( layer->fields() );
      feature.setGeometry( QgsGeometry::fromWkt( record.wkt ) );
      feature.setAttribute( QStringLiteral( "id" ),
                            record.params.value( QStringLiteral( "sourceId" ) ).toString() );
      feature.setAttribute( QStringLiteral( "type" ), record.type );
      QVariantMap params = record.params;
      params.insert( QStringLiteral( "schemaVersion" ), 1 );
      feature.setAttribute( QStringLiteral( "params_json" ),
                            QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( params ) )
                                                   .toJson( QJsonDocument::Compact ) ) );
      feature.setAttribute( QStringLiteral( "schema_version" ), 1 );
      if ( !layer->addFeature( feature ) )
      {
        if ( error )
          *error = QStringLiteral( "memory layer addFeature failed" );
        return false;
      }
    }
    return true;
  };
  bool ok = true;
  if ( !directionsPath.isEmpty() )
    ok = append( directionsPath, QStringLiteral( "direction" ) ) && ok;
  if ( ok && !barriersPath.isEmpty() )
    ok = append( barriersPath, QStringLiteral( "barrier" ) ) && ok;
  if ( !ok )
    return nullptr;
  layer->commitChanges();
  return layer;
}

double jsonNumber( const QJsonValue &value )
{
  return value.isDouble() ? value.toDouble() : std::numeric_limits<double>::quiet_NaN();
}

// Python repr(float) 风格：能 round-trip 的最短十进制（level key 用）。
QString pyRepr( double v )
{
  if ( !std::isfinite( v ) )
    return QStringLiteral( "nan" );
  for ( int p = 1; p <= 17; ++p )
  {
    const QString s = QString::number( v, 'g', p );
    if ( s.toDouble() == v )
      return s;
  }
  return QString::number( v, 'g', 17 );
}

QJsonArray linesToJson( const std::vector<paleo::singlefactor::LineStringPoints> &lines )
{
  QJsonArray out;
  for ( const paleo::singlefactor::LineStringPoints &line : lines )
  {
    QJsonArray pts;
    for ( const paleo::singlefactor::Point2 &p : line )
      pts << QJsonArray{ p.x, p.y };
    out << pts;
  }
  return out;
}

QJsonObject levelMapToJson(
    const std::vector<paleo::singlefactor::ContourLevelLines> &byLevel )
{
  QJsonObject out;
  for ( const paleo::singlefactor::ContourLevelLines &entry : byLevel )
    out.insert( pyRepr( entry.level ), linesToJson( entry.lines ) );
  return out;
}

// ---- 对称 Hausdorff（点→折线最近距离的最大值，双向取大） ----
double pointToSegmentDistance( double px, double py, double ax, double ay, double bx,
                               double by )
{
  const double dx = bx - ax;
  const double dy = by - ay;
  const double len2 = dx * dx + dy * dy;
  double t = 0.0;
  if ( len2 > 0.0 )
    t = std::clamp( ( ( px - ax ) * dx + ( py - ay ) * dy ) / len2, 0.0, 1.0 );
  const double cx = ax + t * dx;
  const double cy = ay + t * dy;
  return std::hypot( px - cx, py - cy );
}

double directedHausdorff( const paleo::singlefactor::LineStringPoints &a,
                          const paleo::singlefactor::LineStringPoints &b )
{
  double worst = 0.0;
  if ( b.empty() )
    return std::numeric_limits<double>::infinity();
  for ( const paleo::singlefactor::Point2 &p : a )
  {
    double best = std::numeric_limits<double>::infinity();
    for ( std::size_t i = 0; i + 1 < b.size(); ++i )
      best = std::min( best, pointToSegmentDistance( p.x, p.y, b[i].x, b[i].y, b[i + 1].x,
                                                     b[i + 1].y ) );
    if ( b.size() == 1 )
      best = std::hypot( p.x - b[0].x, p.y - b[0].y );
    worst = std::max( worst, best );
  }
  return worst;
}

double symmetricHausdorff( const paleo::singlefactor::LineStringPoints &a,
                           const paleo::singlefactor::LineStringPoints &b )
{
  return std::max( directedHausdorff( a, b ), directedHausdorff( b, a ) );
}

paleo::singlefactor::LineStringPoints jsonLine( const QJsonArray &line )
{
  paleo::singlefactor::LineStringPoints out;
  for ( const QJsonValue &p : line )
  {
    const QJsonArray xy = p.toArray();
    out.push_back( { xy.at( 0 ).toDouble(), xy.at( 1 ).toDouble() } );
  }
  return out;
}

bool jsonClosed( const QJsonArray &line )
{
  if ( line.size() < 2 )
    return false;
  const QJsonArray first = line.first().toArray();
  const QJsonArray last = line.last().toArray();
  return first == last;
}

} // namespace

class tst_singlefactor_fieldcontours : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir mDir;

  void ensurePaleo()
  {
    if ( !QgsApplication::processingRegistry()->providerById( QStringLiteral( "paleo" ) ) )
      QgsApplication::processingRegistry()->addProvider( new PaleoProvider() );
    QVERIFY( QgsApplication::processingRegistry()->algorithmById(
        QStringLiteral( "paleo:paleo_structural_idw" ) ) );
  }

  struct StructuralRun
  {
    QJsonObject qc;
    QJsonObject model;
    QString rasterPath;
    QString structuralPath;
    qint64 elapsedMs = 0;
    bool ok = false;
  };

  StructuralRun runStructural( const QString &wellsPath, const QString &boundaryPath,
                               const QString &directionsPath, const QString &barriersPath,
                               int resolution )
  {
    StructuralRun run;
    auto wells = std::make_unique<QgsVectorLayer>( wellsPath, QStringLiteral( "wells" ),
                                                   QStringLiteral( "ogr" ) );
    auto boundary = std::make_unique<QgsVectorLayer>( boundaryPath, QStringLiteral( "boundary" ),
                                                      QStringLiteral( "ogr" ) );
    if ( !wells->isValid() || !boundary->isValid() )
      return run;
    QString importErr;
    std::unique_ptr<QgsVectorLayer> constraints =
        constraintLayer( directionsPath, barriersPath, &importErr );
    if ( !constraints )
    {
      qWarning() << "constraint import failed:" << importErr;
      return run;
    }

    const QString outPath = mDir.filePath(
        QStringLiteral( "structural-%1.tif" ).arg( QUuid::createUuid().toString( QUuid::Id128 ) ) );
    QVariantMap params;
    params.insert( QStringLiteral( "WELLS" ),
                   QVariant::fromValue( static_cast<QgsMapLayer *>( wells.get() ) ) );
    params.insert( QStringLiteral( "WELL_ID_FIELD" ), QStringLiteral( "well_id" ) );
    params.insert( QStringLiteral( "VALUE_FIELD" ), QStringLiteral( "sand_ratio" ) );
    params.insert( QStringLiteral( "FACTOR_MODE" ), QStringLiteral( "direct" ) );
    params.insert( QStringLiteral( "VALUE_RANGE" ), QStringLiteral( "ratio_0_1" ) );
    params.insert( QStringLiteral( "BOUNDARY" ),
                   QVariant::fromValue( static_cast<QgsMapLayer *>( boundary.get() ) ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ),
                   QVariant::fromValue( static_cast<QgsMapLayer *>( constraints.get() ) ) );
    params.insert( QStringLiteral( "GRID_RESOLUTION" ), resolution );
    params.insert( QStringLiteral( "OUTPUT" ), outPath );

    QgsProcessingContext ctx;
    QgsProcessingFeedback fb;
    const QgsProcessingAlgorithm *alg = QgsApplication::processingRegistry()->algorithmById(
        QStringLiteral( "paleo:paleo_structural_idw" ) );
    if ( !alg )
      return run;
    QElapsedTimer timer;
    timer.start();
    const QVariantMap results = alg->run( params, ctx, &fb );
    run.elapsedMs = timer.elapsed();
    if ( results.isEmpty() )
    {
      qWarning() << "structural run failed:" << fb.textLog();
      return run;
    }
    run.rasterPath = results.value( QStringLiteral( "OUTPUT" ) ).toString();
    const QString base = run.rasterPath.left( run.rasterPath.lastIndexOf( QLatin1Char( '.' ) ) );
    run.structuralPath = base + QStringLiteral( ".structural.json" );
    run.qc = readJson( base + QStringLiteral( ".qc.json" ) );
    run.model = readJson( run.structuralPath );
    run.ok = !run.qc.isEmpty() && !run.model.isEmpty();
    return run;
  }

  // 一阶段对比：initial / contours 各 level 的条数、开合、对称 Hausdorff。
  void compareStage( const QString &stageName, const QJsonObject &expected,
                     const std::vector<paleo::singlefactor::ContourLevelLines> &actual,
                     double tol )
  {
    const QStringList keys = expected.keys();
    for ( const QString &key : keys )
    {
      const double level = key.toDouble();
      const QJsonArray expLines = expected.value( key ).toArray();
      const paleo::singlefactor::ContourLevelLines *found = nullptr;
      for ( const paleo::singlefactor::ContourLevelLines &entry : actual )
        if ( std::abs( entry.level - level ) < 1e-9 )
          found = &entry;
      QVERIFY2( found,
                qPrintable( QStringLiteral( "%1: level %2 missing" ).arg( stageName, key ) ) );
      QCOMPARE( found->lines.size(), static_cast<std::size_t>( expLines.size() ) );
      for ( int i = 0; i < expLines.size() && i < static_cast<int>( found->lines.size() );
            ++i )
      {
        const QJsonArray exp = expLines.at( i ).toArray();
        const paleo::singlefactor::LineStringPoints &act = found->lines.at( i );
        // 开合一致性
        QCOMPARE( act.front().x == act.back().x && act.front().y == act.back().y,
                  jsonClosed( exp ) );
        const paleo::singlefactor::LineStringPoints expLine = jsonLine( exp );
        const double d = symmetricHausdorff( act, expLine );
        QVERIFY2( d <= tol,
                  qPrintable( QStringLiteral( "%1 level %2 line %3 hausdorff %4 > %5" )
                                  .arg( stageName, key )
                                  .arg( i )
                                  .arg( d )
                                  .arg( tol ) ) );
      }
    }
  }

  void compareContours( const QJsonObject &golden,
                        const paleo::singlefactor::FieldContourResult &result,
                        const QVector<double> &levels, double step )
  {
    QCOMPARE( result.status, paleo::singlefactor::Status::Ok );
    QCOMPARE( static_cast<int>( result.contours.size() ), levels.size() );
    for ( int i = 0; i < levels.size() && i < static_cast<int>( result.contours.size() );
          ++i )
      QVERIFY( std::abs( result.contours.at( i ).level - levels.at( i ) ) < 1e-9 );

    const double tol = 1e-6 * step;
    compareStage( QStringLiteral( "initial" ),
                  golden.value( QStringLiteral( "initial_contours" ) ).toObject(),
                  result.initial, tol );
    compareStage( QStringLiteral( "final" ),
                  golden.value( QStringLiteral( "contours" ) ).toObject(), result.contours,
                  tol );

    const QJsonObject detour = golden.value( QStringLiteral( "local_detour" ) ).toObject();
    QCOMPARE( result.detourApplied, detour.value( QStringLiteral( "triggered" ) ).toBool() );
    const QJsonArray expCrossed = detour.value( QStringLiteral( "crossed_barriers" ) ).toArray();
    QCOMPARE( static_cast<int>( result.crossedBarrierIndices.size() ), expCrossed.size() );
    for ( int i = 0;
          i < expCrossed.size() && i < static_cast<int>( result.crossedBarrierIndices.size() );
          ++i )
      QCOMPARE( result.crossedBarrierIndices.at( i ), expCrossed.at( i ).toInt() );

    const QJsonObject info = detour.value( QStringLiteral( "info" ) ).toObject();
    QCOMPARE( result.workInfo.coreValue,
              info.value( QStringLiteral( "core_value" ) ).toDouble() );
    QVERIFY( std::abs( result.workInfo.bufferHalfWidth -
                       info.value( QStringLiteral( "buffer_half_width" ) ).toDouble() ) <
             1e-9 );
    QVERIFY( std::abs( result.workInfo.numericalGuard -
                       info.value( QStringLiteral( "numerical_guard" ) ).toDouble() ) <=
             1e-9 * std::max( 1.0, std::abs(
                                       info.value( QStringLiteral( "numerical_guard" ) )
                                           .toDouble() ) ) );
    QVERIFY( std::abs( result.workInfo.transitionDistance -
                       info.value( QStringLiteral( "transition_distance" ) ).toDouble() ) <
             1e-9 );
    QCOMPARE( result.workInfo.modifiedCells,
              info.value( QStringLiteral( "modified_cells" ) ).toInt() );
  }

  void writeContourDump( const QString &caseName, const QJsonObject &model,
                         const paleo::singlefactor::FieldContourResult &result )
  {
    const QString dir = qEnvironmentVariable( "PALEO_STRUCTURAL_DUMP_DIR" );
    if ( dir.isEmpty() )
      return;
    QDir().mkpath( dir );
    QJsonObject dump;
    QJsonObject grid;
    const QJsonObject modelGrid = model.value( QStringLiteral( "grid" ) ).toObject();
    grid.insert( QStringLiteral( "x" ), modelGrid.value( QStringLiteral( "x" ) ) );
    grid.insert( QStringLiteral( "y" ), modelGrid.value( QStringLiteral( "y" ) ) );
    grid.insert( QStringLiteral( "z" ), modelGrid.value( QStringLiteral( "z" ) ) );
    grid.insert( QStringLiteral( "valid_mask" ),
                 model.value( QStringLiteral( "valid_mask" ) ) );
    dump.insert( QStringLiteral( "grid" ), grid );
    QJsonArray levelsUsed;
    for ( const paleo::singlefactor::ContourLevelLines &entry : result.contours )
      levelsUsed << entry.level;
    dump.insert( QStringLiteral( "levels_used" ), levelsUsed );
    dump.insert( QStringLiteral( "initial_contours" ), levelMapToJson( result.initial ) );
    dump.insert( QStringLiteral( "contours" ), levelMapToJson( result.contours ) );
    QFile file( QDir( dir ).filePath( caseName + QStringLiteral( "_contours.json" ) ) );
    if ( file.open( QIODevice::WriteOnly ) )
      file.write( QJsonDocument( dump ).toJson() );
  }

  bool extractCase( const QString &caseName, const QString &wellsPath,
                    const QString &boundaryPath, const QString &directionsPath,
                    const QString &barriersPath, int resolution,
                    const QJsonObject &golden )
  {
    const StructuralRun run =
        runStructural( wellsPath, boundaryPath, directionsPath, barriersPath, resolution );
    if ( !run.ok )
      return false;
    QString err;
    paleo::singlefactor::FieldContourSurface surface;
    if ( !paleo::singlefactor::loadFieldContourSurface( run.model.toVariantMap(), &surface,
                                                      &err ) )
    {
      qWarning() << "surface load failed:" << err;
      return false;
    }
    const QJsonArray expLevels =
        golden.value( QStringLiteral( "levels" ) ).toObject().value( QStringLiteral( "levels" ) )
            .toArray();
    std::vector<double> levels;
    for ( const QJsonValue &v : expLevels )
      levels.push_back( v.toDouble() );

    QElapsedTimer timer;
    timer.start();
    const paleo::singlefactor::FieldContourResult result =
        paleo::singlefactor::extractFieldContours( surface, levels, true );
    const qint64 elapsed = timer.elapsed();
    if ( result.status != paleo::singlefactor::Status::Ok )
      qWarning() << "extract failed:" << QString::fromStdString( result.message );
    writeContourDump( caseName, run.model, result );

    QVector<double> levelVector;
    for ( const double level : levels )
      levelVector << level;
    const double dx = surface.xs.size() > 1 ? surface.xs.at( 1 ) - surface.xs.at( 0 ) : 1.0;
    const double dy = surface.ys.size() > 1 ? surface.ys.at( 1 ) - surface.ys.at( 0 ) : 1.0;
    const double step = std::max( std::abs( dx ), std::abs( dy ) );
    compareContours( golden, result, levelVector, step );
    qInfo() << qPrintable( caseName ) << "contour extraction elapsed" << elapsed << "ms"
            << "(surface" << run.elapsedMs << "ms )";
    return true;
  }

private slots:
  void syntheticGolden();
  void enpingGolden();
  void structuralContoursEndToEnd();
};

void tst_singlefactor_fieldcontours::syntheticGolden()
{
  ensurePaleo();
  const QJsonObject golden =
      readJson( fixturePath( QStringLiteral( "structural_synthetic_golden.json" ) ) );
  QVERIFY( !golden.isEmpty() );
  const QString base = fixturePath( QStringLiteral( "structural_synthetic" ) );
  QVERIFY( extractCase( QStringLiteral( "synthetic" ), base + QStringLiteral( "/wells.shp" ),
                        base + QStringLiteral( "/boundary.shp" ),
                        base + QStringLiteral( "/directions.shp" ),
                        base + QStringLiteral( "/barriers.shp" ), 120, golden ) );
}

void tst_singlefactor_fieldcontours::enpingGolden()
{
  const QString packageEnv = qEnvironmentVariable( "PALEO_ENPING_PACKAGE" );
  const QString goldenEnv = qEnvironmentVariable( "PALEO_ENPING_GOLDEN" );
  if ( packageEnv.isEmpty() || goldenEnv.isEmpty() )
    QSKIP( "PALEO_ENPING_PACKAGE/PALEO_ENPING_GOLDEN not set" );
  ensurePaleo();
  QString d03 = packageEnv;
  if ( QDir( packageEnv ).exists( QStringLiteral( "03_点位与成图边界" ) ) )
    d03 = QDir( packageEnv ).filePath( QStringLiteral( "03_点位与成图边界" ) );
  const QString wells = QDir( d03 ).filePath( QStringLiteral( "外委恩平一二段_井位标注点.shp" ) );
  const QString boundary = QDir( d03 ).filePath( QStringLiteral( "恩平一二段_成图范围.shp" ) );
  const QString directions = QDir( d03 ).filePath( QStringLiteral( "方向线_解释草案.shp" ) );
  const QString barriers =
      QDir( d03 ).filePath( QStringLiteral( "打断线_解释性分区草案.shp" ) );
  for ( const QString &path : { wells, boundary, directions, barriers } )
    QVERIFY2( QFileInfo::exists( path ), qPrintable( path ) );
  const QJsonObject golden = readJson( goldenEnv );
  QVERIFY( !golden.isEmpty() );
  QVERIFY( extractCase( QStringLiteral( "enping" ), wells, boundary, directions, barriers,
                        339, golden ) );
}

// 端到端：声明井点/边界/约束 → structural_idw 因素 → generateContours(0=自动)
// → contours.<h>.<key> GPKG 线层（字段 + 要素数 + 属性语义）。
void tst_singlefactor_fieldcontours::structuralContoursEndToEnd()
{
  ensurePaleo();
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  DataCatalog catalog;
  QVERIFY( catalog.open( dir.path() ) );
  QgisProjectService projectSvc;
  QVERIFY( projectSvc.createProject( dir.filePath( QStringLiteral( "proj.qgz" ) ) ) );
  LayerManifest manifest( dir.filePath( QStringLiteral( "project.sqlite" ) ) );
  QVERIFY( manifest.open() );
  QgisLayerService layers( &projectSvc, &manifest );
  PaleoProjectStore store;
  QgisProcessingService proc( &store );

  const auto decl = []( const QString &layerId, const QString &horizon, const QString &type,
                        const QString &source ) {
    LayerDeclaration d;
    d.layerId = layerId;
    d.horizon = horizon;
    d.type = type;
    d.source = source;
    d.group = QStringLiteral( "00_Test" );
    return d;
  };

  const QString base = fixturePath( QStringLiteral( "structural_synthetic" ) );
  QString err;
  QVERIFY2( layers.declare( decl( QStringLiteral( "wells.T1" ), QStringLiteral( "T1" ),
                                  QStringLiteral( "vector" ),
                                  base + QStringLiteral( "/wells.shp" ) ),
                            &err ),
            qPrintable( err ) );
  QVERIFY2( layers.declare( decl( QStringLiteral( "boundary.T1" ), QStringLiteral( "T1" ),
                                  QStringLiteral( "vector" ),
                                  base + QStringLiteral( "/boundary.shp" ) ),
                            &err ),
            qPrintable( err ) );

  // 约束图层落盘 GPKG（import 管线同款列）再声明为 constraints.T1。
  std::unique_ptr<QgsVectorLayer> constraints =
      constraintLayer( base + QStringLiteral( "/directions.shp" ),
                       base + QStringLiteral( "/barriers.shp" ), &err );
  QVERIFY2( constraints, qPrintable( err ) );
  const QString constraintsPath = dir.filePath( QStringLiteral( "constraints.gpkg" ) );
  QgsVectorFileWriter::SaveVectorOptions options;
  options.driverName = QStringLiteral( "GPKG" );
  options.layerName = QStringLiteral( "features" );
  options.actionOnExistingFile = QgsVectorFileWriter::CreateOrOverwriteFile;
  QString writeErr;
  QCOMPARE( QgsVectorFileWriter::writeAsVectorFormatV3(
                constraints.get(), constraintsPath, QgsProject::instance()->transformContext(),
                options, &writeErr ),
            QgsVectorFileWriter::NoError );
  QVERIFY2( layers.declare( decl( QStringLiteral( "constraints.T1" ), QStringLiteral( "T1" ),
                                  QStringLiteral( "vector" ), constraintsPath ),
                            &err ),
            qPrintable( err ) );

  ConstraintWorkflow wf( &proc, &layers );
  wf.setCatalog( &catalog, dir.path() );

  QVariantMap params;
  params.insert( QStringLiteral( "method" ), QStringLiteral( "structural_idw" ) );
  params.insert( QStringLiteral( "field" ), QStringLiteral( "sand_ratio" ) );
  params.insert( QStringLiteral( "wellIdField" ), QStringLiteral( "well_id" ) );
  params.insert( QStringLiteral( "boundaryLayerId" ), QStringLiteral( "boundary.T1" ) );
  params.insert( QStringLiteral( "gridResolution" ), 120 );
  params.insert( QStringLiteral( "valueRange" ), QStringLiteral( "ratio_0_1" ) );
  QVERIFY2( wf.generateFactor( QStringLiteral( "T1" ), QStringLiteral( "sandthick" ), params,
                               &err ),
            qPrintable( err ) );

  // interval 0 → 自动等值距 → structural 分支（不经过 GDAL contour）。
  QVERIFY2( wf.generateContours( QStringLiteral( "T1" ),
                                 QStringLiteral( "factor.T1.sandthick" ), 0.0, &err ),
            qPrintable( err ) );

  const LayerDeclaration *contourDecl = nullptr;
  for ( const LayerDeclaration &d : layers.declared() )
    if ( d.layerId == QStringLiteral( "contours.T1.sandthick" ) )
      contourDecl = new LayerDeclaration( d );
  QVERIFY( contourDecl );
  QCOMPARE( contourDecl->type, QStringLiteral( "vector" ) );
  QCOMPARE( contourDecl->group, QStringLiteral( "04_SingleFactor/Contours" ) );

  QgsMapLayer *mapLayer = layers.instantiate( QStringLiteral( "contours.T1.sandthick" ), &err );
  QVERIFY2( mapLayer, qPrintable( err ) );
  auto *vectorLayer = qobject_cast<QgsVectorLayer *>( mapLayer );
  QVERIFY( vectorLayer );
  QVERIFY( vectorLayer->isValid() );
  QCOMPARE( vectorLayer->wkbType(), Qgis::WkbType::LineString );

  for ( const QString &name : { QStringLiteral( "level" ), QStringLiteral( "closed" ),
                                QStringLiteral( "closure_type" ),
                                QStringLiteral( "value_source" ),
                                QStringLiteral( "geometry_policy" ),
                                QStringLiteral( "buffer_policy" ),
                                QStringLiteral( "buffer_half_width" ),
                                QStringLiteral( "detour_applied" ) } )
    QVERIFY2( vectorLayer->fields().indexOf( name ) >= 0, qPrintable( name ) );

  // 合成 golden 的 final contours 总条数 = 16；detour 已触发。
  const QJsonObject golden =
      readJson( fixturePath( QStringLiteral( "structural_synthetic_golden.json" ) ) );
  const QJsonObject expectedContours =
      golden.value( QStringLiteral( "contours" ) ).toObject();
  int expectedCount = 0;
  for ( const QString &key : expectedContours.keys() )
    expectedCount += expectedContours.value( key ).toArray().size();
  QCOMPARE( vectorLayer->featureCount(), static_cast<long long>( expectedCount ) );

  const int idxLevel = vectorLayer->fields().indexOf( QStringLiteral( "level" ) );
  const int idxClosed = vectorLayer->fields().indexOf( QStringLiteral( "closed" ) );
  const int idxClosure = vectorLayer->fields().indexOf( QStringLiteral( "closure_type" ) );
  const int idxPolicy = vectorLayer->fields().indexOf( QStringLiteral( "value_source" ) );
  const int idxDetour = vectorLayer->fields().indexOf( QStringLiteral( "detour_applied" ) );
  int closedCount = 0;
  int detourCount = 0;
  QgsFeature f;
  QgsFeatureIterator it = vectorLayer->getFeatures();
  while ( it.nextFeature( f ) )
  {
    QVERIFY( idxLevel < 0 || f.attribute( idxLevel ).isValid() );
    QCOMPARE( f.attribute( idxPolicy ).toString(),
              QStringLiteral( "local_interpretive_detour" ) );
    const QString closure = f.attribute( idxClosure ).toString();
    QVERIFY( closure == QStringLiteral( "interior" ) || closure == QStringLiteral( "open" ) );
    if ( f.attribute( idxClosed ).toInt() == 1 )
      ++closedCount;
    if ( f.attribute( idxDetour ).toInt() == 1 )
      ++detourCount;
  }
  QCOMPARE( detourCount, static_cast<int>( vectorLayer->featureCount() ) );
  QVERIFY( closedCount > 0 );
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true );
  app.initQgis();
  GDALAllRegister();
  tst_singlefactor_fieldcontours tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_singlefactor_fieldcontours.moc"
