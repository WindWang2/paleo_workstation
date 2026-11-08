// 层：数据
// algorithms/depthbatch.cpp — 见 depthbatch.h。数值只委托 VelocityModel。
#include "depthbatch.h"

#include <cmath>
#include <limits>

namespace paleo::depthbatch {
namespace {

BatchResult fail(const std::string &code, const std::string &message) {
  BatchResult result;
  result.ok = false;
  result.code = code;
  result.message = message;
  return result;
}

const paleo::velmodel::VelocityModel::Well *findWell(const paleo::velmodel::VelocityModel &model,
                                                    const QString &wellId) {
  for (const auto &well : model.wells()) {
    if (well.id == wellId)
      return &well;
  }
  return nullptr;
}

} // namespace

double legacyControlDepth(bool hasTvd, double tvd, bool hasMd, double md) {
  if (hasTvd)
    return tvd;
  if (hasMd)
    return md;
  return std::numeric_limits<double>::quiet_NaN();
}

std::string auditKnotOrder(const std::vector<std::pair<double, double>> &twtAndDepth) {
  if (twtAndDepth.size() < 2)
    return "too_few_knots";
  for (std::size_t i = 1; i < twtAndDepth.size(); ++i) {
    if (!(twtAndDepth[i].first > twtAndDepth[i - 1].first) ||
        !(twtAndDepth[i].second > twtAndDepth[i - 1].second))
      return "knots_not_strictly_increasing";
  }
  return {};
}

BatchResult planIntervals(const BatchRequest &request) {
  if (request.mergeDiscontinuous)
    return fail("discontinuous_merge_refused", "discontinuous occurrences are not merged");
  BatchResult result;
  result.ok = true;
  result.code = request.intervals.empty() ? "no_intervals" : "intervals_kept";
  for (const IntervalRef &interval : request.intervals) {
    if (interval.kind == "surface")
      return fail("surface_is_not_interval", interval.intervalId);
    if (interval.intervalId.empty() || interval.occurrenceId.empty())
      return fail("missing_interval_identity", "interval id and occurrence id are required");
    result.occurrenceIds.push_back(interval.occurrenceId);
  }
  return result;
}

BatchResult convertWellBatch(const paleo::velmodel::VelocityModel &model, const BatchRequest &request,
                             const std::function<bool()> &cancelled) {
  const BatchResult intervals = planIntervals(request);
  if (!intervals.ok)
    return intervals;
  if (request.time == TimeSemantic::Unspecified)
    return fail("time_semantic_required", "TWT or OWT must be explicit");
  if (request.depth != DepthSemantic::TvdM)
    return fail("depth_domain_unsupported", "this model emits TVD metres; MD and TVDSS are refused");
  if (request.extrapolation == Extrapolation::Unspecified)
    return fail("extrapolation_required", "extrapolation policy must be explicit");
  double timeScale = 1.0;
  if (request.time == TimeSemantic::OwtMs) {
    if (request.owtPolicy != "owt_double_to_twt_v1")
      return fail("time_domain_unsupported", "OWT is not treated as TWT");
    timeScale = 2.0;
  }
  if (!model.isValid())
    return fail("model_invalid", "velocity model has no wells");
  if (request.modelRevision.empty())
    return fail("model_revision_required", "model revision must be frozen");

  BatchResult result = intervals;
  result.extrapolationUsed = request.extrapolation == Extrapolation::IntervalExtendedV1
                                 ? "interval_extended_v1"
                                 : "reject_outside";
  result.modelType = paleo::velmodel::modelTypeId(model.type()).toStdString();
  if (request.samples.empty()) {
    result.ok = true;
    result.code = "empty_window";
    return result;
  }

  result.samples.reserve(request.samples.size());
  for (const SampleRef &sample : request.samples) {
    if (cancelled && cancelled()) {
      BatchResult cancelledResult = fail("cancelled", "cancelled before commit");
      cancelledResult.occurrenceIds = result.occurrenceIds;
      return cancelledResult;
    }
    SampleOut out;
    out.sampleId = sample.sampleId;
    out.wellId = sample.wellId;
    out.occurrenceId = sample.occurrenceId;
    out.depthM = std::numeric_limits<double>::quiet_NaN();
    const bool missingQuality = !sample.hasQuality || !std::isfinite(sample.quality) || sample.quality == 0.0;
    if (missingQuality || !std::isfinite(sample.time)) {
      out.code = missingQuality ? "nan_quality" : "invalid_time";
      ++result.invalid;
      result.samples.push_back(out);
      continue;
    }
    const auto *well = findWell(model, QString::fromStdString(sample.wellId));
    if (!well) {
      out.code = "unknown_well";
      ++result.invalid;
      result.samples.push_back(out);
      continue;
    }
    const double twt = sample.time * timeScale;
    const double depth = request.extrapolation == Extrapolation::IntervalExtendedV1
                             ? model.wellDepthForTwtExtended(*well, twt)
                             : model.wellDepthForTwt(*well, twt);
    if (!std::isfinite(depth)) {
      out.code = "outside";
      ++result.outside;
    } else {
      out.depthM = depth;
      out.valid = true;
      out.code = "ok";
      ++result.converted;
    }
    result.samples.push_back(out);
  }
  result.ok = true;
  result.code = "converted";
  return result;
}

} // namespace paleo::depthbatch
