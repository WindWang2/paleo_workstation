// 层：功能
// workflow/nativeinputs_wellsiting.h — algo-atom P2：NativeCppAdapter wellsiting 输入（拆自 algorithmadapters.h，
// 只让用到该域的 TU 带入对应核头，压传递扇出）。
#pragma once

#include "../algorithms/wellsiting/wellsiting.h"
#include <vector>

namespace paleo
{

struct DiagnoseCoverageInputs
{
  const std::vector<paleo::wellsiting::Polygon> *domain = nullptr;
  const std::vector<paleo::wellsiting::Point2> *wells = nullptr;
  paleo::wellsiting::SitingOptions options;
};

} // namespace paleo
