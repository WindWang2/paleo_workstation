// 层：数据（测试壳。被测对象是等值线服务对分析栅格的只读提取。）
#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QPointF>
#include <QTemporaryDir>
#include <QVector>

#include <cmath>
#include <limits>
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include "../src/qgis/factorcontour.h"

namespace
{

struct ContourLine
{
  double level = 0;
  QVector<QPointF> points;
};

bool writeGrid( const QString &path, int cols, int rows, double originX, double originY, double cell,
                const QVector<float> &values, float nodata )
{
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

void appendLine( OGRGeometryH geometry, double level, QVector<ContourLine> *out )
{
  if ( !geometry || !out )
    return;
  const OGRwkbGeometryType type = wkbFlatten( OGR_G_GetGeometryType( geometry ) );
  if ( type == wkbLineString )
  {
    ContourLine line;
    line.level = level;
    const int count = OGR_G_GetPointCount( geometry );
    for ( int i = 0; i < count; ++i )
      line.points.push_back( QPointF( OGR_G_GetX( geometry, i ), OGR_G_GetY( geometry, i ) ) );
    if ( line.points.size() >= 2 )
      out->push_back( line );
    return;
  }
  if ( type == wkbMultiLineString || type == wkbGeometryCollection )
  {
    const int count = OGR_G_GetGeometryCount( geometry );
    for ( int i = 0; i < count; ++i )
      appendLine( OGR_G_GetGeometryRef( geometry, i ), level, out );
  }
}

QVector<ContourLine> readContours( const QString &path )
{
  QVector<ContourLine> lines;
  GDALDatasetH dataset = GDALOpenEx( path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr );
  if ( !dataset )
    return lines;
  OGRLayerH layer = GDALDatasetGetLayerByName( dataset, "contours" );
  if ( !layer )
  {
    GDALClose( dataset );
    return lines;
  }
  const int elev = OGR_L_FindFieldIndex( layer, "ELEV", 1 );
  OGR_L_ResetReading( layer );
  while ( OGRFeatureH feature = OGR_L_GetNextFeature( layer ) )
  {
    const double level = elev >= 0 ? OGR_F_GetFieldAsDouble( feature, elev ) : 0.0;
    appendLine( OGR_F_GetGeometryRef( feature ), level, &lines );
    OGR_F_Destroy( feature );
  }
  GDALClose( dataset );
  return lines;
}

void sampleSegment( const QPointF &a, const QPointF &b, double spacing, QVector<QPointF> *out )
{
  const double length = std::hypot( b.x() - a.x(), b.y() - a.y() );
  const int steps = std::max( 1, static_cast<int>( std::ceil( length / spacing ) ) );
  for ( int step = 0; step <= steps; ++step )
  {
    const double t = static_cast<double>( step ) / static_cast<double>( steps );
    out->push_back( QPointF( a.x() + t * ( b.x() - a.x() ), a.y() + t * ( b.y() - a.y() ) ) );
  }
}

} // namespace

class SingleFactorContourTests : public QObject
{
  Q_OBJECT

private slots:
  void constantFieldHasNoFalseLoops();
  void rampSamplesStayOnTheLevel();
  void contoursDoNotCrossANodataHole();
  void rampEndsStayOpen();
  void coneStaysNearTheCircle();
  void fixedLevelApiRejectsEmptyAndNonFinite();
};

void SingleFactorContourTests::constantFieldHasNoFalseLoops()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString raster = dir.filePath( QStringLiteral( "constant.tif" ) );
  QVERIFY( writeGrid( raster, 8, 8, 0, 80, 10, QVector<float>( 64, 5.0f ), -9999.0f ) );
  const QString fixed = dir.filePath( QStringLiteral( "fixed.gpkg" ) );
  const QString interval = dir.filePath( QStringLiteral( "interval.gpkg" ) );
  QString err;
  QVERIFY2( FactorContourService::generateFixedContours( raster, fixed, { 1.0, 3.0, 7.0 }, &err ),
            qPrintable( err ) );
  QVERIFY2( FactorContourService::generateContours( raster, interval, 1.0, &err ), qPrintable( err ) );
  QCOMPARE( readContours( fixed ).size(), 0 );
  QCOMPARE( readContours( interval ).size(), 0 );
}

void SingleFactorContourTests::rampSamplesStayOnTheLevel()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const int cols = 40;
  const int rows = 20;
  const double cell = 10.0;
  QVector<float> values( cols * rows );
  for ( int r = 0; r < rows; ++r )
  {
    for ( int c = 0; c < cols; ++c )
      values[r * cols + c] = static_cast<float>( ( c + 0.5 ) * cell );
  }
  const QString raster = dir.filePath( QStringLiteral( "ramp.tif" ) );
  QVERIFY( writeGrid( raster, cols, rows, 0, rows * cell, cell, values, -9999.0f ) );
  const QString lines = dir.filePath( QStringLiteral( "ramp.gpkg" ) );
  QString err;
  QVERIFY2( FactorContourService::generateFixedContours( raster, lines, { 50.0, 100.0 }, &err ), qPrintable( err ) );
  const QVector<ContourLine> contours = readContours( lines );
  QVERIFY( contours.size() >= 2 );
  const double spacing = cell / 4.0;
  const double tolerance = std::max( 1e-5 * 400.0, 0.02 * 50.0 );
  int samples = 0;
  for ( const ContourLine &line : contours )
  {
    QVERIFY( qFuzzyCompare( line.level + 1.0, 51.0 ) || qFuzzyCompare( line.level + 1.0, 101.0 ) );
    for ( int i = 1; i < line.points.size(); ++i )
    {
      QVector<QPointF> probed;
      sampleSegment( line.points[i - 1], line.points[i], spacing, &probed );
      for ( const QPointF &point : probed )
      {
        QVERIFY2( std::abs( point.x() - line.level ) <= tolerance, qPrintable( QString::number( point.x() ) ) );
        ++samples;
      }
    }
  }
  QVERIFY( samples > 0 );
}

void SingleFactorContourTests::contoursDoNotCrossANodataHole()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const int cols = 40;
  const int rows = 40;
  const double cell = 10.0;
  const float nodata = -9999.0f;
  QVector<float> values( cols * rows );
  for ( int r = 0; r < rows; ++r )
  {
    for ( int c = 0; c < cols; ++c )
      values[r * cols + c] = static_cast<float>( ( c + 0.5 ) * cell );
  }
  for ( int r = 15; r <= 24; ++r )
  {
    for ( int c = 18; c <= 22; ++c )
      values[r * cols + c] = nodata;
  }
  const QString raster = dir.filePath( QStringLiteral( "hole.tif" ) );
  QVERIFY( writeGrid( raster, cols, rows, 0, rows * cell, cell, values, nodata ) );
  const QString lines = dir.filePath( QStringLiteral( "hole.gpkg" ) );
  QString err;
  QVERIFY2( FactorContourService::generateFixedContours( raster, lines, { 205.0 }, &err ), qPrintable( err ) );
  const QVector<ContourLine> contours = readContours( lines );
  QVERIFY( !contours.isEmpty() );
  const double holeLeft = 18 * cell + 1.0;
  const double holeRight = 23 * cell - 1.0;
  const double holeTop = rows * cell - 15 * cell - 1.0;
  const double holeBottom = rows * cell - 25 * cell + 1.0;
  for ( const ContourLine &line : contours )
  {
    for ( int i = 1; i < line.points.size(); ++i )
    {
      const QPointF mid( ( line.points[i - 1].x() + line.points[i].x() ) * 0.5,
                         ( line.points[i - 1].y() + line.points[i].y() ) * 0.5 );
      const bool inside = mid.x() > holeLeft && mid.x() < holeRight && mid.y() < holeTop && mid.y() > holeBottom;
      QVERIFY2( !inside, qPrintable( QStringLiteral( "%1,%2" ).arg( mid.x() ).arg( mid.y() ) ) );
    }
  }
}

void SingleFactorContourTests::rampEndsStayOpen()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const int cols = 20;
  const int rows = 10;
  const double cell = 10.0;
  QVector<float> values( cols * rows );
  for ( int r = 0; r < rows; ++r )
  {
    for ( int c = 0; c < cols; ++c )
      values[r * cols + c] = static_cast<float>( ( c + 0.5 ) * cell );
  }
  const QString raster = dir.filePath( QStringLiteral( "open.tif" ) );
  QVERIFY( writeGrid( raster, cols, rows, 0, rows * cell, cell, values, -9999.0f ) );
  const QString lines = dir.filePath( QStringLiteral( "open.gpkg" ) );
  QString err;
  QVERIFY2( FactorContourService::generateFixedContours( raster, lines, { 100.0 }, &err ), qPrintable( err ) );
  const QVector<ContourLine> contours = readContours( lines );
  QVERIFY( !contours.isEmpty() );
  bool open = false;
  for ( const ContourLine &line : contours )
  {
    if ( QLineF( line.points.front(), line.points.back() ).length() > cell )
      open = true;
  }
  QVERIFY( open );
}

void SingleFactorContourTests::coneStaysNearTheCircle()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const int cols = 41;
  const int rows = 41;
  const double cell = 10.0;
  const double originX = 0;
  const double originY = rows * cell;
  const double centerX = originX + cols * cell * 0.5;
  const double centerY = originY - rows * cell * 0.5;
  QVector<float> values( cols * rows );
  for ( int r = 0; r < rows; ++r )
  {
    for ( int c = 0; c < cols; ++c )
    {
      const double x = originX + ( c + 0.5 ) * cell;
      const double y = originY - ( r + 0.5 ) * cell;
      values[r * cols + c] = static_cast<float>( std::hypot( x - centerX, y - centerY ) );
    }
  }
  const QString raster = dir.filePath( QStringLiteral( "cone.tif" ) );
  QVERIFY( writeGrid( raster, cols, rows, originX, originY, cell, values, -9999.0f ) );
  const QString lines = dir.filePath( QStringLiteral( "cone.gpkg" ) );
  QString err;
  QVERIFY2( FactorContourService::generateFixedContours( raster, lines, { 80.0 }, &err ), qPrintable( err ) );
  const QVector<ContourLine> contours = readContours( lines );
  QVERIFY( !contours.isEmpty() );
  const double limit = 0.25 * cell;
  for ( const ContourLine &line : contours )
  {
    for ( const QPointF &point : line.points )
    {
      const double radius = std::hypot( point.x() - centerX, point.y() - centerY );
      QVERIFY2( std::abs( radius - 80.0 ) <= limit, qPrintable( QString::number( radius ) ) );
    }
  }
}

void SingleFactorContourTests::fixedLevelApiRejectsEmptyAndNonFinite()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString raster = dir.filePath( QStringLiteral( "constant.tif" ) );
  QVERIFY( writeGrid( raster, 4, 4, 0, 40, 10, QVector<float>( 16, 5.0f ), -9999.0f ) );
  QString err;
  QVERIFY( !FactorContourService::generateContours( raster, dir.filePath( QStringLiteral( "bad.gpkg" ) ), 0.0,
                                                    &err ) );
  QVERIFY2( err.contains( QStringLiteral( "positive" ) ), qPrintable( err ) );
  err.clear();
  QVERIFY( !FactorContourService::generateFixedContours( raster, dir.filePath( QStringLiteral( "empty.gpkg" ) ), {},
                                                         &err ) );
  QVERIFY( !err.isEmpty() );
  err.clear();
  QVERIFY( !FactorContourService::generateFixedContours( raster, dir.filePath( QStringLiteral( "nan.gpkg" ) ),
                                                         { std::numeric_limits<double>::quiet_NaN() }, &err ) );
  QVERIFY2( err.contains( QStringLiteral( "finite" ) ), qPrintable( err ) );
}

int main( int argc, char *argv[] )
{
  GDALAllRegister();
  SingleFactorContourTests tests;
  return QTest::qExec( &tests, argc, argv );
}

#include "tst_singlefactor_contours.moc"
