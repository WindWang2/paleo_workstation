// 层：数据
#include "subintervalfactors.h"

#include "domain/litholexicon.h"
#include "paleo_data/service.h"

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

} // namespace paleo::strat
