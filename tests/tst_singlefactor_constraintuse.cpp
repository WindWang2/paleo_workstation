// 层：数据（测试壳位于 tests/，被测对象为 domain/constraintsemantics）
#include <QtTest/QtTest>

#include "domain/constraintsemantics.h"

#include <limits>

using namespace paleo::singlefactor;

namespace
{

const QVector<QPointF> kLine = { QPointF( 0, 0 ), QPointF( 100, 0 ), QPointF( 200, 50 ) };

ConstraintUseDecision decide( const QString &type, const QString &json, const QString &method,
                              const QVector<QPointF> &part = kLine, const ConstraintMethodContext &base = {} )
{
  ConstraintMethodContext context = base;
  context.methodId = method;
  const ConstraintSemantics s =
      resolveConstraintSemantics( type, json, QVariant(), semanticsOptionsForMethod( method ), QStringLiteral( "c1" ) );
  return decideConstraintPart( QStringLiteral( "c1" ), QStringLiteral( "H1" ), type, s, part, 0, 1, context );
}

const QStringList kAllMethods = { QStringLiteral( "local_direction_idw" ), QStringLiteral( "local_direction_kriging" ),
                                  QStringLiteral( "surfer_idw" ),          QStringLiteral( "structural_idw" ),
                                  QStringLiteral( "legacy" ),              QStringLiteral( "kriging" ),
                                  QStringLiteral( "sgs" ),                 QStringLiteral( "cokriging" ),
                                  QStringLiteral( "distance_transform" ),  QStringLiteral( "min_curvature" ) };

} // namespace

class TestConstraintUse : public QObject
{
  Q_OBJECT

private slots:
  void blockModeVocabulary()
  {
    // 上游 constraint_semantics.py 非阻断词表；表外（含未知词/缺省）一律 full_block。
    for ( const char *token : { "none", "off", "no_block", "soft", "partial", "display_only", "0", "false" } )
      QVERIFY2( !isFullBlockModeToken( QString::fromLatin1( token ) ), token );
    QVERIFY( !isFullBlockModeToken( QStringLiteral( "  Display_Only \t" ) ) );
    QVERIFY( isFullBlockModeToken( QStringLiteral( "full_block" ) ) );
    QVERIFY( isFullBlockModeToken( QString() ) );
    QVERIFY( isFullBlockModeToken( QStringLiteral( "whatever" ) ) );
  }

  void typeColumnSemantics()
  {
    const ConstraintSemantics br = resolveConstraintSemantics( QStringLiteral( "break_line" ), QString(), QVariant() );
    QVERIFY( br.valid );
    QCOMPARE( br.semantic, QStringLiteral( "hard_barrier" ) );
    QVERIFY( !br.fromParams );
    const ConstraintSemantics dir = resolveConstraintSemantics( QStringLiteral( "direction_line" ), QString(), QVariant() );
    QCOMPARE( dir.semantic, QStringLiteral( "direction_guide" ) );
    QCOMPARE( dir.ratio, 8.0 );
    const ConstraintSemantics unk = resolveConstraintSemantics( QStringLiteral( "boundary" ), QString(), QVariant() );
    QVERIFY( unk.valid );
    QVERIFY( unk.semantic.isEmpty() );
    QVERIFY( unk.ignoredReason.startsWith( QStringLiteral( "unknown_type" ) ) );
  }

  void paramsSemantics()
  {
    const ConstraintSemantics stop = resolveConstraintSemantics(
        QStringLiteral( "break_line" ), QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"display_only"})" ),
        QVariant() );
    QCOMPARE( stop.declaredSemantic, QStringLiteral( "hard_barrier" ) );
    QCOMPARE( stop.semantic, QStringLiteral( "contour_stop" ) );

    const ConstraintSemantics none = resolveConstraintSemantics(
        QStringLiteral( "break_line" ), QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"none"})" ), QVariant() );
    QVERIFY( none.valid );
    QVERIFY( none.semantic.isEmpty() );
    QCOMPARE( none.ignoredReason, QStringLiteral( "non_blocking" ) );

    // 结构面口径：原文保留，不重分类。
    const ConstraintSemantics kept = resolveConstraintSemantics(
        QStringLiteral( "break_line" ), QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"none"})" ), QVariant(),
        semanticsOptionsForMethod( QStringLiteral( "structural_idw" ) ) );
    QCOMPARE( kept.semantic, QStringLiteral( "hard_barrier" ) );
    QCOMPARE( kept.blockMode, QStringLiteral( "none" ) );

    // 停用行在数值校验之前忽略：坏数值不让整次运行失败。
    const ConstraintSemantics disabled = resolveConstraintSemantics(
        QStringLiteral( "direction_line" ),
        QStringLiteral( R"({"semantic":"direction_guide","enabled":false,"ratio":"abc"})" ), QVariant() );
    QVERIFY( disabled.valid );
    QCOMPARE( disabled.ignoredReason, QStringLiteral( "disabled" ) );
    // 非阻断屏障同理（旧 constraintparse 的判定顺序）。
    const ConstraintSemantics nonBlocking = resolveConstraintSemantics(
        QStringLiteral( "break_line" ),
        QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"soft","ratio":"abc"})" ), QVariant() );
    QVERIFY( nonBlocking.valid );
    QCOMPARE( nonBlocking.ignoredReason, QStringLiteral( "non_blocking" ) );

    const ConstraintSemantics bad = resolveConstraintSemantics(
        QStringLiteral( "direction_line" ), QStringLiteral( R"({"semantic":"direction_guide","ratio":"abc"})" ),
        QVariant() );
    QVERIFY( !bad.valid );
    QVERIFY( bad.error.contains( QStringLiteral( "ratio" ) ) );

    const ConstraintSemantics broken =
        resolveConstraintSemantics( QStringLiteral( "break_line" ), QStringLiteral( "{oops" ), QVariant(), {},
                                    QStringLiteral( "c9" ) );
    QVERIFY( !broken.valid );
    QVERIFY( broken.error.contains( QStringLiteral( "c9" ) ) );

    const ConstraintSemantics future = resolveConstraintSemantics(
        QStringLiteral( "break_line" ), QStringLiteral( R"({"semantic":"hard_barrier","schemaVersion":2})" ), QVariant() );
    QVERIFY( !future.valid );

    const ConstraintSemantics ratio = resolveConstraintSemantics(
        QStringLiteral( "direction_line" ), QStringLiteral( R"({"semantic":"direction_guide","ratio":3.5})" ),
        QVariant() );
    QVERIFY( ratio.valid );
    QCOMPARE( ratio.ratio, 3.5 );
    // 与旧 constraintparse 同口径：数值字段必须是 JSON 数值，字符串数字也算非法。
    QVERIFY( !resolveConstraintSemantics( QStringLiteral( "direction_line" ),
                                          QStringLiteral( R"({"semantic":"direction_guide","ratio":"3.5"})" ),
                                          QVariant() )
                  .valid );
  }

  // 验收：方向线 + 本地方向 IDW / Kriging → consumed。
  void directionLineConsumedByLocalDirection()
  {
    for ( const QString method : { QStringLiteral( "local_direction_idw" ), QStringLiteral( "local_direction_kriging" ) } )
    {
      const ConstraintUseDecision d = decide( QStringLiteral( "direction_line" ), QString(), method );
      QVERIFY2( d.consumed, qPrintable( method ) );
      QVERIFY( d.supported );
      QCOMPARE( d.role, QStringLiteral( "direction_guide" ) );
      QVERIFY( d.reason.isEmpty() );
    }
    // 比值 = 1：引擎跳过（无各向异性），计划如实说不消费。
    const ConstraintUseDecision flat = decide( QStringLiteral( "direction_line" ),
                                               QStringLiteral( R"({"semantic":"direction_guide","ratio":1})" ),
                                               QStringLiteral( "local_direction_idw" ) );
    QVERIFY( !flat.consumed );
    QCOMPARE( flat.reason, QStringLiteral( "direction_ratio_not_above_one" ) );
    const ConstraintUseDecision tooBig = decide( QStringLiteral( "direction_line" ),
                                                 QStringLiteral( R"({"semantic":"direction_guide","ratio":500})" ),
                                                 QStringLiteral( "local_direction_kriging" ) );
    QVERIFY( !tooBig.consumed );
    QVERIFY( tooBig.blocking );
  }

  // 验收：display_only 打断线不是数值硬屏障。
  void displayOnlyIsNotHardBarrier()
  {
    const QString json = QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"display_only"})" );
    for ( const QString &method : kAllMethods )
    {
      const ConstraintUseDecision d = decide( QStringLiteral( "break_line" ), json, method );
      QVERIFY2( d.role != QLatin1String( "hard_barrier" ), qPrintable( method ) );
    }
    const ConstraintUseDecision local = decide( QStringLiteral( "break_line" ), json, QStringLiteral( "local_direction_idw" ) );
    QVERIFY( !local.consumed );
    QVERIFY( local.cartographicOnly );
    QCOMPARE( local.semantic, QStringLiteral( "contour_stop" ) );
    const ConstraintUseDecision legacy = decide( QStringLiteral( "break_line" ), json, QStringLiteral( "legacy" ) );
    QVERIFY( !legacy.consumed );
    QVERIFY( legacy.cartographicOnly );
    // 结构面：上游语义把仅显示停线转为解释软边界（只改趋势，不隔断）。
    const ConstraintUseDecision structural = decide( QStringLiteral( "break_line" ), json, QStringLiteral( "structural_idw" ) );
    QVERIFY( structural.consumed );
    QCOMPARE( structural.role, QStringLiteral( "soft_boundary" ) );
    QVERIFY( structural.warnings.contains( QStringLiteral( "display_only_as_interpretive_soft_boundary" ) ) );
  }

  // 验收：停用约束无数值作用。
  void disabledHasNoNumericEffect()
  {
    for ( const QString &method : kAllMethods )
    {
      for ( const QString &sem : { QStringLiteral( "hard_barrier" ), QStringLiteral( "direction_guide" ),
                                   QStringLiteral( "interpretive_boundary" ) } )
      {
        const ConstraintUseDecision d = decide(
            QStringLiteral( "break_line" ), QStringLiteral( R"({"semantic":"%1","enabled":false})" ).arg( sem ), method );
        QVERIFY2( !d.consumed, qPrintable( method + QLatin1Char( '/' ) + sem ) );
        QVERIFY( d.role.isEmpty() );
        QVERIFY( !d.blocking );
      }
    }
  }

  // 验收：地统 kriging/sgs/cokriging 不读约束，逐条标未使用。
  void geostatFlagsConstraintsUnused()
  {
    for ( const QString method : { QStringLiteral( "kriging" ), QStringLiteral( "sgs" ), QStringLiteral( "cokriging" ) } )
    {
      QVERIFY( !methodReadsConstraints( method ) );
      QVERIFY( !methodConstraintCapabilityNote( method ).isEmpty() );
      for ( const QString type : { QStringLiteral( "break_line" ), QStringLiteral( "direction_line" ) } )
      {
        const ConstraintUseDecision d = decide( type, QString(), method );
        QVERIFY( !d.consumed );
        QCOMPARE( d.reason, QStringLiteral( "method_does_not_read_line_constraints" ) );
      }
    }
    QVERIFY( methodReadsConstraints( QStringLiteral( "idw" ) ) );
    QCOMPARE( canonicalConstraintMethod( QStringLiteral( "idw" ) ), QStringLiteral( "legacy" ) );
  }

  // 验收：无效几何绝不静默参与。
  void invalidGeometryNeverUsed_data()
  {
    QTest::addColumn<QVector<QPointF>>( "part" );
    QTest::addColumn<QString>( "issue" );
    const double nan = std::numeric_limits<double>::quiet_NaN();
    QTest::newRow( "single" ) << QVector<QPointF>{ QPointF( 1, 1 ) } << QStringLiteral( "short_line" );
    QTest::newRow( "zero" ) << QVector<QPointF>{ QPointF( 1, 1 ), QPointF( 1, 1 ), QPointF( 1, 1 ) }
                            << QStringLiteral( "zero_length" );
    QTest::newRow( "bowtie" ) << QVector<QPointF>{ QPointF( 0, 0 ), QPointF( 10, 10 ), QPointF( 10, 0 ), QPointF( 0, 10 ) }
                              << QStringLiteral( "self_intersecting" );
    QTest::newRow( "foldback" ) << QVector<QPointF>{ QPointF( 0, 0 ), QPointF( 10, 0 ), QPointF( 5, 0 ) }
                                << QStringLiteral( "self_intersecting" );
    QTest::newRow( "nan" ) << QVector<QPointF>{ QPointF( 0, 0 ), QPointF( nan, 3 ) }
                           << QStringLiteral( "non_finite_coordinate" );
  }
  void invalidGeometryNeverUsed()
  {
    QFETCH( QVector<QPointF>, part );
    QFETCH( QString, issue );
    QCOMPARE( checkConstraintPart( part ).issue, issue );
    for ( const QString &method : kAllMethods )
    {
      for ( const QString type : { QStringLiteral( "break_line" ), QStringLiteral( "direction_line" ),
                                   QStringLiteral( "boundary" ) } )
      {
        const ConstraintUseDecision d = decide( type, QString(), method, part );
        QVERIFY2( !d.consumed, qPrintable( method + QLatin1Char( '/' ) + type ) );
        QVERIFY( !d.geometryValid );
      }
    }
  }

  void validGeometryEdgeCases()
  {
    // 闭合线首尾相接不算自交；连续重复点只告警。
    const QVector<QPointF> ring = { QPointF( 0, 0 ), QPointF( 10, 0 ), QPointF( 10, 10 ), QPointF( 0, 0 ) };
    QVERIFY( checkConstraintPart( ring ).issue.isEmpty() );
    const QVector<QPointF> dup = { QPointF( 0, 0 ), QPointF( 0, 0 ), QPointF( 10, 0 ) };
    const ConstraintPartCheck c = checkConstraintPart( dup );
    QVERIFY( c.issue.isEmpty() );
    QVERIFY( c.duplicateVertices );
    QCOMPARE( c.length, 10.0 );
    const ConstraintUseDecision d = decide( QStringLiteral( "break_line" ), QString(),
                                            QStringLiteral( "local_direction_idw" ), dup );
    QVERIFY( d.consumed );
    QVERIFY( d.warnings.contains( QStringLiteral( "duplicate_vertices" ) ) );
  }

  void domainAndHorizon()
  {
    ConstraintMethodContext ctx;
    ctx.domain = QRectF( 1000, 1000, 100, 100 );
    const ConstraintUseDecision out = decide( QStringLiteral( "break_line" ), QString(),
                                              QStringLiteral( "local_direction_idw" ), kLine, ctx );
    QVERIFY( out.domainKnown );
    QVERIFY( !out.inDomain );
    QVERIFY( !out.consumed );
    QCOMPARE( out.reason, QStringLiteral( "outside_compute_domain" ) );
    // 线段穿过矩形但端点都在外：真裁剪判定，算在域内。
    QVERIFY( polylineTouchesRect( { QPointF( -10, 50 ), QPointF( 200, 50 ) }, QRectF( 0, 0, 100, 100 ) ) );
    QVERIFY( !polylineTouchesRect( { QPointF( -10, -10 ), QPointF( 200, -5 ) }, QRectF( 0, 0, 100, 100 ) ) );

    ConstraintMethodContext other;
    other.horizon = QStringLiteral( "H2" );
    const ConstraintUseDecision h = decide( QStringLiteral( "break_line" ), QString(),
                                            QStringLiteral( "local_direction_idw" ), kLine, other );
    QVERIFY( !h.consumed );
    QCOMPARE( h.reason, QStringLiteral( "other_horizon" ) );
  }

  void perMethodCapabilities()
  {
    // Surfer：硬屏障为有限断层绕行，方向线不支持（不假装全支持）。
    const ConstraintUseDecision sb = decide( QStringLiteral( "break_line" ), QString(), QStringLiteral( "surfer_idw" ) );
    QVERIFY( sb.consumed );
    QVERIFY( sb.warnings.contains( QStringLiteral( "finite_fault_path_detour" ) ) );
    const ConstraintUseDecision sd = decide( QStringLiteral( "direction_line" ), QString(), QStringLiteral( "surfer_idw" ) );
    QVERIFY( !sd.consumed );
    QCOMPARE( sd.reason, QStringLiteral( "unsupported_by_method" ) );
    // 旧 IDW：方向线 → 全域平均方向；逐线比值不参与；软边界不支持；未知类型 → 凸包裁剪。
    const ConstraintUseDecision ld = decide( QStringLiteral( "direction_line" ), QString(), QStringLiteral( "legacy" ) );
    QCOMPARE( ld.role, QStringLiteral( "global_direction" ) );
    ConstraintMethodContext iso;
    iso.legacyAnisoRatio = 1.0;
    QVERIFY( !decide( QStringLiteral( "direction_line" ), QString(), QStringLiteral( "legacy" ), kLine, iso ).consumed );
    const ConstraintUseDecision ls = decide( QStringLiteral( "boundary" ),
                                             QStringLiteral( R"({"semantic":"interpretive_boundary"})" ),
                                             QStringLiteral( "legacy" ) );
    QVERIFY( !ls.consumed );
    QCOMPARE( decide( QStringLiteral( "boundary" ), QString(), QStringLiteral( "legacy" ) ).role,
              QStringLiteral( "hull_clip" ) );
    // 非法 params：引擎整次拒绝的方法标 blocking，旧 IDW 只是不用。
    const QString broken = QStringLiteral( R"({"semantic":"direction_guide","ratio":"x"})" );
    QVERIFY( decide( QStringLiteral( "direction_line" ), broken, QStringLiteral( "local_direction_idw" ) ).blocking );
    QVERIFY( !decide( QStringLiteral( "direction_line" ), broken, QStringLiteral( "legacy" ) ).blocking );
    // 结构面：停用行保留但不消费。
    const ConstraintUseDecision sdis = decide(
        QStringLiteral( "break_line" ), QStringLiteral( R"({"semantic":"hard_barrier","enabled":false})" ),
        QStringLiteral( "structural_idw" ) );
    QVERIFY( !sdis.consumed );
    QCOMPARE( sdis.reason, QStringLiteral( "disabled" ) );
  }

  // P2：结构面引擎是 contour_only 屏障模型——完全阻断线也只是解释软边界，
  // 计划不得声称硬隔断；距离变换/最小曲率只认完全阻断打断线。
  void structuralContourOnlyAndGriddingEngines()
  {
    const ConstraintUseDecision full = decide( QStringLiteral( "break_line" ), QString(), QStringLiteral( "structural_idw" ) );
    QVERIFY( full.consumed );
    QCOMPARE( full.role, QStringLiteral( "soft_boundary" ) );
    QVERIFY( full.warnings.contains( QStringLiteral( "full_block_as_contour_only_soft_boundary" ) ) );
    ConstraintMethodContext off;
    off.structuralEnableBarriers = false;
    const ConstraintUseDecision disabled =
        decide( QStringLiteral( "break_line" ), QString(), QStringLiteral( "structural_idw" ), kLine, off );
    QVERIFY( !disabled.consumed );
    QVERIFY( disabled.cartographicOnly ); // 仍停等值线
    const ConstraintUseDecision none = decide(
        QStringLiteral( "break_line" ), QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"none"})" ),
        QStringLiteral( "structural_idw" ) );
    QCOMPARE( none.reason, QStringLiteral( "non_blocking" ) );
    const ConstraintUseDecision zeroSoft = decide(
        QStringLiteral( "boundary" ), QStringLiteral( R"({"semantic":"interpretive_boundary","softStrength":0})" ),
        QStringLiteral( "structural_idw" ) );
    QCOMPARE( zeroSoft.reason, QStringLiteral( "soft_strength_zero" ) );
    for ( const QString &method : { QStringLiteral( "distance_transform" ), QStringLiteral( "min_curvature" ) } )
    {
      QVERIFY( methodReadsConstraints( method ) );
      QCOMPARE( decide( QStringLiteral( "break_line" ), QString(), method ).role, QStringLiteral( "hard_barrier" ) );
      const ConstraintUseDecision shown = decide(
          QStringLiteral( "break_line" ), QStringLiteral( R"({"semantic":"hard_barrier","blockMode":"display_only"})" ),
          method );
      QVERIFY2( !shown.consumed, qPrintable( method ) ); // display_only 不隔断
      QVERIFY( shown.cartographicOnly );
      QCOMPARE( decide( QStringLiteral( "direction_line" ), QString(), method ).reason,
                QStringLiteral( "unsupported_by_method" ) );
      QVERIFY( !decide( QStringLiteral( "direction_line" ), QStringLiteral( R"({"semantic":"direction_guide","ratio":"x"})" ),
                        method ).blocking );
      QVERIFY( !methodConstraintCapabilityNote( method ).isEmpty() );
    }
  }

  void multipartIdsAndSummary()
  {
    const ConstraintSemantics s = resolveConstraintSemantics( QStringLiteral( "break_line" ), QString(), QVariant() );
    ConstraintMethodContext ctx;
    ctx.methodId = QStringLiteral( "local_direction_idw" );
    QVector<ConstraintUseDecision> all;
    all << decideConstraintPart( QStringLiteral( "f" ), QString(), QStringLiteral( "break_line" ), s, kLine, 0, 2, ctx );
    all << decideConstraintPart( QStringLiteral( "f" ), QString(), QStringLiteral( "break_line" ), s,
                                 { QPointF( 3, 3 ) }, 1, 2, ctx );
    all << decideNonLineConstraint( QStringLiteral( "p" ), QString(), QStringLiteral( "break_line" ), s, false, ctx );
    QCOMPARE( all[0].id, QStringLiteral( "f#0" ) );
    QCOMPARE( all[1].id, QStringLiteral( "f#1" ) );
    QCOMPARE( all[2].reason, QStringLiteral( "not_line" ) );
    const QVariantMap summary = summarizeConstraintDecisions( all );
    QCOMPARE( summary.value( QStringLiteral( "consumed" ) ).toStringList(), QStringList{ QStringLiteral( "f#0" ) } );
    QCOMPARE( summary.value( QStringLiteral( "unused" ) ).toList().size(), 2 );
    QCOMPARE( summary.value( QStringLiteral( "roles" ) ).toMap().value( QStringLiteral( "hard_barrier" ) ).toInt(), 1 );
    QVERIFY( all[0].toVariant().value( QStringLiteral( "consumed" ) ).toBool() );
  }
};

QTEST_GUILESS_MAIN( TestConstraintUse )
#include "tst_singlefactor_constraintuse.moc"
