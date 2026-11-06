// 层：数据
// algorithms/depthbatch.h — 时深批转换门面。公式只调用 paleo::velmodel，不另写一套。
#pragma once

#include "velocitymodel.h"

#include <functional>
#include <string>
#include <vector>

namespace paleo::depthbatch {

enum class TimeSemantic { Unspecified, TwtMs, OwtMs };
enum class DepthSemantic { Unspecified, TvdM, MdM, TvdssM };
enum class Extrapolation { Unspecified, RejectOutside, IntervalExtendedV1 };

// 栅格生产路径实际走 VelocityModel::depthForTwt（至少一口井在范围内时，
// 范围外的井用 wellDepthForTwtExtended 参与 IDW）。这是已有语义的名字，不是新策略。
inline const char *rasterExtrapolationPolicy() { return "spatial_idw_extended_v1"; }

struct IntervalRef {
  std::string intervalId;
  std::string occurrenceId;
  std::string kind; // "interval" or "surface"
  double top = 0;
  double bottom = 0;
  bool includeBottom = false;
};

struct SampleRef {
  std::string sampleId;
  std::string wellId;
  std::string occurrenceId;
  double time = 0;
  double quality = 1;
  bool hasQuality = true;
};

struct BatchRequest {
  TimeSemantic time = TimeSemantic::Unspecified;
  DepthSemantic depth = DepthSemantic::Unspecified;
  Extrapolation extrapolation = Extrapolation::Unspecified;
  std::string owtPolicy; // empty, or "owt_double_to_twt_v1"
  std::string modelRevision;
  bool mergeDiscontinuous = false;
  std::vector<IntervalRef> intervals;
  std::vector<SampleRef> samples;
};

struct SampleOut {
  std::string sampleId;
  std::string wellId;
  std::string occurrenceId;
  double depthM = 0;
  bool valid = false;
  std::string code;
};

struct BatchResult {
  bool ok = false;
  std::string code;
  std::string message;
  std::string extrapolationUsed;
  std::string modelType;
  std::vector<SampleOut> samples;
  int converted = 0;
  int outside = 0;
  int invalid = 0;
  std::vector<std::string> occurrenceIds;
};

// 旧 TD 文本装配：有 TVD 用 TVD，否则用 MD。这是遗留替用，不是 MD→TVD 换算。
double legacyControlDepth(bool hasTvd, double tvd, bool hasMd, double md);

// 只检查结点顺序，不插值。
std::string auditKnotOrder(const std::vector<std::pair<double, double>> &twtAndDepth);

// 不把不连续出现合并成一个窗。mergeDiscontinuous 或界面被当成层段时直接拒绝。
BatchResult planIntervals(const BatchRequest &request);

BatchResult convertWellBatch(const paleo::velmodel::VelocityModel &model, const BatchRequest &request,
                             const std::function<bool()> &cancelled = {});

} // namespace paleo::depthbatch
