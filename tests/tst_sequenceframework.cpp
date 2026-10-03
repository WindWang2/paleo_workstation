// 层：数据（测试壳位于 tests/，被测对象为数据层格架模型/落库/建议/诊断）
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "../src/algorithms/frameworksuggester.h"
#include "../src/catalog/datacatalog.h"
#include "../src/catalog/frameworkstore.h"
#include "../src/domain/arearules.h"
#include "../src/domain/frameworkdiagnostics.h"
#include "../src/domain/mappinghorizons.h"
#include "../src/domain/sequenceframework.h"
#include "../src/domain/wellrecords.h"
#include "../src/services/frameworkservice.h"

// 方向 28「层序地层格架工作台」的验收面（Oracle 四条）：
//   1. 格架 CRUD round-trip：新建-保存-重开逐字段一致；与 mappingHorizons()
//      映射可双向解析。
//   2. 归属校验：人工构造悬空分层 / 格架空洞 / 重名层各一，诊断器全检出且
//      可定位。
//   3. 建议面只出候选：未确认前 catalog 零写入（mutationSeq 与版本数不变）。
//   4. 诊断报告可复现（同输入 → 同文本）+ 未来 schema 如实报错。
// 本文件不碰 UI——视图层的断言归 tst_sequenceframework_ui。
namespace
{

using namespace SequenceFramework;

const char *kHorizons[ 8 ] = { "C3", "C6", "D53", "D61", "D62", "D63", "D71", "D72" };

// 宏参数里不能出现裸逗号（QCOMPARE 是宏，逗号会被当成实参分隔符）——
// 期望集合一律经这个助手构造。
QStringList strs( const char *a, const char *b = nullptr, const char *c = nullptr )
{
  QStringList out;
  out.append( QString::fromLatin1( a ) );
  if ( b )
    out.append( QString::fromLatin1( b ) );
  if ( c )
    out.append( QString::fromLatin1( c ) );
  return out;
}

QStringList defaultHorizons()
{
  QStringList out;
  for ( const char *h : kHorizons )
    out.append( QString::fromLatin1( h ) );
  return out;
}

// SQ1（层序，被 TST/HST 细分）+ SQ2（层序，无细分）。
Framework sampleFramework()
{
  Framework fw;
  fw.name = QStringLiteral( "测试格架" );

  FrameworkUnit sq1;
  sq1.id = QStringLiteral( "sfu-1" );
  sq1.name = QStringLiteral( "SQ1" );
  sq1.level = UnitLevel::Sequence;
  sq1.ordinal = 0;
  sq1.topBoundary = QStringLiteral( "C3" );
  sq1.baseBoundary = QStringLiteral( "D53" );
  sq1.thickness = 200.0;
  fw.units.append( sq1 );

  FrameworkUnit tst;
  tst.id = QStringLiteral( "sfu-2" );
  tst.parentId = QStringLiteral( "sfu-1" );
  tst.name = QStringLiteral( "TST" );
  tst.level = UnitLevel::SystemsTract;
  tst.ordinal = 0;
  tst.topBoundary = QStringLiteral( "C3" );
  tst.baseBoundary = QStringLiteral( "C6" );
  tst.thickness = 80.0;
  tst.colorKey = QStringLiteral( "tst" );
  tst.note = QStringLiteral( "海进体系域" );
  fw.units.append( tst );

  FrameworkUnit hst;
  hst.id = QStringLiteral( "sfu-3" );
  hst.parentId = QStringLiteral( "sfu-1" );
  hst.name = QStringLiteral( "HST" );
  hst.level = UnitLevel::SystemsTract;
  hst.ordinal = 1;
  hst.topBoundary = QStringLiteral( "C6" );
  hst.baseBoundary = QStringLiteral( "D53" );
  hst.thickness = 120.0;
  fw.units.append( hst );

  FrameworkUnit sq2;
  sq2.id = QStringLiteral( "sfu-4" );
  sq2.name = QStringLiteral( "SQ2" );
  sq2.level = UnitLevel::Sequence;
  sq2.ordinal = 1;
  sq2.topBoundary = QStringLiteral( "D53" );
  sq2.thickness = 300.0;
  fw.units.append( sq2 );

  MarkerBed marker;
  marker.id = QStringLiteral( "sfm-1" );
  marker.name = QStringLiteral( "标志层A" );
  marker.unitId = QStringLiteral( "sfu-2" );
  marker.layerNames.append( QStringLiteral( "C3" ) );
  marker.note = QStringLiteral( "顶界标志" );
  fw.markers.append( marker );

  return fw;
}

void compareUnits( const FrameworkUnit &a, const FrameworkUnit &b )
{
  QCOMPARE( a.id, b.id );
  QCOMPARE( a.parentId, b.parentId );
  QCOMPARE( a.name, b.name );
  QCOMPARE( static_cast<int>( a.level ), static_cast<int>( b.level ) );
  QCOMPARE( a.ordinal, b.ordinal );
  QCOMPARE( a.topBoundary, b.topBoundary );
  QCOMPARE( a.baseBoundary, b.baseBoundary );
  QCOMPARE( a.thickness, b.thickness );
  QCOMPARE( a.colorKey, b.colorKey );
  QCOMPARE( a.note, b.note );
}

void compareFrameworks( const Framework &a, const Framework &b )
{
  QCOMPARE( a.schemaVersion, b.schemaVersion );
  QCOMPARE( a.name, b.name );
  QCOMPARE( a.units.size(), b.units.size() );
  for ( int i = 0; i < a.units.size(); ++i )
    compareUnits( a.units.at( i ), b.units.at( i ) );
  QCOMPARE( a.markers.size(), b.markers.size() );
  for ( int i = 0; i < a.markers.size(); ++i )
  {
    QCOMPARE( a.markers.at( i ).id, b.markers.at( i ).id );
    QCOMPARE( a.markers.at( i ).name, b.markers.at( i ).name );
    QCOMPARE( a.markers.at( i ).unitId, b.markers.at( i ).unitId );
    QCOMPARE( a.markers.at( i ).layerNames, b.markers.at( i ).layerNames );
    QCOMPARE( a.markers.at( i ).note, b.markers.at( i ).note );
  }
}

WellTopRecord top( const QString &well, const QString &layer, double md )
{
  WellTopRecord r;
  r.wellName = well;
  r.topName = layer;
  r.md = md;
  r.hasMd = true;
  return r;
}

} // namespace

class TestSequenceFramework : public QObject
{
  Q_OBJECT

private slots:
  void init()
  {
    // 层位集合走 AreaRules 默认（本工区 8 界面）；其它用例可能改过它。
    AreaRules::reset();
    QCOMPARE( mappingHorizons(), defaultHorizons() );
  }

  // Oracle 1：CRUD round-trip + 与 mappingHorizons() 双向解析。
  void crudRoundTripAndHorizonMapping()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    {
      DataCatalog cat;
      QVERIFY( cat.open( dir.path() ) );
      FrameworkStore store( &cat, dir.path() );
      QVERIFY( !store.hasFramework() );
      QString err;
      QVERIFY2( store.save( sampleFramework(), &err ), qPrintable( err ) );
      QVERIFY( store.hasFramework() );
    }
    DataCatalog reopened;
    QVERIFY( reopened.open( dir.path() ) );
    FrameworkStore store( &reopened, dir.path() );
    QVERIFY( store.hasFramework() );
    Framework loaded;
    QString err;
    QVERIFY2( store.load( &loaded, &err ), qPrintable( err ) );
    compareFrameworks( sampleFramework(), loaded );

    // 正向：单元 → 覆盖层位（顶界含、底界不含）。
    QCOMPARE( horizonsCoveredBy( loaded, QStringLiteral( "sfu-1" ), defaultHorizons() ),
              strs( "C3", "C6" ) );
    QCOMPARE( horizonsCoveredBy( loaded, QStringLiteral( "sfu-2" ), defaultHorizons() ),
              strs( "C3" ) );
    QCOMPARE( horizonsCoveredBy( loaded, QStringLiteral( "sfu-3" ), defaultHorizons() ),
              strs( "C6" ) );

    // 反向：层位 → 归属单元（只看叶子，层序与自家体系域不互相歧义）。
    QCOMPARE( unitsCoveringHorizon( loaded, QStringLiteral( "C3" ), defaultHorizons() ),
              strs( "sfu-2" ) );
    QCOMPARE( unitsCoveringHorizon( loaded, QStringLiteral( "C6" ), defaultHorizons() ),
              strs( "sfu-3" ) );
    QCOMPARE( unitsCoveringHorizon( loaded, QStringLiteral( "D72" ), defaultHorizons() ),
              strs( "sfu-4" ) );
    QCOMPARE( unitPath( loaded, QStringLiteral( "sfu-2" ) ),
              QStringLiteral( "SQ1 / TST" ) );
    QVERIFY( isLeaf( loaded, QStringLiteral( "sfu-2" ) ) );
    QVERIFY( !isLeaf( loaded, QStringLiteral( "sfu-1" ) ) );
  }

  // 版本管线：每次保存涨一个版本；重开读到最新那份。
  void savingAppendsNewVersion()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    DataCatalog cat;
    QVERIFY( cat.open( dir.path() ) );
    FrameworkStore store( &cat, dir.path() );
    QString err;
    QVERIFY2( store.save( sampleFramework(), &err ), qPrintable( err ) );
    QCOMPARE( store.versionCount(), 1 );

    Framework second = sampleFramework();
    second.name = QStringLiteral( "测试格架·第二版" );
    QVERIFY2( store.save( second, &err ), qPrintable( err ) );
    QCOMPARE( store.versionCount(), 2 );

    Framework loaded;
    QVERIFY2( store.load( &loaded, &err ), qPrintable( err ) );
    QCOMPARE( loaded.name, QStringLiteral( "测试格架·第二版" ) );

    // 格架资产必须是受管版本（禁止旁路存储：文件落在工程受管路径下）。
    const QString aid = store.assetId();
    const CatalogVersion ver = cat.currentVersion( aid );
    QVERIFY( ver.managed );
    QCOMPARE( ver.stage, QStringLiteral( "OUTPUT" ) );
    QCOMPARE( ver.versionNumber, 2 );
    QVERIFY( QFile::exists( DataCatalog::resolvedVersionPath( dir.path(), ver ) ) );
  }

  // Oracle 2：悬空分层 / 格架空洞 / 跨单元重名层各一，全检出且可定位。
  void diagnosticsCatchDanglingGapDuplicate()
  {
    Framework fw;
    fw.name = QStringLiteral( "诊断夹具" );
    auto addUnit = [&fw]( const QString &id, const QString &parent, const QString &name,
                          const QString &top, const QString &base ) {
      FrameworkUnit u;
      u.id = id;
      u.parentId = parent;
      u.name = name;
      u.level = parent.isEmpty() ? UnitLevel::Sequence : UnitLevel::SystemsTract;
      u.topBoundary = top;
      u.baseBoundary = base;
      fw.units.append( u );
    };
    addUnit( QStringLiteral( "sfu-1" ), QString(), QStringLiteral( "SQ1" ),
             QStringLiteral( "C3" ), QStringLiteral( "D53" ) );
    addUnit( QStringLiteral( "sfu-2" ), QStringLiteral( "sfu-1" ), QStringLiteral( "TST" ),
             QStringLiteral( "C3" ), QStringLiteral( "C6" ) );
    addUnit( QStringLiteral( "sfu-3" ), QStringLiteral( "sfu-1" ), QStringLiteral( "HST" ),
             QStringLiteral( "C6" ), QStringLiteral( "D53" ) );
    addUnit( QStringLiteral( "sfu-4" ), QString(), QStringLiteral( "SQ2" ),
             QStringLiteral( "D53" ), QStringLiteral( "D72" ) );
    addUnit( QStringLiteral( "sfu-5" ), QString(), QStringLiteral( "SQ3" ),
             QStringLiteral( "D72" ), QString() );

    auto addMarker = [&fw]( const QString &id, const QString &name, const QString &unit,
                            const QStringList &layers ) {
      MarkerBed m;
      m.id = id;
      m.name = name;
      m.unitId = unit;
      m.layerNames = layers;
      fw.markers.append( m );
    };
    // ① 悬空分层：GHOST 不在井分层全集里。
    addMarker( QStringLiteral( "sfm-1" ), QStringLiteral( "M-TST" ), QStringLiteral( "sfu-2" ),
               QStringList{ QStringLiteral( "C3" ), QStringLiteral( "GHOST" ) } );
    // ② 悬空单元：sfu-999 不存在。
    addMarker( QStringLiteral( "sfm-2" ), QStringLiteral( "M-ORPHAN" ),
               QStringLiteral( "sfu-999" ), QStringList{ QStringLiteral( "D61" ) } );
    // ③④ 跨单元重名层：D53 同时挂在 sfu-4 与 sfu-5 下。
    addMarker( QStringLiteral( "sfm-3" ), QStringLiteral( "M-A" ), QStringLiteral( "sfu-4" ),
               QStringList{ QStringLiteral( "D53" ) } );
    addMarker( QStringLiteral( "sfm-4" ), QStringLiteral( "M-B" ), QStringLiteral( "sfu-5" ),
               QStringList{ QStringLiteral( "D53" ) } );

    DiagnosticInput in;
    in.framework = fw;
    in.horizons = defaultHorizons();
    // 只有 C3 / D53 / D72 有编图数据 → HST（C6）落到「单元无编图数据」。
    in.mappedHorizons = QStringList{ QStringLiteral( "C3" ), QStringLiteral( "D53" ),
                                     QStringLiteral( "D72" ) };
    in.tops.clear();
    in.tops.append( top( QStringLiteral( "W1" ), QStringLiteral( "C3" ), 1000.0 ) );
    in.tops.append( top( QStringLiteral( "W1" ), QStringLiteral( "C6" ), 1100.0 ) );
    in.tops.append( top( QStringLiteral( "W1" ), QStringLiteral( "D53" ), 1200.0 ) );
    in.tops.append( top( QStringLiteral( "W2" ), QStringLiteral( "C3" ), 1010.0 ) );
    in.tops.append( top( QStringLiteral( "W2" ), QStringLiteral( "C6" ), 1110.0 ) );
    in.tops.append( top( QStringLiteral( "W2" ), QStringLiteral( "D53" ), 1210.0 ) );
    // XZ：既不落在任何叶子单元内、也未被任何标志层引用 → 孤立井分层。
    in.tops.append( top( QStringLiteral( "W2" ), QStringLiteral( "XZ" ), 1500.0 ) );
    // D61 在井分层全集里（只是本夹具的井没钻到它）——它不是悬空引用，
    // 悬空的只有 GHOST。
    in.knownLayerNames = QStringList{ QStringLiteral( "C3" ), QStringLiteral( "C6" ),
                                      QStringLiteral( "D53" ), QStringLiteral( "D61" ),
                                      QStringLiteral( "XZ" ) };

    const DiagnosticReport report = diagnose( in );
    QCOMPARE( report.countOf( DiagnosticKind::MarkerDanglingLayer ), 1 );
    QCOMPARE( report.countOf( DiagnosticKind::MarkerDanglingUnit ), 1 );
    QCOMPARE( report.countOf( DiagnosticKind::DuplicateLayerName ), 1 );
    QCOMPARE( report.countOf( DiagnosticKind::FrameworkGap ), 1 );
    QCOMPARE( report.countOf( DiagnosticKind::OrphanWellLayer ), 1 );
    QCOMPARE( report.countOf( DiagnosticKind::UnitWithoutMappingData ), 1 );
    QCOMPARE( report.countOf( DiagnosticKind::HorizonWithoutUnit ), 0 );
    QCOMPARE( report.countOf( DiagnosticKind::AmbiguousHorizonUnit ), 0 );
    QCOMPARE( report.countOf( DiagnosticKind::BoundaryOutsideSet ), 0 );

    // 可定位：每条诊断的 locations 里带得回构造夹具的关键字。
    auto locate = [&report]( DiagnosticKind kind ) {
      for ( const Diagnostic &d : report.items )
        if ( d.kind == kind )
          return d.locations.join( QStringLiteral( "|" ) );
      return QString();
    };
    QVERIFY2( locate( DiagnosticKind::MarkerDanglingLayer ).contains( QStringLiteral( "GHOST" ) ),
              qPrintable( locate( DiagnosticKind::MarkerDanglingLayer ) ) );
    QVERIFY2( locate( DiagnosticKind::MarkerDanglingUnit ).contains( QStringLiteral( "sfu-999" ) ),
              qPrintable( locate( DiagnosticKind::MarkerDanglingUnit ) ) );
    QVERIFY2( locate( DiagnosticKind::DuplicateLayerName ).contains( QStringLiteral( "D53" ) ),
              qPrintable( locate( DiagnosticKind::DuplicateLayerName ) ) );
    // 空洞 = SQ3（D72 区间在两口井的分层里都没有落点）。
    QVERIFY2( locate( DiagnosticKind::FrameworkGap ).contains( QStringLiteral( "SQ3" ) ),
              qPrintable( locate( DiagnosticKind::FrameworkGap ) ) );
    QVERIFY2( locate( DiagnosticKind::OrphanWellLayer ).contains( QStringLiteral( "XZ" ) ),
              qPrintable( locate( DiagnosticKind::OrphanWellLayer ) ) );
    // 无编图数据 = SQ1 / HST（C6 区间内没有已入库层位）。
    QVERIFY2( locate( DiagnosticKind::UnitWithoutMappingData ).contains( QStringLiteral( "HST" ) ),
              qPrintable( locate( DiagnosticKind::UnitWithoutMappingData ) ) );

    // 严重度口径：悬空与歧义是 High，无归属/空洞是 Medium，重名与孤立是 Low。
    QCOMPARE( report.countOf( Severity::High ), 2 );
    QVERIFY( report.countOf( Severity::Medium ) >= 2 );
    QCOMPARE( report.countOf( Severity::Low ), 2 );
  }

  // Oracle 3：建议只出候选——未确认前 catalog 零写入。
  void suggestionsAreCandidatesOnly()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    DataCatalog cat;
    QVERIFY( cat.open( dir.path() ) );

    Framework fw;
    fw.name = QStringLiteral( "建议夹具" );
    FrameworkUnit sq1;
    sq1.id = QStringLiteral( "sfu-1" );
    sq1.name = QStringLiteral( "SQ1" );
    sq1.topBoundary = QStringLiteral( "C3" );
    sq1.baseBoundary = QStringLiteral( "D53" );
    fw.units.append( sq1 );
    FrameworkUnit sq2;
    sq2.id = QStringLiteral( "sfu-2" );
    sq2.name = QStringLiteral( "SQ2" );
    sq2.ordinal = 1;
    sq2.topBoundary = QStringLiteral( "D53" );
    sq2.baseBoundary = QStringLiteral( "D63" );
    fw.units.append( sq2 );
    MarkerBed seed1;
    seed1.id = QStringLiteral( "sfm-1" );
    seed1.name = QStringLiteral( "M-C3" );
    seed1.unitId = QStringLiteral( "sfu-1" );
    seed1.layerNames.append( QStringLiteral( "C3" ) );
    fw.markers.append( seed1 );
    MarkerBed seed2;
    seed2.id = QStringLiteral( "sfm-2" );
    seed2.name = QStringLiteral( "M-D53" );
    seed2.unitId = QStringLiteral( "sfu-2" );
    seed2.layerNames.append( QStringLiteral( "D53" ) );
    fw.markers.append( seed2 );

    FrameworkService service( dir.path() );
    service.attachCatalog( &cat );
    QString err;
    QVERIFY2( service.save( fw, &err ), qPrintable( err ) );

    const quint64 seqBefore = cat.mutationSeq();
    const QString aid = cat.assets().isEmpty() ? QString() : cat.assets().first().id;
    int versionsBefore = 0;
    for ( const CatalogAsset &a : cat.assets() )
      if ( a.type == FrameworkStore::assetType() )
        versionsBefore = cat.versionsForAsset( a.id ).size();
    QCOMPARE( versionsBefore, 1 );

    QVector<WellTopRecord> tops;
    tops.append( top( QStringLiteral( "A1" ), QStringLiteral( "C3" ), 1000.0 ) );
    tops.append( top( QStringLiteral( "A1" ), QStringLiteral( "C6" ), 1100.0 ) );
    tops.append( top( QStringLiteral( "A1" ), QStringLiteral( "D53" ), 1200.0 ) );
    tops.append( top( QStringLiteral( "A1" ), QStringLiteral( "D61" ), 1300.0 ) );
    tops.append( top( QStringLiteral( "A2" ), QStringLiteral( "C3" ), 1010.0 ) );
    tops.append( top( QStringLiteral( "A2" ), QStringLiteral( "C6" ), 1110.0 ) );
    tops.append( top( QStringLiteral( "A2" ), QStringLiteral( "D53" ), 1210.0 ) );
    tops.append( top( QStringLiteral( "A2" ), QStringLiteral( "D61" ), 1310.0 ) );

    const QVector<SuggestionCandidate> candidates = service.suggest( tops );
    // C6 落进 SQ1 的本井区间、D61 落进 SQ2 的本井区间 → 各井各一条，共 4 条。
    QCOMPARE( candidates.size(), 4 );
    QCOMPARE( candidates.at( 0 ).layerName, QStringLiteral( "C6" ) );
    QCOMPARE( candidates.at( 0 ).unitId, QStringLiteral( "sfu-1" ) );
    QCOMPARE( candidates.at( 0 ).wellName, QStringLiteral( "A1" ) );
    QCOMPARE( candidates.at( 2 ).layerName, QStringLiteral( "D61" ) );
    QCOMPARE( candidates.at( 2 ).unitId, QStringLiteral( "sfu-2" ) );
    for ( const SuggestionCandidate &c : candidates )
    {
      QVERIFY( !c.accepted );               // 构造恒 false——未经人工确认
      QVERIFY( c.usedWellInterval );        // 用的是本井区间，不是全工区均值
      QCOMPARE( c.depthGap, 0.0 );          // 落在已确认区间内
    }

    // 建议面零写入：mutationSeq 与版本数都没动，也没多出资产。
    QCOMPARE( cat.mutationSeq(), seqBefore );
    int versionsAfter = 0;
    for ( const CatalogAsset &a : cat.assets() )
      if ( a.type == FrameworkStore::assetType() )
        versionsAfter = cat.versionsForAsset( a.id ).size();
    QCOMPARE( versionsAfter, versionsBefore );
    QCOMPARE( cat.assetById( aid ).id, aid );

    // 全未确认 → 不落盘（不空涨 revision）。
    QVERIFY2( service.commitAccepted( candidates, &err ), qPrintable( err ) );
    QCOMPARE( cat.mutationSeq(), seqBefore );
    int versionsAfterNoop = 0;
    for ( const CatalogAsset &a : cat.assets() )
      if ( a.type == FrameworkStore::assetType() )
        versionsAfterNoop = cat.versionsForAsset( a.id ).size();
    QCOMPARE( versionsAfterNoop, versionsBefore );

    // 只确认 C6 那两条 → 落一个新版本，且格架里只多了 C6 的归属。
    QVector<SuggestionCandidate> confirmed = candidates;
    int acceptedCount = 0;
    for ( SuggestionCandidate &c : confirmed )
      if ( c.layerName == QStringLiteral( "C6" ) )
      {
        c.accepted = true;
        ++acceptedCount;
      }
    QCOMPARE( acceptedCount, 2 );
    QVERIFY2( service.commitAccepted( confirmed, &err ), qPrintable( err ) );
    int versionsFinal = 0;
    for ( const CatalogAsset &a : cat.assets() )
      if ( a.type == FrameworkStore::assetType() )
        versionsFinal = cat.versionsForAsset( a.id ).size();
    QCOMPARE( versionsFinal, versionsBefore + 1 );

    const Framework &after = service.framework();
    bool hasC6 = false, hasD61 = false;
    for ( const MarkerBed &m : after.markers )
    {
      if ( m.layerNames.contains( QStringLiteral( "C6" ) ) )
      {
        hasC6 = true;
        QCOMPARE( m.unitId, QStringLiteral( "sfu-1" ) );
      }
      if ( m.layerNames.contains( QStringLiteral( "D61" ) ) )
        hasD61 = true;
    }
    QVERIFY( hasC6 );
    QVERIFY( !hasD61 ); // 未确认的那条确实没写进去

    // 重开后仍在（走的是 catalog 版本，不是内存）。
    DataCatalog reopened;
    QVERIFY( reopened.open( dir.path() ) );
    FrameworkStore store( &reopened, dir.path() );
    Framework loaded;
    QVERIFY2( store.load( &loaded, &err ), qPrintable( err ) );
    bool persisted = false;
    for ( const MarkerBed &m : loaded.markers )
      if ( m.layerNames.contains( QStringLiteral( "C6" ) ) )
        persisted = true;
    QVERIFY( persisted );
  }

  // Oracle 4：报告可复现 + 序列化对坏输入如实报错。
  void reportIsReproducibleAndRejectsBadSchema()
  {
    Framework fw = sampleFramework();
    DiagnosticInput in;
    in.framework = fw;
    in.horizons = defaultHorizons();
    in.mappedHorizons = defaultHorizons();
    in.tops.append( top( QStringLiteral( "W1" ), QStringLiteral( "C3" ), 1000.0 ) );
    in.knownLayerNames = QStringList{ QStringLiteral( "C3" ) };

    const DiagnosticReport a = diagnose( in );
    const DiagnosticReport b = diagnose( in );
    QCOMPARE( a.toText(), b.toText() );
    QCOMPARE( a.toMarkdown(), b.toMarkdown() );
    QVERIFY( a.toMarkdown().contains( QStringLiteral( "|" ) ) );

    // JSON round-trip 无损。
    const QByteArray bytes = toJsonBytes( fw );
    const Framework back = fromJson( QJsonDocument::fromJson( bytes ).object() );
    compareFrameworks( fw, back );

    // 未来版本信号：schema_version 高于本二进制所知 → 拒用，不静默截断。
    QJsonObject future = QJsonDocument::fromJson( bytes ).object();
    future.insert( QStringLiteral( "schema_version" ), kSchemaVersion + 1 );
    QString err;
    const Framework bad = fromJson( future, &err );
    QVERIFY( !err.isEmpty() );
    QVERIFY( bad.units.isEmpty() );
  }
};

int main( int argc, char *argv[] )
{
  QCoreApplication app( argc, argv );
  TestSequenceFramework tc;
  QList<QByteArray> forwarded;
  forwarded << QByteArray( argv[ 0 ] );
  for ( int i = 1; i < argc; ++i )
    forwarded << QByteArray( argv[ i ] );
  forwarded << QByteArray( "-o" )
            << QByteArray( QT_TESTCASE_BUILDDIR ) + "/tst_sequenceframework-result.txt,txt";
  QList<char *> cargv;
  cargv.reserve( forwarded.size() );
  for ( QByteArray &a : forwarded )
    cargv << a.data();
  const int rc = QTest::qExec( &tc, cargv.size(), cargv.data() );
  return rc;
}

#include "tst_sequenceframework.moc"
