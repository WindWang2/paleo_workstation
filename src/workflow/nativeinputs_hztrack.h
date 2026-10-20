// 层：功能
// workflow/nativeinputs_hztrack.h — algo-atom P2：NativeCppAdapter hztrack 输入（拆自 algorithmadapters.h，
// 只让用到该域的 TU 带入对应核头，压传递扇出）。
#pragma once

#include "../algorithms/horizontrack.h"
#include <functional>
#include <vector>

namespace paleo
{

struct TrackSectionInputs
{
  const float *section = nullptr;
  int nTraces = 0;
  int nSamples = 0;
  paleo::hztrack::SeedPoint seed;
  paleo::hztrack::TrackOptions options;
  const float *coherence = nullptr;
  std::function<bool()> cancelled;
};

struct PropagateVolumeInputs
{
  int nIl = 0;
  int nXl = 0;
  int nS = 0;
  paleo::hztrack::SectionProvider sectionAt;
  int seedIl = 0;
  std::vector<paleo::hztrack::SeedPoint> seeds;
  paleo::hztrack::TrackOptions options;
  int maxInlineStep = 0;
  std::function<bool()> cancelled;
};

} // namespace paleo
