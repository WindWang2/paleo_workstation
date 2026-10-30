// 层：测试壳
// 四子层段体系端到端合成测试：T70/T71/T72 → 2 个父单元 → 8 个子层段；
// 导入 → 校验 → 单井相 → 地震掩膜 → 单因素 → 编图 → 综合 → 修改 → 失效 → 保存重开。
#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cmath>
#include <paleo_data/service.h>
#include "../src/domain/mappinghorizons.h"
#include "../src/domain/stratigraphicunit.h"
#include "../src/services/stratigraphicsubintervals.h"
#include "../src/linkage/unitselectionbridge.h"
#include "../src/linkage/selectioncontext.h"
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

  void bindAnalysisIgnoresPageHorizonString()
  {
    // combo / pageHorizon 不可驱动分析键；无 UnitSelection → analysisKey 空。
    SelectionContext ctx;
    ctx.setActiveHorizon( QStringLiteral( "T70" ) );
    {
      const auto b = bindAnalysis( &ctx, QStringLiteral( "D61" ) );
      QVERIFY( !b.hasUnit );
      QVERIFY( b.analysisKey.isEmpty() );
      QCOMPARE( b.constraintHorizon, QStringLiteral( "D61" ) ); // 界面编辑 legacy
    }
    UnitSelection u;
    u.schemeId = QStringLiteral( "sch" );
    u.revision = QStringLiteral( "r1" );
    u.intervalId = QStringLiteral( "int-1" );
    u.subIntervalId = QStringLiteral( "sub-abc" );
    u.subIndex = 2;
    u.topBoundary = QStringLiteral( "T70" );
    u.bottomBoundary = QStringLiteral( "T71" );
    u.displayName = QStringLiteral( "T70–T71/S2" );
    QVERIFY( u.isValid() );
    ctx.setActiveUnit( u );
    {
      const auto b = bindAnalysis( &ctx, QStringLiteral( "D61" ) );
      QVERIFY( b.hasUnit );
      QCOMPARE( b.analysisKey, QStringLiteral( "sub-abc" ) );
      QCOMPARE( b.constraintHorizon, QStringLiteral( "T70" ) );
      const auto id = b.publishIdentity();
      QCOMPARE( id.value( QStringLiteral( "subIntervalId" ) ).toString(), QStringLiteral( "sub-abc" ) );
      QCOMPARE( id.value( QStringLiteral( "source_horizon" ) ).toString(), QStringLiteral( "sub-abc" ) );
    }
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


  void autoBindAndSeismicPublishPath()
  {
    // 生产路径：提取行 → autoBindSandRatio；切片投票 → publishSeismicSliceAggregate
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const QString projectDir = dir.path();
    const QString areaRoot = projectDir + QStringLiteral( "/artifacts/metadata/area-model" );
    QVERIFY( QDir().mkpath( areaRoot ) );

    UnitSelection s2;
    QVariantList rows;
    {
      std::string err;
      paleo::data::DataService ds;
      QVERIFY2( ds.createProject( areaRoot.toStdString(), "自动绑定", err ), err.c_str() );
      auto draft = draftScheme( "auto-scheme", "自动", boundaries(), tops() );
      paleo::data::StratigraphicScheme head;
      std::vector<paleo::data::FrameworkIssue> issues;
      QVERIFY2( ds.commitStratigraphicScheme( draft, {}, head, issues, err ), err.c_str() );
      const auto options = selectorOptions( head );
      s2 = makeSelection( head, options[0].intervalId, 2 );
      QVERIFY( s2.isValid() );

      QVariantMap row;
      row.insert( QStringLiteral( "well_id" ), QStringLiteral( "W1" ) );
      row.insert( QStringLiteral( "value" ), 0.8 );
      row.insert( QStringLiteral( "contributing" ), true );
      row.insert( QStringLiteral( "windowTopMd" ), 1025.0 );
      row.insert( QStringLiteral( "windowBottomMd" ), 1050.0 );
      rows << row;

      const auto metrics = metricsFromKnownRatio( 1025.0, 1050.0, 0.8 );
      QVERIFY( metrics.ok );
      QCOMPARE( metrics.faciesClass.id, QStringLiteral( "clean_sand" ) );

      QVector<SeismicSliceVote> votes{ { 3, 10 }, { 3, 5 }, { 2, 4 } };
      const auto pub = publishSeismicSliceAggregate( ds, s2, votes, QStringLiteral( "seis-int-pub" ), 0.5,
                                                     true, QStringLiteral( "seis-slice-pub" ) );
      QVERIFY2( pub.ok, qPrintable( pub.error ) );
      QCOMPARE( pub.aggregate.classCode, 3 );
      QVERIFY( pub.intervalBind.ok );
      QVERIFY( pub.sliceBinds.size() >= 2 );
      QVERIFY( !bindSeismicFaciesResult( ds, s2, SeismicFaciesScope::Interval, 1, 0.9, {}, "bad" ).ok );
    }

    // 无 area-model → 跳过
    const auto skip = autoBindSandRatioExtraction( projectDir + QStringLiteral( "/empty" ), s2, rows,
                                                   QStringLiteral( "asset" ) );
    QVERIFY( !skip.attempted );

    // 有 area-model/data.sqlite → 自动 bind 砂地比+井相
    const auto bound = autoBindSandRatioExtraction( projectDir, s2, rows, QStringLiteral( "asset-auto" ) );
    QVERIFY2( bound.attempted, qPrintable( bound.message ) );
    QCOMPARE( bound.boundWells, 1 );
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

  void occurrenceSummaryAndSchemeLoad()
  {
    // 红绿灯：井出现 complete/affected/pending + bindings current/stale
    const auto scheme = draftScheme( QStringLiteral( "occ" ), QStringLiteral( "出现" ), boundaries(), tops() );
    const auto opts = selectorOptions( scheme );
    QVERIFY( !opts.isEmpty() );
    const QString iid = opts.at( 0 ).intervalId;
    // 草稿含井 1 complete、井 2 not_penetrated（缺 T72 但 T70–T71 完整）— 对 T70–T71：两井均 complete
    auto rows = summarizeSubIntervalOccurrences( scheme, iid, nullptr );
    QCOMPARE( rows.size(), 4 );
    QCOMPARE( rows.at( 0 ).label, QStringLiteral( "S1" ) );
    QVERIFY( rows.at( 0 ).wellsComplete >= 1 );
    QCOMPARE( rows.at( 0 ).light, QStringLiteral( "green" ) );
    QVERIFY( rows.at( 0 ).line.contains( QStringLiteral( "[绿]" ) ) );

    // 注入 stale 绑定 → 应变红
    std::vector<paleo::data::SubIntervalResultBinding> binds;
    paleo::data::SubIntervalResultBinding stale;
    stale.subInterval.value = rows.at( 1 ).subIntervalId.toStdString();
    stale.status = "stale";
    stale.kind = "single_factor";
    binds.push_back( stale );
    auto withStale = summarizeSubIntervalOccurrences( scheme, iid, &binds );
    QCOMPARE( withStale.at( 1 ).bindingsStale, 1 );
    QCOMPARE( withStale.at( 1 ).light, QStringLiteral( "red" ) );
    QVERIFY( withStale.at( 1 ).line.contains( QStringLiteral( "[红]" ) ) );

    // makeSelection 写入 schemeId（草稿 revision 可空）
    const UnitSelection sel = makeSelection( scheme, iid, 2 );
    QVERIFY( sel.isValid() );
    QCOMPARE( sel.schemeId, QStringLiteral( "occ" ) );
    QCOMPARE( sel.subIndex, 2 );

    // 方案目录：提交后可 list/load
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const std::string path = dir.filePath( "scheme-ui" ).toStdString();
    std::string err;
    paleo::data::DataService ds;
    QVERIFY2( ds.createProject( path, "方案UI", err ), err.c_str() );
    auto draft = draftScheme( QStringLiteral( "ui-scheme" ), QStringLiteral( "UI方案" ), boundaries(), tops() );
    paleo::data::StratigraphicScheme committed;
    std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY2( ds.commitStratigraphicScheme( draft, {}, committed, issues, err ), err.c_str() );
    QString qerr;
    const auto choices = listProjectSchemes( ds, &qerr );
    QVERIFY2( !choices.isEmpty(), qerr.toUtf8().constData() );
    QCOMPARE( choices.first().id, QStringLiteral( "ui-scheme" ) );
    paleo::data::StratigraphicScheme loaded;
    QVERIFY( loadProjectScheme( ds, QStringLiteral( "ui-scheme" ), {}, loaded, &qerr ) );
    QCOMPARE( QString::fromStdString( loaded.revision.value ), QString::fromStdString( committed.revision.value ) );
    const UnitSelection fromLoaded = makeSelection( loaded, selectorOptions( loaded ).at( 0 ).intervalId, 1 );
    QCOMPARE( fromLoaded.schemeId, QStringLiteral( "ui-scheme" ) );
    QCOMPARE( fromLoaded.revision, QString::fromStdString( committed.revision.value ) );
  }


  void pageLabeledChoicesFiltersAndPages()
  {
    QVector<LabeledIdChoice> all;
    for ( int i = 0; i < 95; ++i )
    {
      LabeledIdChoice c;
      c.id = QStringLiteral( "id-%1" ).arg( i, 3, 10, QChar( '0' ) );
      c.label = QStringLiteral( "修订-%1%2" ).arg( i ).arg( i % 7 == 0 ? QStringLiteral( "-特殊" ) : QString() );
      all.push_back( c );
    }
    // 默认页大小 40 → 3 页
    auto p0 = pageLabeledChoices( all, {}, 0 );
    QCOMPARE( p0.filteredTotal, 95 );
    QCOMPARE( p0.pageCount, 3 );
    QCOMPARE( p0.pageIndex, 0 );
    QCOMPARE( p0.items.size(), kSchemeRevisionPageSize );
    QCOMPARE( p0.items.first().id, QStringLiteral( "id-000" ) );

    auto p2 = pageLabeledChoices( all, {}, 2 );
    QCOMPARE( p2.pageIndex, 2 );
    QCOMPARE( p2.items.size(), 15 );
    QCOMPARE( p2.items.last().id, QStringLiteral( "id-094" ) );

    // 越界夹到末页
    auto pOver = pageLabeledChoices( all, {}, 99 );
    QCOMPARE( pOver.pageIndex, 2 );

    // 过滤「特殊」：95/7 ≈ 14 条（0,7,...,91）→ 单页
    auto filtered = pageLabeledChoices( all, QStringLiteral( "特殊" ), 0 );
    QCOMPARE( filtered.filteredTotal, 14 );
    QCOMPARE( filtered.pageCount, 1 );
    QVERIFY( filtered.items.first().label.contains( QStringLiteral( "特殊" ) ) );

    // 小页大小
    auto tiny = pageLabeledChoices( all, {}, 1, 10 );
    QCOMPARE( tiny.pageSize, 10 );
    QCOMPARE( tiny.pageCount, 10 );
    QCOMPARE( tiny.pageIndex, 1 );
    QCOMPARE( tiny.items.size(), 10 );
    QCOMPARE( tiny.items.first().id, QStringLiteral( "id-010" ) );

    // 无匹配
    auto none = pageLabeledChoices( all, QStringLiteral( "不存在的关键字XYZ" ), 0 );
    QCOMPARE( none.filteredTotal, 0 );
    QCOMPARE( none.pageCount, 0 );
    QVERIFY( none.items.isEmpty() );
  }

};

QTEST_MAIN( StratigraphicSubIntervalsTest )
#include "tst_stratigraphic_subintervals.moc"
