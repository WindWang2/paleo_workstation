// 层：数据
// services/nativerunners.h — algo-atom：services 层可调用的纯核 → AlgorithmRunResult
// 包装。hztrack 生产路径在 seismictaskservice（不可链 workflow），故与
// workflow/NativeCppAdapter 共用此入口。
#pragma once

#include "algorithmspec.h"

#include "../algorithms/horizontrack.h"

#include <QString>
#include <functional>

namespace paleo::native_runners
{

// 包装 hztrack::trackSection；核结果经 out 回传。
AlgorithmRunResult runTrackSection( const float *section, int nTraces, int nSamples,
                                    paleo::hztrack::SeedPoint seed,
                                    const paleo::hztrack::TrackOptions &options,
                                    const float *coherence,
                                    const std::function<bool()> &cancelled,
                                    paleo::hztrack::TrackResult *out = nullptr,
                                    QString *error = nullptr );

} // namespace paleo::native_runners
