// 层：测试壳
// 四子层段体系端到端合成测试：T70/T71/T72 → 2 个父单元 → 8 个子层段；
// 导入 → 校验 → 单井相 → 地震掩膜 → 单因素 → 编图 → 综合 → 修改 → 失效 → 保存重开。
#include <QtTest>
#include <QTemporaryDir>
#include <cmath>
#include <paleo_data/service.h>
#include "../src/domain/mappinghorizons.h"
#include "../src/domain/stratigraphicunit.h"
#include "../src/services/stratigraphicsubintervals.h"
#include "../src/linkage/unitselectionbridge.h"
#include "../src/services/subintervalfactors.h"
#include "../src/domain/wellsection.h"

namespace fw = paleo::data::framework;
using namespace paleo::strat;

class StratigraphicSubIntervalsTest : public QObject
{
  Q_OBJECT
private:
  static QStringList boundaries() { return { QStringLiteral( "T70" ), QStringLiteral( "T71" ), QStringLiteral( "T72" ) }; }
  static WellTopTable tops()
  {
    WellTopTable t;
    t[1] = { { "T70", 1000 }, { "T71", 1100 }, { "T72", 1180 } };
    t[2] = { { "T70", 1250 }, { "T71", 1290 } }; // 未钻穿 T72
    return t;
  }

private slots:
  void labelsAreClassifiedNotRenamed()
  {
    QCOMPARE( classifyLabel( "T70", boundaries() ).kind, LabelKind::Boundary );
    QCOMPARE( classifyLabel( "T70–T71", boundaries() ).kind, LabelKind::Interval );
    const auto s = classifyLabel( "T70-T71/S3", boundaries() );
    QCOMPARE( s.kind, LabelKind::SubInterval );
    QCOMPARE( s.subIndex, 3 );
    QCOMPARE( classifyLabel( "T70-T72", boundaries() ).kind, LabelKind::NonAdjacent ); // 旧 E12 段
    QCOMPARE( classifyLabel( "D61", boundaries() ).kind, LabelKind::Unknown );
    QCOMPARE( adjacentIntervals( boundaries() ).size(), 2 );
    QVERIFY( requiresSubInterval( "single_factor" ) && requiresSubInterval( "map" ) );
    // 工程默认界面名单由参数决定，mappingHorizons 仍可用（标定/历史兼容）
    QVERIFY( adjacentIntervals( mappingHorizons() ).size() == std::max<qsizetype>( 0, mappingHorizons().size() - 1 ) );
  }

  
  void bridgePackingAndTopBottom()
  {
    using namespace paleo::strat;
    qRegisterMetaType<UnitSelection>();
    const QStringList bounds{ QStringLiteral( "T70" ), QStringLiteral( "T71" ), QStringLiteral( "T72" ) };
    const auto scheme = draftScheme( QStringLiteral( "project-default" ), QStringLiteral( "t" ), bounds, {} );
    const auto opts = selectorOptions( scheme );
    QVERIFY( opts.size() >= 1 );
    const UnitSelection s2 = makeSelection( scheme, opts.at( 0 ).intervalId, 2 );
    QVERIFY( s2.isValid() );
    QCOMPARE( s2.topBoundary, QStringLiteral( "T70" ) );
    QCOMPARE( s2.bottomBoundary, QStringLiteral( "T71" ) );
    QCOMPARE( s2.subIndex, 2 );
    const QVariantMap m = toMap( s2 );
    QVERIFY( m.contains( QStringLiteral( "subIntervalId" ) ) );
    QCOMPARE( fromMap( m ), s2 );
    QCOMPARE( analysisLayerKey( s2 ), layerToken( s2 ) );
    QCOMPARE( analysisLayerKey( {}, QStringLiteral( "T70" ) ), QStringLiteral( "T70" ) );
  }


  void sandRatioWellFaciesAndEvidence()
  {
    // 子层段 → 砂地比单因素 → 井相分类 → DS 持久化 → 编图证据可读
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const std::string path = dir.filePath( "facies" ).toStdString();
    std::string err;
    paleo::data::DataService ds;
    QVERIFY2( ds.createProject( path, "相", err ), err.c_str() );
    auto draft = draftScheme( "facies-scheme", "相方案", boundaries(), tops() );
    paleo::data::StratigraphicScheme head;
    std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY2( ds.commitStratigraphicScheme( draft, {}, head, issues, err ), err.c_str() );
    const auto options = selectorOptions( head );
    const UnitSelection s2 = makeSelection( head, options[0].intervalId, 2 );
    QVERIFY( s2.isValid() );

    // 井 1：T70–T71 = [1000,1100]，S2 等比 = [1025,1050)
    QVector<wellsection::LithoSegment> segs;
    wellsection::LithoSegment mud, sand;
    mud.topMd = 1000; mud.baseMd = 1030; mud.litho = QStringLiteral( "泥岩" );
    sand.topMd = 1030; sand.baseMd = 1060; sand.litho = QStringLiteral( "细砂岩" );
    segs << mud << sand;

    const auto metrics = sandRatioForWellSubInterval( head, s2.subIntervalId, 1, segs, nullptr );
    QVERIFY2( metrics.ok, qPrintable( metrics.reason ) );
    QCOMPARE( metrics.topMd, 1025.0 );
    QCOMPARE( metrics.bottomMd, 1050.0 );
    QCOMPARE( metrics.thicknessMd, 25.0 );
    QCOMPARE( metrics.sandThicknessMd, 20.0 ); // [1030,1050)
    QCOMPARE( metrics.sandRatio, 0.8 );
    QCOMPARE( metrics.faciesClass.id, QStringLiteral( "clean_sand" ) );

    const qlonglong wid = stableWellKey( QStringLiteral( "W1" ) );
    const auto factorBind = bindSandRatioFactor( ds, s2, wid, metrics, QStringLiteral( "asset-sand" ) );
    QVERIFY2( factorBind.ok, qPrintable( factorBind.error ) );
    const auto faciesBind = bindWellFaciesFromSandRatio( ds, s2, wid, metrics, QStringLiteral( "asset-facies" ) );
    QVERIFY2( faciesBind.ok, qPrintable( faciesBind.error ) );

    // 地震：切片 ≠ 层段；层段综合需策略
    {
      BindResult bad = bindSeismicFaciesResult( ds, s2, SeismicFaciesScope::Interval, 3, 0.9, {},
                                                QStringLiteral( "seis" ) );
      QVERIFY( !bad.ok );
    }
    QVector<SeismicSliceVote> votes{ { 3, 10 }, { 3, 5 }, { 2, 4 }, { 0, 2 } };
    const auto agg = aggregateSeismicSliceVotes( votes, 0.5 );
    QVERIFY( agg.ok );
    QCOMPARE( agg.classCode, 3 );
    const auto seisBind = bindSeismicFaciesResult( ds, s2, SeismicFaciesScope::Interval, agg.classCode,
                                                   agg.coverage, agg.strategyId, QStringLiteral( "seis-int" ) );
    QVERIFY2( seisBind.ok, qPrintable( seisBind.error ) );
    const auto sliceBind = bindSeismicFaciesResult( ds, s2, SeismicFaciesScope::Slice, 2, 0, {},
                                                    QStringLiteral( "seis-slice" ) );
    QVERIFY2( sliceBind.ok, qPrintable( sliceBind.error ) );

    std::vector<paleo::data::SubIntervalResultBinding> results;
    QVERIFY( ds.listSubIntervalResults( "facies-scheme", {}, results, err ) );
    QVERIFY( results.size() >= 4 );
    const auto rows = evidenceRowsFromBindings(
      results, { QStringLiteral( "single_factor" ), QStringLiteral( "well_facies" ),
                 QStringLiteral( "seismic_facies" ) } );
    QVERIFY( rows.size() >= 4 );
    for ( const auto &v : rows )
    {
      const auto m = v.toMap();
      // 证据身份是稳定 SubIntervalId，不是 horizon 显示名
      QCOMPARE( m.value( QStringLiteral( "source_horizon" ) ).toString(), s2.subIntervalId );
      QVERIFY( m.value( QStringLiteral( "subIntervalId" ) ).toString() == s2.subIntervalId );
    }

    // provisional 窗与 wellRange 一致（未解释内部分界时）
    double pt = 0, pb = 0;
    QVERIFY( provisionalSubRange( 1000, 1100, 2, &pt, &pb ) );
    QCOMPARE( pt, 1025.0 );
    QCOMPARE( pb, 1050.0 );
    const auto prox = sandRatioForProvisionalSub( 1000, 1100, 2, segs, nullptr );
    QVERIFY( prox.ok );
    QCOMPARE( prox.sandRatio, metrics.sandRatio );
  }

  void endToEnd()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const std::string path = dir.filePath( "area" ).toStdString();
    std::string err;
    paleo::data::StratigraphicScheme head;
    UnitSelection s2, s1, agg;
    {
      paleo::data::DataService ds;
      QVERIFY2( ds.createProject( path, "合成", err ), err.c_str() );
      auto draft = draftScheme( "synthetic", "合成方案", boundaries(), tops() );
      QCOMPARE( draft.intervals.size(), size_t( 2 ) );
      QCOMPARE( draft.subIntervals.size(), size_t( 8 ) );
      std::vector<paleo::data::FrameworkIssue> issues;
      QVERIFY2( ds.commitStratigraphicScheme( draft, {}, head, issues, err ), err.c_str() );
      const auto options = selectorOptions( head );
      QCOMPARE( options.size(), 2 );
      QCOMPARE( options[0].label, QStringLiteral( "T70–T71" ) );
      QCOMPARE( options[0].subLabels, QStringList( { "S1", "S2", "S3", "S4" } ) );

      // 单井相：同一 SubIntervalId 在两口井深度不同
      s2 = makeSelection( head, options[0].intervalId, 2 );
      s1 = makeSelection( head, options[0].intervalId, 1 );
      agg = makeSelection( head, options[0].intervalId, 0 );
      QVERIFY( s2.isValid() && s1.isValid() && agg.isValid() && agg.aggregate );
      QCOMPARE( s2.displayName, QStringLiteral( "T70–T71/S2" ) );
      const QVector<double> depths{ 1000, 1020, 1030, 1049, 1050, 1260, 1265, 1270 };
      const auto m1 = wellSampleMask( head, 1, s2.subIntervalId, depths );
      QCOMPARE( m1, QVector<char>( { 0, 0, 1, 1, 0, 0, 0, 0 } ) );
      const auto m2 = wellSampleMask( head, 2, s2.subIntervalId, depths );
      QCOMPARE( m2, QVector<char>( { 0, 0, 0, 0, 0, 1, 1, 0 } ) );
      // 未钻穿：T71–T72 的子层段在 2 井不可解析
      QString reason;
      QVERIFY( !wellSampleMask( head, 2, options[1].subIntervalIds[0], depths, &reason ).contains( 1 ) );
      QVERIFY( !reason.isEmpty() );

      // 地震：由该子层段顶/底时间生成，不是固定时间四等分
      paleo::data::SubIntervalDivision div;
      const auto mask = fw::seismicMask( { 100, 60 }, { 140, 180 }, 0, 4, 50, div, 2 );
      QCOMPARE( int( mask.values[28] ), 1 ); // 道 0：S2=[110,120)，112ms
      QCOMPARE( int( mask.values[50 + 25] ), 1 ); // 道 1 [90,120) 内 100ms
      QCOMPARE( int( mask.values[50 + 22] ), 0 );

      // 单因素：只在子层段内取值；样点不足 → NaN，不复制父单元
      const QVector<double> values{ 1, 2, 3, 5, 9, 4, 6, 8 };
      QCOMPARE( wellSubIntervalMean( head, 1, s2.subIntervalId, depths, values ), 4.0 );
      QVERIFY( std::isnan( wellSubIntervalMean( head, 1, s2.subIntervalId, depths, values, 3 ) ) );
      // 综合：缺任一子层段值 → 显式 NaN + 缺失列表
      QStringList missing;
      QHash<QString, double> per{ { options[0].subIntervalIds[0], 1 }, { options[0].subIntervalIds[1], 2 } };
      QVERIFY( std::isnan( aggregateInterval( head, 1, options[0].intervalId, per, &missing ) ) );
      QCOMPARE( missing, QStringList( { "S3", "S4" } ) );
      per[options[0].subIntervalIds[2]] = 3;
      per[options[0].subIntervalIds[3]] = 4;
      QCOMPARE( aggregateInterval( head, 1, options[0].intervalId, per ), 2.5 );

      // 成果登记：单井相 S2、单因素 S1、编图 S2、综合
      auto bind = [&]( const char *kind, const UnitSelection &sel, long long well ) {
        paleo::data::SubIntervalResultBinding b, out;
        b.kind = kind;
        b.schemeId = sel.schemeId.toStdString();
        b.schemeRevision.value = sel.revision.toStdString();
        b.interval.value = sel.intervalId.toStdString();
        b.subInterval.value = sel.subIntervalId.toStdString();
        b.aggregate = sel.aggregate;
        b.wellId = well;
        b.assetRef = cacheKey( sel, QString::fromStdString( fw::subIntervalFingerprint( head, b.subInterval ) ), "p" ).toStdString();
        QVERIFY2( ds.bindSubIntervalResult( b, out, err ), err.c_str() );
      };
      bind( "well_facies", s2, 1 );
      bind( "single_factor", s1, 0 );
      bind( "map", s2, 0 );
      bind( "composite", agg, 0 );
    }
    // 保存重开 → 修改 S2/S3 分界 → 只失效相关成果
    paleo::data::DataService ds;
    QVERIFY2( ds.openProject( path, err ), err.c_str() );
    paleo::data::StratigraphicScheme reopened;
    QVERIFY( ds.readStratigraphicScheme( "synthetic", {}, reopened, err ) );
    QCOMPARE( reopened.revision.value, head.revision.value );
    QCOMPARE( selectorOptions( reopened )[0].subIntervalIds, selectorOptions( head )[0].subIntervalIds );
    auto edit = reopened;
    std::string why;
    QVERIFY( fw::setWellInternalBoundaries( edit, 1, paleo::data::IntervalId{ s2.intervalId.toStdString() },
                                            { 1025, 1060, 1075 }, "confirmed", why ) ); // 只改 S2/S3 分界（1050→1060）
    paleo::data::StratigraphicScheme next;
    std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY2( ds.commitStratigraphicScheme( edit, reopened.revision.value, next, issues, err ), err.c_str() );
    std::vector<paleo::data::SubIntervalResultBinding> results;
    QVERIFY( ds.listSubIntervalResults( "synthetic", {}, results, err ) );
    QCOMPARE( results.size(), size_t( 4 ) );
    for ( const auto &r : results )
    {
      const bool affected = r.subInterval.value == s2.subIntervalId.toStdString() || r.aggregate;
      QCOMPARE( QString::fromStdString( r.status ), affected ? QStringLiteral( "stale" ) : QStringLiteral( "current" ) );
      QCOMPARE( QString::fromStdString( r.schemeRevision.value ), QString::fromStdString( head.revision.value ) );
    }
    // 新修订下的不等厚范围（同一身份）
    const auto r = fw::wellRange( next, 1, paleo::data::SubIntervalId{ s2.subIntervalId.toStdString() } );
    QVERIFY( r.resolved && !r.provisional );
    QCOMPARE( r.top, 1025.0 );
    QCOMPARE( r.bottom, 1060.0 );
  }
};

QTEST_MAIN( StratigraphicSubIntervalsTest )
#include "tst_stratigraphic_subintervals.moc"
