// 层：功能
// workflow/nativeinputs_facies.h — algo-atom P2：NativeCppAdapter faciesmapping 输入（拆自 algorithmadapters.h，
// 只让用到该域的 TU 带入对应核头，压传递扇出）。
#pragma once

#include "../algorithms/faciesmapping/evidencesynthesis.h"
#include <vector>

namespace paleo
{

struct SynthesizeFaciesInputs
{
  const std::vector<paleo::faciesmapping::CandidateRegion> *regions = nullptr;
  const std::vector<paleo::faciesmapping::EvidenceSource> *sources = nullptr;
  paleo::faciesmapping::SynthesisOptions options;
  const paleo::faciesmapping::Control *control = nullptr;
};

} // namespace paleo
