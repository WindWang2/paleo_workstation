#include <QtTest>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgsrasterlayer.h>

#include <gdal.h>

#include "algorithmbase.h"

// wave3/model-hardening — 算法测试 harness 的验收用例（TODOS P1「算法测试
// 框架自建」）。三个不同输出形态的 paleo:* 算法全部只通过
// AlgorithmTestBase 的原语驱动，证明 harness 真的可替代 Python/YAML 基座：
//
//   paleo:paleo_constraint_idw      点图层 → 栅格（run + makeRaster 点工厂 +
//                                   compareRasters 逐像元零容差自比较）
//   paleo:paleo_isopach             栅格×2 → 栅格（对拍手工计算的期望栅格）
//   paleo:paleo_facies_polygonize   栅格 → 矢量（compareVectors 要素数+几何
//                                   近似 + featureCount）
class TestAlgorithmHarness : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir mDir;
  AlgorithmTestBase mHarness{ mDir.path() };

private slots:
  void initTestCase()
  {
    QVERIFY( mDir.isValid() );
    AlgorithmTestBase::ensurePaleoProvider();
    GDALAllRegister();
  }

  // 1) IDW：同一输入连跑两次，输出逐像元一致（tol=0）；顺带校验一个
  //    已知中心的值（harness.makeRaster/readRaster 同时被本案覆盖）。
  void constraintIdwDeterministicRerun()
  {
    auto *pts = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "pts" ),
        { { QgsPointXY( 0, 0 ), 0.0 },
          { QgsPointXY( 4, 0 ), 8.0 },
          { QgsPointXY( 0, 4 ), 4.0 },
          { QgsPointXY( 4, 4 ), 8.0 } } );
    QVERIFY( pts->isValid() );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( pts ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );

    const QString outA = mDir.filePath( QStringLiteral( "idw_a.tif" ) );
    const QString outB = mDir.filePath( QStringLiteral( "idw_b.tif" ) );
    QString log;
    QVariantMap paramsA = params;
    paramsA.insert( QStringLiteral( "OUTPUT" ), outA );
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_constraint_idw" ), paramsA, &log ).isEmpty(),
              qPrintable( log ) );
    QVariantMap paramsB = params;
    paramsB.insert( QStringLiteral( "OUTPUT" ), outB );
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_constraint_idw" ), paramsB, &log ).isEmpty(),
              qPrintable( log ) );

    AlgorithmTestBase::RasterDiff diff;
    QVERIFY2( AlgorithmTestBase::compareRasters( outA, outB, 0.0, &diff ),
              qPrintable( diff.message ) );

    // 已知值：角落像元 (0.1,3.9) 紧邻值为 4 的 (0,4) → ≈4；(4.1,3.9) 紧邻
    // 值为 8 的 (4,4) → ≈8（IDW power 2）。
    int w = 0, h = 0;
    QVector<float> px;
    QVERIFY( AlgorithmTestBase::readRaster( outA, w, h, px ) );
    QCOMPARE( w, 5 );
    QCOMPARE( h, 5 );
    QVERIFY2( std::fabs( px.value( 0 ) - 4.0f ) < 0.1f,
              qPrintable( QString::number( px.value( 0 ) ) ) );
    QVERIFY2( std::fabs( px.value( 4 ) - 8.0f ) < 0.1f,
              qPrintable( QString::number( px.value( 4 ) ) ) );

    delete pts;
  }

  // 2) Isopach：top−base 手工算出期望栅格，对拍（tol=1e-4）。
  void isopachMatchesHandComputedRaster()
  {
    const QVector<float> top = { 100, 200, 300,
                                 150, 250, 50,
                                 10,  20,  30 };
    const QVector<float> base = { 50,  150, 250,
                                  100, 300, -9999,
                                  5,   10,  15 };
    const QString pTop = mHarness.makeRaster( QStringLiteral( "top.tif" ), 3, 3, top );
    const QString pBase = mHarness.makeRaster( QStringLiteral( "base.tif" ), 3, 3, base, true, -9999.0 );
    QVERIFY( !pTop.isEmpty() && !pBase.isEmpty() );

    auto *rTop = new QgsRasterLayer( pTop, QStringLiteral( "top" ), QStringLiteral( "gdal" ) );
    auto *rBase = new QgsRasterLayer( pBase, QStringLiteral( "base" ), QStringLiteral( "gdal" ) );
    QVERIFY( rTop->isValid() && rBase->isValid() );

    const QString out = mDir.filePath( QStringLiteral( "iso.tif" ) );
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT_TOP" ),
                   QVariant::fromValue( static_cast<QgsMapLayer *>( rTop ) ) );
    params.insert( QStringLiteral( "INPUT_BASE" ),
                   QVariant::fromValue( static_cast<QgsMapLayer *>( rBase ) ) );
    params.insert( QStringLiteral( "OUTPUT" ), out );
    QString log;
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_isopach" ), params, &log ).isEmpty(),
              qPrintable( log ) );

    // 期望：top−base；base 的 nodata 传播。
    const QVector<float> want = { 50, 50, 50,
                                  50, -50, -9999,
                                  5,  10, 15 };
    const QString pWant = mHarness.makeRaster( QStringLiteral( "want_iso.tif" ), 3, 3, want, true, -9999.0 );
    AlgorithmTestBase::RasterDiff diff;
    QVERIFY2( AlgorithmTestBase::compareRasters( out, pWant, 1e-4, &diff ),
              qPrintable( diff.message.isEmpty()
                              ? QStringLiteral( "%1 differing cells, max %2" )
                                    .arg( diff.differingCells )
                                    .arg( diff.maxAbsDiff )
                              : diff.message ) );

    delete rTop;
    delete rBase;
  }

  // 3) Facies polygonize：同输入连跑两次 → gpkg 矢量，要素数一致且几何
  //    逐顶点重合（xyTol=1e-9）；两个相码各一个面。
  void faciesPolygonizeReproducibleVector()
  {
    // 4x4：左半 code 1、右半 code 2 → 两个面。
    const QVector<float> coded = { 1, 1, 2, 2,
                                   1, 1, 2, 2,
                                   1, 1, 2, 2,
                                   1, 1, 2, 2 };
    const QString pIn = mHarness.makeRaster( QStringLiteral( "coded.tif" ), 4, 4, coded, true, -9999.0 );
    QVERIFY( !pIn.isEmpty() );
    auto *rl = new QgsRasterLayer( pIn, QStringLiteral( "coded" ), QStringLiteral( "gdal" ) );
    QVERIFY( rl->isValid() );

    const QString outA = mDir.filePath( QStringLiteral( "facies_a.gpkg" ) );
    const QString outB = mDir.filePath( QStringLiteral( "facies_b.gpkg" ) );
    auto runOnce = [this, rl]( const QString &out, QString *log ) {
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ),
                     QVariant::fromValue( static_cast<QgsMapLayer *>( rl ) ) );
      params.insert( QStringLiteral( "MIN_AREA" ), 0.0 );
      params.insert( QStringLiteral( "SIMPLIFY" ), 0.0 );
      params.insert( QStringLiteral( "OUTPUT" ), out );
      return AlgorithmTestBase::run(
          QStringLiteral( "paleo:paleo_facies_polygonize" ), params, log );
    };
    QString log;
    QVERIFY2( !runOnce( outA, &log ).isEmpty(), qPrintable( log ) );
    QVERIFY2( !runOnce( outB, &log ).isEmpty(), qPrintable( log ) );

    QString msg;
    QVERIFY2( AlgorithmTestBase::compareVectors(
                  outA, outB, QStringLiteral( "facies_polygons" ), 1e-9, &msg ),
              qPrintable( msg ) );
    QCOMPARE( AlgorithmTestBase::featureCount( outA, QStringLiteral( "facies_polygons" ) ), 2 );

    delete rl;
  }
};

int main( int argc, char *argv[] )
{
  QgsApplication app( argc, argv, false );
  app.setPrefixPath( QStringLiteral( "/usr" ), true );
  app.initQgis();
  QgsApplication::processingRegistry();
  GDALAllRegister();
  TestAlgorithmHarness tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_algorithm_harness.moc"
