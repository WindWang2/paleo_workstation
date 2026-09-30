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
