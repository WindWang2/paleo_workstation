// 层：数据
#include "welllogfacies.h"
#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <limits>
using namespace WellComposite;
namespace {
QString tr(const char *s) {
  return QCoreApplication::translate("WellFacies", s);
}
template <class T> const T *at(const QVector<T> &items, double depth) {
  const T *best = nullptr;
  for (const auto &i : items)
    if (depth >= i.topDepth && depth < i.bottomDepth &&
        (!best ||
         i.bottomDepth - i.topDepth < best->bottomDepth - best->topDepth))
      best = &i;
  return best;
}
bool curveValue(const CurveData &c, double d, double *value) {
  const auto it = std::lower_bound(c.depths.cbegin(), c.depths.cend(), d);
  if (it == c.depths.cend())
    return false;
  const auto n = it - c.depths.cbegin();
  if (*it == d)
    *value = c.values[n];
  else {
    if (!n)
      return false;
    const double a = c.values[n - 1], b = c.values[n];
    if (!std::isfinite(a) || !std::isfinite(b) || a == -9999 || b == -9999 ||
        a == -999.25 || b == -999.25)
      return false;
    *value =
        a + (b - a) * (d - c.depths[n - 1]) / (c.depths[n] - c.depths[n - 1]);
  }
  return std::isfinite(*value) && *value != -9999 && *value != -999.25;
}
} // namespace
WellFaciesInput prepareWellFaciesInput(const ComprehensiveWellData &data,
                                       const WellFaciesModel &model) {
  WellFaciesInput out;
  const auto fail = [&out](const QString &s) {
    out.rows = {};
    out.reason = s;
    return out;
  };
  if (model.id.isEmpty() || model.curves.isEmpty() || model.window < 1)
    return fail(tr("模型输入要求无效"));
  QVector<const CurveData *> curves;
  for (const auto &name : model.curves) {
    const CurveData *found = nullptr;
    for (const auto &c : data.continuousCurves) {
      const QString curve = c.name.trimmed();
      const bool exact = curve.compare(name, Qt::CaseInsensitive) == 0;
      const bool withUnit =
          curve.startsWith(name, Qt::CaseInsensitive) && curve.size() > name.size()
          && !curve.at(name.size()).isLetterOrNumber();
      if (exact || withUnit) {
        found = &c;
        break;
      }
    }
    if (!found)
      return fail(tr("缺少模型所需曲线：%1").arg(name));
    if (found->depths.isEmpty() || found->depths.size() != found->values.size())
      return fail(tr("曲线 %1 的深度与数值不完整").arg(name));
    double prev = -std::numeric_limits<double>::infinity();
    for (float d : found->depths) {
      if (!std::isfinite(d) || d <= prev)
        return fail(tr("曲线 %1 的深度必须连续递增").arg(name));
      prev = d;
    }
    curves.append(found);
  }
  if (data.formationIntervals.isEmpty())
    return fail(tr("缺少段井道（地层单位）"));
  if (data.lithologyIntervals.isEmpty())
    return fail(tr("缺少岩性井道"));
  QVector<FormationInterval> segments;
  for (const auto &f : data.formationIntervals)
    if (f.unitType == QStringLiteral("段") ||
        f.name.trimmed().endsWith(QStringLiteral("段")))
      segments.append(f);
  if (segments.isEmpty())
    return fail(tr("缺少段井道（只有组分层，不能代替段）"));
  bool namedGroup = false;
  bool groupHit = model.formationGroup.isEmpty();
  for (const auto &f : data.formationIntervals) {
    if (f.unitType == QStringLiteral("组") || f.name.trimmed().endsWith(QStringLiteral("组")))
      namedGroup = true;
    if (!model.formationGroup.isEmpty() &&
        (f.name == model.formationGroup ||
         f.name.startsWith(model.formationGroup + QStringLiteral("上段")) ||
         f.name.startsWith(model.formationGroup + QStringLiteral("下段")) ||
         f.name.startsWith(model.formationGroup)))
      groupHit = true;
  }
  for (const auto &f : data.stratigraphyIntervals)
    if (f.formation == model.formationGroup)
      groupHit = true;
  // 井道图只有 C1、D61 这类段名、没有「组」时，段名就是当前图的分层，
  // 不再因为对不上模型的组名而整井拒绝。已经标了别的组则仍然拒绝。
  const bool restrictToGroup = !model.formationGroup.isEmpty() && (groupHit || namedGroup);
  const auto &depths = curves.first()->depths;
  double step = std::numeric_limits<double>::infinity();
  for (qsizetype i = 1; i < depths.size(); ++i)
    step = std::min(step, double(depths[i] - depths[i - 1]));
  double previous = -std::numeric_limits<double>::infinity();
  for (double depth : depths) {
    bool inGroup = !restrictToGroup;
    if (restrictToGroup) {
      for (const auto &f : data.formationIntervals)
        if (depth >= f.topDepth && depth < f.bottomDepth &&
            (f.name == model.formationGroup ||
             f.name.startsWith(model.formationGroup + QStringLiteral("上段")) ||
             f.name.startsWith(model.formationGroup + QStringLiteral("下段"))))
          inGroup = true;
      for (const auto &f : data.stratigraphyIntervals)
        if (depth >= f.topDepth && depth < f.bottomDepth &&
            f.formation == model.formationGroup)
          inGroup = true;
      for (const auto &f : segments)
        if (depth >= f.topDepth && depth < f.bottomDepth &&
            (f.name.startsWith(model.formationGroup) ||
             (model.formationGroup.endsWith(QStringLiteral("组")) &&
              f.name.startsWith(model.formationGroup.chopped(1)))))
          inGroup = true;
    }
    if (!inGroup)
      continue;
    const auto *segment = at(segments, depth);
    const auto *lith = at(data.lithologyIntervals, depth);
    // 曲线比岩性道长时，岩性范围外的点跳过，不让井口那一段废掉整井。
    // 跳过的点不计入缺口；一旦进入岩性和段都覆盖的连续段，中间再断就拒绝。
    if (!segment || segment->name.trimmed().isEmpty() || !lith ||
        lith->lithoName.trimmed().isEmpty())
      continue;
    if (std::isfinite(previous) && depth - previous > step * 1.5 + 0.001)
      return fail(tr("目标井段存在深度缺口，不能作为连续井段预测"));
    if (segment->name.size() > 128 || lith->lithoName.size() > 128)
      return fail(tr("段或岩性文本超过 128 字符"));
    QJsonObject row{{QStringLiteral("深度"), depth},
                    {QStringLiteral("段"), segment->name.trimmed()},
                    {QStringLiteral("岩性"), lith->lithoName.trimmed()}};
    for (qsizetype n = 0; n < curves.size(); ++n) {
      double value;
      if (!curveValue(*curves[n], depth, &value))
        return fail(
            tr("深度 %1 m 的 %2 缺少有效数值").arg(depth).arg(model.curves[n]));
      row.insert(model.curves[n], value);
    }
    out.rows.append(row);
    previous = depth;
  }
  if (out.rows.isEmpty())
    return fail(tr("没有模型对应的 %1 井段").arg(model.formationGroup));
  if (out.rows.size() < model.window)
    return fail(tr("有效深度点 %1 个，模型至少需要 %2 个")
                    .arg(out.rows.size())
                    .arg(model.window));
  return out;
}
bool parseWellFaciesResult(const QJsonObject &json, const QString &wellName,
                           const WellFaciesInput &input,
                           WellFaciesResult *result, QString *error) {
  const auto fail = [error](const QString &s) {
    if (error)
      *error = s;
    return false;
  };
  const auto predictions = json.value("predictions").toArray();
  if (!input.ready() || json.value("status").toString() != "completed")
    return fail(tr("预测结果未完成或输入井段为空"));
  if (predictions.size() != input.rows.size())
    return fail(tr("预测结果点数与提交的井段不一致"));
  WellFaciesResult out;
  out.jobId = json.value("jobId").toString();
  out.modelName = json.value("model").toObject().value("name").toString();
  out.modelVersion = json.value("model").toObject().value("version").toString();
  out.response = json;
  out.confidence.name = tr("置信度");
  out.confidence.unit = QStringLiteral("%");
  out.confidence.minScale = 0;
  out.confidence.maxScale = 100;
  for (qsizetype n = 0; n < predictions.size(); ++n) {
    const auto p = predictions[n].toObject();
    const double depth =
        p.value("depth").toDouble(std::numeric_limits<double>::quiet_NaN());
    const double expected =
        input.rows[n].toObject().value(QStringLiteral("深度")).toDouble();
    const double confidence = p.value("confidence").toDouble(-1);
    const QString label = p.value("label").toString().trimmed();
    if (p.value("wellName").toString() != wellName || !std::isfinite(depth) ||
        std::abs(depth - expected) > 0.001 || label.isEmpty() ||
        !std::isfinite(confidence) || confidence < 0 || confidence > 1)
      return fail(tr("预测结果包含错误井名、深度、相类别或置信度"));
    const double top = n ? (expected + input.rows[n - 1]
                                           .toObject()
                                           .value(QStringLiteral("深度"))
                                           .toDouble()) /
                               2
                         : expected;
    const double bottom = n + 1 < input.rows.size()
                              ? (expected + input.rows[n + 1]
                                                .toObject()
                                                .value(QStringLiteral("深度"))
                                                .toDouble()) /
                                    2
                              : expected;
    if (!out.intervals.isEmpty() && out.intervals.last().text == label)
      out.intervals.last().bottomDepth = bottom;
    else {
      TextInterval i;
      i.topDepth = top;
      i.bottomDepth = bottom;
      i.text = label;
      out.intervals.append(i);
    }
    out.confidence.depths.append(depth);
    out.confidence.values.append(confidence * 100);
  }
  *result = out;
  return true;
}
