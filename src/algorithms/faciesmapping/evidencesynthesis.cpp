// 层：数据
#include "evidencesynthesis.h"

#include "../singlefactor/geosutil.h"

#include <algorithm>
#include <cmath>

namespace paleo::faciesmapping
{

SynthesisResult synthesizeFaciesRegions( const std::vector<CandidateRegion> &regions,
                                         const std::vector<EvidenceSource> &sources,
                                         const SynthesisOptions &options,
                                         const Control *control )
{
  using singlefactor::GeosContext;
  using singlefactor::GeomPtr;
  using singlefactor::makePoint;
  using singlefactor::makePolygon;

  SynthesisResult result;
  if ( regions.empty() )
  {
    result.status = Status::InvalidInput;
    result.message = "no candidate regions supplied";
    return result;
  }

  GeosContext ctx;

  // 单元面几何一次性构建；退化面（<3 点外环）按空处理——点票为 0，
  // 只剩约束继承路径。
  std::vector<GeomPtr> regionGeoms;
  regionGeoms.reserve( regions.size() );
  for ( const CandidateRegion &region : regions )
  {
    if ( region.geometry.exterior.points.size() < 3 )
    {
      regionGeoms.emplace_back();
      continue;
    }
    regionGeoms.push_back( makePolygon( ctx.handle, region.geometry ) );
  }

  // 点证据（faciesCode >= 0）一次性构建。
  struct Voter
  {
    GeomPtr point;
    const EvidenceSource *source;
    const EvidenceSample *sample;
  };
  std::vector<Voter> voters;
  for ( const EvidenceSource &source : sources )
  {
    for ( const EvidenceSample &sample : source.samples )
    {
      if ( sample.faciesCode < 0 )
        continue;
      Voter voter;
      voter.point = makePoint( ctx.handle, sample.x, sample.y );
      voter.source = &source;
      voter.sample = &sample;
      if ( voter.point )
        voters.push_back( std::move( voter ) );
    }
  }

  int assignedCount = 0;
  int inheritedCount = 0;
  int unassignedCount = 0;
  int multiSourceRegions = 0;
  result.regions.reserve( regions.size() );
  for ( std::size_t i = 0; i < regions.size(); ++i )
  {
    if ( control && control->cancelled && control->cancelled() )
    {
      result.status = Status::Cancelled;
      result.message = "cancelled";
      result.regions.clear();
      return result;
    }

    RegionSynthesis synthesis;
    synthesis.regionId = regions[i].regionId;
    const GeomPtr &regionGeom = regionGeoms[i];

    std::map<std::string, int> sourceVotes;
    for ( const Voter &voter : voters )
    {
      if ( !regionGeom ||
           GEOSIntersects_r( ctx.handle, regionGeom.get(), voter.point.get() ) != 1 )
        continue;
      const double weight = voter.source->weight * std::clamp( voter.sample->confidence, 0.0, 1.0 ) *
                            ( voter.sample->weightBoost > 0 ? voter.sample->weightBoost : 1.0 );
      synthesis.scores[voter.sample->faciesCode] += weight;
      synthesis.voteCount++;
      sourceVotes[voter.source->id]++;
    }
    for ( const auto &[sourceId, count] : sourceVotes )
    {
      if ( count > 0 )
        synthesis.contributingSources.push_back( sourceId );
    }
    if ( sourceVotes.size() > 1 )
      multiSourceRegions++;

    double totalScore = 0;
    for ( const auto &[code, score] : synthesis.scores )
    {
      totalScore += score;
      synthesis.scoresRanked.emplace_back( code, score );
    }
    std::sort( synthesis.scoresRanked.begin(), synthesis.scoresRanked.end(),
               []( const auto &a, const auto &b ) {
                 return a.second != b.second ? a.second > b.second : a.first < b.first;
               } );

    if ( synthesis.voteCount >= std::max( 1, options.minVotes ) && totalScore > 0 )
    {
      const auto &top = synthesis.scoresRanked.front();
      synthesis.confidence = top.second / totalScore;
      if ( synthesis.confidence + 1e-12 >= options.assignThreshold )
        synthesis.assignedCode = top.first; // 过阈值才赋相
    }
    if ( synthesis.assignedCode >= 0 )
      assignedCount++;
    else if ( synthesis.voteCount == 0 && regions[i].faciesCode >= 0 )
    {
      // 无点证据但有约束相代码：继承约束（confidence 如实为 0——证据是
      // 专家约束线，不是井/图观测）。
      synthesis.assignedCode = regions[i].faciesCode;
      synthesis.constraintInherited = true;
      inheritedCount++;
    }
    else
      unassignedCount++;

    result.regions.push_back( std::move( synthesis ) );
  }

  result.diagnostics.insert( QStringLiteral( "region_count" ),
                             static_cast<int>( regions.size() ) );
  result.diagnostics.insert( QStringLiteral( "assigned" ), assignedCount );
  result.diagnostics.insert( QStringLiteral( "constraint_inherited" ), inheritedCount );
  result.diagnostics.insert( QStringLiteral( "unassigned" ), unassignedCount );
  result.diagnostics.insert( QStringLiteral( "multi_source_regions" ), multiSourceRegions );
  result.diagnostics.insert( QStringLiteral( "voter_count" ),
                             static_cast<int>( voters.size() ) );
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::faciesmapping
