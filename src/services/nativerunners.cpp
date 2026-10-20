// 层：数据
#include "nativerunners.h"
#include "../algorithms/horizontrack.h"

namespace paleo::native_runners
{

AlgorithmRunResult runTrackSection( const float *section, int nTraces, int nSamples,
                                    paleo::hztrack::SeedPoint seed,
                                    const paleo::hztrack::TrackOptions &options,
                                    const float *coherence,
                                    const std::function<bool()> &cancelled,
                                    paleo::hztrack::TrackResult *out, QString *error )
{
  AlgorithmRunResult r;
  paleo::hztrack::TrackResult local;
  paleo::hztrack::TrackResult &tr = out ? *out : local;
  tr = paleo::hztrack::trackSection( section, nTraces, nSamples, seed, options, coherence,
                                     cancelled );
  if ( !tr.valid()
       && ( tr.stopLeft.reason == paleo::hztrack::StopReason::Invalid
            || tr.stopRight.reason == paleo::hztrack::StopReason::Invalid ) )
  {
    r.ok = false;
    r.error = QStringLiteral( "hztrack:track_section：输入几何或种子非法" );
    if ( error )
      *error = r.error;
    return r;
  }
  r.ok = true;
  r.diagnostics.insert( QStringLiteral( "pick_count" ), static_cast<int>( tr.picks.size() ) );
  r.diagnostics.insert( QStringLiteral( "stop_left" ), static_cast<int>( tr.stopLeft.reason ) );
  r.diagnostics.insert( QStringLiteral( "stop_right" ), static_cast<int>( tr.stopRight.reason ) );
  r.diagnostics.insert( QStringLiteral( "algorithm_id" ),
                        QStringLiteral( "hztrack:track_section" ) );
  return r;
}

AlgorithmRunResult runPropagateVolumeWindowed(
    int nIl, int nXl, int nS, const paleo::hztrack::SectionProvider &sectionAt, int seedIl,
    const std::vector<paleo::hztrack::SeedPoint> &seeds,
    const paleo::hztrack::TrackOptions &options, int maxInlineStep,
    const std::function<bool()> &cancelled, paleo::hztrack::PropagateResult *out,
    QString *error )
{
  AlgorithmRunResult r;
  paleo::hztrack::PropagateResult local;
  paleo::hztrack::PropagateResult &pr = out ? *out : local;
  pr = paleo::hztrack::propagateVolumeWindowed( nIl, nXl, nS, sectionAt, seedIl, seeds, options,
                                                maxInlineStep, cancelled );
  if ( pr.stopReason == paleo::hztrack::StopReason::Invalid )
  {
    r.ok = false;
    r.error = QStringLiteral( "hztrack:propagate_volume：输入几何或种子非法" );
    if ( error )
      *error = r.error;
    r.diagnostics.insert( QStringLiteral( "algorithm_id" ),
                          QStringLiteral( "hztrack:propagate_volume" ) );
    return r;
  }
  // ReadFailure / Cancelled / Completed 均如实回传；编排侧解读 stopReason。
  r.ok = true;
  r.diagnostics.insert( QStringLiteral( "pick_count" ), static_cast<int>( pr.picks.size() ) );
  r.diagnostics.insert( QStringLiteral( "stop_reason" ), static_cast<int>( pr.stopReason ) );
  r.diagnostics.insert( QStringLiteral( "seed_il" ), pr.seedIl );
  r.diagnostics.insert( QStringLiteral( "il_min" ), pr.ilMin );
  r.diagnostics.insert( QStringLiteral( "il_max" ), pr.ilMax );
  r.diagnostics.insert( QStringLiteral( "algorithm_id" ),
                        QStringLiteral( "hztrack:propagate_volume" ) );
  return r;
}

} // namespace paleo::native_runners
