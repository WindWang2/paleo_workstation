// PW-010/PW-011：工作流栅格地理参考校验（rastergeo_internal.h）。
#include <QtTest>
#include "../src/workflow/rastergeo_internal.h"
#include <cpl_conv.h>

using namespace paleo::workflow_detail;

class TestRasterGeo : public QObject
{
  Q_OBJECT
private slots:
  void initTestCase() { GDALAllRegister(); }

  void missingGeoTransformRejected()
  {
    GDALDatasetH ds = GDALCreate( GDALGetDriverByName( "MEM" ), "", 4, 3, 1, GDT_Float32, nullptr );
    double gt[6];
    QString why;
    QVERIFY( !readNorthUpGeoTransform( ds, gt, &why ) );
    QVERIFY( !why.isEmpty() );
    double g[6] = { 100, 10, 0, 500, 0, -10 };
    GDALSetGeoTransform( ds, g );
    QVERIFY( readNorthUpGeoTransform( ds, gt ) );
    GDALClose( ds );
  }

  void rotatedOrFlippedRejected()
  {
    const double rot[6] = { 100, 10, 0.5, 500, 0, -10 };
    const double southUp[6] = { 100, 10, 0, 500, 0, 10 };
    const double negDx[6] = { 100, -10, 0, 500, 0, -10 };
    QVERIFY( !validNorthUpGeoTransform( rot ) );
    QVERIFY( !validNorthUpGeoTransform( southUp ) );
    QVERIFY( !validNorthUpGeoTransform( negDx ) );
  }

  void sameGridComparesOriginAndCrs()
  {
    const double a[6] = { 100, 10, 0, 500, 0, -10 };
    const double shifted[6] = { 110, 10, 0, 500, 0, -10 };
    QVERIFY( sameGeoGrid( 4, 3, a, {}, 4, 3, a, {} ) );
    QVERIFY( !sameGeoGrid( 4, 3, a, {}, 4, 3, shifted, {} ) );
    QVERIFY( !sameGeoGrid( 4, 3, a, {}, 3, 4, a, {} ) );
    auto wkt = []( int epsg ) {
      OGRSpatialReferenceH s = OSRNewSpatialReference( nullptr );
      OSRImportFromEPSG( s, epsg );
      char *w = nullptr;
      OSRExportToWkt( s, &w );
      const QByteArray out( w );
      CPLFree( w );
      OSRDestroySpatialReference( s );
      return out;
    };
    QVERIFY( sameGeoGrid( 4, 3, a, wkt( 32650 ), 4, 3, a, wkt( 32650 ) ) );
    QVERIFY( !sameGeoGrid( 4, 3, a, wkt( 32650 ), 4, 3, a, wkt( 32651 ) ) );
    QVERIFY( !sameGeoGrid( 4, 3, a, wkt( 32650 ), 4, 3, a, {} ) );
  }
};

QTEST_GUILESS_MAIN( TestRasterGeo )
#include "tst_rastergeo.moc"
