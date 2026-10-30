// 层：功能
#include "stratigraphyworkflow_state.h"
#include "../services/previewdoc.h"
#include "../domain/wellcompositemodel.h"
#include <algorithm>
#include <cmath>

bool StratigraphyWorkflow::previewWellData(qlonglong well,
    WellComposite::ComprehensiveWellData *data, QString *error, bool *baseLoaded) const {
  if (!data) return false;
  const bool loaded = PreviewDocService::wellCompositeForWell(d->path, well, data, error);
  if (baseLoaded) *baseLoaded = loaded;
  if (!loaded) {
    *data = {}; data->minDepth = INFINITY; data->maxDepth = -INFINITY;
    // #827：基础模型失败不能只在这里静默清空——下面只要解释版本还有派生曲线，
    // 函数就会返回 true，用户看到的是「只有部分井道」的正常界面。把原因带进
    // dataWarnings 供面板展示，调用方另按 baseLoaded 决定是否提示。
    data->dataWarnings << tr("基础模型读取失败：%1；当前仅显示解释版本派生曲线")
                              .arg(error && !error->isEmpty() ? *error
                                                              : tr("未知原因"));
  }
  // 算法选用的冻结原始曲线及 MD 派生曲线随解释版本显示；其它井道来自共用预览服务。
  for (const auto &track : tracks()) if (track.wellId == well) {
    auto found = std::find_if(data->continuousCurves.begin(), data->continuousCurves.end(),
        [&](const auto &curve) { return curve.name == track.mnemonic; });
    WellComposite::CurveData curve;
    if (found != data->continuousCurves.end()) curve = *found;
    curve.name = track.mnemonic; curve.depths = track.depths; curve.values = track.values;
    double lo = INFINITY, hi = -INFINITY;
    for (float value : curve.values) if (std::isfinite(value)) { lo = std::min(lo,double(value)); hi = std::max(hi,double(value)); }
    if (std::isfinite(lo)) { curve.minScale = float(lo); curve.maxScale = float(hi > lo ? hi : lo+1); }
    if (found == data->continuousCurves.end()) data->continuousCurves.append(curve);
    else *found = curve;
    data->wellName = track.name;
    for (float depth : track.depths) if (std::isfinite(depth)) {
      data->minDepth = std::min(data->minDepth, double(depth)); data->maxDepth = std::max(data->maxDepth, double(depth));
    }
  }
  return !data->isEmpty();
}
