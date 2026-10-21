// 层：功能
// workflow/nativeinputs_inversion.h — algo-atom P2：NativeCppAdapter inversion 输入（拆自 algorithmadapters.h，
// 只让用到该域的 TU 带入对应核头，压传递扇出）。
#pragma once

#include "../algorithms/inversion/bandlimit.h"
#include "../algorithms/inversion/sparse.h"

namespace paleo
{

struct BandlimitedInputs
{
  const float *trace = nullptr;
  int n = 0;
  double sampleIntervalMs = 1.0;
  const paleo::inversion::Wavelet *wavelet = nullptr;
  const float *lowFreq = nullptr;
  paleo::inversion::BandlimitedOptions options;
};

struct SparseSpikeInputs
{
  const float *trace = nullptr;
  int n = 0;
  double sampleIntervalMs = 1.0;
  const paleo::inversion::Wavelet *wavelet = nullptr; // 必填（核要求引用）
  const float *lowFreq = nullptr;
  paleo::inversion::SparseSpikeOptions options;
};

} // namespace paleo
