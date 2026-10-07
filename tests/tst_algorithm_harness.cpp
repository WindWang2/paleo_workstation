#include <QtTest>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgsrasterlayer.h>

#include <gdal.h>

#include "algorithmbase.h"
#include "../src/catalog/datacatalog.h"

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

  // 审计基线（docs/ALGORITHM_AUDIT.md §1）：C++-only 应用（不加载 Python
  // provider）在系统 QGIS 4.2.2 上的 Processing 面分两层——
  //   ① 未注册的：native/3d/pdal 不会自动进 registry（QGIS 4 须显式
  //      addProvider），gdal:/qgis:/grass: 在本发行版是 Python 实现，
  //      C++-only 进程永远拿不到（libqgis_analysis 无 C++ GDAL provider）；
  //   ② 显式注册后可用的：QgsNativeAlgorithms（qgis_analysis，纯 C++），
  //      343 个 native:* 算法——审计的替代路径全部以此为准。
  void providerSurfaceContract()
  {
    auto *reg = QgsApplication::processingRegistry();
    QVERIFY( reg->providerById( QStringLiteral( "paleo" ) ) != nullptr );

    // ① 现状：QgsApplication::initQgis() 不自动注册任何 processing provider。
    QVERIFY( reg->providerById( QStringLiteral( "native" ) ) == nullptr );
    QVERIFY( reg->providerById( QStringLiteral( "gdal" ) ) == nullptr );
    QVERIFY( reg->providerById( QStringLiteral( "qgis" ) ) == nullptr );
    QVERIFY( reg->providerById( QStringLiteral( "grass" ) ) == nullptr );

    // ② 一行注册 QgsNativeAlgorithms 后，native 面可用（审计建议可行性实证）。
    AlgorithmTestBase::ensureNativeAlgorithms();
    QVERIFY( reg->providerById( QStringLiteral( "native" ) ) != nullptr );

    // native 面上审计关心的替代算法确实可解析（C++ 实现）。
    const QStringList nativeAvailable = {
        QStringLiteral( "native:distancematrix" ),
        QStringLiteral( "native:distancetonearesthub" ),
        QStringLiteral( "native:zonalstatistics" ),
        QStringLiteral( "native:polygonize" )};
    for ( const QString &id : nativeAvailable )
      QVERIFY2( reg->algorithmById( id ) != nullptr, qPrintable( id ) );

    // 审计确认的缺口：这些 ID 注册了 native 后仍然解析不到——它们在
    // Python-only provider 里（gdal:*/qgis:*），或 QGIS4 已移除/改名。
    const QStringList notInApp = {
        QStringLiteral( "gdal:contour" ),
        QStringLiteral( "gdal:grid" ),
        QStringLiteral( "gdal:proximity" ),
        QStringLiteral( "qgis:idwinterpolation" ),
        QStringLiteral( "qgis:tininterpolation" ),
        QStringLiteral( "native:rastercalculator" )};
    for ( const QString &id : notInApp )
      QVERIFY2( reg->algorithmById( id ) == nullptr, qPrintable( id ) );

    // native:* 不只是能解析——headless 真能跑（距最近 hub 连线，最小用例；
    // QGIS4 参数名：INPUT/HUBS/FIELD/UNIT/OUTPUT_LINES）。
    auto *hubs = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "hubs" ), { { QgsPointXY( 0, 0 ), 1.0 } } );
    auto *pts = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "pts" ), { { QgsPointXY( 3, 4 ), 0.0 } } );
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( pts ) );
    params.insert( QStringLiteral( "HUBS" ), QVariant::fromValue( hubs ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "UNIT" ), 0 ); // meters
    const QString out = mDir.filePath( QStringLiteral( "hublines.gpkg" ) );
    params.insert( QStringLiteral( "OUTPUT_LINES" ), out );
    params.insert( QStringLiteral( "OUTPUT_POINTS" ),
                   QStringLiteral( "memory:hubpoints" ) ); // 双 sink：点输出丢弃
    QString log;
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "native:distancetonearesthub" ), params, &log ).isEmpty(),
              qPrintable( log ) );
    QCOMPARE( AlgorithmTestBase::featureCount( out ), 1 );
    delete hubs;
    delete pts;
  }

  // 0) welldist（wave/data-foundation T11）：精确最近井距离——两口井
  //    (0,0)/(10,0)、CELL_SIZE=1 → 逐格与解析期望 hypot 对拍（tol=1e-4）；
  //    空输入拒绝；重复运行确定性。
  void paleoWellDistanceExactTest()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 0, 0 ), 0.0 }, { QgsPointXY( 10, 0 ), 0.0 } } );
    QVERIFY( wells->isValid() );

    const QString out = mDir.filePath( QStringLiteral( "welldist.tif" ) );
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    params.insert( QStringLiteral( "OUTPUT" ), out );
    QString log;
    QVERIFY2( !AlgorithmTestBase::run( QStringLiteral( "paleo:paleo_welldist" ), params, &log ).isEmpty(),
              qPrintable( log ) );

    int w = 0, h = 0;
    QVector<float> px;
    QVERIFY( AlgorithmTestBase::readRaster( out, w, h, px ) );
    // 范围 (0,0)-(10,0) 外扩 10%：x∈[-1,11] y∈[-1,1] → 12×2 格。
    QCOMPARE( w, 12 );
    QCOMPARE( h, 2 );
    const double x0 = -1.0, y1 = 1.0; // GeoTransform 起点
    for ( int r = 0; r < h; ++r )
    {
      const double y = y1 - ( r + 0.5 );
      for ( int c = 0; c < w; ++c )
      {
        const double x = x0 + ( c + 0.5 );
        const double expect = std::min( std::hypot( x, y ), std::hypot( x - 10.0, y ) );
        const float got = px.at( r * w + c );
        QVERIFY2( std::fabs( got - static_cast<float>( expect ) ) < 1e-4,
                  qPrintable( QStringLiteral( "cell(%1,%2) got %3 want %4" )
                                  .arg( r )
                                  .arg( c )
                                  .arg( got )
                                  .arg( expect ) ) );
      }
    }

    // 确定性重跑。
    const QString out2 = mDir.filePath( QStringLiteral( "welldist_b.tif" ) );
    QVariantMap params2 = params;
    params2.insert( QStringLiteral( "OUTPUT" ), out2 );
    QVERIFY2( !AlgorithmTestBase::run( QStringLiteral( "paleo:paleo_welldist" ), params2, &log ).isEmpty(),
              qPrintable( log ) );
    AlgorithmTestBase::RasterDiff diff;
    QVERIFY2( AlgorithmTestBase::compareRasters( out, out2, 0.0, &diff ),
              qPrintable( diff.message ) );

    // 空输入拒绝（无可用点要素）。
    auto *empty = AlgorithmTestBase::makePointLayer( QStringLiteral( "empty" ), {} );
    QVariantMap paramsE;
    paramsE.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( empty ) );
    paramsE.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    paramsE.insert( QStringLiteral( "OUTPUT" ),
                    mDir.filePath( QStringLiteral( "welldist_e.tif" ) ) );
    QVERIFY2( AlgorithmTestBase::run( QStringLiteral( "paleo:paleo_welldist" ), paramsE, &log ).isEmpty(),
              qPrintable( log ) ); // 期望失败：返回空 map

    delete wells;
    delete empty;
  }

  // 0b) 审计 02 M-2：welldist / distance_transform 原先各自复制的
  //     createFloatRaster 不写 CRS——输出 GeoTIFF 无坐标系。收敛到共享写口后，
  //     两者输出必须带上输入图层的 CRS（投影 + PALEO_CRS_WKT 元数据）。
  void distanceOutputsCarryCrs()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 0, 0 ), 0.0 }, { QgsPointXY( 10, 0 ), 0.0 } } );
    QVERIFY( wells->isValid() );
    QVERIFY( wells->crs().isValid() );
    for ( const QString &alg : { QStringLiteral( "paleo:paleo_welldist" ),
                                 QStringLiteral( "paleo:paleo_distance_transform" ) } )
    {
      const QString out = mDir.filePath( QStringLiteral( "crs_%1.tif" ).arg( alg.section( ':', 1 ) ) );
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
      params.insert( QStringLiteral( "OUTPUT" ), out );
      QString log;
      QVERIFY2( !AlgorithmTestBase::run( alg, params, &log ).isEmpty(), qPrintable( log ) );
      GDALDatasetH ds = GDALOpen( out.toUtf8().constData(), GA_ReadOnly );
      QVERIFY( ds );
      const QString wkt = QString::fromUtf8( GDALGetProjectionRef( ds ) );
      // 元数据串归 dataset 所有：GDALClose 前拷出（否则 UAF）。
      const QString meta = QString::fromUtf8( GDALGetMetadataItem( ds, "PALEO_CRS_WKT", nullptr ) );
      double gt[6] = {};
      const bool hasGt = GDALGetGeoTransform( ds, gt ) == CE_None;
      GDALClose( ds );
      QVERIFY2( !wkt.isEmpty(), qPrintable( alg + QStringLiteral( ": output GeoTIFF has no CRS" ) ) );
      QVERIFY2( !meta.isEmpty(), qPrintable( alg ) );
      QCOMPARE( QgsCoordinateReferenceSystem::fromWkt( wkt ), wells->crs() );
      QVERIFY( hasGt );
      QCOMPARE( gt[1], 1.0 );
    }
    delete wells;
  }

  // 0c) ARCH-05：局部网格输出的规范 WKT 逐字保真。QGIS 对工程坐标系的
  //     WKT 再导出在部分构建上丢 EDATUM——丢失后 GeoTIFF 与工程网格不再
  //     判等。规范 WKT 现由 LOCAL_GRID_WKT 处理参数注入（算法核不问
  //     catalog）；注入时 PALEO_CRS_WKT 元数据必须逐字等于注入串。
  void distanceOutputsPreserveLocalGridDatum()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells-lg" ),
        { { QgsPointXY( 0, 0 ), 0.0 }, { QgsPointXY( 10, 0 ), 0.0 } } );
    QVERIFY( wells->isValid() );
    const QString canonical = DataCatalog::localGridCrsWkt();
    wells->setCrs( QgsCoordinateReferenceSystem::fromWkt( canonical ) );
    QVERIFY( wells->crs() == QgsCoordinateReferenceSystem::fromWkt( canonical ) );

    const QString out = mDir.filePath( QStringLiteral( "crs_localgrid.tif" ) );
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    const QString canonicalVerbatim = canonical + QLatin1Char( ' ' );
    params.insert( QStringLiteral( "LOCAL_GRID_WKT" ), canonicalVerbatim );
    params.insert( QStringLiteral( "OUTPUT" ), out );
    QString log;
    QVERIFY2( !AlgorithmTestBase::run( QStringLiteral( "paleo:paleo_welldist" ), params, &log ).isEmpty(),
              qPrintable( log ) );
    GDALDatasetH ds = GDALOpen( out.toUtf8().constData(), GA_ReadOnly );
    QVERIFY( ds );
    const QString wkt = QString::fromUtf8( GDALGetProjectionRef( ds ) );
    const QString meta = QString::fromUtf8( GDALGetMetadataItem( ds, "PALEO_CRS_WKT", nullptr ) );
    GDALClose( ds );
    // GeoTIFF 的 projection 域会被 GDAL 重导出（ENGCRS → WKT1 LOCAL_CS）；
    // EDATUM 保真的消费契约在 PALEO_CRS_WKT 元数据（factorcontour /
    // mappingartifactwriter 读它重建 CRS）——必须逐字等于规范串，且从它
    // 重建的 CRS 与输入层判等。
    QVERIFY2( !wkt.isEmpty(), "projection must be non-empty" );
    // 逐字保真钉法：canonical 串尾附加一个空格（WKT 解析等价、文本可区
    // 分）。参数化正确时输出按 canonical 原串写出（含空格）；穿线被拆
    //（mutation）时输出回落 QGIS 规范化导出（无尾随空格）→ 红。EDATUM
    // 丢失本身是 QGIS 构建相关的（本机 superbuild 已保真），逐字保真
    // 是不依赖该环境差异的上位契约。
    QCOMPARE( meta, canonicalVerbatim );
    QCOMPARE( QgsCoordinateReferenceSystem::fromWkt( meta ), wells->crs() );
    delete wells;
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

  // 1b) C1 break_line 屏障：竖直打断线把两口井分居两侧——左半格直取左井值、
  //     右半格直取右井值、屏障列 nodata；同一几何不带 break_line 类型
  //     （旧 shape 词）时中间格是两口井的混合（旧行为逐位保持）。
  void constraintIdwBreakLineBarrier()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 1, 2 ), 0.0 }, { QgsPointXY( 9, 2 ), 10.0 } } );
    QVERIFY( wells->isValid() );
    // 竖直打断线 x=5.5，纵贯网格高度（extent y∈[1,3]，线跨 [0,4]）。
    const QList<QgsPointXY> wall = { QgsPointXY( 5.5, 0 ), QgsPointXY( 5.5, 4 ) };

    auto *typed = AlgorithmTestBase::makeTypedLineLayer(
        QStringLiteral( "typed" ), { wall },
        QStringList{ QStringLiteral( "break_line" ) } );
    auto *legacy = AlgorithmTestBase::makeLineLayer( QStringLiteral( "legacy" ), { wall } );
    QVERIFY( typed->isValid() && legacy->isValid() );

    auto runIdw = [this, wells]( QgsVectorLayer *constraints, const QString &out ) {
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
      params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
      params.insert( QStringLiteral( "OUTPUT" ), out );
      if ( constraints )
        params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
      QString log;
      QVERIFY2( !AlgorithmTestBase::run(
                    QStringLiteral( "paleo:paleo_constraint_idw" ), params, &log ).isEmpty(),
                qPrintable( log ) );
    };

    const QString outB = mDir.filePath( QStringLiteral( "idw_break.tif" ) );
    const QString outL = mDir.filePath( QStringLiteral( "idw_legacy.tif" ) );
    runIdw( typed, outB );
    runIdw( legacy, outL );

    // 10 列（extent [0.2,9.8]）；col 5 = 屏障（x=5.5 落在 [5.2,6.2)）。
    QCOMPARE( AlgorithmTestBase::rasterCell( outB, 0, 0 ), 0.0f );
    QCOMPARE( AlgorithmTestBase::rasterCell( outB, 1, 0 ), 0.0f );
    QCOMPARE( AlgorithmTestBase::rasterCell( outB, 0, 4 ), 0.0f ); // 屏障左侧只剩 z=0 井
    QCOMPARE( AlgorithmTestBase::rasterCell( outB, 0, 9 ), 10.0f );
    QCOMPARE( AlgorithmTestBase::rasterCell( outB, 1, 6 ), 10.0f ); // 屏障右侧只剩 z=10 井
    QCOMPARE( AlgorithmTestBase::rasterCell( outB, 0, 5 ), -9999.0f ); // 屏障列本身
    QCOMPARE( AlgorithmTestBase::rasterCell( outB, 1, 5 ), -9999.0f );

    // 旧行为：无 type 列 → 不屏障，中间列是 0/10 的混合（严格介于两者）。
    const float mid = AlgorithmTestBase::rasterCell( outL, 0, 5 );
    QVERIFY2( mid > 0.0f && mid < 10.0f,
              qPrintable( QStringLiteral( "legacy middle cell %1 must be a blend" ).arg( mid ) ) );

    // 元数据：屏障/方向条数落 PALEO_BREAK_LINES / PALEO_DIRECTION_LINES。
    GDALDatasetH ds = GDALOpen( outB.toUtf8().constData(), GA_ReadOnly );
    QVERIFY2( ds != nullptr, "open idw_break.tif for metadata" );
    const char *nBreak = GDALGetMetadataItem( ds, "PALEO_BREAK_LINES", nullptr );
    const char *nDir = GDALGetMetadataItem( ds, "PALEO_DIRECTION_LINES", nullptr );
    QVERIFY2( nBreak && QByteArray( nBreak ) == QByteArrayLiteral( "1" ), "PALEO_BREAK_LINES=1" );
    QVERIFY2( nDir && QByteArray( nDir ) == QByteArrayLiteral( "0" ), "PALEO_DIRECTION_LINES=0" );
    GDALClose( ds );

    delete wells;
    delete typed;
    delete legacy;
  }

  // 1c) C1 direction_line 方向场：仅方向线（无打断线）时——① 不参与 ROI 凸包
  //     裁剪（同几何不带类型会裁掉角格）；② 各向异性权重把等距两井的混合值
  //     沿方向轴偏置（无方向场时恰为 50/50）。
  void constraintIdwDirectionAniso()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 5.5, 3.0 ), 0.0 },   // 测试格正下方（横向 across）
          { QgsPointXY( 8.0, 5.5 ), 10.0 } } ); // 测试格正右方（沿方向 along）
    QVERIFY( wells->isValid() );
    // 测试格 (row0,col0) 中心 (5.75,5.25)：到两井距离平方均为 5.125。
    // 横向方向线 y=5.5 → θ=0；r=2 时 along 井（右）权重≈13×across 井（下）。

    const QList<QgsPointXY> axis = { QgsPointXY( 5.25, 5.5 ), QgsPointXY( 8.25, 5.5 ) };
    auto *dirLines = AlgorithmTestBase::makeTypedLineLayer(
        QStringLiteral( "dir" ), { axis },
        QStringList{ QStringLiteral( "direction_line" ) } );
    auto *plainLines = AlgorithmTestBase::makeLineLayer( QStringLiteral( "plain" ), { axis } );
    QVERIFY( dirLines->isValid() && plainLines->isValid() );

    auto runWith = [this, wells]( QgsVectorLayer *constraints, const QString &out ) {
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
      params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
      params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
      params.insert( QStringLiteral( "OUTPUT" ), out );
      QString log;
      QVERIFY2( !AlgorithmTestBase::run(
                    QStringLiteral( "paleo:paleo_constraint_idw" ), params, &log ).isEmpty(),
                qPrintable( log ) );
    };

    const QString outA = mDir.filePath( QStringLiteral( "idw_aniso.tif" ) );
    const QString outL = mDir.filePath( QStringLiteral( "idw_noline.tif" ) );
    runWith( dirLines, outA );
    // 无约束基线：50/50 混合 → 5.0。
    QVariantMap paramsN;
    paramsN.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    paramsN.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    paramsN.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    paramsN.insert( QStringLiteral( "OUTPUT" ), outL );
    QString logN;
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_constraint_idw" ), paramsN, &logN ).isEmpty(),
              qPrintable( logN ) );

    const float base = AlgorithmTestBase::rasterCell( outL, 0, 0 );
    QVERIFY2( std::fabs( base - 5.0f ) < 1e-3f,
              qPrintable( QStringLiteral( "isotropic equidistant mix %1" ).arg( base ) ) );
    const float aniso = AlgorithmTestBase::rasterCell( outA, 0, 0 );
    QVERIFY2( aniso > 9.0f,
              qPrintable( QStringLiteral( "anisotropic mix %1 must lean along-axis" ).arg( aniso ) ) );

    // 元数据：方向条数 + 各向异性参数。
    GDALDatasetH ds = GDALOpen( outA.toUtf8().constData(), GA_ReadOnly );
    QVERIFY2( ds != nullptr, "open idw_aniso.tif for metadata" );
    QCOMPARE( QByteArray( GDALGetMetadataItem( ds, "PALEO_DIRECTION_LINES", nullptr ) ),
              QByteArrayLiteral( "1" ) );
    QCOMPARE( QByteArray( GDALGetMetadataItem( ds, "PALEO_BREAK_LINES", nullptr ) ),
              QByteArrayLiteral( "0" ) );
    QVERIFY2( GDALGetMetadataItem( ds, "PALEO_ANISO_RATIO", nullptr ) != nullptr,
              "PALEO_ANISO_RATIO present" );
    GDALClose( ds );

    // ① 方向线不参与凸包裁剪：两条交叉方向线的凸包是正方形（会裁掉角格），
    //    带方向类型时角格仍有值；同一对线不带类型（旧行为）时角格 nodata。
    auto *wells2 = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells2" ),
        { { QgsPointXY( 1, 1 ), 5.0 }, { QgsPointXY( 9, 9 ), 5.0 } } );
    const QList<QgsPointXY> cross1 = { QgsPointXY( 1, 9 ), QgsPointXY( 9, 1 ) };
    const QList<QgsPointXY> cross2 = { QgsPointXY( 1, 1 ), QgsPointXY( 9, 9 ) };
    auto *crossTyped = AlgorithmTestBase::makeTypedLineLayer(
        QStringLiteral( "xt" ), { cross1, cross2 },
        QStringList{ QStringLiteral( "direction_line" ), QStringLiteral( "direction_line" ) } );
    auto *crossPlain = AlgorithmTestBase::makeLineLayer(
        QStringLiteral( "xp" ), { cross1, cross2 } );

    auto runCross = [this, wells2]( QgsVectorLayer *constraints, const QString &out ) {
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells2 ) );
      params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
      params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
      params.insert( QStringLiteral( "OUTPUT" ), out );
      QString log;
      QVERIFY2( !AlgorithmTestBase::run(
                    QStringLiteral( "paleo:paleo_constraint_idw" ), params, &log ).isEmpty(),
                qPrintable( log ) );
    };
    const QString outXt = mDir.filePath( QStringLiteral( "idw_xt.tif" ) );
    const QString outXp = mDir.filePath( QStringLiteral( "idw_xp.tif" ) );
    runCross( crossTyped, outXt );
    runCross( crossPlain, outXp );
    QVERIFY2( AlgorithmTestBase::rasterCell( outXt, 0, 0 ) != -9999.0f,
              "direction lines must not clip the ROI hull" );
    QCOMPARE( AlgorithmTestBase::rasterCell( outXp, 0, 0 ), -9999.0f ); // 旧语义：凸包裁剪

    delete wells;
    delete dirLines;
    delete plainLines;
    delete wells2;
    delete crossTyped;
    delete crossPlain;
  }

  // 1d) C5 paleo:paleo_distance_transform（welldist 冻结契约引擎）：
  //     ① 无约束（或约束无 break_line）时与 paleo:paleo_welldist 逐像元一致
  //        （精确欧氏口径相同）；② break_line 绕障：Dijkstra 路径距离
  //        （井格 0、正交邻格 1、对角 √2）；③ 屏障格 nodata；④ 元数据
  //        PALEO_BARRIER_AWARE 反映是否绕障。
  void distanceTransformContract()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 0, 0 ), 0.0 }, { QgsPointXY( 10, 0 ), 0.0 } } );
    QVERIFY( wells->isValid() );

    auto baseParams = []( QgsVectorLayer *input, const QString &out ) {
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( input ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
      params.insert( QStringLiteral( "OUTPUT" ), out );
      return params;
    };
    const QString outPlain = mDir.filePath( QStringLiteral( "dt_plain.tif" ) );
    const QString outWelldist = mDir.filePath( QStringLiteral( "dt_welldist.tif" ) );
    QString log;
    QVariantMap paramsPlain = baseParams( wells, outPlain );
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_distance_transform" ), paramsPlain, &log ).isEmpty(),
              qPrintable( log ) );
    QVariantMap paramsW = baseParams( wells, outWelldist );
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_welldist" ), paramsW, &log ).isEmpty(),
              qPrintable( log ) );
    AlgorithmTestBase::RasterDiff diff;
    QVERIFY2( AlgorithmTestBase::compareRasters( outPlain, outWelldist, 0.0, &diff ),
              qPrintable( diff.message ) );

    // 无 break_line 的约束层（旧 shape 词 + direction_line）不改变距离面。
    const QList<QgsPointXY> stray = { QgsPointXY( -1, -1 ), QgsPointXY( 11, 1 ) };
    auto *notBreak = AlgorithmTestBase::makeTypedLineLayer(
        QStringLiteral( "nb" ), { stray, stray },
        QStringList{ QStringLiteral( "line" ), QStringLiteral( "direction_line" ) } );
    const QString outNoBreak = mDir.filePath( QStringLiteral( "dt_nobreak.tif" ) );
    QVariantMap paramsNb = baseParams( wells, outNoBreak );
    paramsNb.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( notBreak ) );
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_distance_transform" ), paramsNb, &log ).isEmpty(),
              qPrintable( log ) );
    QVERIFY2( AlgorithmTestBase::compareRasters( outNoBreak, outWelldist, 0.0, &diff ),
              qPrintable( diff.message ) );

    // break_line 绕障：两口井 + 竖直墙（与 1b 同几何）。
    auto *wells2 = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells2" ),
        { { QgsPointXY( 1, 2 ), 0.0 }, { QgsPointXY( 9, 2 ), 0.0 } } );
    const QList<QgsPointXY> wall = { QgsPointXY( 5.5, 0 ), QgsPointXY( 5.5, 4 ) };
    auto *typed = AlgorithmTestBase::makeTypedLineLayer(
        QStringLiteral( "typed" ), { wall },
        QStringList{ QStringLiteral( "break_line" ) } );
    QVERIFY( wells2->isValid() && typed->isValid() );

    const QString outBarrier = mDir.filePath( QStringLiteral( "dt_barrier.tif" ) );
    QVariantMap paramsB = baseParams( wells2, outBarrier );
    paramsB.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( typed ) );
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_distance_transform" ), paramsB, &log ).isEmpty(),
              qPrintable( log ) );

    // 网格 [0.2,9.8]×[1,3]（10×2）：井 (1,2)→格(r1,c0)、井 (9,2)→格(r1,c8)。
    const float wellCell = AlgorithmTestBase::rasterCell( outBarrier, 1, 0 );
    QVERIFY2( std::fabs( wellCell ) < 1e-4f, qPrintable( QString::number( wellCell ) ) );
    const float ortho = AlgorithmTestBase::rasterCell( outBarrier, 0, 0 );
    QVERIFY2( std::fabs( ortho - 1.0f ) < 1e-4f, qPrintable( QString::number( ortho ) ) );
    const float diag = AlgorithmTestBase::rasterCell( outBarrier, 0, 9 );
    QVERIFY2( std::fabs( diag - std::sqrt( 2.0f ) ) < 1e-3f,
              qPrintable( QString::number( diag ) ) );
    QCOMPARE( AlgorithmTestBase::rasterCell( outBarrier, 0, 5 ), -9999.0f ); // 屏障列
    QCOMPARE( AlgorithmTestBase::rasterCell( outBarrier, 1, 5 ), -9999.0f );

    GDALDatasetH ds = GDALOpen( outBarrier.toUtf8().constData(), GA_ReadOnly );
    QVERIFY2( ds != nullptr, "open dt_barrier.tif for metadata" );
    QCOMPARE( QByteArray( GDALGetMetadataItem( ds, "PALEO_BARRIER_AWARE", nullptr ) ),
              QByteArrayLiteral( "1" ) );
    QCOMPARE( QByteArray( GDALGetMetadataItem( ds, "PALEO_BREAK_LINES", nullptr ) ),
              QByteArrayLiteral( "1" ) );
    GDALClose( ds );
    ds = GDALOpen( outPlain.toUtf8().constData(), GA_ReadOnly );
    QVERIFY2( ds != nullptr, "open dt_plain.tif for metadata" );
    QCOMPARE( QByteArray( GDALGetMetadataItem( ds, "PALEO_BARRIER_AWARE", nullptr ) ),
              QByteArrayLiteral( "0" ) );
    GDALClose( ds );

    // 空输入拒绝（契约同 welldist）。
    auto *empty = AlgorithmTestBase::makePointLayer( QStringLiteral( "empty" ), {} );
    QVariantMap paramsE = baseParams( empty, mDir.filePath( QStringLiteral( "dt_e.tif" ) ) );
    QVERIFY2( AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_distance_transform" ), paramsE, &log ).isEmpty(),
              qPrintable( log ) ); // 期望失败：返回空 map

    delete wells;
    delete notBreak;
    delete wells2;
    delete typed;
    delete empty;
  }

  // 1d-2) issue #292：斜向 break_line 角点密封。30°/60° 贯穿网格的断层
  //     半格采样栅格化只保证障碍格 8 连通（相邻障碍格仅角接），8 邻接
  //     Dijkstra 若不做对角穿角检查会经角点缝隙斜穿、退化成近欧氏
  //     距离。两口井分居墙两侧：探测格取墙根下侧、欧氏上离对侧井
  //     很近的一格——密封时它只能在本侧区内绕行（30°≈8.7、60°≈9.7），
  //     泄漏时穿角抄近路取对侧井（30°≈4.2、60°≈3.8）。无约束输出
  //     同格 ≈3.1/3.3，作对照。
  void distanceTransformDiagonalBarrier()
  {
    auto baseParams = []( QgsVectorLayer *input, const QString &out ) {
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( input ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
      params.insert( QStringLiteral( "OUTPUT" ), out );
      return params;
    };
    // 井 (6,0.5)/(14,7) → 网格 11×8：xMin=5.2、xMax=15.8、yMax=7.65
    // （10% 外扩，extent [5.2,15.8]×[-0.15,7.65]）。
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wellsDiag" ),
        { { QgsPointXY( 6, 0.5 ), 0.0 }, { QgsPointXY( 14, 7 ), 0.0 } } );
    QVERIFY( wells->isValid() );

    const QString outPlain = mDir.filePath( QStringLiteral( "dt_diag_plain.tif" ) );
    QString log;
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_distance_transform" ),
                  baseParams( wells, outPlain ), &log ).isEmpty(),
              qPrintable( log ) );

    struct DiagCase
    {
      double angleDeg;
      QList<QgsPointXY> line;
      int probeRow, probeCol; // 墙根下侧、贴近对侧井的探测格
      double sealedMin;       // 密封时探测格距离下界（本侧区内绕行）
    };
    constexpr double kPi = 3.14159265358979323846;
    const double tan30 = std::tan( 30.0 * kPi / 180.0 ); // ≈0.5774
    const double tan60 = std::tan( 60.0 * kPi / 180.0 ); // ≈1.7321
    const QList<DiagCase> cases = {
        // 30°：左缘 (c0,r6) → 右缘角格 (c10,r0)，整幅分隔下/上两侧
        { 30.0,
          { QgsPointXY( 5.2, 1.0 ), QgsPointXY( 15.8, 1.0 + 10.6 * tan30 ) },
          3, 7, 6.5 },
        // 60°：底缘 (r7,c2) → 顶缘 (r0,c6)，整幅分隔左/右两侧
        { 60.0,
          { QgsPointXY( 7.55, 0.4 ), QgsPointXY( 7.55 + 7.2 / tan60, 7.6 ) },
          0, 5, 7.0 },
    };
    for ( const DiagCase &tc : cases )
    {
      auto *typed = AlgorithmTestBase::makeTypedLineLayer(
          QStringLiteral( "diag%1" ).arg( tc.angleDeg ), { tc.line },
          QStringList{ QStringLiteral( "break_line" ) } );
      QVERIFY( typed->isValid() );
      const QString out = mDir.filePath(
          QStringLiteral( "dt_diag_%1.tif" ).arg( tc.angleDeg ) );
      QVariantMap params = baseParams( wells, out );
      params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( typed ) );
      QVERIFY2( !AlgorithmTestBase::run(
                    QStringLiteral( "paleo:paleo_distance_transform" ), params, &log ).isEmpty(),
                qPrintable( log ) );

      // 泄漏判定：密封时探测格只能在本侧区内绕行（>sealedMin）；穿角
      // 缝隙会把它抄近路拉到对侧井（≈4.2/3.8，低于下界）
      const float probe = AlgorithmTestBase::rasterCell( out, tc.probeRow, tc.probeCol );
      QVERIFY2( std::isfinite( probe ) && probe > static_cast<float>( tc.sealedMin ),
                qPrintable( QStringLiteral( "angle=%1 probe=%2 (leak: 穿角缝隙)" )
                                .arg( tc.angleDeg )
                                .arg( probe ) ) );
      // 无约束输出同格是对侧井的近欧氏距离（远小于密封绕障距离）
      const float plain = AlgorithmTestBase::rasterCell( outPlain, tc.probeRow, tc.probeCol );
      QVERIFY2( std::isfinite( plain ) && plain < 5.0f,
                qPrintable( QStringLiteral( "angle=%1 plain=%2" ).arg( tc.angleDeg ).arg( plain ) ) );
      delete typed;
    }
    delete wells;
  }

  // ---- WP3 Round 1：输入域/预算/取消/CRS 守卫 --------------------------------

  // 1d) 网格预算守卫：极小 CELL_SIZE × 常规范围（>1 亿像元）必须显式拒绝，
  //     而不是 double→int 截断 UB / 整网格前置分配失控（OOM）。三个引擎
  //     同口径（constraint_idw / welldist / distance_transform）。
  void gridBudgetRejection()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 0, 0 ), 1.0 }, { QgsPointXY( 10, 10 ), 2.0 } } );
    QVERIFY( wells->isValid() );

    // 范围 [0,10]² 外扩后 ≈ 11×11，CELL_SIZE=1e-6 → ≈1.2e14 像元。
    const QStringList engines = { QStringLiteral( "paleo:paleo_constraint_idw" ),
                                  QStringLiteral( "paleo:paleo_welldist" ),
                                  QStringLiteral( "paleo:paleo_distance_transform" ) };
    for ( const QString &id : engines )
    {
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
      if ( id.endsWith( QLatin1String( "constraint_idw" ) ) )
        params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 1e-6 );
      params.insert( QStringLiteral( "OUTPUT" ),
                     mDir.filePath( QStringLiteral( "budget_%1.tif" ).arg(
                         id.section( u':', 1 ) ) ) );
      QString log;
      QVERIFY2( AlgorithmTestBase::run( id, params, &log ).isEmpty(),
                qPrintable( QStringLiteral( "%1 must reject an oversized grid" ).arg( id ) ) );
      QVERIFY2( log.contains( QStringLiteral( "cell budget" ) ),
                qPrintable( QStringLiteral( "%1: %2" ).arg( id, log ) ) );
    }
    delete wells;
  }

  // 1e) 地理 CRS：不拒绝（历史兼容），但 Processing feedback 必须给出单位
  //     原因态（度 ≠ 米）。三个引擎同文案。
  void geographicCrsWarningSurfaced()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 0, 0 ), 1.0 }, { QgsPointXY( 4, 4 ), 2.0 } } );
    QVERIFY( wells->isValid() );

    const QStringList engines = { QStringLiteral( "paleo:paleo_constraint_idw" ),
                                  QStringLiteral( "paleo:paleo_welldist" ),
                                  QStringLiteral( "paleo:paleo_distance_transform" ) };
    for ( const QString &id : engines )
    {
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
      if ( id.endsWith( QLatin1String( "constraint_idw" ) ) )
        params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
      params.insert( QStringLiteral( "OUTPUT" ),
                     mDir.filePath( QStringLiteral( "geo_%1.tif" ).arg(
                         id.section( u':', 1 ) ) ) );
      QString log, fbText;
      QVERIFY2( !AlgorithmTestBase::run( id, params, &log, nullptr, &fbText ).isEmpty(),
                qPrintable( log ) );
      QVERIFY2( fbText.contains( QStringLiteral( "geographic" ) ) &&
                    fbText.contains( QStringLiteral( "degrees" ) ),
                qPrintable( QStringLiteral( "%1: %2" ).arg( id, fbText ) ) );
    }
    delete wells;
  }

  // 1f) ANISO_RATIO 上界：极端比值（>1000）拒绝并给原因，不静默产出
  //     全 nodata / NaN 面。
  void anisoRatioUpperBoundRejected()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 0, 0 ), 1.0 }, { QgsPointXY( 4, 0 ), 3.0 } } );
    const QList<QgsPointXY> dir = { QgsPointXY( 0, 2 ), QgsPointXY( 4, 2 ) };
    auto *dirs = AlgorithmTestBase::makeTypedLineLayer(
        QStringLiteral( "dirs" ), { dir },
        QStringList{ QStringLiteral( "direction_line" ) } );
    QVERIFY( wells->isValid() && dirs->isValid() );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( dirs ) );
    params.insert( QStringLiteral( "ANISO_RATIO" ), 2000.0 );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    params.insert( QStringLiteral( "OUTPUT" ),
                    mDir.filePath( QStringLiteral( "aniso_big.tif" ) ) );
    QString log;
    QVERIFY2( AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_constraint_idw" ), params, &log ).isEmpty(),
              "oversized ANISO_RATIO must be rejected" );
    QVERIFY2( log.contains( QStringLiteral( "ANISO_RATIO" ) ), qPrintable( log ) );

    delete wells;
    delete dirs;
  }

  // 1g) 取消粒度：distance_transform 的绕障阶段（屏障栅格化 + Dijkstra）
  //     必须可取消。setProgressHook 在第一次 setProgress 即取消——修复后
  //     第一次 setProgress 发生在 Dijkstra 节流点（行写出之前），输出栅格
  //     第 0 行保持未写（GTiff 稀疏读 0.0）；修复前第一次 setProgress 在
  //     第 0 行写出之后，第 0 行是真距离值（>0）。
  void dtCancellationHonoredBeforeRowWrite()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 10, 10 ), 0.0 }, { QgsPointXY( 90, 90 ), 0.0 } } );
    // 单条竖墙贯穿网格外（x=50，y 跨 [-10,110]）；网格 [2,98]² = 96×96。
    const QList<QgsPointXY> wall = { QgsPointXY( 50, -10 ), QgsPointXY( 50, 110 ) };
    auto *typed = AlgorithmTestBase::makeTypedLineLayer(
        QStringLiteral( "typed" ), { wall },
        QStringList{ QStringLiteral( "break_line" ) } );
    QVERIFY( wells->isValid() && typed->isValid() );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( typed ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    const QString out = mDir.filePath( QStringLiteral( "dt_cancel.tif" ) );
    params.insert( QStringLiteral( "OUTPUT" ), out );

    // 第一次 progressChanged 即取消。修复后第一次 setProgress 发生在
    // Dijkstra 节流点（行写出之前）——GTiff 未写块读回 nodata(-9999)；
    // 修复前第一次 setProgress 在第 0 行写出之后——角格是真实距离(>0)。
    QgsProcessingFeedback fb;
    QVERIFY( QObject::connect( &fb, &QgsFeedback::progressChanged, &fb,
                               [&fb]( double ) { fb.cancel(); } ) );
    QString log;
    QVERIFY2( AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_distance_transform" ), params, &log, &fb ).isEmpty(),
              "canceled run must fail" );
    QVERIFY2( log.contains( QStringLiteral( "Canceled" ) ), qPrintable( log ) );
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 0, 0 ), -9999.0f );

    delete wells;
    delete typed;
  }

  void idwBfsCancellationHonoredBeforeRowWrite()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 10, 10 ), 5.0 }, { QgsPointXY( 90, 90 ), 9.0 } } );
    // 单墙贯穿网格外：hull 退化为线（不裁剪 ROI），BFS 仍有 ~9100 格可走。
    const QList<QgsPointXY> wall = { QgsPointXY( 50, -10 ), QgsPointXY( 50, 110 ) };
    auto *typed = AlgorithmTestBase::makeTypedLineLayer(
        QStringLiteral( "typed" ), { wall },
        QStringList{ QStringLiteral( "break_line" ) } );
    QVERIFY( wells->isValid() && typed->isValid() );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( typed ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    const QString out = mDir.filePath( QStringLiteral( "idw_cancel.tif" ) );
    params.insert( QStringLiteral( "OUTPUT" ), out );

    QgsProcessingFeedback fb;
    QVERIFY( QObject::connect( &fb, &QgsFeedback::progressChanged, &fb,
                               [&fb]( double ) { fb.cancel(); } ) );
    QString log;
    QVERIFY2( AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_constraint_idw" ), params, &log, &fb ).isEmpty(),
              "canceled run must fail" );
    QVERIFY2( log.contains( QStringLiteral( "Canceled" ) ), qPrintable( log ) );
    // GTiff 未写块读回 = nodata 需要 GDAL>=3.x（稀疏块按 nodata 填充）；
    // 本仓 vendored QGIS 4.2 面的 GDAL 满足。
    // 未写块 = nodata；若 BFS 不可取消，第 0 行会写出混合值（有限正值）。
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 0, 0 ), -9999.0f );

    delete wells;
    delete typed;
  }

  // ---- WP3 Round 3：ConstraintIDW 合成几何边界矩阵 --------------------------

  // 3a) 多 break_line：两道平行竖墙把网格分成三区——左右两区各有井直取本区
  //     井值，中区无井整片 nodata（不造混合值），墙列本身 nodata。
  void constraintIdwMultipleBreakLines()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 1, 2 ), 0.0 }, { QgsPointXY( 9, 2 ), 10.0 } } );
    // 同一条竖墙拆成两段共线 break_line（x=5.5，y∈[0,2] 与 y∈[2,4]）：
    // 每一段都必须各自参与屏障栅格化——只处理第一条要素时上半格会漏。
    // 共线两段的 unaryUnion 仍是线（凸包退化为非多边形）→ 无 ROI 裁剪，
    // 与单墙（1b）同一外显。
    const QList<QgsPointXY> segLow = { QgsPointXY( 5.5, 0 ), QgsPointXY( 5.5, 2 ) };
    const QList<QgsPointXY> segHigh = { QgsPointXY( 5.5, 2 ), QgsPointXY( 5.5, 4 ) };
    auto *typed = AlgorithmTestBase::makeTypedLineLayer(
        QStringLiteral( "typed" ), { segLow, segHigh },
        QStringList{ QStringLiteral( "break_line" ), QStringLiteral( "break_line" ) } );
    QVERIFY( wells->isValid() && typed->isValid() );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( typed ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    const QString out = mDir.filePath( QStringLiteral( "idw_multi_break.tif" ) );
    params.insert( QStringLiteral( "OUTPUT" ), out );
    QString log;
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_constraint_idw" ), params, &log ).isEmpty(),
              qPrintable( log ) );

    // 网格 [0.2,9.8]×[1,3]（10 列 × 2 行）；墙 x=5.5→col5。
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 0, 0 ), 0.0f );    // 左区直取
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 1, 4 ), 0.0f );
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 0, 5 ), -9999.0f ); // 上半墙段
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 1, 5 ), -9999.0f ); // 下半墙段
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 0, 6 ), 10.0f );   // 右区直取
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 1, 9 ), 10.0f );

    delete wells;
    delete typed;
  }

  // 3b) 凹形/孔洞：四面墙围出的闭合口袋内有一口井——口袋内部只从口袋井
  //     插值；口袋外左右两区互不污染；墙格 nodata。二维封锁（纵墙封列、
  //     横墙封行）是本例与 3a（纯平行墙）的区别。
  void constraintIdwWalledPocketIsolated()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 1, 1 ), 0.0 },
          { QgsPointXY( 5, 2 ), 100.0 },
          { QgsPointXY( 9, 3 ), 10.0 } } );
    // 网格 [0.2,9.8]×[0.8,3.2]（10 列 × 3 行）。口袋 x∈[4,7]、y∈[1,3]：
    // 纵墙 x=4→col3、x=7→col6（y 跨越整格高）；横墙 y=3→row0、y=1→row2
    // （x 跨 [3.6,6.8]，不越进右区）。内部自由格 = (r1,c4),(r1,c5)；口袋井
    // (5,2)→(r1,c4)。
    // 注：≥2 条 break_line 的凸包成为多边形 ROI（「屏障即边界」的 C1 冻结
    // 语义）——口袋墙凸包 = [4,7]×[1,3]，口袋外整片裁成 nodata（外侧井不
    // 参与）。本案钉的是：四面墙全部各自生效 + 口袋内部只从口袋井插值。
    auto *typed = AlgorithmTestBase::makeTypedLineLayer(
        QStringLiteral( "pocket" ),
        { { QgsPointXY( 4, 0.5 ), QgsPointXY( 4, 3.5 ) },
          { QgsPointXY( 7, 0.5 ), QgsPointXY( 7, 3.5 ) },
          { QgsPointXY( 3.6, 3 ), QgsPointXY( 6.8, 3 ) },
          { QgsPointXY( 3.6, 1 ), QgsPointXY( 6.8, 1 ) } },
        QStringList{ QStringLiteral( "break_line" ), QStringLiteral( "break_line" ),
                     QStringLiteral( "break_line" ), QStringLiteral( "break_line" ) } );
    QVERIFY( wells->isValid() && typed->isValid() );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( typed ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    const QString out = mDir.filePath( QStringLiteral( "idw_pocket.tif" ) );
    params.insert( QStringLiteral( "OUTPUT" ), out );
    QString log;
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_constraint_idw" ), params, &log ).isEmpty(),
              qPrintable( log ) );

    QCOMPARE( AlgorithmTestBase::rasterCell( out, 1, 4 ), 100.0f ); // 口袋内部
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 1, 5 ), 100.0f );
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 1, 3 ), -9999.0f ); // 纵墙
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 1, 6 ), -9999.0f );
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 0, 4 ), -9999.0f ); // 横墙
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 2, 4 ), -9999.0f );
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 1, 0 ), -9999.0f ); // 凸包外（ROI 裁剪）
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 1, 8 ), -9999.0f );

    delete wells;
    delete typed;
  }

  // 3c) 重合井：同位两口井（z=3 先入、z=7 后入）——井格 d²=0 命中第一口
  //     （要素迭代序），连跑两遍一致（确定性钉死并列规则）。
  void constraintIdwCoincidentWellsFirstWinsDeterministic()
  {
    // 精确命中分支：同位两口井 (5,4)（z=3 先入、z=7 后入）+ 两口对称远井
    // (2,1)/(8,7) 使 bbox 中心 = (5,4)；CELL_SIZE=2.4 下 3×3 网格的中心格
    // 格心恰为 (5,4) → d²=0 命中第一口（要素序）。连跑两遍逐位一致。
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 5, 4 ), 3.0 },
          { QgsPointXY( 5, 4 ), 7.0 },
          { QgsPointXY( 2, 1 ), 50.0 },
          { QgsPointXY( 8, 7 ), 90.0 } } );
    QVERIFY( wells->isValid() );

    auto runOnce = [this, wells]( const QString &out ) {
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
      params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 2.4 );
      params.insert( QStringLiteral( "OUTPUT" ), out );
      QString log;
      QVERIFY2( !AlgorithmTestBase::run(
                    QStringLiteral( "paleo:paleo_constraint_idw" ), params, &log ).isEmpty(),
                qPrintable( log ) );
    };
    const QString outA = mDir.filePath( QStringLiteral( "idw_coin_a.tif" ) );
    const QString outB = mDir.filePath( QStringLiteral( "idw_coin_b.tif" ) );
    runOnce( outA );
    runOnce( outB );

    int w = 0, h = 0;
    QVector<float> px;
    QVERIFY( AlgorithmTestBase::readRaster( outA, w, h, px ) );
    QCOMPARE( w, 3 );
    QCOMPARE( h, 3 );
    QCOMPARE( AlgorithmTestBase::rasterCell( outA, 1, 1 ), 3.0f ); // 井位格心 → 第一口
    for ( int i = 0; i < px.size(); ++i )
      QVERIFY2( std::isfinite( px[i] ), qPrintable( QString::number( px[i] ) ) );
    AlgorithmTestBase::RasterDiff diff;
    QVERIFY2( AlgorithmTestBase::compareRasters( outA, outB, 0.0, &diff ),
              qPrintable( diff.message ) );

    delete wells;
  }

  // 3d) 极近井：两口井相距 2e-6——输出全格有限（无 inf/NaN 毒化；非井格
  //     的 IDW 混合权重 1/d² 仍在 double 范围内）。
  void constraintIdwNearWellsFiniteOutput()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 5, 2.000001 ), 1.0 },
          { QgsPointXY( 5, 1.999999 ), 9.0 },
          { QgsPointXY( 9, 2 ), 5.0 } } );
    QVERIFY( wells->isValid() );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    const QString out = mDir.filePath( QStringLiteral( "idw_near.tif" ) );
    params.insert( QStringLiteral( "OUTPUT" ), out );
    QString log;
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_constraint_idw" ), params, &log ).isEmpty(),
              qPrintable( log ) );

    int w = 0, h = 0;
    QVector<float> px;
    QVERIFY( AlgorithmTestBase::readRaster( out, w, h, px ) );
    // 两重合近井 (5,2±1e-6) 等权混合 = 5；远井 (9,2) 只在其邻域抬值——
    // 中列（距远井 ≥3 格）应为 5.0±0.1，其余格有限。
    int midCol = w / 2;
    for ( int r = 0; r < h; ++r )
      QVERIFY2( std::fabs( px[r * w + midCol] - 5.0f ) < 0.1f,
                qPrintable( QStringLiteral( "mid col r%1 = %2" ).arg( r ).arg( px[r * w + midCol] ) ) );
    for ( int i = 0; i < px.size(); ++i )
    {
      QVERIFY2( std::isfinite( px[i] ) || px[i] == -9999.0f,
                qPrintable( QStringLiteral( "cell %1 = %2" ).arg( i ).arg( px[i] ) ) );
    }
    delete wells;
  }

  // 3e) 单井：无约束时整面 = 井值（IDW 退化平面），网格退化轴（单点零宽）
  //     仍得 2×2 有效格。
  void constraintIdwSingleWellConstantPlane()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ), { { QgsPointXY( 5, 5 ), 42.0 } } );
    QVERIFY( wells->isValid() );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    const QString out = mDir.filePath( QStringLiteral( "idw_single.tif" ) );
    params.insert( QStringLiteral( "OUTPUT" ), out );
    QString log;
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_constraint_idw" ), params, &log ).isEmpty(),
              qPrintable( log ) );

    int w = 0, h = 0;
    QVector<float> px;
    QVERIFY( AlgorithmTestBase::readRaster( out, w, h, px ) );
    QCOMPARE( w, 2 );
    QCOMPARE( h, 2 );
    for ( int i = 0; i < px.size(); ++i )
      QCOMPARE( px[i], 42.0f );
    delete wells;
  }

  // 3f) 非有限 z 跳过：NaN/Inf/非数值属性的井不参与插值（不毒化权重和）；
  //     全部井不可用时显式拒绝。
  void constraintIdwNonFiniteZSkippedAndAllInvalidRejected()
  {
    auto *mixed = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "mixed" ),
        { { QgsPointXY( 3, 2 ), std::numeric_limits<double>::quiet_NaN() },
          { QgsPointXY( 7, 2 ), 5.0 } } );
    QVERIFY( mixed->isValid() );
    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( mixed ) );
    params.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    const QString out = mDir.filePath( QStringLiteral( "idw_nan_skip.tif" ) );
    params.insert( QStringLiteral( "OUTPUT" ), out );
    QString log;
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_constraint_idw" ), params, &log ).isEmpty(),
              qPrintable( log ) );
    int w = 0, h = 0;
    QVector<float> px;
    QVERIFY( AlgorithmTestBase::readRaster( out, w, h, px ) );
    for ( int i = 0; i < px.size(); ++i )
      QCOMPARE( px[i], 5.0f ); // 唯一可用井 → 常数面
    delete mixed;

    auto *allBad = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "allbad" ),
        { { QgsPointXY( 3, 2 ), std::numeric_limits<double>::infinity() },
          { QgsPointXY( 7, 2 ), std::numeric_limits<double>::quiet_NaN() } } );
    QVariantMap paramsBad;
    paramsBad.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( allBad ) );
    paramsBad.insert( QStringLiteral( "FIELD" ), QStringLiteral( "z" ) );
    paramsBad.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    paramsBad.insert( QStringLiteral( "OUTPUT" ),
                      mDir.filePath( QStringLiteral( "idw_all_bad.tif" ) ) );
    QVERIFY2( AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_constraint_idw" ), paramsBad, &log ).isEmpty(),
              "all-non-finite z must be rejected" );
    QVERIFY2( log.contains( QStringLiteral( "no usable point features" ) ),
              qPrintable( log ) );
    delete allBad;
  }

  // ---- WP3 Round 4：distance transform 边界矩阵 -----------------------------

  // 4a) 种子重合 + 确定性：三口井其中两口完全同位——多源 Dijkstra 起点重合
  //     无异常，两次运行输出逐像元一致。
  void dtCoincidentSeedsDeterministic()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 2, 2 ), 0.0 },
          { QgsPointXY( 2, 2 ), 0.0 },
          { QgsPointXY( 8, 5 ), 0.0 } } );
    QVERIFY( wells->isValid() );

    auto runOnce = [this, wells]( const QString &out ) {
      QVariantMap params;
      params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
      params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
      params.insert( QStringLiteral( "OUTPUT" ), out );
      QString log;
      QVERIFY2( !AlgorithmTestBase::run(
                    QStringLiteral( "paleo:paleo_distance_transform" ), params, &log ).isEmpty(),
                qPrintable( log ) );
    };
    const QString outA = mDir.filePath( QStringLiteral( "dt_coin_a.tif" ) );
    const QString outB = mDir.filePath( QStringLiteral( "dt_coin_b.tif" ) );
    runOnce( outA );
    runOnce( outB );
    AlgorithmTestBase::RasterDiff diff;
    QVERIFY2( AlgorithmTestBase::compareRasters( outA, outB, 0.0, &diff ),
              qPrintable( diff.message ) );
    delete wells;
  }

  // 4b) 分辨率一致性：同一几何 CELL_SIZE=1 与 0.5 的无屏障输出在同一世界
  //     点上的距离一致（格心偏移 ≤ 半粗格对角，tol=0.8）。
  void dtGridResolutionConsistency()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 0, 0 ), 0.0 }, { QgsPointXY( 10, 10 ), 0.0 } } );
    QVERIFY( wells->isValid() );

    const QString outCoarse = mDir.filePath( QStringLiteral( "dt_res_c.tif" ) );
    const QString outFine = mDir.filePath( QStringLiteral( "dt_res_f.tif" ) );
    QString log;
    QVariantMap paramsC;
    paramsC.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    paramsC.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    paramsC.insert( QStringLiteral( "OUTPUT" ), outCoarse );
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_distance_transform" ), paramsC, &log ).isEmpty(),
              qPrintable( log ) );
    QVariantMap paramsF = paramsC;
    paramsF.insert( QStringLiteral( "CELL_SIZE" ), 0.5 );
    paramsF.insert( QStringLiteral( "OUTPUT" ), outFine );
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_distance_transform" ), paramsF, &log ).isEmpty(),
              qPrintable( log ) );

    // 井 bbox [0,10]² 外扩 10% → 粗/细格都是 [-1,11]²（cell 1 / 0.5）。
    // 粗格 (3,3) 格心 (2.5,7.5)；细格 (7,7) 格心 (2.75,7.25)。
    const auto expectedAt = []( double cx, double cy ) -> double {
      const double d1 = std::hypot( cx - 0.0, cy - 0.0 );
      const double d2 = std::hypot( cx - 10.0, cy - 10.0 );
      return std::min( d1, d2 );
    };
    // 粗格 (3.7,6.2)→col floor(3.5)=3,row floor(9.8-6.2)=3；细格→col floor(7.2)=7,row floor(7.4)=7。
    const float vc = AlgorithmTestBase::rasterCell( outCoarse, 3, 3 );
    const float vf = AlgorithmTestBase::rasterCell( outFine, 7, 7 );
    QVERIFY2( std::fabs( vc - expectedAt( 2.5, 7.5 ) ) < 0.2,
              qPrintable( QString::number( vc ) ) );
    QVERIFY2( std::fabs( vf - expectedAt( 2.75, 7.25 ) ) < 0.2,
              qPrintable( QString::number( vf ) ) );
    QVERIFY2( std::fabs( vc - vf ) < 0.8,
              qPrintable( QStringLiteral( "coarse %1 vs fine %2" ).arg( vc ).arg( vf ) ) );
    delete wells;
  }

  // 4c) 不可达区：四面墙围出的闭合口袋内没有井——口袋内部 nodata（无路
  //     可达），口袋外距离面正常（有限值）。
  void dtUnreachablePocketNodata()
  {
    auto *wells = AlgorithmTestBase::makePointLayer(
        QStringLiteral( "wells" ),
        { { QgsPointXY( 1, 1 ), 0.0 }, { QgsPointXY( 9, 3 ), 0.0 } } );
    auto *typed = AlgorithmTestBase::makeTypedLineLayer(
        QStringLiteral( "pocket" ),
        { { QgsPointXY( 4, 0.5 ), QgsPointXY( 4, 3.5 ) },
          { QgsPointXY( 7, 0.5 ), QgsPointXY( 7, 3.5 ) },
          { QgsPointXY( 3.6, 3 ), QgsPointXY( 6.8, 3 ) },
          { QgsPointXY( 3.6, 1 ), QgsPointXY( 6.8, 1 ) } },
        QStringList{ QStringLiteral( "break_line" ), QStringLiteral( "break_line" ),
                     QStringLiteral( "break_line" ), QStringLiteral( "break_line" ) } );
    QVERIFY( wells->isValid() && typed->isValid() );

    QVariantMap params;
    params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( wells ) );
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( typed ) );
    params.insert( QStringLiteral( "CELL_SIZE" ), 1.0 );
    const QString out = mDir.filePath( QStringLiteral( "dt_pocket.tif" ) );
    params.insert( QStringLiteral( "OUTPUT" ), out );
    QString log;
    QVERIFY2( !AlgorithmTestBase::run(
                  QStringLiteral( "paleo:paleo_distance_transform" ), params, &log ).isEmpty(),
              qPrintable( log ) );

    // 同 3b 的网格/口袋几何：内部自由格 (r1,c4),(r1,c5) 不可达 → nodata。
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 1, 4 ), -9999.0f );
    QCOMPARE( AlgorithmTestBase::rasterCell( out, 1, 5 ), -9999.0f );
    // 口袋外正常：左下角格到井 (1,1) 的绕障距离有限且 > 0。
    const float outside = AlgorithmTestBase::rasterCell( out, 1, 0 );
    QVERIFY2( std::isfinite( outside ) && outside > 0.0f,
              qPrintable( QString::number( outside ) ) );
    delete wells;
    delete typed;
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
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  QgsApplication::processingRegistry();
  GDALAllRegister();
  TestAlgorithmHarness tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_algorithm_harness.moc"
