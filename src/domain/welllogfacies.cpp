// 层：数据
#include "welllogfacies.h"
#include <QCoreApplication>
#include <QHash>
#include <QSet>
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
bool isGroupName(const FormationInterval &f) {
  return f.unitType == QStringLiteral("组") ||
         f.name.trimmed().endsWith(QStringLiteral("组"));
}
// 井上标注的地层组（组井道 + 地层系统组合道），按顶深自上而下去重。
QStringList wellGroups(const ComprehensiveWellData &data) {
  QVector<QPair<double, QString>> found;
  for (const auto &f : data.formationIntervals)
    if (isGroupName(f) && !f.name.trimmed().isEmpty())
      found.append({f.topDepth, f.name.trimmed()});
  for (const auto &s : data.stratigraphyIntervals)
    if (!s.formation.trimmed().isEmpty())
      found.append({s.topDepth, s.formation.trimmed()});
  std::stable_sort(found.begin(), found.end(),
                   [](const auto &a, const auto &b) { return a.first < b.first; });
  QStringList out;
  for (const auto &entry : found)
    if (!out.contains(entry.second))
      out.append(entry.second);
  return out;
}
qint64 depthKey(const QJsonValue &row) {
  return qRound64(row.toObject().value(QStringLiteral("深度")).toDouble() * 1000);
}
void appendUnique(QStringList &list, const QString &value) {
  if (!value.isEmpty() && !list.contains(value))
    list.append(value);
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
WellFaciesPlan planWellFaciesRuns(const ComprehensiveWellData &data,
                                  const QVector<WellFaciesModel> &models,
                                  const QString &preferredModelId) {
  WellFaciesPlan plan;
  if (models.isEmpty()) {
    plan.reason = tr("没有可调用的测井相模型，请刷新模型");
    return plan;
  }
  const WellFaciesModel *preferred = &models.first();
  for (const auto &m : models)
    if (m.id == preferredModelId)
      preferred = &m;
  // 每个地层组取一个模型；所选模型所在的组排最前，重叠时它优先。
  QStringList order{preferred->formationGroup};
  QHash<QString, const WellFaciesModel *> chosen;
  for (const auto &m : models) {
    if (!order.contains(m.formationGroup))
      order.append(m.formationGroup);
    if (!chosen.contains(m.formationGroup) || m.id == preferredModelId)
      chosen.insert(m.formationGroup, &m);
  }
  const QStringList groups = wellGroups(data);
  QSet<qint64> used;
  for (const QString &group : order) {
    const bool present =
        group.isEmpty() ||
        std::any_of(groups.cbegin(), groups.cend(), [&group](const QString &w) {
          return w.startsWith(group);
        });
    if (!present)
      continue;
    const WellFaciesModel &model = *chosen.value(group);
    const QString label = group.isEmpty() ? tr("整井") : group;
    WellFaciesInput input = prepareWellFaciesInput(data, model);
    if (!input.ready()) {
      plan.skipped << tr("%1（%2）").arg(label, input.reason);
      continue;
    }
    const bool overlap = std::any_of(
        input.rows.cbegin(), input.rows.cend(),
        [&used](const QJsonValue &row) { return used.contains(depthKey(row)); });
    if (overlap) {
      plan.skipped << tr("%1（井段与其他地层组重叠）").arg(label);
      continue;
    }
    for (const auto &row : input.rows)
      used.insert(depthKey(row));
    plan.runs.append({group, model, input});
  }
  if (plan.runs.isEmpty()) {
    // 没有任何组命中模型：沿用整井口径，只用所选模型，原因也取它的。
    plan.skipped.clear();
    WellFaciesInput input = prepareWellFaciesInput(data, *preferred);
    if (input.ready())
      plan.runs.append({preferred->formationGroup, *preferred, input});
    else
      plan.reason = input.reason;
    return plan;
  }
  for (const QString &w : groups) {
    const bool covered =
        std::any_of(models.cbegin(), models.cend(), [&w](const WellFaciesModel &m) {
          return m.formationGroup.isEmpty() || w.startsWith(m.formationGroup);
        });
    if (!covered)
      plan.skipped << tr("%1（无对应模型）").arg(w);
  }
  std::stable_sort(plan.runs.begin(), plan.runs.end(),
                   [](const WellFaciesRun &a, const WellFaciesRun &b) {
                     return depthKey(a.input.rows.first()) <
                            depthKey(b.input.rows.first());
                   });
  return plan;
}
WellFaciesPlan planWellFaciesFormationRun(const ComprehensiveWellData &data,
                                          const QVector<WellFaciesModel> &models,
                                          const QString &horizon) {
  WellFaciesPlan plan;
  const auto current = std::find_if(data.formationIntervals.cbegin(), data.formationIntervals.cend(),
      [&horizon](const auto &f) { return f.name.compare(horizon, Qt::CaseInsensitive) == 0; });
  if (current == data.formationIntervals.cend() ||
      !std::isfinite(current->topDepth) || !std::isfinite(current->bottomDepth) ||
      current->bottomDepth <= current->topDepth) {
    plan.reason = tr("当前地层 %1 缺少有效顶底深").arg(horizon);
    return plan;
  }
  const double top = current->topDepth, base = current->bottomDepth;
  ComprehensiveWellData scoped = data;
  scoped.minDepth = top; scoped.maxDepth = base;
  // 保留每根曲线在当前地层内的原始采样，不向层外扩窗或补模拟数据。
  for (auto &curve : scoped.continuousCurves) {
    const auto original = curve;
    curve.depths.clear(); curve.values.clear();
    for (qsizetype i = 0; i < original.depths.size(); ++i) {
      const double depth = original.depths[i];
      if (!std::isfinite(depth) || depth < top || depth >= base) continue;
      curve.depths << original.depths[i];
      if (i < original.values.size()) curve.values << original.values[i];
      // 保留不完整状态，后续仅在模型确实需要这根曲线时拒绝。
    }
  }
  const auto clip = [top, base](auto &intervals) {
    intervals.erase(std::remove_if(intervals.begin(), intervals.end(), [top, base](const auto &v) {
      return !std::isfinite(v.topDepth) || !std::isfinite(v.bottomDepth) ||
             v.bottomDepth <= top || v.topDepth >= base || v.bottomDepth <= v.topDepth;
    }), intervals.end());
    for (auto &v : intervals) {
      v.topDepth = float(std::max(top, double(v.topDepth)));
      v.bottomDepth = float(std::min(base, double(v.bottomDepth)));
    }
  };
  clip(scoped.formationIntervals);
  clip(scoped.stratigraphyIntervals);
  clip(scoped.lithologyIntervals);
  if (!isGroupName(*current))
    scoped.formationIntervals.erase(std::remove_if(scoped.formationIntervals.begin(), scoped.formationIntervals.end(),
        [&horizon](const auto &f) { return !isGroupName(f) && f.name.compare(horizon, Qt::CaseInsensitive) != 0; }),
        scoped.formationIntervals.end());
  // 模型只从当前范围包含的地层组匹配；未标组的编图段名沿用服务默认模型。
  const QStringList groups = wellGroups(scoped);
  const WellFaciesModel *model = nullptr;
  for (const auto &candidate : models) {
    if (candidate.formationGroup.isEmpty()) continue;
    if (groups.contains(candidate.formationGroup) ||
        horizon.startsWith(candidate.formationGroup)) {
      model = &candidate;
      break;
    }
  }
  if (!model)
    for (const auto &candidate : models)
      if (candidate.formationGroup.isEmpty()) { model = &candidate; break; }
  if (!model && groups.isEmpty() && !models.isEmpty()) model = &models.first();
  if (!model) {
    plan.reason = models.isEmpty() ? tr("没有可调用的测井相模型")
                                  : tr("当前地层 %1 没有对应模型").arg(horizon);
    return plan;
  }
  auto input = prepareWellFaciesInput(scoped, *model);
  if (!input.ready()) {
    plan.reason = input.reason;
    return plan;
  }
  plan.runs.append({horizon, *model, input});
  return plan;
}

WellFaciesResult mergeWellFaciesResults(const QVector<WellFaciesResult> &parts) {
  if (parts.size() < 2)
    return parts.isEmpty() ? WellFaciesResult() : parts.first();
  WellFaciesResult out;
  QStringList groups, jobs, names, versions;
  QJsonArray responses;
  QVector<QPair<float, float>> samples;
  for (const auto &part : parts) {
    out.intervals += part.intervals;
    appendUnique(groups, part.group);
    appendUnique(jobs, part.jobId);
    appendUnique(names, part.modelName);
    appendUnique(versions, part.modelVersion);
    responses.append(QJsonObject{{QStringLiteral("group"), part.group},
                                 {QStringLiteral("response"), part.response}});
    for (qsizetype i = 0; i < part.confidence.depths.size() &&
                          i < part.confidence.values.size();
         ++i)
      samples.append({part.confidence.depths[i], part.confidence.values[i]});
  }
  std::stable_sort(out.intervals.begin(), out.intervals.end(),
                   [](const TextInterval &a, const TextInterval &b) {
                     return a.topDepth < b.topDepth;
                   });
  std::stable_sort(samples.begin(), samples.end(),
                   [](const auto &a, const auto &b) { return a.first < b.first; });
  out.confidence = parts.first().confidence;
  out.confidence.depths.clear();
  out.confidence.values.clear();
  for (const auto &sample : samples) {
    out.confidence.depths.append(sample.first);
    out.confidence.values.append(sample.second);
  }
  const QString separator = QStringLiteral("、");
  out.group = groups.join(separator);
  out.jobId = jobs.join(separator);
  out.modelName = names.join(separator);
  out.modelVersion = versions.join(separator);
  out.response = QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                             {QStringLiteral("groups"), responses}};
  return out;
}
