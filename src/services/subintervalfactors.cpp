// 层：数据
#include "subintervalfactors.h"

#include "domain/litholexicon.h"
#include "paleo_data/service.h"
#include "io/areamodel.h"
#include <QDir>
#include <QFileInfo>

#include <QJsonDocument>
#include <QJsonObject>
#include <QtNumeric>
#include <cmath>
#include <vector>

namespace paleo::strat
{
namespace
{

namespace fw = paleo::data::framework;

bool sandIn( const wellsection::LithoSegment &seg, const paleo::domain::LithoLexicon *lex )
{
  if ( lex && !seg.litho.isEmpty() )
    return lex->isSand( seg.litho );
  const QString n = seg.litho;
  return n.contains( QStringLiteral( "砂" ) ) || n.contains( QStringLiteral( "Sand" ), Qt::CaseInsensitive );
}

double overlapMd( double a0, double a1, double b0, double b1 )
{
  const double lo = std::max( a0, b0 );
  const double hi = std::min( a1, b1 );
  return hi > lo ? ( hi - lo ) : 0.0;
}

QString detailJsonOf( const QVariantMap &detail )
{
  if ( detail.isEmpty() )
    return {};
  return QString::fromUtf8( QJsonDocument( QJsonObject::fromVariantMap( detail ) ).toJson( QJsonDocument::Compact ) );
}

BindResult bindKind( paleo::data::DataService &ds, const UnitSelection &sel, const QString &kind,
                     qlonglong wellId, const QString &assetRef, const QString &status,
                     const QVariantMap &detail )
{
  BindResult r;
  if ( !sel.isValid() )
  {
    r.error = QStringLiteral( "UnitSelection 无效" );
    return r;
  }
  if ( kind == QLatin1String( "well_facies" ) || kind == QLatin1String( "single_factor" ) ||
       kind == QLatin1String( "seismic_facies" ) || kind == QLatin1String( "map" ) ||
       kind == QLatin1String( "composite" ) )
  {
    // ok
  }
  else
  {
    r.error = QStringLiteral( "未知成果 kind" );
    return r;
  }
  paleo::data::SubIntervalResultBinding b;
  b.kind = kind.toStdString();
  b.schemeId = sel.schemeId.toStdString();
  b.schemeRevision.value = sel.revision.toStdString();
  b.interval.value = sel.intervalId.toStdString();
  b.subInterval.value = sel.subIntervalId.toStdString();
  b.aggregate = sel.aggregate;
  b.wellId = wellId;
  b.assetRef = assetRef.toStdString();
  b.status = status.toStdString();
  b.detailJson = detailJsonOf( detail ).toStdString();
  std::string err;
  if ( !ds.bindSubIntervalResult( b, r.stored, err ) )
  {
    r.error = QString::fromStdString( err );
    return r;
  }
  r.ok = true;
  QVariantMap detailOut = detail;
  if ( !r.stored.detailJson.empty() )
  {
    const auto doc = QJsonDocument::fromJson( QByteArray( r.stored.detailJson.data(), int( r.stored.detailJson.size() ) ) );
    if ( doc.isObject() )
      detailOut = doc.object().toVariantMap();
  }
  r.evidenceRow = evidenceRowForBinding(
    kind, sel.schemeId, sel.revision, sel.intervalId, sel.subIntervalId, sel.aggregate, wellId, assetRef,
    QString::fromStdString( r.stored.status ), detailOut );
  return r;
}

} // namespace

qlonglong stableWellKey( const QString &entityId )
{
  const QByteArray utf8 = entityId.toUtf8();
  quint64 h = 14695981039346656037ull;
  for ( unsigned char c : utf8 )
  {
    h ^= c;
    h *= 1099511628211ull;
  }
  const qlonglong v = static_cast<qlonglong>( h & 0x7fffffffffffffffULL );
  return v == 0 ? 1 : v;
}

std::optional<double> sandThicknessInRange( const QVector<wellsection::LithoSegment> &segments, double topMd,
                                            double bottomMd, const paleo::domain::LithoLexicon *lexicon )
{
  if ( !( bottomMd > topMd ) )
    return std::nullopt;
  double sand = 0;
  bool any = false;
  for ( const auto &seg : segments )
  {
    if ( !( seg.baseMd > seg.topMd ) )
      continue;
    const double o = overlapMd( seg.topMd, seg.baseMd, topMd, bottomMd );
    if ( o <= 0 )
      continue;
    any = true;
    if ( sandIn( seg, lexicon ) )
      sand += o;
  }
  if ( !any )
    return std::nullopt;
  return sand;
}

SandRatioMetrics sandRatioForWellSubInterval( const paleo::data::StratigraphicScheme &scheme,
                                              const QString &subIntervalId, qlonglong wellId,
                                              const QVector<wellsection::LithoSegment> &segments,
                                              const paleo::domain::LithoLexicon *lexicon,
                                              const SandRatioBins &bins )
{
  SandRatioMetrics m;
  const auto range =
    fw::wellRange( scheme, wellId, paleo::data::SubIntervalId{ subIntervalId.toStdString() } );
  if ( !range.resolved )
  {
    m.reason = range.reason.empty() ? QStringLiteral( "井在该子层段无有效顶底" )
                                    : QString::fromStdString( range.reason );
    return m;
  }
  const auto sand = sandThicknessInRange( segments, range.top, range.bottom, lexicon );
  if ( !sand )
  {
    m.topMd = range.top;
    m.bottomMd = range.bottom;
    m.thicknessMd = range.bottom - range.top;
    m.reason = QStringLiteral( "子层段内无岩性段可累计砂厚" );
    return m;
  }
  return makeSandRatioMetrics( range.top, range.bottom, *sand, bins );
}

SandRatioMetrics sandRatioForProvisionalSub( double parentTopMd, double parentBottomMd, int subIndex1to4,
                                             const QVector<wellsection::LithoSegment> &segments,
                                             const paleo::domain::LithoLexicon *lexicon,
                                             const SandRatioBins &bins )
{
  double top = 0, bottom = 0;
  if ( !provisionalSubRange( parentTopMd, parentBottomMd, subIndex1to4, &top, &bottom ) )
  {
    SandRatioMetrics m;
    m.reason = QStringLiteral( "等比子层段深度窗无效" );
    return m;
  }
  const auto sand = sandThicknessInRange( segments, top, bottom, lexicon );
  if ( !sand )
  {
    SandRatioMetrics m;
    m.topMd = top;
    m.bottomMd = bottom;
    m.thicknessMd = bottom - top;
    m.reason = QStringLiteral( "子层段内无岩性段可累计砂厚" );
    return m;
  }
  return makeSandRatioMetrics( top, bottom, *sand, bins );
}

BindResult bindSandRatioFactor( paleo::data::DataService &ds, const UnitSelection &sel, qlonglong wellId,
                                const SandRatioMetrics &metrics, const QString &assetRef )
{
  if ( !metrics.ok )
  {
    BindResult r;
    r.error = metrics.reason.isEmpty() ? QStringLiteral( "砂地比指标无效" ) : metrics.reason;
    return r;
  }
  QVariantMap detail;
  detail.insert( QStringLiteral( "factor" ), QStringLiteral( "sand_ratio" ) );
  detail.insert( QStringLiteral( "topMd" ), metrics.topMd );
  detail.insert( QStringLiteral( "bottomMd" ), metrics.bottomMd );
  detail.insert( QStringLiteral( "thicknessMd" ), metrics.thicknessMd );
  detail.insert( QStringLiteral( "sandThicknessMd" ), metrics.sandThicknessMd );
  detail.insert( QStringLiteral( "sandRatio" ), metrics.sandRatio );
  detail.insert( QStringLiteral( "classCode" ), metrics.faciesClass.code );
  detail.insert( QStringLiteral( "classId" ), metrics.faciesClass.id );
  detail.insert( QStringLiteral( "classLabelZh" ), metrics.faciesClass.labelZh );
  return bindKind( ds, sel, QStringLiteral( "single_factor" ), wellId, assetRef, QStringLiteral( "current" ),
                   detail );
}

BindResult bindWellFaciesFromSandRatio( paleo::data::DataService &ds, const UnitSelection &sel, qlonglong wellId,
                                        const SandRatioMetrics &metrics, const QString &assetRef )
{
  if ( !metrics.ok )
  {
    BindResult r;
    r.error = metrics.reason.isEmpty() ? QStringLiteral( "井相分类输入无效" ) : metrics.reason;
    return r;
  }
  QVariantMap detail;
  detail.insert( QStringLiteral( "sourceFactor" ), QStringLiteral( "sand_ratio" ) );
  detail.insert( QStringLiteral( "sandRatio" ), metrics.sandRatio );
  detail.insert( QStringLiteral( "classCode" ), metrics.faciesClass.code );
  detail.insert( QStringLiteral( "classId" ), metrics.faciesClass.id );
  detail.insert( QStringLiteral( "classLabelZh" ), metrics.faciesClass.labelZh );
  detail.insert( QStringLiteral( "topMd" ), metrics.topMd );
  detail.insert( QStringLiteral( "bottomMd" ), metrics.bottomMd );
  return bindKind( ds, sel, QStringLiteral( "well_facies" ), wellId, assetRef, QStringLiteral( "current" ),
                   detail );
}

BindResult bindSeismicFaciesResult( paleo::data::DataService &ds, const UnitSelection &sel,
                                    SeismicFaciesScope scope, int classCode, double coverage,
                                    const QString &strategyId, const QString &assetRef, const QString &status )
{
  BindResult r;
  if ( classCode <= 0 )
  {
    r.error = QStringLiteral( "地震相码无效" );
    return r;
  }
  if ( scope == SeismicFaciesScope::Interval && strategyId.isEmpty() )
  {
    r.error = QStringLiteral( "层段综合必须声明可验证聚合策略，禁止切片冒充整段" );
    return r;
  }
  QVariantMap detail;
  detail.insert( QStringLiteral( "scope" ), seismicFaciesScopeId( scope ) );
  detail.insert( QStringLiteral( "scopeLabelZh" ), seismicFaciesScopeLabelZh( scope ) );
  detail.insert( QStringLiteral( "classCode" ), classCode );
  if ( scope == SeismicFaciesScope::Interval )
  {
    detail.insert( QStringLiteral( "coverage" ), coverage );
    detail.insert( QStringLiteral( "strategyId" ), strategyId );
  }
  return bindKind( ds, sel, QStringLiteral( "seismic_facies" ), 0, assetRef, status, detail );
}


SandRatioMetrics metricsFromKnownRatio( double topMd, double bottomMd, double sandRatio,
                                        const SandRatioBins &bins )
{
  SandRatioMetrics m;
  if ( !std::isfinite( sandRatio ) || sandRatio < 0.0 || sandRatio > 1.0 + 1e-9 )
  {
    m.reason = QStringLiteral( "砂地比超出 [0,1] 或非有限值" );
    return m;
  }
  if ( !std::isfinite( topMd ) || !std::isfinite( bottomMd ) || !( bottomMd > topMd ) )
  {
    m.reason = QStringLiteral( "子层段顶底无效或倒置" );
    return m;
  }
  const double thickness = bottomMd - topMd;
  return makeSandRatioMetrics( topMd, bottomMd, sandRatio * thickness, bins );
}

AutoBindReport autoBindSandRatioExtraction( const QString &projectDir, const UnitSelection &sel,
                                            const QVariantList &rows, const QString &assetRef,
                                            double parentTopMd, double parentBottomMd )
{
  AutoBindReport report;
  if ( !sel.isValid() || sel.aggregate )
  {
    report.message = QStringLiteral( "无有效子层段选择，跳过 DS 绑定" );
    return report;
  }
  const QString root = paleo::area_model::directory( projectDir );
  if ( root.isEmpty() || !QFileInfo::exists( root + QStringLiteral( "/data.sqlite" ) ) )
  {
    report.message = QStringLiteral( "工程尚无 Data Service 工区，跳过 DS 绑定" );
    return report;
  }
  paleo::data::DataService ds;
  std::string err;
  if ( !ds.openProject( root.toUtf8().toStdString(), err ) )
  {
    report.message = QStringLiteral( "打开 Data Service 失败：%1" ).arg( QString::fromStdString( err ) );
    return report;
  }
  report.attempted = true;
  const bool haveParent = std::isfinite( parentTopMd ) && std::isfinite( parentBottomMd )
                          && parentBottomMd > parentTopMd;
  for ( const auto &v : rows )
  {
    const auto row = v.toMap();
    if ( !row.value( QStringLiteral( "contributing" ) ).toBool()
         && !row.value( QStringLiteral( "participating" ) ).toBool() )
    {
      ++report.skipped;
      continue;
    }
    const QString wellId = row.value( QStringLiteral( "well_id" ) ).toString();
    if ( wellId.isEmpty() )
    {
      ++report.skipped;
      continue;
    }
    double ratio = qQNaN();
    if ( row.contains( QStringLiteral( "value" ) ) )
      ratio = row.value( QStringLiteral( "value" ) ).toDouble();
    else if ( row.contains( QStringLiteral( "sand_ratio" ) ) )
      ratio = row.value( QStringLiteral( "sand_ratio" ) ).toDouble();
    else if ( row.contains( QStringLiteral( "factor_value" ) ) )
      ratio = row.value( QStringLiteral( "factor_value" ) ).toDouble();
    if ( !std::isfinite( ratio ) )
    {
      ++report.skipped;
      continue;
    }
    bool okTop = false, okBottom = false;
    double top = row.value( QStringLiteral( "windowTopMd" ) ).toDouble( &okTop );
    double bottom = row.value( QStringLiteral( "windowBottomMd" ) ).toDouble( &okBottom );
    if ( !okTop || !okBottom || !( bottom > top ) )
    {
      if ( sel.subIndex >= 1 && sel.subIndex <= 4 && haveParent )
        provisionalSubRange( parentTopMd, parentBottomMd, sel.subIndex, &top, &bottom );
      else
      {
        ++report.skipped;
        continue;
      }
    }
    const auto metrics = metricsFromKnownRatio( top, bottom, ratio );
    if ( !metrics.ok )
    {
      ++report.skipped;
      continue;
    }
    const qlonglong wid = stableWellKey( wellId );
    const QString ref = assetRef.isEmpty() ? QStringLiteral( "wellfactor.%1" ).arg( wellId ) : assetRef;
    const auto f = bindSandRatioFactor( ds, sel, wid, metrics, ref );
    const auto w = bindWellFaciesFromSandRatio( ds, sel, wid, metrics, ref + QStringLiteral( ".facies" ) );
    if ( f.ok && w.ok )
      ++report.boundWells;
    else
      ++report.skipped;
  }
  report.message = QStringLiteral( "DS 绑定：成功 %1 井，跳过 %2" )
                       .arg( report.boundWells )
                       .arg( report.skipped );
  return report;
}

SeismicPublishReport publishSeismicSliceAggregate( paleo::data::DataService &ds, const UnitSelection &sel,
                                                   const QVector<SeismicSliceVote> &votes,
                                                   const QString &intervalAssetRef, double minCoverage,
                                                   bool alsoBindSlices, const QString &sliceAssetPrefix )
{
  SeismicPublishReport report;
  if ( !sel.isValid() || sel.aggregate )
  {
    report.error = QStringLiteral( "层段综合需要有效子层段 UnitSelection" );
    return report;
  }
  report.aggregate = aggregateSeismicSliceVotes( votes, minCoverage );
  if ( !report.aggregate.ok )
  {
    report.error = report.aggregate.reason;
    return report;
  }
  if ( alsoBindSlices )
  {
    int i = 0;
    for ( const auto &v : votes )
    {
      if ( v.classCode <= 0 || !( v.weight > 0 ) )
        continue;
      const QString ref = ( sliceAssetPrefix.isEmpty() ? QStringLiteral( "seis-slice" ) : sliceAssetPrefix )
                          + QStringLiteral( ".%1" ).arg( i++ );
      report.sliceBinds << bindSeismicFaciesResult( ds, sel, SeismicFaciesScope::Slice, v.classCode, 0, {},
                                                    ref );
    }
  }
  report.intervalBind = bindSeismicFaciesResult( ds, sel, SeismicFaciesScope::Interval,
                                                 report.aggregate.classCode, report.aggregate.coverage,
                                                 report.aggregate.strategyId, intervalAssetRef );
  report.ok = report.intervalBind.ok;
  if ( !report.ok )
    report.error = report.intervalBind.error;
  return report;
}

QVariantList evidenceRowsFromBindings( const std::vector<paleo::data::SubIntervalResultBinding> &bindings,
                                       const QStringList &kinds )
{
  QVariantList rows;
  for ( const auto &b : bindings )
  {
    const QString kind = QString::fromStdString( b.kind );
    if ( !kinds.isEmpty() && !kinds.contains( kind ) )
      continue;
    QVariantMap detail;
    if ( !b.detailJson.empty() )
    {
      const auto doc = QJsonDocument::fromJson( QByteArray( b.detailJson.data(), int( b.detailJson.size() ) ) );
      if ( doc.isObject() )
        detail = doc.object().toVariantMap();
    }
    rows.append( evidenceRowForBinding(
      kind, QString::fromStdString( b.schemeId ), QString::fromStdString( b.schemeRevision.value ),
      QString::fromStdString( b.interval.value ), QString::fromStdString( b.subInterval.value ), b.aggregate,
      b.wellId, QString::fromStdString( b.assetRef ), QString::fromStdString( b.status ), detail ) );
  }
  return rows;
}

bool openProjectDataService( const QString &projectDir, paleo::data::DataService &ds, QString *error )
{
  const QString root = paleo::area_model::directory( projectDir );
  if ( root.isEmpty() || !QFileInfo::exists( root + QStringLiteral( "/data.sqlite" ) ) )
  {
    if ( error )
      *error = QStringLiteral( "工程尚无 Data Service 工区" );
    return false;
  }
  std::string err;
  if ( !ds.openProject( root.toUtf8().toStdString(), err ) )
  {
    if ( error )
      *error = QStringLiteral( "打开 Data Service 失败：%1" ).arg( QString::fromStdString( err ) );
    return false;
  }
  return true;
}

QVector<SchemeChoice> listProjectSchemes( paleo::data::DataService &ds, QString *error )
{
  QVector<SchemeChoice> out;
  std::vector<paleo::data::StratigraphicSchemeSummary> rows;
  std::string err;
  if ( !ds.listStratigraphicSchemes( rows, err ) )
  {
    if ( error )
      *error = QStringLiteral( "列举地层方案失败：%1" ).arg( QString::fromStdString( err ) );
    return out;
  }
  out.reserve( static_cast<int>( rows.size() ) );
  for ( const auto &r : rows )
  {
    SchemeChoice c;
    c.id = QString::fromStdString( r.id );
    c.name = QString::fromStdString( r.name );
    c.headRevision = QString::fromStdString( r.headRevision.value );
    out.push_back( c );
  }
  return out;
}

QVector<SchemeRevisionChoice> listProjectSchemeRevisions( paleo::data::DataService &ds, const QString &schemeId,
                                                          QString *error )
{
  QVector<SchemeRevisionChoice> out;
  std::vector<paleo::data::StratigraphicSchemeRevisionSummary> rows;
  std::string err;
  if ( !ds.listStratigraphicSchemeRevisions( schemeId.toStdString(), rows, err ) )
  {
    if ( error )
      *error = QStringLiteral( "列举方案修订失败：%1" ).arg( QString::fromStdString( err ) );
    return out;
  }
  out.reserve( static_cast<int>( rows.size() ) );
  for ( const auto &r : rows )
  {
    SchemeRevisionChoice c;
    c.revision = QString::fromStdString( r.revision.value );
    c.parentRevision = QString::fromStdString( r.parentRevision.value );
    c.created = QString::fromStdString( r.created );
    c.author = QString::fromStdString( r.author );
    c.note = QString::fromStdString( r.note );
    out.push_back( c );
  }
  return out;
}

bool loadProjectScheme( paleo::data::DataService &ds, const QString &schemeId, const QString &revision,
                        paleo::data::StratigraphicScheme &out, QString *error )
{
  std::string err;
  if ( !ds.readStratigraphicScheme( schemeId.toStdString(), revision.toStdString(), out, err ) )
  {
    if ( error )
      *error = QStringLiteral( "读取地层方案失败：%1" ).arg( QString::fromStdString( err ) );
    return false;
  }
  return true;
}

bool loadProjectSchemeFromDir( const QString &projectDir, const QString &schemeId, const QString &revision,
                               paleo::data::StratigraphicScheme &out,
                               std::vector<paleo::data::SubIntervalResultBinding> *bindings, QString *error )
{
  paleo::data::DataService ds;
  if ( !openProjectDataService( projectDir, ds, error ) )
    return false;
  QString sid = schemeId;
  QString rev = revision;
  if ( sid.isEmpty() )
  {
    QString listErr;
    const auto schemes = listProjectSchemes( ds, &listErr );
    if ( schemes.isEmpty() )
    {
      if ( error )
        *error = listErr.isEmpty() ? QStringLiteral( "工程尚无地层方案" ) : listErr;
      return false;
    }
    sid = schemes.first().id;
    if ( rev.isEmpty() )
      rev = schemes.first().headRevision;
  }
  if ( !loadProjectScheme( ds, sid, rev, out, error ) )
    return false;
  if ( bindings )
  {
    std::string err;
    paleo::data::SubIntervalId empty;
    if ( !ds.listSubIntervalResults( out.id, empty, *bindings, err ) && error && error->isEmpty() )
      *error = QStringLiteral( "列举子层段成果失败：%1" ).arg( QString::fromStdString( err ) );
    // 成果列举失败不阻断方案加载——红绿灯退化为仅 occurrence。
  }
  return true;
}


bool listProjectSchemesFromDir( const QString &projectDir, QVector<SchemeChoice> &out, QString *error )
{
  out.clear();
  paleo::data::DataService ds;
  if ( !openProjectDataService( projectDir, ds, error ) )
    return false;
  out = listProjectSchemes( ds, error );
  return !out.isEmpty() || ( error && error->isEmpty() );
}

bool listProjectSchemeRevisionsFromDir( const QString &projectDir, const QString &schemeId,
                                        QVector<SchemeRevisionChoice> &out, QString *error )
{
  out.clear();
  paleo::data::DataService ds;
  if ( !openProjectDataService( projectDir, ds, error ) )
    return false;
  out = listProjectSchemeRevisions( ds, schemeId, error );
  return true;
}

PagedLabeledChoices pageLabeledChoices( const QVector<LabeledIdChoice> &all, const QString &filter, int page,
                                        int pageSize )
{
  PagedLabeledChoices out;
  out.pageSize = pageSize > 0 ? pageSize : kSchemeRevisionPageSize;
  QVector<LabeledIdChoice> filtered;
  filtered.reserve( all.size() );
  const QString needle = filter.trimmed();
  for ( const auto &c : all )
  {
    if ( needle.isEmpty()
         || c.label.contains( needle, Qt::CaseInsensitive )
         || c.id.contains( needle, Qt::CaseInsensitive ) )
      filtered.push_back( c );
  }
  out.filteredTotal = filtered.size();
  out.pageCount = out.filteredTotal == 0 ? 0 : ( out.filteredTotal + out.pageSize - 1 ) / out.pageSize;
  if ( out.pageCount == 0 )
  {
    out.pageIndex = 0;
    return out;
  }
  out.pageIndex = qBound( 0, page, out.pageCount - 1 );
  const int begin = out.pageIndex * out.pageSize;
  const int end = qMin( begin + out.pageSize, out.filteredTotal );
  out.items.reserve( end - begin );
  for ( int i = begin; i < end; ++i )
    out.items.push_back( filtered.at( i ) );
  return out;
}

} // namespace paleo::strat
