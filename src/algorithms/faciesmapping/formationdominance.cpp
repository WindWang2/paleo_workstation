// 层：数据
#include "formationdominance.h"

#include "dominantfacies.h"

#include <QHash>

#include <algorithm>

namespace paleo::faciesmapping
{

QVector<FormationDominance> dominantFaciesByFormation(
  const QVector<WellComposite::FormationInterval> &formations,
  const QVector<FaciesSpan> &spans )
{
  QVector<FaciesSpan> ordered;
  for ( const FaciesSpan &span : spans )
    if ( !span.label.trimmed().isEmpty() && span.bottom > span.top )
      ordered.append( span );
  std::stable_sort( ordered.begin(), ordered.end(),
                    []( const FaciesSpan &a, const FaciesSpan &b ) { return a.top < b.top; } );

  // 标签 → 局部码（自上而下首次出现，并列时取小码即更浅者）；记下各标签的分类编码。
  QHash<QString, int> localCode;
  QVector<QString> labels;
  QVector<int> schemaCode;
  for ( const FaciesSpan &span : ordered )
  {
    const QString label = span.label.trimmed();
    if ( localCode.contains( label ) )
      continue;
    localCode.insert( label, labels.size() );
    labels.append( label );
    schemaCode.append( span.code );
  }

  std::vector<WellFaciesColumn> columns;
  QVector<int> source;
  for ( int i = 0; i < formations.size(); ++i )
  {
    const WellComposite::FormationInterval &f = formations[i];
    if ( f.name.trimmed().isEmpty() || !( f.bottomDepth > f.topDepth ) )
      continue;
    WellFaciesColumn column;
    column.wellId = std::to_string( i );
    column.top = f.topDepth;
    column.bottom = f.bottomDepth;
    for ( const FaciesSpan &span : ordered )
      if ( span.bottom > column.top && span.top < column.bottom )
        column.intervals.push_back( { span.top, span.bottom,
                                      localCode.value( span.label.trimmed() ),
                                      span.confidence } );
    columns.push_back( std::move( column ) );
    source.append( i );
  }
  if ( columns.empty() )
    return {};
  const DominantFaciesResult result = computeDominantFacies( columns );
  if ( result.status != paleo::singlefactor::Status::Ok )
    return {};

  QVector<FormationDominance> out;
  for ( size_t n = 0; n < result.rows.size(); ++n )
  {
    const DominantFaciesRow &row = result.rows[n];
    if ( row.classifiedThickness <= 0 || row.dominantCode < 0 )
      continue;
    const WellComposite::FormationInterval &f = formations[source[static_cast<int>( n )]];
    FormationDominance d;
    d.formation = f.name.trimmed();
    d.unitType = f.unitType.trimmed().isEmpty()
                   ? ( d.formation.endsWith( QStringLiteral( "组" ) ) ? QStringLiteral( "组" )
                                                                       : QStringLiteral( "段" ) )
                   : f.unitType.trimmed();
    d.top = f.topDepth;
    d.bottom = f.bottomDepth;
    d.thickness = row.horizonThickness;
    d.classifiedThickness = row.classifiedThickness;
    d.coverage = row.coverage;
    d.tie = row.tie;
    QStringList parts;
    for ( const FaciesFrequency &freq : row.frequencies )
    {
      const QString label = labels.value( freq.faciesCode );
      parts << QStringLiteral( "%1 %2 m" ).arg( label ).arg( freq.thickness, 0, 'f', 2 );
      if ( freq.faciesCode == row.dominantCode )
      {
        d.dominant = label;
        d.dominantCode = schemaCode.value( freq.faciesCode, -1 );
        d.dominantThickness = freq.thickness;
        d.ratio = freq.fraction;
      }
    }
    d.composition = parts.join( QStringLiteral( "；" ) );
    out.append( d );
  }
  return out;
}

} // namespace paleo::faciesmapping
