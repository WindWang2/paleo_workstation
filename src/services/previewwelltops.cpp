// 层：数据
#include "previewdoc.h"
#include "projectdata.h"
#include "previewwellbases.h"
#include "../catalog/datacatalog.h"
#include "../domain/wellrecords.h"
#include <algorithm>
#include <cmath>

void PreviewDocService::wellTopsForLog(const QString &wellId, QVector<WellTopRecord> *out,
                                      QStringList *warnings, QHash<QString, double> *bases) const
{
  if (!out) return;
  out->clear(); if (warnings) warnings->clear();
  if (bases) bases->clear();
  ProjectDataFacade data; data.setCatalog(catalog());
  const auto tops = data.topsFor(wellId);
  const auto rawBases = bases ? previewWellBases(catalog(), wellId, tops, warnings) : QVector<WellTopRecord>();
  const bool needsConversion = std::any_of(tops.cbegin(), tops.cend(), [](const WellTop &top) {
    return !std::isfinite(top.md) && std::isfinite(top.tvd);
  }) || std::any_of(rawBases.cbegin(), rawBases.cend(), [](const WellTopRecord &base) {
    return !base.hasMd && base.hasTvd;
  });
  QString surveyError;
  const auto survey = needsConversion ? data.trajectoryFor(wellId, &surveyError) : std::nullopt;
  if (needsConversion && !survey && surveyError.isEmpty() && catalog())
    for (const auto &link : catalog()->linksForEntity(wellId))
      if (link.role == QLatin1String("trajectory") && !link.unresolved) { surveyError = tr("关联的测斜文件缺失或不可读取"); break; }
  QVector<QPair<double, double>> pairs;
  if (needsConversion && !survey && surveyError.isEmpty())
    for (const auto &sample : data.tdTableFor(wellId))
      if (std::isfinite(sample.tvd) && std::isfinite(sample.md)) pairs.append({sample.tvd, sample.md});
  std::sort(pairs.begin(), pairs.end()); pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
  // MD 和 TVD 均以井口为深度零点（TVDSS 是另一个字段）。
  // 对照从井下开始时补几何原点，浅部顶界落在井口与首对照之间，不作深部外推。
  if (!pairs.isEmpty() && pairs.first().first > 0 && pairs.first().second > 0) pairs.prepend({0.0, 0.0});
  bool validPairs = pairs.size() >= 2, identity = validPairs;
  for (int i = 0; i < pairs.size(); ++i) {
    identity &= std::abs(pairs[i].first - pairs[i].second) < 1e-6;
    if (i && (pairs[i].first <= pairs[i - 1].first || pairs[i].second <= pairs[i - 1].second)) validPairs = false;
  }
  const auto warn = [&](const QString &message) { if (warnings && !warnings->contains(message)) warnings->append(message); };
  if (needsConversion) {
    if (!surveyError.isEmpty()) warn(tr("测斜不可用，只有 TVD 的分层暂不绘制：%1").arg(surveyError));
    else if (survey) warn(tr("分层 TVD 已按测斜转换为显示 MD，原始分层深度未改写。"));
    else if (validPairs) warn(tr("分层 TVD 已按时深表的 MD/TVD 对照定位，原始 MD 仍保留为空。"));
    else if (!pairs.isEmpty()) warn(tr("时深表的 MD/TVD 对照不足或不单调，只有 TVD 的分层暂不绘制。"));
    else warn(tr("分层只有 TVD，未提供测斜或 MD/TVD 对照；按直井等深显示，原始 MD 仍保留为空。"));
  }
  const auto measuredDepth = [&](double md, double tvd) {
    if (!std::isfinite(md) && std::isfinite(tvd) && surveyError.isEmpty()) {
      if (survey) md = survey->tvdToMd(tvd);
      else if (validPairs) {
        const auto upper = std::lower_bound(pairs.cbegin(), pairs.cend(), tvd, [](const auto &pair, double value) { return pair.first < value; });
        if (upper != pairs.cend() && upper->first == tvd) md = upper->second;
        else if (upper != pairs.cbegin() && upper != pairs.cend()) {
          const auto &lower = *(upper - 1);
          md = lower.second + (tvd - lower.first) * (upper->second - lower.second) / (upper->first - lower.first);
        } else if (identity) {
          md = tvd; warn(tr("部分分层超出时深对照范围，依据等深对照按直井等深显示。"));
        } else warn(tr("部分分层超出 MD/TVD 对照范围，暂不绘制，不自动外推。"));
      } else if (pairs.isEmpty()) md = tvd;
    }
    return md;
  };
  for (const auto &top : tops) {
    const auto md = measuredDepth(top.md, top.tvd);
    if (!std::isfinite(md)) continue;
    WellTopRecord record; record.wellName = catalog() ? catalog()->entityById(wellId).name : QString();
    record.topName = top.horizon; record.md = md; record.hasMd = true;
    record.tvd = top.tvd; record.hasTvd = std::isfinite(top.tvd); out->append(record);
  }
  if (bases) for (const auto &base : rawBases) {
    const auto md = measuredDepth(base.hasMd ? base.md : qQNaN(), base.hasTvd ? base.tvd : qQNaN());
    const auto top = std::find_if(out->cbegin(), out->cend(), [&](const auto &top) { return top.topName == base.topName; });
    if (std::isfinite(md) && top != out->cend()) {
      if (md > top->md) bases->insert(base.topName, md);
      else warn(tr("部分分层底界不深于顶界，按相邻顶界或测井显示范围截取。"));
    }
  }
  std::stable_sort(out->begin(), out->end(), [](const auto &a, const auto &b) { return a.md < b.md; });
}
