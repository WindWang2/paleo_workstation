// 层：测试（算法层共享写口 PaleoRasterOut）
// BIZ-11（方向58）：无 CRS 输入经共享写口 → GeoTIFF 带规范 CRS；
// 无规范覆盖的直调保持旧行为（不编造 CRS）；有效 CRS 路径不变。
#include <QtTest>
#include <QTemporaryDir>

#include <gdal.h>
#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>

#include "../src/algorithms/rasterout.h"
#include "../src/catalog/datacatalog.h"

namespace
{
QString projectionOf( const QString &path, QString *paleoMeta )
{
  GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
  if ( !ds )
    return QStringLiteral( "<open failed>" );
  const char *wkt = GDALGetProjectionRef( ds );
  const char *meta = GDALGetMetadataItem( ds, "PALEO_CRS_WKT", nullptr );
  if ( paleoMeta )
    *paleoMeta = meta ? QString::fromUtf8( meta ) : QString();
  const QString out = wkt ? QString::fromUtf8( wkt ) : QString();
  GDALClose( ds );
  return out;
}

bool writeOne( const QString &path, const QgsCoordinateReferenceSystem &crs, const QString &canonical )
{
  const double gt[6] = { 0.0, 10.0, 0.0, 100.0, 0.0, -10.0 };
  GDALDatasetH ds = PaleoRasterOut::createFloatRaster( path, 4, 4, gt, crs, -9999.0, canonical );
  if ( !ds )
    return false;
  GDALClose( ds );
  return true;
}
} // namespace

class TestAlgorithmRasterOutCrs : public QObject
{
  Q_OBJECT
private slots:
  void noCrsWithCanonicalGetsCrs()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString path = dir.filePath( QStringLiteral( "nocrs.tif" ) );
    QVERIFY( writeOne( path, QgsCoordinateReferenceSystem(), DataCatalog::localGridCrsWkt() ) );
    QString meta;
    const QString proj = projectionOf( path, &meta );
    QVERIFY2( !proj.isEmpty(), "无 CRS 输入 + 规范 WKT 仍写出无投影 GeoTIFF（BIZ-11 旧行为）" );
    QCOMPARE( meta, DataCatalog::localGridCrsWkt() );
  }

  void noCrsNoCanonicalStaysHonest()
  {
    // 直调残余面：不编造 CRS（文档化行为，非静默伪造）。
    QTemporaryDir dir;
    const QString path = dir.filePath( QStringLiteral( "bare.tif" ) );
    QVERIFY( writeOne( path, QgsCoordinateReferenceSystem(), QString() ) );
    QString meta;
    QVERIFY( projectionOf( path, &meta ).isEmpty() );
    QVERIFY( meta.isEmpty() );
  }

  void validCrsUnchanged()
  {
    QTemporaryDir dir;
    const QString path = dir.filePath( QStringLiteral( "utm.tif" ) );
    const QgsCoordinateReferenceSystem utm( QStringLiteral( "EPSG:32650" ) );
    QVERIFY( utm.isValid() );
    QVERIFY( writeOne( path, utm, DataCatalog::localGridCrsWkt() ) );
    QString meta;
    QVERIFY( !projectionOf( path, &meta ).isEmpty() );
    QCOMPARE( meta, utm.toWkt( Qgis::CrsWktVariant::PreferredGdal ) ); // 非局部网格：走 QGIS 导出
  }
};

// 同 tst_algorithm_harness：QgsApplication + 前缀路径 + initQgis（EPSG 查表需 srs.db）。
int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", QStringLiteral( "/usr" ) ), true );
  app.initQgis();
  GDALAllRegister();
  TestAlgorithmRasterOutCrs tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}
#include "tst_algorithm_rasterout_crs.moc"
