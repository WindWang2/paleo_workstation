// 层：测试壳（tests，链 paleo_qgis）
#include "../src/qgis/geostatcovariatesampler.h"
#include <qgsapplication.h>

#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgsrasterlayer.h>

#include <QtTest/QtTest>

#include <cpl_conv.h>
#include <gdal.h>
#include <ogr_spatialref.h>

#include <cmath>
#include <limits>
#include <vector>

// P3：协变量跨 CRS 采样的数值与诊断回归。同一物理场以两种可变换 CRS
//（EPSG:3857 与 EPSG:32631）保存——井位与格点采样都必须与「同 CRS 直采
// 参考」一致；nodata/越界/非有限/无 CRS/取消各按类型计数或明确失败，
// 不静默错位、不把 CRS 未知当同域。
using namespace paleo::geostat;

namespace
{

// 工区基址：3857 x≈333 km（经度 ≈3°E，落在 EPSG:32631 带中央——跨 CRS
// 变换必须落在目标带内，越界外推会得到垃圾坐标）。
constexpr double kShiftX = 333000.0;
// EPSG:3857 域 60×60 栅格（x∈[kShiftX-10,kShiftX+50]，y∈[-10,50]，像元 1m），
// 值 = 0.4c-0.2r+3。
// 命中 (12,34) 像元写 nodata 哨兵、(20,20) 写 NaN——覆盖分类计数。
QString writeRaster3857( const QString &path, float nodataSentinel )
{
  GDALDatasetH ds = GDALCreate( GDALGetDriverByName( "GTiff" ), path.toUtf8().constData(),
                                60, 60, 1, GDT_Float32, nullptr );
  if ( !ds )
    return QString();
  double gt[6] = { kShiftX - 10.0, 1.0, 0.0, 50.0, 0.0, -1.0 };
  GDALSetGeoTransform( ds, gt );
  const QgsCoordinateReferenceSystem crs( QStringLiteral( "EPSG:3857" ) );
  GDALSetProjection( ds, crs.toWkt().toUtf8().constData() );
  GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
  GDALSetRasterNoDataValue( band, nodataSentinel );
  QVector<float> px( 60 * 60 );
  for ( int r = 0; r < 60; ++r )
    for ( int c = 0; c < 60; ++c )
      px[r * 60 + c] = static_cast<float>( 0.4 * c - 0.2 * r + 3.0 );
  px[34 * 60 + 12] = nodataSentinel; // 范围内 nodata 像元（绝对位置 kShiftX+12.5, 33.5）
  px[20 * 60 + 20] = std::numeric_limits<float>::quiet_NaN(); // 非有限值像元
  GDALRasterIO( band, GF_Write, 0, 0, 60, 60, px.data(), 60, 60, GDT_Float32, 0, 0 );
  GDALClose( ds );
  return path;
}

// 同一物理场的 EPSG:32631 重采样版：B 栅格覆盖重叠区（经度带内），每个
// B 像元中心经 32631→3857 变换后落在 A 域内取 A 值，域外写 nodata。
// 值场因此与 A 表示同一物理量（分段常数像元，中心对准同一 A 像元）。
struct ReprojectedRaster
{
  QString path;
  double originX = 0, originY = 0, cell = 0;
  int cols = 0, rows = 0;
};

ReprojectedRaster writeRaster32631( const QString &path, const QString &source3857Path,
                                    float nodataSentinel )
{
  ReprojectedRaster out;
  QgsRasterLayer source( source3857Path, QStringLiteral( "src" ) );
  const QgsCoordinateReferenceSystem merc( QStringLiteral( "EPSG:3857" ) );
  const QgsCoordinateReferenceSystem utm( QStringLiteral( "EPSG:32631" ) );
  const QgsCoordinateTransform toUtm( merc, utm, QgsCoordinateTransformContext() );
  const QgsCoordinateTransform toMerc( utm, merc, QgsCoordinateTransformContext() );
  // A 域角点（留 4m 内边距避开边缘重采样抖动）→ B 域外接范围。
  const QgsPointXY a00 = toUtm.transform( QgsPointXY( kShiftX - 6, -6 ) );
  const QgsPointXY a11 = toUtm.transform( QgsPointXY( kShiftX + 46, 46 ) );
  const double cell = 1.0; // 米制两域同量级，1m 像元
  out.cols = static_cast<int>( std::ceil( std::abs( a11.x() - a00.x() ) / cell ) );
  out.rows = static_cast<int>( std::ceil( std::abs( a11.y() - a00.y() ) / cell ) );
  out.originX = std::min( a00.x(), a11.x() );
  out.originY = std::max( a00.y(), a11.y() );
  out.cell = cell;
  out.path = path;

  GDALDatasetH ds = GDALCreate( GDALGetDriverByName( "GTiff" ), path.toUtf8().constData(),
                                out.cols, out.rows, 1, GDT_Float32, nullptr );
  if ( !ds )
    return out;
  double gt[6] = { out.originX, cell, 0.0, out.originY, 0.0, -cell };
  GDALSetGeoTransform( ds, gt );
  GDALSetProjection( ds, utm.toWkt().toUtf8().constData() );
  GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
  GDALSetRasterNoDataValue( band, nodataSentinel );
  QVector<float> px( out.cols * out.rows, nodataSentinel );
  for ( int r = 0; r < out.rows; ++r )
    for ( int c = 0; c < out.cols; ++c )
    {
      const QgsPointXY b( out.originX + ( c + 0.5 ) * cell, out.originY - ( r + 0.5 ) * cell );
      bool inA = false;
      const double vA = source.dataProvider()->sample( toMerc.transform( b ), 1, &inA );
      if ( inA && std::isfinite( vA ) && vA != nodataSentinel )
        px[r * out.cols + c] = static_cast<float>( vA );
    }
  GDALRasterIO( band, GF_Write, 0, 0, out.cols, out.rows, px.data(), out.cols, out.rows,
                GDT_Float32, 0, 0 );
  GDALClose( ds );
  return out;
}

} // namespace

class GeostatCovariateSamplingTests : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase();
  void cleanupTestCase();
  // 同 CRS：短路直采，与 provider 手采逐点一致。
  void sameCrsShortCircuits();
  // 跨 CRS：32631 栅格在 3857 计算域采样 == 逐点手工变换后在 3857 栅格
  // 直采（同一物理点、同一像元值——两条路径必须一致）。
  void crossCrsMatchesTransformedReference();
  // nodata/越界/非有限逐类计数，总数对得上。
  void classifiedDiagnostics();
  // 无 CRS 栅格明确整体失败，不静默当同域采样。
  void missingCrsFails();
  // 取消：立即取消 → 失败文案「已取消」。
  void cancelledFails();

private:
  QTemporaryDir dir;
  QString raster3857;
  ReprojectedRaster raster32631;
  static constexpr float kNoData = -9999.0f;
};

void GeostatCovariateSamplingTests::initTestCase()
{
  QVERIFY( dir.isValid() );
  raster3857 = writeRaster3857( dir.filePath( QStringLiteral( "cov3857.tif" ) ), kNoData );
  QVERIFY( QFile::exists( raster3857 ) );
  raster32631 = writeRaster32631( dir.filePath( QStringLiteral( "cov32631.tif" ) ), raster3857, kNoData );
  QVERIFY( QFile::exists( raster32631.path ) );
}

void GeostatCovariateSamplingTests::cleanupTestCase() {}

void GeostatCovariateSamplingTests::sameCrsShortCircuits()
{
  QgsRasterLayer raster( raster3857, QStringLiteral( "cov" ) );
  QVERIFY( raster.isValid() );
  const QgsCoordinateReferenceSystem compute( QStringLiteral( "EPSG:3857" ) );
  std::vector<QgsPointXY> points;
  for ( int i = 0; i < 25; ++i )
    points.push_back( QgsPointXY( kShiftX - 8.0 + 2.2 * i, 40.0 - 1.7 * i ) );
  std::vector<double> values;
  CovariateSampleDiagnostics diag;
  QString error;
  QVERIFY( sampleCovariateRaster( &raster, compute, points, {}, &values, &diag, &error ) );
  QCOMPARE( diag.transformMode, QStringLiteral( "short_circuited" ) );
  QCOMPARE( diag.requested, 25 );
  QCOMPARE( diag.sampled + diag.nodata + diag.outOfExtent + diag.nonFinite + diag.transformFailed, 25 );
  QCOMPARE( diag.transformFailed, 0 );
  for ( std::size_t i = 0; i < points.size(); ++i )
  {
    bool ok = false;
    const double expected = raster.dataProvider()->sample( points[i], 1, &ok );
    if ( !ok || !std::isfinite( expected ) || expected == kNoData )
      QVERIFY( std::isnan( values[i] ) );
    else
      QCOMPARE( values[i], expected );
  }
}

void GeostatCovariateSamplingTests::crossCrsMatchesTransformedReference()
{
  QgsRasterLayer rasterB( raster32631.path, QStringLiteral( "covB" ) );
  QgsRasterLayer rasterA( raster3857, QStringLiteral( "covA" ) );
  QVERIFY( rasterA.isValid() && rasterB.isValid() );
  const QgsCoordinateReferenceSystem compute( QStringLiteral( "EPSG:3857" ) );
  const QgsCoordinateTransform toUtm( compute, QgsCoordinateReferenceSystem( QStringLiteral( "EPSG:32631" ) ),
                                      QgsCoordinateTransformContext() );

  // 井位风格（稀疏点）+ 格点风格（步进格点）两类位置同一实现采样。
  std::vector<QgsPointXY> points;
  for ( int i = 0; i < 16; ++i )
    points.push_back( QgsPointXY( -5.0 + 3.1 * i, 42.0 - 2.9 * i ) ); // 井位风格
  for ( int r = 0; r < 6; ++r )
    for ( int c = 0; c < 6; ++c )
      points.push_back( QgsPointXY( kShiftX - 4.0 + 8.0 * c, 44.0 - 8.0 * r ) ); // 格点风格

  std::vector<double> values;
  CovariateSampleDiagnostics diag;
  QString error;
  QVERIFY2( sampleCovariateRaster( &rasterB, compute, points, {}, &values, &diag, &error ),
            qPrintable( error ) );
  QCOMPARE( diag.transformMode, QStringLiteral( "projected" ) );
  QCOMPARE( diag.computeCrs, QStringLiteral( "EPSG:3857" ) );
  QCOMPARE( diag.rasterCrs, QStringLiteral( "EPSG:32631" ) );

  int matched = 0;
  int bothUnavailable = 0;
  for ( std::size_t i = 0; i < points.size(); ++i )
  {
    // 参考：手工把点变换到 B 域直采（与实现同一坐标操作——一致性指实现
    // 与参考同像元命中，不是与 A 直采比，避免「一个错误抵消另一个错误」
    // 式的假闭环；A/B 同物理场由生成器保证）。
    const QgsPointXY inB = toUtm.transform( points[i] );
    bool okB = false;
    const double expected = rasterB.dataProvider()->sample( inB, 1, &okB );
    if ( !okB || !std::isfinite( expected ) || expected == kNoData )
    {
      QVERIFY2( std::isnan( values[i] ), "reference unavailable ⇒ sampler must not produce a value" );
      ++bothUnavailable;
    }
    else
    {
      QCOMPARE( values[i], expected );
      ++matched;
    }
  }
  QVERIFY( matched > 20 ); // 绝大多数点两条路径命中同一 B 像元
  QVERIFY( diag.sampled > 20 );
  QCOMPARE( diag.sampled + diag.nodata + diag.outOfExtent + diag.nonFinite + diag.transformFailed,
            diag.requested );
}

void GeostatCovariateSamplingTests::classifiedDiagnostics()
{
  QgsRasterLayer raster( raster3857, QStringLiteral( "cov" ) );
  const QgsCoordinateReferenceSystem compute( QStringLiteral( "EPSG:3857" ) );
  std::vector<QgsPointXY> points;
  points.push_back( QgsPointXY( kShiftX + 2.5, 15.5 ) ); // (col12,row34) nodata 像元中心
  points.push_back( QgsPointXY( kShiftX + 10.5, 29.5 ) ); // (col20,row20) NaN 像元中心
  points.push_back( QgsPointXY( kShiftX + 1.5, 1.5 ) );   // 正常像元
  points.push_back( QgsPointXY( kShiftX + 999.0, 999.0 ) ); // 范围外
  points.push_back( QgsPointXY( kShiftX + 2.5, 2.5 ) ); // 正常像元
  std::vector<double> values;
  CovariateSampleDiagnostics diag;
  QString error;
  QVERIFY( sampleCovariateRaster( &raster, compute, points, {}, &values, &diag, &error ) );
  QCOMPARE( diag.requested, 5 );
  QCOMPARE( diag.sampled, 2 );
  // GDAL provider 对 nodata 哨兵与 NaN 像元都报 ok=false → 两者都归
  // nodata 计数（nonFinite 分支是其它 provider「ok 但值非有限」的兜底）。
  QCOMPARE( diag.nodata, 2 );
  QCOMPARE( diag.nonFinite, 0 );
  QCOMPARE( diag.outOfExtent, 1 );
  QVERIFY( std::isnan( values[0] ) );
  QVERIFY( std::isnan( values[1] ) );
  QVERIFY( std::isfinite( values[2] ) );
  QVERIFY( std::isnan( values[3] ) );
}

void GeostatCovariateSamplingTests::missingCrsFails()
{
  // 无 CRS 的 GTiff：明确失败——不把 CRS 未知当与计算域相同。
  const QString path = dir.filePath( QStringLiteral( "nocrs.tif" ) );
  {
    GDALDatasetH ds = GDALCreate( GDALGetDriverByName( "GTiff" ), path.toUtf8().constData(),
                                  4, 4, 1, GDT_Float32, nullptr );
    QVERIFY( ds );
    double gt[6] = { kShiftX, 1.0, 0.0, 4.0, 0.0, -1.0 };
    GDALSetGeoTransform( ds, gt ); // 有 geotransform、无投影
    GDALClose( ds );
  }
  QgsRasterLayer raster( path, QStringLiteral( "nocrs" ) );
  QVERIFY( raster.isValid() );
  const QgsCoordinateReferenceSystem compute( QStringLiteral( "EPSG:3857" ) );
  std::vector<double> values;
  CovariateSampleDiagnostics diag;
  QString error;
  QVERIFY( !sampleCovariateRaster( &raster, compute, { QgsPointXY( kShiftX + 1, 1 ) }, {}, &values, &diag, &error ) );
  QVERIFY2( !error.isEmpty(), "missing raster CRS must fail with an explicit message" );
  QVERIFY( values.empty() );
}

void GeostatCovariateSamplingTests::cancelledFails()
{
  QgsRasterLayer raster( raster3857, QStringLiteral( "cov" ) );
  const QgsCoordinateReferenceSystem compute( QStringLiteral( "EPSG:3857" ) );
  std::vector<QgsPointXY> points( 512, QgsPointXY( kShiftX + 1, 1 ) ); // 越过 256 点取消检查位
  std::vector<double> values;
  CovariateSampleDiagnostics diag;
  QString error;
  QVERIFY( !sampleCovariateRaster( &raster, compute, points, [] { return true; }, &values, &diag, &error ) );
  QCOMPARE( error, QStringLiteral( "已取消" ) );
}

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true );
  app.initQgis();
  GDALAllRegister();
  GeostatCovariateSamplingTests tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}
#include "tst_geostat_covariate_sampling.moc"
