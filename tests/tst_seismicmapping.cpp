#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include "../src/io/horizonbinner.h"
#include "../src/io/timedeptool.h"
#include "../src/io/wellfileparsers.h"
#include "../src/services/seismicmapping.h"

// wave3/derived-publish — §40 SeismicMapLink 的两个新建依赖（E1 决议）：
//   · SurveyGridGeometry：P1/P2/P3 测网仿射，map XY ↔ (inline, crossline)；
//     超出测网 → 明确失败 + 原因（不夹取）。
//   · LineCdpMap：逐 inline 的 (crossline ↔ CDP) 线性映射（道头观测构造）。
//   · VelocityModel：井 TD 表双向插值 depth↔TWT（TimeDepthTool 契约：文件
//     顺序、不排序、不外推；反向按 time 列同一契约实现）。
// 几何参数用真 D61 头（411×641，P1(1315,4165,0,0)→P2(1315,4805,12793,0)→
// P3(1725,4805,12793,16406)）；TD 用真 A1 表夹具（testdata/project_area）。
class TestSeismicMapping : public QObject
{
  Q_OBJECT

private:
  static HorizonHeader realD61Header()
  {
    HorizonHeader h;
    h.gridRows = 411;
    h.gridCols = 641;
    h.p1Inline = 1315; h.p1Xline = 4165; h.p1x = 0.0; h.p1y = 0.0;
    h.p2Inline = 1315; h.p2Xline = 4805; h.p2x = 12793.0; h.p2y = 0.0;
    h.p3Inline = 1725; h.p3Xline = 4805; h.p3x = 12793.0; h.p3y = 16406.0;
    h.hasP1 = h.hasP2 = h.hasP3 = true;
    h.zUnits = QStringLiteral( "ms" );
    return h;
  }

  // {inline, xline, cdp} 观测（模拟道头索引；真文件每 inline 641 道）。
  static QVector<QVector<qint64>> cdpObservations()
  {
    QVector<QVector<qint64>> obs;
    for ( int inl = 1515; inl <= 1517; ++inl )
      for ( int xl = 4165; xl <= 4169; ++xl )
        obs.append( QVector<qint64>{ inl, xl, inl * 100000 + ( xl - 4165 ) } );
    return obs;
  }

private slots:

  // 真 D61 几何：锚点精确、格点往返一致（XY→(inline,xline)→XY 复原）。
  void gridGeometryAnchorsAndRoundTrip()
  {
    const SurveyGridGeometry g = SurveyGridGeometry::fromHorizonHeader( realD61Header() );
    QVERIFY( g.valid );
    QCOMPARE( g.inlineMin, 1315 );
    QCOMPARE( g.inlineMax, 1725 );
    QCOMPARE( g.xlineMin, 4165 );
    QCOMPARE( g.xlineMax, 4805 );

    double x = 0, y = 0;
    g.inlineXlineToXy( 1315, 4165, &x, &y );
    QVERIFY( qAbs( x - 0.0 ) < 1e-6 && qAbs( y - 0.0 ) < 1e-6 );
    g.inlineXlineToXy( 1725, 4805, &x, &y );
    QVERIFY( qAbs( x - 12793.0 ) < 1e-6 && qAbs( y - 16406.0 ) < 1e-6 );
    g.inlineXlineToXy( 1315, 4805, &x, &y );
    QVERIFY( qAbs( x - 12793.0 ) < 1e-6 && qAbs( y - 0.0 ) < 1e-6 );

    // 网内采样点往返：整数 inline/xline → XY → 整数 inline/xline 复原。
    for ( int inl = g.inlineMin; inl <= g.inlineMax; inl += 97 )
      for ( int xl = g.xlineMin; xl <= g.xlineMax; xl += 113 )
      {
        g.inlineXlineToXy( inl, xl, &x, &y );
        int inl2 = -1, xl2 = -1;
        QString reason;
        QVERIFY2( g.xyToInlineXline( x, y, &inl2, &xl2, &reason ),
                  qPrintable( QStringLiteral( "(%1,%2): %3" ).arg( x ).arg( y ).arg( reason ) ) );
        QCOMPARE( inl2, inl );
        QCOMPARE( xl2, xl );
      }

    // 真井 A1（5288.67, 8219.94）：inline ≈ 1315 + 8219.94/16406·410、
    // xline ≈ 4165 + 5288.67/12793·640（与残差行 inline 口径一致）。
    int inlA1 = -1, xlA1 = -1;
    QVERIFY( g.xyToInlineXline( 5288.67, 8219.94, &inlA1, &xlA1 ) );
    QCOMPARE( inlA1, 1315 + static_cast<int>( qRound( 8219.94 / 16406.0 * 410.0 ) ) );
    QCOMPARE( xlA1, 4165 + static_cast<int>( qRound( 5288.67 / 12793.0 * 640.0 ) ) );
  }

  // 超网：明确失败 + 原因；绝不夹取到网边界。
  void gridGeometryOutsideFailsExplicitly()
  {
    const SurveyGridGeometry g = SurveyGridGeometry::fromHorizonHeader( realD61Header() );
    QVERIFY( g.valid );
    // 网外点都取「半道以外」（最近格点语义：网缘半道内归边缘道，见头注释）。
    const QVector<QPair<double, double>> outside = {
      { -100.0, -100.0 }, { 20000.0, 100.0 }, { 100.0, 20000.0 },
      { -1000.0, 100.0 }, { 14000.0, 100.0 } };
    for ( const auto &xy : outside )
    {
      int inl = -999, xl = -999;
      QString reason;
      QVERIFY2( !g.xyToInlineXline( xy.first, xy.second, &inl, &xl, &reason ),
                qPrintable( QStringLiteral( "(%1,%2) should be outside" ).arg( xy.first ).arg( xy.second ) ) );
      QVERIFY2( !reason.isEmpty(), "outside-grid failure must carry a reason" );
      QVERIFY( reason.contains( QStringLiteral( "超出测网" ) ) );
      // 不夹取：返回值不落到网边界。
      QVERIFY( inl != g.inlineMin && inl != g.inlineMax && xl != g.xlineMin && xl != g.xlineMax );
    }

    // 恰在网边界角上的点属于测网（含边界）。
    int inl = 0, xl = 0;
    double x = 0, y = 0;
    g.inlineXlineToXy( g.inlineMax, g.xlineMax, &x, &y );
    QVERIFY( g.xyToInlineXline( x, y, &inl, &xl ) );
    QCOMPARE( inl, g.inlineMax );
    QCOMPARE( xl, g.xlineMax );
  }

  // 斜测网（P1→P2 带旋转）：仿射仍往返一致。
  void gridGeometrySkewedNetwork()
  {
    HorizonHeader h;
    h.gridRows = 5;
    h.gridCols = 7;
    h.p1Inline = 100; h.p1Xline = 200; h.p1x = 0.0; h.p1y = 0.0;
    h.p2Inline = 100; h.p2Xline = 206; h.p2x = 60.0; h.p2y = 25.0;   // crossline 方向斜
    h.p3Inline = 104; h.p3Xline = 206; h.p3x = 60.0; h.p3y = 105.0; // inline 方向正北
    h.hasP1 = h.hasP2 = h.hasP3 = true;
    const SurveyGridGeometry g = SurveyGridGeometry::fromHorizonHeader( h );
    QVERIFY( g.valid );
    for ( int inl = 100; inl <= 104; ++inl )
      for ( int xl = 200; xl <= 206; ++xl )
      {
        double x = 0, y = 0;
        g.inlineXlineToXy( inl, xl, &x, &y );
        int inl2 = -1, xl2 = -1;
        QString reason;
        QVERIFY2( g.xyToInlineXline( x, y, &inl2, &xl2, &reason ), qPrintable( reason ) );
        QCOMPARE( inl2, inl );
        QCOMPARE( xl2, xl );
      }
  }

  // 缺锚/退化几何 → 构造失败（valid=false），不产生伪映射。
  void gridGeometryRejectsDegenerate()
  {
    HorizonHeader broken = realD61Header();
    broken.hasP2 = false;
    QVERIFY( !SurveyGridGeometry::fromHorizonHeader( broken ).valid );
    HorizonHeader degenerate = realD61Header();
    degenerate.p2x = 0.0; degenerate.p2y = 0.0; // P1==P2 → 行列式 0
    QVERIFY( !SurveyGridGeometry::fromHorizonHeader( degenerate ).valid );
  }

  // 逐线 CDP：正向 cdpFor、反向 xlineFor 往返；未知测线/无变化 CDP 明确失败。
  void lineCdpAffineRoundTrip()
  {
    const LineCdpMap map = LineCdpMap::fromObservations( cdpObservations() );
    QVERIFY( map.valid );
    for ( int inl = 1515; inl <= 1517; ++inl )
      for ( int xl = 4165; xl <= 4169; ++xl )
      {
        qint64 cdp = -1;
        QString reason;
        QVERIFY2( map.cdpFor( inl, xl, &cdp, &reason ), qPrintable( reason ) );
        QCOMPARE( cdp, qint64( inl * 100000 + ( xl - 4165 ) ) );
        int xl2 = -1;
        QVERIFY2( map.xlineFor( inl, cdp, &xl2, &reason ), qPrintable( reason ) );
        QCOMPARE( xl2, xl );
      }

    qint64 cdp = -1;
    QString reason;
    QVERIFY( !map.cdpFor( 1999, 4165, &cdp, &reason ) );
    QVERIFY( !reason.isEmpty() );
    int xl = -1;
    QVERIFY( !map.xlineFor( 1999, 100, &xl, &reason ) );
    QVERIFY( !reason.isEmpty() );
  }

  // 非仿射 CDP（道头乱序/重复错位）→ 该线被拒，可读原因。
  void lineCdpRejectsNonAffine()
  {
    QVector<QVector<qint64>> obs = cdpObservations();
    // 1517 线中间道 CDP 跳变 → 非线性。
    for ( auto &o : obs )
      if ( o[0] == 1517 && o[1] == 4167 )
        o[2] += 500;
    const LineCdpMap map = LineCdpMap::fromObservations( obs );
    qint64 cdp = -1;
    QString reason;
    QVERIFY( !map.cdpFor( 1517, 4167, &cdp, &reason ) );
    QVERIFY( !reason.isEmpty() );
    // 未受影响的线照常可用。
    QVERIFY( map.cdpFor( 1515, 4165, &cdp ) );
    QCOMPARE( cdp, qint64( 1515 * 100000 ) );
  }

  // 井 TD 双向插值：真 A1 表（testdata 夹具）——depth→TWT→depth 不发散；
  // 范围外明确 OutOfRange（不外推）；未知井 NoTable。
  void velocityModelRoundTripsRealTable()
  {
#ifdef PROJECT_FIXTURE_DIR
    QFile f( QStringLiteral( PROJECT_FIXTURE_DIR ) + QStringLiteral( "/A1_TD.dat" ) );
    QVERIFY2( f.open( QIODevice::ReadOnly ), qPrintable( f.fileName() ) );
    const TimeDepthTable table = parseTimeDepthText( f.readAll() );
    f.close();
    QVERIFY( table.rows.size() > 10 );
    QVERIFY( !table.wellName.isEmpty() );

    VelocityModel model;
    model.setWellTable( QStringLiteral( "well-A1" ), table );
    QVERIFY( model.hasWell( QStringLiteral( "well-A1" ) ) );

    // 样点与样点间往返：线性段的正反映射互逆，误差数值级 1e-9。
    const double t0 = table.rows.first().timeMs;
    const double t1 = table.rows.last().timeMs;
    for ( int i = 0; i < 24; ++i )
    {
      const double t = t0 + ( t1 - t0 ) * i / 23.0;
      const TimeDepthTool::TdResult d = model.depthForTwt( QStringLiteral( "well-A1" ), t );
      QVERIFY2( d.ok(), qPrintable( TimeDepthTool::reasonText( d.status ) ) );
      const TimeDepthTool::TdResult back = model.twtForDepth( QStringLiteral( "well-A1" ), d.timeMs );
      QVERIFY2( back.ok(), qPrintable( TimeDepthTool::reasonText( back.status ) ) );
      QVERIFY2( qAbs( back.timeMs - t ) < 1e-6 * ( 1.0 + qAbs( t ) ),
                qPrintable( QStringLiteral( "t=%1 back=%2" ).arg( t ).arg( back.timeMs ) ) );
    }

    // 已知样点手算：首行 390ms ↔ TVD 274.208；末行 1356ms ↔ 2169.893。
    {
      const TimeDepthTool::TdResult d = model.depthForTwt( QStringLiteral( "well-A1" ), 390.0 );
      QVERIFY( d.ok() );
      QVERIFY( qAbs( d.timeMs - 274.208 ) < 1e-6 );
      const TimeDepthTool::TdResult t = model.twtForDepth( QStringLiteral( "well-A1" ), 274.208 );
      QVERIFY( t.ok() );
      QVERIFY( qAbs( t.timeMs - 390.0 ) < 1e-6 );
    }

    // 范围外：明确 OutOfRange（不外推、不夹端点）。
    QCOMPARE( model.depthForTwt( QStringLiteral( "well-A1" ), t0 - 10.0 ).status,
              TimeDepthTool::TdStatus::OutOfRange );
    QCOMPARE( model.depthForTwt( QStringLiteral( "well-A1" ), t1 + 10.0 ).status,
              TimeDepthTool::TdStatus::OutOfRange );
    QCOMPARE( model.twtForDepth( QStringLiteral( "well-A1" ), 1.0 ).status,
              TimeDepthTool::TdStatus::OutOfRange );
    QCOMPARE( model.twtForDepth( QStringLiteral( "well-A1" ), 99999.0 ).status,
              TimeDepthTool::TdStatus::OutOfRange );
    // 未知井 → NoTable。
    QCOMPARE( model.twtForDepth( QStringLiteral( "well-XX" ), 1000.0 ).status,
              TimeDepthTool::TdStatus::NoTable );
    QCOMPARE( model.depthForTwt( QStringLiteral( "well-XX" ), 1000.0 ).status,
              TimeDepthTool::TdStatus::NoTable );
#else
    QSKIP( "PROJECT_FIXTURE_DIR not defined" );
#endif
  }

  // 反向插值同契约：time 列不单调 → NonMonotonic；可用样点不足 → NoTable。
  void velocityModelContractMirrorsTimeDepthTool()
  {
    {
      TimeDepthTable bad;
      TdRow a; a.timeMs = 100; a.tvd = 100; a.hasTvd = true;
      TdRow b; b.timeMs = 200; b.tvd = 200; b.hasTvd = true;
      TdRow c; c.timeMs = 150; c.tvd = 150; c.hasTvd = true; // 文件顺序不单调
      bad.rows = { a, b, c };
      VelocityModel model;
      model.setWellTable( QStringLiteral( "w" ), bad );
      QCOMPARE( model.depthForTwt( QStringLiteral( "w" ), 120.0 ).status,
                TimeDepthTool::TdStatus::NonMonotonic );
    }
    {
      TimeDepthTable one;
      TdRow a; a.timeMs = 100; a.tvd = 100; a.hasTvd = true;
      one.rows = { a };
      VelocityModel model;
      model.setWellTable( QStringLiteral( "w" ), one );
      QCOMPARE( model.depthForTwt( QStringLiteral( "w" ), 100.0 ).status,
                TimeDepthTool::TdStatus::NoTable );
    }
  }
};

QTEST_MAIN( TestSeismicMapping )
#include "tst_seismicmapping.moc"
