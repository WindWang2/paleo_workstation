// 层：数据（测试壳位于 tests/，被测对象为 domain/constraintsemantics）
#include <QtTest/QtTest>

#include "domain/constraintsemantics.h"

#include <QElapsedTimer>
#include <QRandomGenerator>

#include <cmath>
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

// P4 对照：改造前的全段对 O(n²) 自交判定（逐字照搬谓词与排除规则），用于等价性测试。
double refCross( QPointF a, QPointF b, QPointF c )
{
  return ( b.x() - a.x() ) * ( c.y() - a.y() ) - ( b.y() - a.y() ) * ( c.x() - a.x() );
}
bool refOnSegment( QPointF p, QPointF q, QPointF r )
{
  return q.x() <= std::max( p.x(), r.x() ) && q.x() >= std::min( p.x(), r.x() ) && q.y() <= std::max( p.y(), r.y() ) &&
         q.y() >= std::min( p.y(), r.y() );
}
bool refSegmentsIntersect( QPointF p1, QPointF p2, QPointF p3, QPointF p4 )
{
  const double d1 = refCross( p3, p4, p1 ), d2 = refCross( p3, p4, p2 ), d3 = refCross( p1, p2, p3 ),
               d4 = refCross( p1, p2, p4 );
  if ( ( ( d1 > 0 && d2 < 0 ) || ( d1 < 0 && d2 > 0 ) ) && ( ( d3 > 0 && d4 < 0 ) || ( d3 < 0 && d4 > 0 ) ) )
    return true;
  return ( d1 == 0 && refOnSegment( p3, p1, p4 ) ) || ( d2 == 0 && refOnSegment( p3, p2, p4 ) ) ||
         ( d3 == 0 && refOnSegment( p1, p3, p2 ) ) || ( d4 == 0 && refOnSegment( p1, p4, p2 ) );
}
bool refFoldsBack( QPointF a, QPointF b, QPointF c )
{
  if ( refCross( a, b, c ) != 0 )
    return false;
  return ( b.x() - a.x() ) * ( c.x() - b.x() ) + ( b.y() - a.y() ) * ( c.y() - b.y() ) < 0;
}
bool refSelfIntersecting( const QVector<QPointF> &part )
{
  QVector<QPointF> u;
  for ( const QPointF &p : part )
    if ( u.isEmpty() || u.last() != p )
      u.append( p );
  if ( u.size() < 2 )
    return false;
  const int segments = static_cast<int>( u.size() ) - 1;
  const bool closed = u.size() > 3 && u.first() == u.last();
  for ( int i = 0; i + 1 < segments; ++i )
    if ( refFoldsBack( u[i], u[i + 1], u[i + 2] ) )
      return true;
  if ( closed && refFoldsBack( u[segments - 1], u[segments], u[1] ) )
    return true;
  for ( int i = 0; i < segments; ++i )
    for ( int j = i + 2; j < segments; ++j )
    {
      if ( closed && i == 0 && j == segments - 1 )
        continue;
      if ( refSegmentsIntersect( u[i], u[i + 1], u[j], u[j + 1] ) )
        return true;
    }
  return false;
}

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

  // P0/P1（pr457-closure）：schemaVersion 必须是合法范围内的整数——非整数
  // （1.5/1.1）不得被 toInt() 截断后当成 1 接受。
  void fractionalSchemaVersionIsRejected()
  {
    for ( const char *json : { R"({"semantic":"hard_barrier","schemaVersion":1.5})",
                               R"({"semantic":"hard_barrier","schemaVersion":1.1})",
                               R"({"semantic":"hard_barrier","schemaVersion":0})",
                               R"({"semantic":"hard_barrier","schemaVersion":-1})",
                               R"({"semantic":"hard_barrier","schemaVersion":1e300})" } )
    {
      const ConstraintSemantics s = resolveConstraintSemantics( QStringLiteral( "break_line" ),
                                                                 QString::fromLatin1( json ), QVariant() );
      QVERIFY2( !s.valid, json );
      QVERIFY2( !s.error.isEmpty(), json );
    }
    // 字符串版本号同样非法（类型错误，不做宽松转换）。
    QVERIFY( !resolveConstraintSemantics( QStringLiteral( "break_line" ),
                                          QStringLiteral( R"({"semantic":"hard_barrier","schemaVersion":"1"})" ),
                                          QVariant() )
                  .valid );
    // 整数 1 正常接受。
    QVERIFY( resolveConstraintSemantics( QStringLiteral( "break_line" ),
                                         QStringLiteral( R"({"semantic":"hard_barrier","schemaVersion":1})" ),
                                         QVariant() )
                  .valid );
  }

  void unsupportedSchemaVersionIsRejected()
  {
    // 未知更高版本不能被静默当成旧版本。
    for ( const char *json : { R"({"semantic":"hard_barrier","schemaVersion":2})",
                               R"({"semantic":"hard_barrier","schemaVersion":99})" } )
    {
      const ConstraintSemantics s = resolveConstraintSemantics( QStringLiteral( "break_line" ),
                                                                 QString::fromLatin1( json ), QVariant() );
      QVERIFY2( !s.valid, json );
      QVERIFY2( s.error.contains( QStringLiteral( "版本" ) ), json );
    }
  }

  // P0/P1：enabled 存在时必须是布尔——"false"/0/null/[] 不得经 toBool(true)
  // 默认成启用。
  void enabledMustBeBoolean()
  {
    // 合法：字段不存在 → 默认启用；布尔 false → 停用；布尔 true → 启用。
    QVERIFY( resolveConstraintSemantics( QStringLiteral( "break_line" ),
                                         QStringLiteral( R"({"semantic":"hard_barrier"})" ), QVariant() )
                  .enabled );
    QVERIFY( !resolveConstraintSemantics( QStringLiteral( "break_line" ),
                                          QStringLiteral( R"({"semantic":"hard_barrier","enabled":false})" ), QVariant() )
                   .enabled );
    QVERIFY( resolveConstraintSemantics( QStringLiteral( "break_line" ),
                                         QStringLiteral( R"({"semantic":"hard_barrier","enabled":true})" ), QVariant() )
                  .enabled );
    // 非法类型：字段存在但不是布尔 → invalid（带诊断），不猜语义。
    for ( const char *json : { R"({"semantic":"hard_barrier","enabled":"false"})",
                               R"({"semantic":"hard_barrier","enabled":"true"})",
                               R"({"semantic":"hard_barrier","enabled":0})",
                               R"({"semantic":"hard_barrier","enabled":1})",
                               R"({"semantic":"hard_barrier","enabled":null})",
                               R"({"semantic":"hard_barrier","enabled":[]})" } )
    {
      const ConstraintSemantics s = resolveConstraintSemantics( QStringLiteral( "break_line" ),
                                                                 QString::fromLatin1( json ), QVariant() );
      QVERIFY2( !s.valid, json );
      QVERIFY2( !s.error.isEmpty(), json );
    }
  }

  // P0/P1：非法参数（含非有限数值、blockMode 非字符串）在语义层就 invalid，
  // 不会以「有效约束」身份进入任何引擎的分派。
  void invalidConstraintParameterDoesNotReachEngine()
  {
    for ( const char *json : { R"({"semantic":"hard_barrier","blockMode":5})",
                               R"({"semantic":"hard_barrier","blockMode":null})",
                               R"({"semantic":"direction_guide","ratio":1e999})",
                               R"({"semantic":"interpretive_boundary","softStrength":1e999})" } )
    {
      const ConstraintSemantics s = resolveConstraintSemantics( QStringLiteral( "break_line" ),
                                                                 QString::fromLatin1( json ), QVariant() );
      QVERIFY2( !s.valid, json );
      QVERIFY2( !s.error.isEmpty(), json );
      // 所有方法下都不消费、且不进入方法分派。
      for ( const QString &method : kAllMethods )
      {
        const ConstraintUseDecision d = decide( QStringLiteral( "break_line" ), QString::fromLatin1( json ), method );
        QVERIFY2( !d.consumed, qPrintable( method + QLatin1Char( ' ' ) + json ) );
      }
    }
  }

  // 兼容：合法旧数据（无 params_json 仅 type 列；v1 schema；停用行）照常加载。
  void legacyValidConstraintStillLoads()
  {
    const ConstraintSemantics legacyRow = resolveConstraintSemantics( QStringLiteral( "break_line" ), QString(),
                                                                       QVariant() );
    QVERIFY( legacyRow.valid );
    QCOMPARE( legacyRow.semantic, QStringLiteral( "hard_barrier" ) );
    const ConstraintSemantics legacyDirection = resolveConstraintSemantics( QStringLiteral( "direction_line" ),
                                                                             QString(), QVariant() );
    QCOMPARE( legacyDirection.semantic, QStringLiteral( "direction_guide" ) );
    const ConstraintSemantics v1 = resolveConstraintSemantics(
        QStringLiteral( "break_line" ),
        QStringLiteral( R"({"schemaVersion":1,"semantic":"hard_barrier","blockMode":"full_block","ratio":2.5})" ),
        QVariant() );
    QVERIFY( v1.valid );
    QCOMPARE( v1.ratio, 2.5 );
    // schema_version 列（非 JSON 字段）仍作为后备版本来源。
    const ConstraintSemantics viaColumn = resolveConstraintSemantics(
        QStringLiteral( "break_line" ), QStringLiteral( R"({"semantic":"hard_barrier"})" ), QVariant( 1 ) );
    QVERIFY( viaColumn.valid );
    // 列里未知版本同样拒绝。
    QVERIFY( !resolveConstraintSemantics( QStringLiteral( "break_line" ),
                                          QStringLiteral( R"({"semantic":"hard_barrier"})" ), QVariant( 2 ) )
                  .valid );
    // 停用行为不变。
    const ConstraintSemantics disabledRow = resolveConstraintSemantics(
        QStringLiteral( "break_line" ), QStringLiteral( R"({"semantic":"hard_barrier","enabled":false})" ), QVariant() );
    QVERIFY( disabledRow.valid );
    QCOMPARE( disabledRow.ignoredReason, QStringLiteral( "disabled" ) );
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

  // P1（pr457-closure）：数值鲁棒——实数意义下共线的折线（浮点 ulp 噪声）在
  // 任意坐标量级下都不得误判自交。斜率取 1/3、2/7 等二进制不可精确表示的值，
  // 顶点 y 坐标带逐点舍入噪声（这正是 ulp 级「近共线」的现实来源）。
  void nearCollinearLargeCoordinatesAreStable()
  {
    QRandomGenerator rng( 20264570 );
    const QList<double> slopes = { 1.0 / 3.0, 2.0 / 7.0, -1.0 / 3.0, 5.0 / 6.0, 0.25, -2.0 / 9.0 };
    int checked = 0;
    for ( int n = 0; n < 4000; ++n )
    {
      const double scale = std::pow( 10.0, double( rng.bounded( 8 ) ) ); // 1e0..1e7
      const double slope = slopes.at( rng.bounded( slopes.size() ) );
      const double offset = double( rng.bounded( 21 ) - 10 ) * scale;
      const int count = 4 + static_cast<int>( rng.bounded( 8 ) );
      QVector<QPointF> part;
      for ( int k = 0; k < count; ++k )
      {
        const double x = offset + double( k ) * scale;
        part.append( QPointF( x, offset + slope * ( x - offset ) ) );
      }
      const ConstraintPartCheck c = checkConstraintPart( part );
      QVERIFY2( c.issue.isEmpty(),
                qPrintable( QStringLiteral( "scale=%1 slope=%2 count=%3: %4" )
                               .arg( scale )
                               .arg( slope )
                               .arg( count )
                               .arg( c.issue ) ) );
      ++checked;
    }
    QVERIFY( checked > 3000 );
  }

  // P1：平移不变性——同一折线整体平移（真实几何不变），自交判定必须一致。
  // 该 fixture 在精确浮点相等谓词下会翻转（单位量级判自交、平移 1e7 后判
  // 干净）：p1→p2→p3 实数共线且 p3 从 p2 折回 p1 延长线（真回折），大坐标
  // 下 cross 的坐标舍入噪声（~u·M·L）淹没精确零。
  void selfIntersectionIsTranslationInvariant()
  {
    const QVector<QPointF> part{ QPointF( 8.142857142857142, 5.428571428571429 ),
                                 QPointF( 0.5555555555555556, 0.6666666666666666 ),
                                 QPointF( 0.3333333333333333, 0.3333333333333333 ),
                                 QPointF( 4.333333333333333, 6.333333333333333 ) };
    QCOMPARE( checkConstraintPart( part ).issue, QStringLiteral( "self_intersecting" ) );
    for ( const double shift : { 1e7, 1e9, 123456789.125, 9876543.2 } )
    {
      QVector<QPointF> moved;
      for ( const QPointF &p : part )
        moved.append( QPointF( p.x() + shift, p.y() + shift ) );
      QCOMPARE( checkConstraintPart( moved ).issue, QStringLiteral( "self_intersecting" ) );
    }
    // 干净折线同样不得因平移变脏。
    const QVector<QPointF> clean{ QPointF( 0.0, 0.0 ), QPointF( 4.0 + 1.0 / 3.0, 0.0 ),
                                  QPointF( 4.0 + 1.0 / 3.0, 3.0 + 1.0 / 7.0 ), QPointF( 8.0 + 2.0 / 7.0, 6.0 ) };
    QCOMPARE( checkConstraintPart( clean ).issue, QString() );
    for ( const double shift : { 1e7, 1e9, 123456789.125 } )
    {
      QVector<QPointF> moved;
      for ( const QPointF &p : clean )
        moved.append( QPointF( p.x() + shift, p.y() + shift ) );
      QCOMPARE( checkConstraintPart( moved ).issue, QString() );
    }
    // 随机小数坐标电池：base 与平移后判定必须逐一一致。
    QRandomGenerator rng( 20264571 );
    const QList<double> fracs = { 1.0 / 3.0, 2.0 / 3.0, 1.0 / 7.0, 2.0 / 7.0, 3.0 / 7.0,
                                  1.0 / 9.0,  2.0 / 9.0,  4.0 / 9.0,  5.0 / 9.0 };
    for ( int n = 0; n < 3000; ++n )
    {
      const int count = 3 + static_cast<int>( rng.bounded( 7 ) );
      QVector<QPointF> part2;
      for ( int k = 0; k < count; ++k )
        part2.append( QPointF( double( rng.bounded( 9 ) ) + fracs.at( rng.bounded( fracs.size() ) ),
                               double( rng.bounded( 9 ) ) + fracs.at( rng.bounded( fracs.size() ) ) ) );
      if ( n % 5 == 0 )
        part2.append( part2.first() );
      const QString base = checkConstraintPart( part2 ).issue;
      for ( const double shift : { 1e7, 123456789.125 } )
      {
        QVector<QPointF> moved;
        for ( const QPointF &p : part2 )
          moved.append( QPointF( p.x() + shift, p.y() + shift ) );
        QCOMPARE( checkConstraintPart( moved ).issue, base );
      }
    }
  }

  // P1：闭合环（首末点同一 double）在任意量级下合法；端点相接的相邻段合法；
  // 真正的自交（bowtie）仍然分得开。
  void closedRingsAndRealIntersectionsStaySeparable()
  {
    const double big = 1e7;
    QCOMPARE( checkConstraintPart( QVector<QPointF>{ QPointF( big, big ), QPointF( big + 10, big ),
                                                     QPointF( big + 10, big + 10 ), QPointF( big, big + 10 ),
                                                     QPointF( big, big ) } ).issue,
              QString() );
    QCOMPARE( checkConstraintPart( QVector<QPointF>{ QPointF( 0, 0 ), QPointF( 5, 0 ), QPointF( 5, 5 ) } ).issue,
              QString() );
    QCOMPARE( checkConstraintPart( QVector<QPointF>{ QPointF( 0, 0 ), QPointF( 10, 10 ), QPointF( 10, 0 ),
                                                     QPointF( 0, 10 ) } ).issue,
              QStringLiteral( "self_intersecting" ) );
    // 顺原路回溯（共线重叠）依旧判自交——不能因鲁棒化放过真退化。
    QCOMPARE( checkConstraintPart( QVector<QPointF>{ QPointF( 0, 0 ), QPointF( 10, 0 ), QPointF( 20, 0 ),
                                                     QPointF( 10, 0 ) } ).issue,
              QStringLiteral( "self_intersecting" ) );
  }

  // P1：极短线段与近重复顶点——重复顶点只告警不算自交，微小但不为零的线段
  // 不是 zero_length。
  void tinySegmentsAndNearDuplicateVertices()
  {
    const double big = 1e6;
    // 完全重复顶点：duplicateVertices 告警、几何仍有效（非零长）。
    ConstraintPartCheck dup = checkConstraintPart(
        QVector<QPointF>{ QPointF( big, big ), QPointF( big, big ), QPointF( big + 1, big ) } );
    QVERIFY( dup.duplicateVertices );
    QCOMPARE( dup.issue, QString() );
    // 相对坐标 1e-13（64u 量级以内）的近重复顶点：同完全重复处理。
    ConstraintPartCheck near = checkConstraintPart(
        QVector<QPointF>{ QPointF( big, big ), QPointF( big + big * 1e-13, big ), QPointF( big + 1, big ) } );
    QCOMPARE( near.issue, QString() );
    // 微小非零长度（单位量级坐标）：不是 zero_length，也不是自交。
    ConstraintPartCheck tiny =
        checkConstraintPart( QVector<QPointF>{ QPointF( 0, 0 ), QPointF( 1e-9, 0 ), QPointF( 1e-9, 1e-9 ) } );
    QCOMPARE( tiny.issue, QString() );
    // 全程重复 → zero_length 语义保留。
    QCOMPARE( checkConstraintPart( QVector<QPointF>{ QPointF( 1, 1 ), QPointF( 1, 1 ), QPointF( 1, 1 ) } ).issue,
              QStringLiteral( "zero_length" ) );
  }

  // P4：扫描剪枝与改造前全段对判定逐例等价（小整数坐标，大量共线/端点相触退化情形）。
  void selfIntersectionSweepMatchesBruteForce()
  {
    QRandomGenerator rng( 20261015 );
    int intersecting = 0, clean = 0;
    for ( int n = 0; n < 4000; ++n )
    {
      const int count = 3 + static_cast<int>( rng.bounded( 10 ) );
      QVector<QPointF> part;
      for ( int k = 0; k < count; ++k )
        part.append( QPointF( rng.bounded( 6 ), rng.bounded( 6 ) ) );
      if ( n % 5 == 0 )
        part.append( part.first() ); // 闭合
      const ConstraintPartCheck c = checkConstraintPart( part );
      if ( c.issue == QLatin1String( "zero_length" ) )
        continue;
      const bool expected = refSelfIntersecting( part );
      QCOMPARE( c.issue == QLatin1String( "self_intersecting" ), expected );
      ( expected ? intersecting : clean )++;
    }
    QVERIFY( intersecting > 100 && clean > 100 ); // 两类都充分覆盖
  }

  // P4 性能：数万顶点导入线（单调折线 + 闭合圆环）在 UI 线程可接受时间内判完。
  // 改造前全段对约 n²/2 ≈ 1.25e9 次（5 万段）；预算按扫描剪枝留足余量。
  void selfIntersectionLargeLinesAreFast()
  {
    QVector<QPointF> zigzag;
    for ( int k = 0; k < 50000; ++k )
      zigzag.append( QPointF( k, ( k % 2 ) ? 3.0 : 0.0 ) );
    QVector<QPointF> ring;
    for ( int k = 0; k < 50000; ++k )
    {
      const double t = 2.0 * std::acos( -1.0 ) * k / 50000.0;
      ring.append( QPointF( 1000.0 * std::cos( t ), 1000.0 * std::sin( t ) ) );
    }
    ring.append( ring.first() );
    QVector<QPointF> crossed = zigzag;
    crossed.append( QPointF( 25000.5, -10 ) ); // 末段回穿整条折线
    QElapsedTimer timer;
    timer.start();
    QVERIFY( checkConstraintPart( zigzag ).issue.isEmpty() );
    QVERIFY( checkConstraintPart( ring ).issue.isEmpty() );
    QCOMPARE( checkConstraintPart( crossed ).issue, QStringLiteral( "self_intersecting" ) );
    const qint64 ms = timer.elapsed();
    qInfo( "self-intersection 3x50k vertices: %lld ms", static_cast<long long>( ms ) );
    QVERIFY2( ms < 2000, qPrintable( QStringLiteral( "%1 ms" ).arg( ms ) ) );
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
