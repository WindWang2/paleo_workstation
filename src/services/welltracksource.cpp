// 层：数据
#include "welltracksource.h"
#include "areawellchart.h"
#include "domain/welllogfacies.h"
#include "domain/wellnumeric.h"
#include <paleo_data/service.h>
#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace paleo::welltrack {
namespace {

constexpr auto kStore = "area-model";
constexpr qsizetype kMaxChoices = 2048;

bool isLas(const data::SourceIndexEntry &entry) {
  return QFileInfo(QString::fromUtf8(entry.path)).suffix().compare(QLatin1String("las"),
                                                                  Qt::CaseInsensitive) == 0;
}

bool matchingSource(data::DataService &service, const document::SourceRef &ref) {
  data::SourceIndexEntry entry;
  std::string error;
  return service.readSourceIndexEntry(ref.sourceId, entry, error) && entry.active &&
         isLas(entry) && QString::fromUtf8(entry.sha256) == ref.fileRevisionId;
}

bool matchingCurve(data::DataService &service, qlonglong source, qlonglong well,
                   qlonglong logSet, qlonglong curve, std::string &error) {
  std::vector<data::DataEntry> entries;
  if (!service.listSourceData("curves", source, entries, error))
    return false;
  return std::any_of(entries.cbegin(), entries.cend(), [&](const data::DataEntry &entry) {
    return entry.id == curve && entry.wellId == well && entry.sourceId == source &&
           entry.parentId == logSet;
  });
}

bool validWindow(const AreaWellCurveWindow &window) {
  return !std::isinf(window.topMd) && !std::isinf(window.bottomMd) &&
         !(window.topMd > window.bottomMd) &&
         window.maxSamples > 0 && window.maxSamples <= 8'000'000 &&
         window.maxReadBytes > 0 && window.maxReadBytes <= 256LL * 1024 * 1024;
}

} // namespace

bool listCurveChoices(const QString &modelDirectory, const QString &documentId, qlonglong wellId,
                      QVector<CurveChoice> &out, QString *error,
                      const std::function<bool()> &cancelled) {
  out.clear();
  if (error) error->clear();
  const auto fail = [&](const QString &reason) {
    if (error) *error = reason;
    return false;
  };
  const auto stopped = [&] { return cancelled && cancelled(); };
  if (stopped())
    return fail(QCoreApplication::translate("WellTrackSource", "已取消读取曲线目录。"));
  if (modelDirectory.isEmpty() || documentId.isEmpty() || wellId <= 0)
    return fail(QCoreApplication::translate("WellTrackSource", "需要已登记的井和工区模型。"));

  data::DataService service;
  std::string detail;
  std::vector<data::DataEntry> entries;
  if (!service.openProjectReadOnly(modelDirectory.toStdString(), detail) ||
      !service.listData("curves", wellId, entries, detail))
    return fail(QString::fromUtf8(detail));

  QVector<CurveChoice> choices;
  QHash<qlonglong, CurveChoice> sources;
  for (const auto &entry : entries) {
    if (stopped())
      return fail(QCoreApplication::translate("WellTrackSource", "已取消读取曲线目录。"));
    if (entry.count <= 0 || entry.sourceId <= 0 || entry.id <= 0 || entry.parentId <= 0 ||
        entry.wellId != wellId)
      continue;
    if (!sources.contains(entry.sourceId)) {
      data::SourceIndexEntry registered;
      if (!service.readSourceIndexEntry(entry.sourceId, registered, detail))
        return fail(QString::fromUtf8(detail));
      CurveChoice choice;
      const auto fingerprint = document::SourceFingerprint::fromStored(QString::fromUtf8(registered.sha256));
      if (registered.active && isLas(registered) && fingerprint.kind == document::FingerprintKind::Sha256) {
        choice.sourceId = entry.sourceId;
        choice.filePath = QString::fromUtf8(registered.path);
        choice.fileName = QFileInfo(choice.filePath).fileName();
        choice.source = {documentId, QLatin1String(kStore), entry.sourceId, fingerprint.stored()};
      }
      sources.insert(entry.sourceId, std::move(choice));
    }
    auto choice = sources.value(entry.sourceId);
    if (!choice.source.valid())
      continue;
    if (choices.size() >= kMaxChoices)
      return fail(QCoreApplication::translate("WellTrackSource", "井的曲线选项超出上限（%1 条）。")
                    .arg(kMaxChoices));
    choice.curveId = entry.id;
    choice.logSetId = entry.parentId;
    choice.curveName = QString::fromUtf8(entry.name);
    choices.append(std::move(choice));
  }
  if (stopped())
    return fail(QCoreApplication::translate("WellTrackSource", "已取消读取曲线目录。"));
  out = std::move(choices);
  return true;
}

CurveReadResult readCurve(const document::SourceRecord &source, qlonglong wellId, qlonglong logSetId,
                          qlonglong curveId, const AreaWellCurveWindow &window,
                          const std::function<bool()> &cancelled) {
  CurveReadResult result;
  const auto fail = [&](const QString &reason) {
    result.error = reason;
    return result;
  };
  const auto stopped = [&] { return cancelled && cancelled(); };
  if (stopped())
    return fail(QCoreApplication::translate("WellTrackSource", "已取消读取测井曲线。"));
  if (!source.ref.valid() || source.ref.modelStoreId != QLatin1String(kStore) ||
      source.modelDirectory.isEmpty() || wellId <= 0 || logSetId <= 0 || curveId <= 0 ||
      !validWindow(window))
    return fail(QCoreApplication::translate("WellTrackSource", "测井曲线引用或读取窗口无效。"));
  if (source.fingerprint.kind != document::FingerprintKind::Sha256 ||
      source.fingerprint.stored() != source.ref.fileRevisionId)
    return fail(QCoreApplication::translate("WellTrackSource", "测井来源版本已变更，请重新选择。"));

  data::DataService service;
  std::string detail;
  if (!service.openProjectReadOnly(source.modelDirectory.toStdString(), detail))
    return fail(QString::fromUtf8(detail));
  if (!matchingSource(service, source.ref))
    return fail(QCoreApplication::translate("WellTrackSource", "测井来源版本已变更，请重新选择。"));
  if (!matchingCurve(service, source.ref.sourceId, wellId, logSetId, curveId, detail))
    return fail(detail.empty()
                  ? QCoreApplication::translate("WellTrackSource", "所选曲线与井、来源或曲线组不匹配。")
                  : QString::fromUtf8(detail));
  if (stopped())
    return fail(QCoreApplication::translate("WellTrackSource", "已取消读取测井曲线。"));

  data::CurveReadOptions options;
  options.maxSamples = window.maxSamples;
  options.maxReadBytes = window.maxReadBytes;
  options.cancelled = cancelled;
  data::CurveWindowQuery query;
  query.curveId = curveId;
  query.sampleCount = 0;
  query.includeAuxiliaryAxes = false;
  data::CurveWindowResult metadata;
  if (!service.readCurveWindow(query, metadata, detail, options))
    return fail(QString::fromUtf8(detail));
  const auto depthUnit = QString::fromUtf8(metadata.depthInfo.depthUnit);
  const auto scale = wellnumeric::depthScaleToMetres(depthUnit);
  if (metadata.depthInfo.depthType != "MD" || !scale)
    return fail(QCoreApplication::translate("WellTrackSource", "曲线需要已声明的 MD 测深及米或英尺单位。"));

  const double top = window.topMd / *scale, bottom = window.bottomMd / *scale;
  if (std::isinf(top) || std::isinf(bottom))
    return fail(QCoreApplication::translate("WellTrackSource", "测井曲线读取窗口超出深度范围。"));
  data::CurveSlice slice;
  if (!service.readCurveSlice(curveId, top, bottom, true, slice, detail, options, &result.stats))
    return fail(QString::fromUtf8(detail));
  if (stopped())
    return fail(QCoreApplication::translate("WellTrackSource", "已取消读取测井曲线。"));
  if (!matchingSource(service, source.ref))
    return fail(QCoreApplication::translate("WellTrackSource", "测井来源版本已变更，请重新选择。"));

  if (slice.curveId != curveId || slice.values.size() != slice.depths.size() ||
      slice.values.size() != slice.quality.size())
    return fail(QCoreApplication::translate("WellTrackSource", "测井曲线样点数据不完整。"));
  QVector<double> values(slice.values.cbegin(), slice.values.cend());
  for (size_t i = 0; i < slice.values.size(); ++i) {
    if (i % 1024 == 0 && stopped())
      return fail(QCoreApplication::translate("WellTrackSource", "已取消读取测井曲线。"));
    if ((slice.quality[i] & 1) != 0)
      values[qsizetype(i)] = qQNaN();
  }
  WellComposite::CurveData curve;
  QString normalizationError;
  if (!normalizeDepthCurve(QVector<double>(slice.depths.cbegin(), slice.depths.cend()), values,
                           depthUnit, &curve, &normalizationError))
    return fail(normalizationError);
  if (stopped())
    return fail(QCoreApplication::translate("WellTrackSource", "已取消读取测井曲线。"));
  float minimum = std::numeric_limits<float>::infinity();
  float maximum = -minimum;
  for (float &value : curve.values) {
    if (std::isfinite(value)) {
      minimum = std::min(minimum, value);
      maximum = std::max(maximum, value);
    } else {
      value = std::numeric_limits<float>::quiet_NaN();
    }
  }
  if (!std::isfinite(minimum))
    return fail(QCoreApplication::translate("WellTrackSource", "所选曲线在当前深度窗没有有效样点。"));
  if (!std::isfinite(curve.depths.front()) || !std::isfinite(curve.depths.back()))
    return fail(QCoreApplication::translate("WellTrackSource", "测井曲线样点数据不完整。"));
  if (minimum == maximum) {
    constexpr double limit = std::numeric_limits<float>::max();
    const double padding = std::max(1.0, std::abs(double(minimum)) * 0.01);
    minimum = float(std::max(-limit, double(minimum) - padding));
    maximum = float(std::min(limit, double(maximum) + padding));
  }

  curve.name = QString::fromUtf8(slice.name);
  curve.unit = QString::fromUtf8(slice.unit);
  curve.minScale = minimum;
  curve.maxScale = maximum;
  curve.modelSourceId = source.ref.sourceId;
  curve.modelLogSetId = logSetId;
  result.curve = std::move(curve);
  return result;
}

} // namespace paleo::welltrack
