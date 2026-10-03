// 层：数据
#include "dominantfacies.h"

#include <algorithm>
#include <cmath>

namespace paleo::faciesmapping
{

namespace
{

struct CodeAccumulator
{
  double thickness = 0;
  double score = 0;
};

// 最上优先吸收：区间按 top 升序，cursor 之后才算厚度——重叠段归先到者，
// 不双算（诚实计数）。区间先裁剪到 [top, bottom] 层段范围。
void accumulateColumn( const WellFaciesColumn &well,
                       std::map<int, CodeAccumulator> *codes, double *classified )
{
  std::vector<FaciesInterval> intervals = well.intervals;
  std::sort( intervals.begin(), intervals.end(),
             []( const FaciesInterval &a, const FaciesInterval &b ) { return a.top < b.top; } );
  double cursor = well.top;
  for ( const FaciesInterval &interval : intervals )
  {
    if ( interval.bottom <= interval.top )
      continue; // 零长/倒置区间无效
    const double from = std::max( interval.top, cursor );
    const double to = std::min( interval.bottom, well.bottom );
    if ( to <= from )
      continue;
    const double thickness = to - from;
    if ( interval.faciesCode >= 0 )
    {
      CodeAccumulator &acc = ( *codes )[interval.faciesCode];
      acc.thickness += thickness;
      const double confidence = std::clamp( interval.confidence, 0.0, 1.0 );
      acc.score += thickness * confidence;
      *classified += thickness;
    }
    cursor = std::max( cursor, to );
  }
}

} // namespace

DominantFaciesResult computeDominantFacies( const std::vector<WellFaciesColumn> &wells,
                                            const DominantFaciesOptions &options,
                                            const Control *control )
{
  DominantFaciesResult result;
  if ( wells.empty() )
  {
    result.status = Status::InvalidInput;
    result.message = "no well facies columns supplied";
    return result;
  }

  int noCodeRows = 0;
  int tieRows = 0;
  int gatedRows = 0;
  double coverageSum = 0;
  result.rows.reserve( wells.size() );
  for ( const WellFaciesColumn &well : wells )
  {
    if ( control && control->cancelled && control->cancelled() )
    {
      result.status = Status::Cancelled;
      result.message = "cancelled";
      return result;
    }

    DominantFaciesRow row;
    row.wellId = well.wellId;
    row.x = well.x;
    row.y = well.y;
    row.horizonThickness = std::max( 0.0, well.bottom - well.top );

    std::map<int, CodeAccumulator> codes;
    double classified = 0;
    accumulateColumn( well, &codes, &classified );
    row.classifiedThickness = classified;
    row.coverage = row.horizonThickness > 0 ? classified / row.horizonThickness : 0;

    double totalScore = 0;
    row.frequencies.reserve( codes.size() );
    for ( const auto &[code, acc] : codes )
    {
      FaciesFrequency freq;
      freq.faciesCode = code;
      freq.thickness = acc.thickness;
      freq.fraction = classified > 0 ? acc.thickness / classified : 0;
      freq.score = acc.score;
      totalScore += acc.score;
      row.frequencies.push_back( freq );
    }
    std::sort( row.frequencies.begin(), row.frequencies.end(),
               []( const FaciesFrequency &a, const FaciesFrequency &b ) {
                 return a.thickness != b.thickness ? a.thickness > b.thickness
                                                   : a.faciesCode < b.faciesCode;
               } );

    if ( totalScore <= 0 || row.frequencies.empty() )
    {
      noCodeRows++;
    }
    else
    {
      // 优势裁决：score 最高者；并列（相对容差内）取小 code 并记 tie。
      const double tieTolerance = 1e-12 * std::max( 1.0, totalScore );
      std::vector<const FaciesFrequency *> ranked = [&row]() {
        std::vector<const FaciesFrequency *> ranked;
        ranked.reserve( row.frequencies.size() );
        for ( const FaciesFrequency &freq : row.frequencies )
          ranked.push_back( &freq );
        std::sort( ranked.begin(), ranked.end(),
                   []( const FaciesFrequency *a, const FaciesFrequency *b ) {
                     return a->score != b->score ? a->score > b->score
                                                 : a->faciesCode < b->faciesCode;
                   } );
        return ranked;
      }();
      const FaciesFrequency *top = ranked.front();
      row.dominantCode = top->faciesCode;
      row.dominance = top->score / totalScore;
      if ( ranked.size() > 1 &&
           ranked.at( 1 )->score >= top->score - tieTolerance )
      {
        row.tie = true;
        tieRows++;
      }
      if ( row.coverage + 1e-12 < options.minCoverage ||
           row.dominance + 1e-12 < options.minDominance )
      {
        row.dominantCode = -1;
        gatedRows++;
      }
    }
    coverageSum += row.coverage;
    result.rows.push_back( std::move( row ) );
  }

  result.diagnostics.insert( QStringLiteral( "well_count" ),
                             static_cast<int>( wells.size() ) );
  result.diagnostics.insert( QStringLiteral( "no_code_rows" ), noCodeRows );
  result.diagnostics.insert( QStringLiteral( "tie_rows" ), tieRows );
  result.diagnostics.insert( QStringLiteral( "gated_rows" ), gatedRows );
  result.diagnostics.insert( QStringLiteral( "mean_coverage" ),
                             coverageSum / static_cast<double>( wells.size() ) );
  result.diagnostics.insert( QStringLiteral( "overlap_policy" ),
                             QStringLiteral( "topmost_wins_no_double_count" ) );
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::faciesmapping
