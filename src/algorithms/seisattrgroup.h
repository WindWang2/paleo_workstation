// 层：数据
// algorithms/seisattrgroup.h — 已有地震属性核的批次门面。不复制 Hilbert / RMS / 相干公式。
#pragma once

#include "../domain/seismic/coherenceweightmode.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace paleo::seisattr {

enum class TraceAttribute {
  Envelope,
  Phase,
  Frequency,
  Quality,
  Rms,
  MaxAbs,
  MeanEnergy,
  Sweetness
};

// 单道单属性。瞬时族每次调用一次 complexTraceAnalysis；滑窗族调用对应 windowed*。
void computeTraceAttribute(TraceAttribute kind, const float *trace, int n, int halfWindow,
                           double sampleIntervalMs, float *out);

struct TraceGroupRequest {
  const float *traces = nullptr; // nTraces 条，每条 traceLength 个样，步长 stride
  int nTraces = 0;
  int traceLength = 0;
  int stride = 0;
  double sampleIntervalMs = 0;
  int rmsHalf = 0;
  int outputBegin = 0;
  int outputEnd = 0; // <= begin 表示整道
  bool wantRms = false;
  bool wantEnvelope = false;
  bool wantPhase = false;
  bool fullTrace = true; // 包络/相位为假时拒绝，不能把短窗当整道
};

struct TraceGroupResult {
  bool ok = false;
  std::string code;
  int nOut = 0;
  int analyticEvaluations = 0;
  int samplesRead = 0;
  int rmsHalfApplied = 0;
  std::vector<float> rms;
  std::vector<float> envelope;
  std::vector<float> phase;
};

TraceGroupResult computeAttributeGroup(const TraceGroupRequest &request);

// 有限晕：在 [begin,end) 上与整道 windowedRms 逐位一致。调用方必须传入真邻点。
bool rmsInteriorFromHalo(const float *trace, int n, int half, int begin, int end, float *out);

struct CoherenceTile {
  const float *volume = nullptr;
  int nIl = 0;
  int nXl = 0;
  int nS = 0;
  int ilHalf = 0;
  int xlHalf = 0;
  int timeHalf = 0;
  double ilSpacing = 0;
  double xlSpacing = 0;
};

struct CoherenceTileResult {
  bool ok = false;
  std::string code;
  int interiorIl = 0;
  int interiorXl = 0;
  int nS = 0;
  int samplesRead = 0;
  std::vector<float> interior; // 去掉空间边界带后的值，布局 (il, xl, s)
};

// 边界带按核的约定为 NaN，不写入 interior。mode 传给 semblanceCoherenceWeighted。
CoherenceTileResult coherenceInterior(const CoherenceTile &tile,
                                     CoherenceWeightMode mode = CoherenceWeightMode::Equal);

// 沿 XL 把内区分成若干块，每块带上 xlHalf 晕再调用 coherenceInterior。
// interiorXlChunk 是每块内区的 XL 数。结果与一次整幅 coherenceInterior 的 interior 逐元素一致。
struct CoherenceXlSpan {
  int sourceXl0 = 0;
  int sourceXl1 = 0; // 不含端点，含两侧晕
  int interiorXl0 = 0; // 在整幅内区中的 XL 起点
};

std::vector<CoherenceXlSpan> coherenceXlSpans(int nXl, int xlHalf, int interiorChunk);

CoherenceTileResult computeCoherenceTiled(const CoherenceTile &volume, CoherenceWeightMode mode, int interiorXlChunk,
                                          const std::function<bool()> &cancelled = {});

// 三维样点数。任一维非正、乘积溢出 int64、或放不进 int 时返回 false，且不写 *count。
bool volumeIndexCount(int nIl, int nXl, int nS, int *count);

struct AttributeBudgetPlan {
  std::string code; // ok | split | budget_exceeded
  std::string shape;
  std::int64_t estimatedBytes = 0;
  std::int64_t minimumBytes = 0;
  int traceBatch = 0;
  int xlChunk = 0;
};

// 包络按整道计（含 FFT 草稿的下界），可按道批切分。
// 只要请求相干，就按整幅计价。最小窗放不进预算时 budget_exceeded。
// 整幅放不进、但一个带晕的 XL 块放得进时返回 split，xlChunk 是内区 XL 数。
// computeCoherenceTiled 必须按这个块长执行。
// volumeSamples > 0 时相干用该样数，否则用 nSamples。与道属性同时请求时，估计值是两者之和。
AttributeBudgetPlan planAttributeBudget(int nTraces, int nSamples, int rmsHalf, bool envelope, bool rms,
                                        bool coherence, int nIl, int nXl, int ilHalf, int xlHalf,
                                        std::int64_t budgetBytes, int volumeSamples = 0);

} // namespace paleo::seisattr
