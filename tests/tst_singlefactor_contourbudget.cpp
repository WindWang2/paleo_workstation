// 层：测试壳
#include <QtTest/QtTest>

#include <QFile>
#include <QTemporaryDir>
#include <QVector>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gdal.h>
#include <ogr_srs_api.h>

#include "algorithms/singlefactor/support.h"
#include "../src/qgis/factorcontour.h"

using namespace paleo::singlefactor;

namespace
{

constexpr int kCols = 512;
constexpr int kRows = 512;
constexpr double kCell = 10.0;
constexpr float kNodata = -9999.0f;

qint64 elapsedMs( std::chrono::steady_clock::time_point started )
{
  return std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() - started ).count();
}

bool writeGrid( const QString &path, int cols, int rows, double originX, double originY, double cell,
                const QVector<float> &values, float nodata )
{
  GDALAllRegister();
  GDALDriverH driver = GDALGetDriverByName( "GTiff" );
  if ( !driver || values.size() != cols * rows )
    return false;
  GDALDatasetH dataset = GDALCreate( driver, path.toUtf8().constData(), cols, rows, 1, GDT_Float32, nullptr );
  if ( !dataset )
    return false;
  double geoTransform[6] = { originX, cell, 0.0, originY, 0.0, -cell };
  if ( GDALSetGeoTransform( dataset, geoTransform ) != CE_None )
  {
    GDALClose( dataset );
    return false;
  }
  OGRSpatialReferenceH srs = OSRNewSpatialReference( nullptr );
  if ( !srs || OSRImportFromEPSG( srs, 3857 ) != OGRERR_NONE )
  {
    if ( srs )
      OSRDestroySpatialReference( srs );
    GDALClose( dataset );
    return false;
  }
  OSRSetAxisMappingStrategy( srs, OAMS_TRADITIONAL_GIS_ORDER );
  const CPLErr srsRc = GDALSetSpatialRef( dataset, srs );
  OSRDestroySpatialReference( srs );
  if ( srsRc != CE_None )
  {
    GDALClose( dataset );
    return false;
  }
  GDALRasterBandH band = GDALGetRasterBand( dataset, 1 );
  GDALSetRasterNoDataValue( band, nodata );
  if ( GDALRasterIO( band, GF_Write, 0, 0, cols, rows, const_cast<float *>( values.constData() ), cols, rows,
                     GDT_Float32, 0, 0 ) != CE_None )
  {
    GDALClose( dataset );
    return false;
  }
  GDALClose( dataset );
  return QFile::exists( path );
}

std::vector<double> tenLevels()
{
  std::vector<double> levels;
  levels.reserve( 10 );
  for ( int i = 0; i < 10; ++i )
    levels.push_back( 40.0 + 40.0 * static_cast<double>( i ) );
  return levels;
}

// 短对角段，横穿对应级别的竖直等值线，且整段落在 5120 域内。
std::vector<ConstraintLine> eightDetours( const std::vector<double> &levels )
{
  std::vector<ConstraintLine> lines;
  lines.reserve( 8 );
  for ( int i = 0; i < 8; ++i )
  {
    const double x = ( levels[static_cast<std::size_t>( i )] + 0.5 ) * kCell;
    const double y = 480.0 + 520.0 * static_cast<double>( i );
    ConstraintLine line;
    line.stableId = "detour-" + std::to_string( i );
    line.semantic = Semantic::CartographicDetour;
    line.enabled = true;
    line.points = { { x - 160.0, y }, { x + 160.0, y + 90.0 } };
    lines.push_back( std::move( line ) );
  }
  return lines;
}

std::vector<ContourPolyline> levelPolylines( const std::vector<double> &levels )
{
  std::vector<ContourPolyline> lines;
  lines.reserve( levels.size() );
  for ( double level : levels )
  {
    const double x = ( level + 0.5 ) * kCell;
    ContourPolyline poly;
    poly.level = level;
    poly.points = { { x, 20.0 }, { x, kRows * kCell - 20.0 } };
    lines.push_back( std::move( poly ) );
  }
  return lines;
}

struct PipelineResult
{
  bool detourOk = false;
  bool pipelineOk = false;
  qint64 ms = -1;
  int modifiedCells = 0;
  bool unchanged = true;
  QString error;
};

PipelineResult runDetourAndContours( const GridSpec &grid, const std::vector<double> &analysis,
                                     const std::vector<ConstraintLine> &stops,
                                     const std::vector<ContourPolyline> &contours, const std::vector<double> &levels,
                                     const QVector<double> &qtLevels, const QString &workPath,
                                     const QString &linePath )
{
  PipelineResult result;
  const auto started = std::chrono::steady_clock::now();
  const std::vector<std::uint8_t> valid;
  const WorkField work = buildCartographicWork( grid, analysis, valid, stops, contours, levels, 0.0 );
  if ( work.status != Status::Ok || static_cast<int>( work.values.size() ) != kCols * kRows )
  {
    const std::string text = work.message.empty() ? std::string( statusName( work.status ) ) : work.message;
    result.error = QString::fromStdString( text );
    return result;
  }
  result.detourOk = true;
  result.modifiedCells = work.modifiedCells;
  result.unchanged = work.unchanged;

  QVector<float> pixels( kCols * kRows );
  for ( int i = 0; i < pixels.size(); ++i )
  {
    const double value = work.values[static_cast<std::size_t>( i )];
    pixels[i] = std::isfinite( value ) ? static_cast<float>( value ) : kNodata;
  }
  if ( !writeGrid( workPath, kCols, kRows, grid.originX, grid.originY, kCell, pixels, kNodata ) )
  {
    result.error = QStringLiteral( "work grid write failed" );
    return result;
  }
  QString contourError;
  if ( !FactorContourService::generateFixedContours( workPath, linePath, qtLevels, &contourError ) )
  {
    result.error = contourError;
    return result;
  }
  result.pipelineOk = true;
  result.ms = elapsedMs( started );
  return result;
}

qint64 sampleFixedContours( const QString &raster, const QString &output, const QVector<double> &levels, QString *error )
{
  const auto started = std::chrono::steady_clock::now();
  QString local;
  const bool ok = FactorContourService::generateFixedContours( raster, output, levels, &local );
  const qint64 ms = elapsedMs( started );
  if ( !ok )
  {
    if ( error )
      *error = local;
    return -1;
  }
  return ms;
}

} // namespace

class SingleFactorContourBudgetTests : public QObject
{
  Q_OBJECT

private slots:
  void extractionPlusDetourMedianWithinTwentySeconds();
};

void SingleFactorContourBudgetTests::extractionPlusDetourMedianWithinTwentySeconds()
{
  QVector<float> ramp( kCols * kRows );
  std::vector<double> analysis( static_cast<std::size_t>( kCols * kRows ) );
  for ( int row = 0; row < kRows; ++row )
  {
    for ( int column = 0; column < kCols; ++column )
    {
      const int index = row * kCols + column;
      ramp[index] = static_cast<float>( column );
      analysis[static_cast<std::size_t>( index )] = static_cast<double>( column );
    }
  }

  GridSpec grid;
  grid.cols = kCols;
  grid.rows = kRows;
  grid.originX = 0.0;
  grid.originY = kRows * kCell;
  grid.pixelWidth = kCell;
  grid.pixelHeight = -kCell;
  grid.crs = "EPSG:3857";

  const std::vector<double> levels = tenLevels();
  QVector<double> qtLevels;
  qtLevels.reserve( static_cast<int>( levels.size() ) );
  for ( double level : levels )
    qtLevels.push_back( level );
  const std::vector<ConstraintLine> stops = eightDetours( levels );
  const std::vector<ContourPolyline> contourLines = levelPolylines( levels );
  QCOMPARE( grid.cols, 512 );
  QCOMPARE( grid.rows, 512 );
  QCOMPARE( qtLevels.size(), 10 );
  QCOMPARE( static_cast<int>( stops.size() ), 8 );

  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString rampPath = dir.filePath( QStringLiteral( "ramp.tif" ) );
  QVERIFY( writeGrid( rampPath, kCols, kRows, grid.originX, grid.originY, kCell, ramp, kNodata ) );

  int serial = 0;
  auto run = [&]() {
    const int id = serial++;
    return runDetourAndContours( grid, analysis, stops, contourLines, levels, qtLevels,
                                 dir.filePath( QStringLiteral( "work-%1.tif" ).arg( id ) ),
                                 dir.filePath( QStringLiteral( "lines-%1.gpkg" ).arg( id ) ) );
  };

  const PipelineResult warm = run();
  if ( !warm.detourOk )
  {
    QString contourError;
    const qint64 contourWarm =
        sampleFixedContours( rampPath, dir.filePath( QStringLiteral( "ramp-lines-warm.gpkg" ) ), qtLevels,
                             &contourError );
    QVERIFY2( contourWarm >= 0, qPrintable( contourError ) );
    std::vector<qint64> contourSamples;
    contourSamples.reserve( 3 );
    for ( int trial = 0; trial < 3; ++trial )
    {
      const qint64 ms = sampleFixedContours( rampPath, dir.filePath( QStringLiteral( "ramp-lines-%1.gpkg" ).arg( trial ) ),
                                              qtLevels, &contourError );
      QVERIFY2( ms >= 0, qPrintable( contourError ) );
      contourSamples.push_back( ms );
    }
    std::sort( contourSamples.begin(), contourSamples.end() );
    const qint64 contourMedian = contourSamples[contourSamples.size() / 2];
    const qint64 contourMax = contourSamples.back();
    qInfo().noquote() << QStringLiteral( "contour-only median=%1ms max=%2ms grid=512 (detour skipped)" )
                             .arg( contourMedian )
                             .arg( contourMax );
    const QByteArray message =
        QStringLiteral( "buildCartographicWork cannot run on a pure ramp without wells: %1. "
                        "Contour-only median %2 ms is not a §15 detour pass." )
            .arg( warm.error, QString::number( contourMedian ) )
            .toUtf8();
    QSKIP( message.constData() );
  }

  QVERIFY2( warm.pipelineOk, qPrintable( warm.error ) );
  QVERIFY2( !warm.unchanged && warm.modifiedCells > 0,
            "cartographic detour left the 512² ramp unchanged" );

  std::vector<qint64> samples;
  samples.reserve( 3 );
  int modifiedCells = warm.modifiedCells;
  for ( int trial = 0; trial < 3; ++trial )
  {
    const PipelineResult result = run();
    QVERIFY2( result.detourOk && result.pipelineOk, qPrintable( result.error ) );
    samples.push_back( result.ms );
    modifiedCells = result.modifiedCells;
  }
  std::sort( samples.begin(), samples.end() );
  const qint64 medianMs = samples[samples.size() / 2];
  const qint64 maxMs = samples.back();
  qInfo().noquote() << QStringLiteral( "contour budget median=%1ms max=%2ms grid=512 levels=10 constraints=8 "
                                       "modified=%3" )
                           .arg( medianMs )
                           .arg( maxMs )
                           .arg( modifiedCells );
  QVERIFY2( medianMs <= 20000, qPrintable( QStringLiteral( "median %1 ms exceeds 20000" ).arg( medianMs ) ) );
}

QTEST_MAIN( SingleFactorContourBudgetTests )
#include "tst_singlefactor_contourbudget.moc"
