// 层：数据（测试壳。被测对象是等值线服务对分析栅格的只读提取。）
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointF>
#include <QTemporaryDir>
#include <QVector>

#include <cmath>
#include <limits>
#include <gdal.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

#include "../src/qgis/factorcontour.h"
#include "../src/services/paleotaskservice.h"

namespace
{

struct ContourLine
{
  double level = 0;
  QVector<QPointF> points;
};


bool writeGridWithGt( const QString &path, int cols, int rows, const double geoTransform[6],
                      const QVector<float> &values, float nodata )
{
  GDALDriverH driver = GDALGetDriverByName( "GTiff" );
  if ( !driver || values.size() != cols * rows )
    return false;
  GDALDatasetH dataset = GDALCreate( driver, path.toUtf8().constData(), cols, rows, 1, GDT_Float32, nullptr );
  if ( !dataset )
    return false;
  if ( GDALSetGeoTransform( dataset, const_cast<double *>( geoTransform ) ) != CE_None )
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

bool writeGrid( const QString &path, int cols, int rows, double originX, double originY, double cell,
                const QVector<float> &values, float nodata )
{
  const double geoTransform[6] = { originX, cell, 0.0, originY, 0.0, -cell };
  return writeGridWithGt( path, cols, rows, geoTransform, values, nodata );
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
  void intervalContoursIgnoreNodata();
  void blockedOutputLeavesPreviousContentIntact();
  void failedRequestKeepsPreviousOutput();
  void analysisFieldContoursRejectRotatedGrid(); // #834 handoff: contract, not silent wrong XY
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

// P3 审计：等间距等值线必须把 nodata 当未知值，否则 -9999 洞边缘会追出
// -9900…0 的假等值线。实测本机 GDAL 的 GDALContourGenerateEx 自动沿用波段
// nodata（未传 NODATA= 也不追假线）；此用例作为跨 GDAL 版本的回归护栏。
void SingleFactorContourTests::intervalContoursIgnoreNodata()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const int cols = 40;
  const int rows = 40;
  const double cell = 10.0;
  const float nodata = -9999.0f;
  QVector<float> values( cols * rows );
  for ( int r = 0; r < rows; ++r )
    for ( int c = 0; c < cols; ++c )
      values[r * cols + c] = static_cast<float>( ( c + 0.5 ) * cell );
  for ( int r = 15; r <= 24; ++r )
    for ( int c = 18; c <= 22; ++c )
      values[r * cols + c] = nodata;
  const QString raster = dir.filePath( QStringLiteral( "hole-interval.tif" ) );
  QVERIFY( writeGrid( raster, cols, rows, 0, rows * cell, cell, values, nodata ) );
  const QString lines = dir.filePath( QStringLiteral( "hole-interval.gpkg" ) );
  QString err;
  QVERIFY2( FactorContourService::generateContours( raster, lines, 100.0, &err ), qPrintable( err ) );
  const QVector<ContourLine> contours = readContours( lines );
  QVERIFY( !contours.isEmpty() );
  for ( const ContourLine &line : contours )
    QVERIFY2( line.level >= 5.0 && line.level <= 395.0, qPrintable( QString::number( line.level ) ) );
}

// #588 B.13：等值线 GPKG 落盘是「同目录临时文件 + 成功后原子替换」。目标
// 路径被占用（此处为目录）导致替换失败时必须报错、清理临时文件、且不毁既有
// 内容——修复前先 QFile::remove 旧产物再 GDALCreate，创建/替换失败即
// 「两边都没有输出」。
void SingleFactorContourTests::blockedOutputLeavesPreviousContentIntact()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString raster = dir.filePath( QStringLiteral( "blocked.tif" ) );
  QVector<float> values( 64, 5.0f );
  values[27] = 50.0f;
  QVERIFY( writeGrid( raster, 8, 8, 0, 80, 10, values, -9999.0f ) );

  const QString blocked = dir.filePath( QStringLiteral( "contours.gpkg" ) );
  QVERIFY( QDir().mkpath( blocked ) );
  QFile marker( QDir( blocked ).filePath( QStringLiteral( "keep.txt" ) ) );
  QVERIFY( marker.open( QIODevice::WriteOnly ) );
  QCOMPARE( marker.write( "previous" ), qint64( 8 ) );
  marker.close();

  QString err;
  QVERIFY( !FactorContourService::generateContours( raster, blocked, 1.0, &err ) );
  QVERIFY2( !err.isEmpty(), "替换失败必须给出错误" );
  QFile kept( QDir( blocked ).filePath( QStringLiteral( "keep.txt" ) ) );
  QVERIFY2( kept.open( QIODevice::ReadOnly ), "失败不得毁掉目标路径下的既有内容" );
  QCOMPARE( kept.readAll(), QByteArray( "previous" ) );
  kept.close();
  QVERIFY2( !QFileInfo::exists( dir.filePath( QStringLiteral( "contours.gpkg.tmp" ) ) ),
            "失败路径不得残留临时文件" );
}

// #588 B.13：异步入口的失败/取消路径同样不许删正式输出。等值线落盘已是
// 「临时文件 + 成功后原子替换」，失败时正式产物保持调用前状态；这里再
// QFile::remove(outputGpkg) 会把上一版好产物一起删掉——用户拿到错误，
// 新旧两边都没有输出。
void SingleFactorContourTests::failedRequestKeepsPreviousOutput()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const QString output = dir.filePath( QStringLiteral( "contours.gpkg" ) );
  QFile previous( output );
  QVERIFY( previous.open( QIODevice::WriteOnly ) );
  const QByteArray contents( "previous output" );
  QCOMPARE( previous.write( contents ), qint64( contents.size() ) );
  previous.close();

  PaleoTaskService tasks;
  bool finished = false;
  bool succeeded = true;
  FactorContourService::requestContours( dir.filePath( QStringLiteral( "missing.tif" ) ),
                                         output, 1.0, &tasks, this,
                                         [&finished, &succeeded]( bool ok, const QString & ) {
                                           finished = true;
                                           succeeded = ok;
                                         } );
  QTRY_VERIFY_WITH_TIMEOUT( finished, 10000 );
  QVERIFY( !succeeded );
  QFile kept( output );
  QVERIFY2( kept.open( QIODevice::ReadOnly ), "失败不得删掉上一版产物" );
  QCOMPARE( kept.readAll(), contents );
}


// #834 旁路：factorcontour 的分析场提线用可分 xs/ys，不能表示旋转；契约是
// 拒绝旋转栅格（响亮失败），不是像 velocitymodel 那样静默用错坐标。
void SingleFactorContourTests::analysisFieldContoursRejectRotatedGrid()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  const double s = 10.0 * std::sqrt( 0.5 );
  const double gt[6] = { 0.0, s, -s, 0.0, s, s }; // 45°
  const QString raster = dir.filePath( QStringLiteral( "rotated.tif" ) );
  QVERIFY( writeGridWithGt( raster, 4, 4, gt, QVector<float>( 16, 5.0f ), -9999.0f ) );
  const QString out = dir.filePath( QStringLiteral( "out.gpkg" ) );
  QString err;
  QVERIFY2( !FactorContourService::generateAnalysisFieldContours( raster, out, { 5.0 }, 0.0, nullptr, &err ),
            "rotated analysis field must be rejected" );
  QVERIFY2( err.contains( QStringLiteral( "北向上" ) ) || err.contains( QStringLiteral( "旋转" ) )
                || err.contains( QStringLiteral( "规则网格" ) ),
            qPrintable( err ) );
  QVERIFY( !QFile::exists( out ) );
}

int main( int argc, char *argv[] )
{
  GDALAllRegister();
  // 异步入口（requestContours）经 PaleoTaskService 排队回调回主线程，
  // 需要 QCoreApplication 实例才有事件循环可派发。
  QCoreApplication app( argc, argv );
  SingleFactorContourTests tests;
  return QTest::qExec( &tests, argc, argv );
}

#include "tst_singlefactor_contours.moc"
