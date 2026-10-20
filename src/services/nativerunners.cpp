// 层：数据
#include "nativerunners.h"

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

} // namespace paleo::native_runners
