// 层：功能
// G05 证据变化空间影响分析：直接依赖 / 潜在影响 / 可确定空间影响三级分明。
// 可确定影响用冻结输入版本的像元级决策重放 diff（composeattribution::replayGrid）
// 证明；证明不了的部分如实归 potential。分析快照固定 before/after 版本，
// 报告 JSON 落 artifacts/reports/（与一致性报告同目录约定）。
#include "mappingworkbench.h"
#include "../domain/interpretationevidence.h"
#include "../domain/singlefactorrequest.h"
#include "../qgis/composeattribution.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <functional>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsrasterblock.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsspatialindex.h>
#include <qgsvectorlayer.h>

namespace {
constexpr qint64 kImpactMaxGridCells = 4000000; // 与 composeRaster 上限同口径
constexpr int kImpactMaxProducts = 64;

void fail(QString *error, const QString &text) {
  if (error)
    *error = text;
}

// 从 composed 版本构造冻结输入列表；substitute 提供 versionId→新版本替换
// （影响分析注入变化后的版本）。复用 MappingWorkbench 的私有采样图层缓存。
struct FrozenCompose {
  QString composedVersionId, horizon;
  QVariantList facies;
  QList<double> thresholds;
  QList<paleo::attribution::ResolvedInput> inputs;
  QStringList unresolved;
};

bool resolveFrozenCompose(const DataCatalog *catalog, const QString &projectDir,
                          const QString &composedVersionId,
                          const QHash<QString, QString> &substitute,
                          const std::function<QgsMapLayer *(const QString &)> &opener,
                          FrozenCompose *out) {
  const auto composed = catalog->versionById(composedVersionId);
  out->composedVersionId = composedVersionId;
  out->horizon = composed.extra.value("horizon").toString();
  out->facies = composed.extra.value("facies").toList();
  for (const auto &t : composed.extra.value("thresholds").toList())
    out->thresholds << t.toDouble();
  const auto ordered = composed.extra.value("ordered_inputs").toList();
  if (ordered.isEmpty()) {
    out->unresolved << QStringLiteral("成果缺少 ordered_inputs 冻结记录，无法重放");
    return false;
  }
  for (const auto &entryValue : ordered) {
    const auto entry = entryValue.toMap();
    paleo::attribution::ResolvedInput input;
    input.layerId = entry.value("layer_id").toString();
    input.versionId = entry.value("version_id").toString();
    input.priority = entry.value("priority").toInt();
    const auto effective = substitute.value(input.versionId, input.versionId);
    const auto v = catalog->versionById(effective);
    input.title = v.extra.value("title").toString();
    input.assetId = v.assetId;
    const auto kind = v.extra.value("kind").toString();
    const auto valueSource = v.extra.value("value_source").toString();
    input.continuous =
        input.layerId.startsWith(QLatin1String("factor.")) ||
        v.extra.contains("factor_id") ||
        paleo::singlefactor::isAnalysisFactorRaster(kind, valueSource);
    if (v.id.isEmpty()) {
      input.missing = true;
      input.missingReason = QStringLiteral("版本 %1 不在目录中").arg(effective);
    } else {
      const auto path = DataCatalog::resolvedVersionPath(
                            projectDir, v) +
                        v.extra.value("source_suffix").toString();
      if (path.isEmpty() || !QFileInfo::exists(path.section('|', 0, 0))) {
        input.missing = true;
        input.missingReason = QStringLiteral("版本 %1 的冻结文件缺失").arg(effective);
      } else {
        input.layer = opener ? opener(effective) : nullptr;
        if (!input.layer) {
          input.missing = true;
          input.missingReason = QStringLiteral("版本 %1 无法打开").arg(effective);
        }
      }
    }
    if (input.missing)
      out->unresolved << input.missingReason;
    out->inputs << input;
  }
  return out->unresolved.isEmpty();
}

// 像元级 diff：同一网格上重放两次，返回变化像元与范围。
struct GridDiff {
  bool valid = false;
  qint64 changedCells = 0, totalCells = 0;
  QgsRectangle changedExtent;
  QVector<QgsPointXY> changedCenters; // 有界采样，供图斑归属（下界）
  int samplingStride = 1;             // >1 时 affected_features 是采样下界
};
GridDiff diffGrids(const paleo::attribution::GridReplay &before,
                   const paleo::attribution::GridReplay &after) {
  GridDiff diff;
  // 网格口径必须完全一致（与 compareVersions 同防御）：分辨率相同但范围
  // 漂移时逐下标比较的是空间错位的像元，结论会伪造——退化为潜在影响。
  if (!before.valid || !after.valid || before.cols != after.cols ||
      before.rows != after.rows || before.extent != after.extent)
    return diff;
  diff.valid = true;
  diff.totalCells = qint64(before.cols) * before.rows;
  const double cw = before.extent.width() / before.cols;
  const double ch = before.extent.height() / before.rows;
  const int stride = std::max<int>(1, int(std::sqrt(double(before.cols * before.rows) / 20000.0)));
  bool firstChangedSeeded = false;
  for (int y = 0; y < before.rows; ++y) {
    for (int x = 0; x < before.cols; ++x) {
      const int i = y * before.cols + x;
      if (before.codes[i] == after.codes[i])
        continue;
      ++diff.changedCells;
      // 保证至少采样到一个变化像元：子格点采样可能全部落在格点之间，
      // 否则「有变化像元却无受影响图斑」是误导性结论。
      const bool sampled = (x % stride == 0 && y % stride == 0) || !firstChangedSeeded;
      if (sampled) {
        firstChangedSeeded = true;
        const QgsPointXY center(before.extent.xMinimum() + (x + .5) * cw,
                                before.extent.yMaximum() - (y + .5) * ch);
        diff.changedCenters << center;
        diff.changedExtent.combineExtentWith(
            QgsRectangle(center.x() - cw / 2, center.y() - ch / 2,
                         center.x() + cw / 2, center.y() + ch / 2));
      }
    }
  }
  diff.samplingStride = stride;
  return diff;
}
} // namespace

QVariantMap MappingWorkbench::analyzeImpact(const QVariantMap &change,
                                            QString *error) {
  if (!m_catalog || !m_catalog->isOpen() || m_dir.isEmpty()) {
    fail(error, tr("请先打开工程再分析影响"));
    return {};
  }
  const QString kind = change.value("kind").toString();
  const QString before = change.value("before_version_id").toString();
  const QString after = change.value("after_version_id").toString();
  if (kind.isEmpty()) {
    fail(error, tr("影响分析需要变化类型"));
    return {};
  }
  if (kind == QLatin1String("input_version") &&
      (before.isEmpty() || after.isEmpty())) {
    fail(error, tr("输入版本变化需要 before/after 版本号"));
    return {};
  }
  // 会话期禁用归因图层缓存驱逐：frozen/substituted 双解析 + 后续 replayGrid
  // 全程持有裸 QgsMapLayer*；产品输入 ≥8 时第 9 个版本打开会驱逐仍在用的层。
  const AttributionPin attributionPin(this);
  QVariantMap out{{"kind", kind},
                  {"source_before_version", before},
                  {"source_after_version", after},
                  {"horizon", change.value("horizon")},
                  {"analysis_method",
                   kind == QLatin1String("input_version")
                       ? QStringLiteral("deterministic_cell_replay")
                       : QStringLiteral("lineage_potential")},
                  {"analyzed_at",
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                  {"scope", change.value("detail")},
                  {"unresolved", QVariantList{}},
                  {"direct_products", QVariantList{}},
                  {"potential_products", QVariantList{}},
                  {"determined", QVariantList{}}};
  // 血缘事实：变化的版本（或约束消费方）的下游闭包。
  QStringList seeds;
  if (kind == QLatin1String("input_version")) {
    seeds << before;
  } else if (kind == QLatin1String("constraint_edited")) {
    // 约束库不在 catalog 版本体系内：按回执反查实际消费过这些约束的版本。
    const auto ids = change.value("constraint_ids").toStringList();
    QSet<QString> wanted(ids.cbegin(), ids.cend());
    if (wanted.isEmpty()) {
      // 无具体 id：所有带约束回执的单因素版本都视为潜在受影响。
      for (const auto &v : m_catalog->versions())
        if (v.extra.contains("constraints_consumed"))
          seeds << v.id;
    } else {
      for (const auto &v : m_catalog->versions()) {
        for (const auto &entry : v.extra.value("constraints_consumed").toList()) {
          if (wanted.contains(entry.toMap().value("id").toString())) {
            seeds << v.id;
            break;
          }
        }
      }
    }
    if (seeds.isEmpty()) {
      out.insert("message", tr("没有成果实际消费过这些约束（修改未被使用的证据不产生下游影响）"));
    }
  } else if (!before.isEmpty()) {
    seeds << before;
  } else {
    fail(error, tr("该变化类型需要版本号"));
    return {};
  }
  QStringList downstream;
  for (const auto &seed : seeds)
    for (const auto &v : m_catalog->downstreamClosure(seed))
      downstream << v.id;
  downstream.removeDuplicates();
  if (downstream.size() > kImpactMaxProducts) {
    downstream = downstream.mid(0, kImpactMaxProducts);
    out.insert("truncated", true);
  }
  QVariantList direct, potential;
  for (const auto &vid : downstream) {
    const auto v = m_catalog->versionById(vid);
    if (v.id.isEmpty())
      continue;
    direct << QVariantMap{{"version_id", vid},
                          {"kind", v.extra.value("kind")},
                          {"title", v.extra.value("title")}};
  }
  out.insert("direct_products", direct);
  // 可确定空间影响：只对 composed_facies 且直接消费了变化输入的成果做像元 diff。
  QVariantList determined;
  QVariantList unresolvedOut = out.value("unresolved").toList();
  for (const auto &vid : downstream) {
    const auto v = m_catalog->versionById(vid);
    if (v.extra.value("kind").toString() != QLatin1String("composed_facies"))
      continue;
    if (kind == QLatin1String("input_version") &&
        !v.parentVersionIds.contains(before)) {
      potential << QVariantMap{{"version_id", vid},
                               {"reason", tr("间接下游：无法局部证明空间影响")}};
      continue;
    }
    if (kind != QLatin1String("input_version")) {
      potential << QVariantMap{{"version_id", vid},
                               {"reason", tr("上游单因素需重算后才能确定空间影响")}};
      continue;
    }
    FrozenCompose frozen, substituted;
    const QHash<QString, QString> sub{{before, after}};
    const auto opener = [this](const QString &id) { return attributionLayer(id); };
    const bool baseOk = resolveFrozenCompose(m_catalog, m_dir, vid, {}, opener, &frozen);
    const bool subOk = resolveFrozenCompose(m_catalog, m_dir, vid, sub, opener, &substituted);
    if (!baseOk || !subOk) {
      unresolvedOut << QVariantMap{{"version_id", vid},
                                   {"reason", frozen.unresolved + substituted.unresolved}};
      potential << QVariantMap{{"version_id", vid},
                               {"reason", tr("冻结输入不完整，退化为潜在影响")}};
      continue;
    }
    const auto beforeGrid = paleo::attribution::replayGrid(
        frozen.inputs, frozen.facies, frozen.thresholds, kImpactMaxGridCells);
    const auto afterGrid = paleo::attribution::replayGrid(
        substituted.inputs, substituted.facies, substituted.thresholds,
        kImpactMaxGridCells);
    const auto diff = diffGrids(beforeGrid, afterGrid);
    if (!diff.valid) {
      unresolvedOut << QVariantMap{{"version_id", vid},
                                   {"reason", tr("网格口径不同或超出像元上限，无法像元级对比")}};
      potential << QVariantMap{{"version_id", vid},
                               {"reason", tr("整体可能受影响（无法局部化）")}};
      continue;
    }
    QVariantMap entry{{"product_version", vid},
                      {"algorithm", QStringLiteral("priority-first-valid; "
                                                   "categorical-nearest; "
                                                   "threshold-classification")},
                      {"changed_cells", QVariant::fromValue(diff.changedCells)},
                      {"total_cells", QVariant::fromValue(diff.totalCells)},
                      {"affected_features", QVariantList{}}};
    if (diff.changedCells > 0) {
      entry.insert("changed_extent",
                   QVariantList{diff.changedExtent.xMinimum(),
                                diff.changedExtent.yMinimum(),
                                diff.changedExtent.xMaximum(),
                                diff.changedExtent.yMaximum()});
      // 变化像元 → 相面图斑归属（有界；相面子版本 = 直接下游 facies_polygons）。
      QVariantList affected;
      for (const auto &child : downstream) {
        const auto cv = m_catalog->versionById(child);
        if (cv.extra.value("kind").toString() != QLatin1String("facies_polygons") ||
            !cv.parentVersionIds.contains(vid))
          continue;
        auto *layer = qobject_cast<QgsVectorLayer *>(
            attributionLayer(child));
        if (!layer) {
          unresolvedOut << QVariantMap{{"version_id", child},
                                       {"reason", tr("相面版本无法打开，图斑归属不可用")}};
          continue;
        }
        QgsSpatialIndex index;
        QHash<QgsFeatureId, QString> uids;
        {
          QgsFeature f;
          auto it = layer->getFeatures();
          while (it.nextFeature(f)) {
            if (f.geometry().isEmpty())
              continue;
            QgsFeature indexed(f.id());
            indexed.setGeometry(f.geometry());
            index.addFeature(indexed);
            const int field = f.fields().indexOf("paleo_feature_uid");
            uids.insert(f.id(), field < 0 ? QString() : f.attribute(field).toString());
          }
        }
        QSet<QString> seen;
        for (const auto &center : diff.changedCenters) {
          const auto candidates = index.intersects(
              QgsRectangle(center.x(), center.y(), center.x(), center.y()));
          for (const auto id : candidates) {
            QgsFeature f = layer->getFeature(id);
            if (!f.hasGeometry() ||
                !f.geometry().contains(center.x(), center.y()))
              continue;
            const auto uid = uids.value(id);
            if (uid.isEmpty() || seen.contains(uid))
              continue;
            seen.insert(uid);
            affected << QVariantMap{{"feature_uid", uid},
                                    {"layer_id", cv.extra.value("layer_id")},
                                    {"product_version", child}};
          }
        }
      }
      entry.insert("affected_features", affected);
      if (diff.samplingStride > 1)
        entry.insert("affected_features_note",
                     tr("受影响图斑为 %1 步长采样下界，非全量清单")
                         .arg(diff.samplingStride));
    }
    determined << entry;
  }
  out.insert("potential_products", potential);
  out.insert("determined", determined);
  out.insert("unresolved", unresolvedOut);
  out.insert("status", determined.isEmpty() && potential.isEmpty()
                           ? QStringLiteral("no_impact")
                           : QStringLiteral("ok"));
  // 历史快照：固定版本对的完整报告落盘（不建立在当前活动图层上）。
  if (QDir().mkpath(m_dir + "/artifacts/reports")) {
    QJsonDocument doc(QJsonObject::fromVariantMap(out));
    const auto path = m_dir + "/artifacts/reports/impact-" +
                      QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) +
                      ".json";
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      file.write(doc.toJson(QJsonDocument::Indented));
      out.insert("report_path", path);
    }
  }
  return out;
}

QVariantMap MappingWorkbench::recomputePlan(const QVariantMap &impactResult,
                                            QString *error) {
  if (!m_catalog || !m_catalog->isOpen()) {
    fail(error, tr("请先打开工程再生成重算计划"));
    return {};
  }
  // 保护口径逐 uid 打开旧成果 GPKG（attributionLayer）——会话期禁用驱逐。
  const AttributionPin attributionPin(this);
  QVariantMap plan{{"created_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                   {"impact_report", impactResult.value("report_path")},
                   {"entries", QVariantList{}}};
  QVariantList entries;
  const auto determined = impactResult.value("determined").toList();
  const auto potential = impactResult.value("potential_products").toList();
  QSet<QString> planned;
  const auto currentOf = [this](const QString &frozen) {
    const auto fv = m_catalog->versionById(frozen);
    return fv.id.isEmpty() ? QString()
                           : m_catalog->currentVersion(fv.assetId).id;
  };
  for (const auto &value : determined) {
    const auto entry = value.toMap();
    const auto vid = entry.value("product_version").toString();
    if (vid.isEmpty() || planned.contains(vid))
      continue;
    const auto v = m_catalog->versionById(vid);
    if (v.id.isEmpty() ||
        v.extra.value("kind").toString() != QLatin1String("composed_facies"))
      continue;
    QVariantList inputs;
    for (const auto &inputValue : v.extra.value("ordered_inputs").toList()) {
      const auto input = inputValue.toMap();
      const auto current = currentOf(input.value("version_id").toString());
      inputs << QVariantMap{{"layer_id", input.value("layer_id")},
                            {"frozen_version", input.value("version_id")},
                            {"current_version", current},
                            // 重算入口：ordered_inputs 里的 layer_id 可能是
                            // 冻结当时的 product id（永远指向旧版本）；重算必须
                            // 用当前版本的 product 声明 id 才能拾取新输入。
                            {"rerun_layer_id",
                             current.isEmpty() ? input.value("layer_id")
                                               : QStringLiteral("product.%1").arg(current)}};
    }
    // 保护口径：受影响图斑中已确认/待复核的人工解释数量（读取旧成果 GPKG，
    // 有界精确查询）。确认记录保留在旧版本；新版本不自动迁移。
    int confirmed = 0, review = 0;
    // 直接相面子版本只取一次（闭包内孙代不进保护计数口径）。
    QString polygonChild;
    for (const auto &cv : m_catalog->downstreamClosure(vid)) {
      if (cv.extra.value("kind").toString() == QLatin1String("facies_polygons") &&
          cv.parentVersionIds.contains(vid)) {
        polygonChild = cv.id;
        break;
      }
    }
    for (const auto &featureValue : entry.value("affected_features").toList()) {
      const auto uid = featureValue.toMap().value("feature_uid").toString();
      if (uid.isEmpty() || polygonChild.isEmpty())
        continue;
      {
        const auto &child = polygonChild;
        const auto cv = m_catalog->versionById(child);
        if (cv.extra.value("kind").toString() != QLatin1String("facies_polygons"))
          continue;
        auto *layer = qobject_cast<QgsVectorLayer *>(attributionLayer(child));
        if (!layer)
          continue;
        QgsFeatureRequest request;
        request.setFilterExpression(
            QStringLiteral("paleo_feature_uid = '%1'").arg(uid));
        request.setLimit(2);
        QgsFeature f;
        auto it = layer->getFeatures(request);
        while (it.nextFeature(f)) {
          QVariantMap interpretation;
          QString parseError;
          if (paleo::evidence::decodeInterpretation(
                  f.attribute("paleo_interpretation").toString(), &interpretation,
                  &parseError)) {
            const auto status = interpretation.value("status").toString();
            if (status == QLatin1String("Confirmed"))
              ++confirmed;
            else if (status == QLatin1String("NeedsReview") ||
                     status == QLatin1String("Disputed"))
              ++review;
          }
        }
        // 逐 uid 精确查询（N1 回归修复：此处曾误 break 出外层图斑循环，
        // 只统计首个受影响图斑）。
      }
    }
    entries << QVariantMap{{"target_version", vid},
                           {"horizon", v.extra.value("horizon")},
                           {"mode", entry.value("changed_cells").toLongLong() > 0
                                        ? QStringLiteral("local_rerun")
                                        : QStringLiteral("whole_rerun")},
                           {"inputs", inputs},
                           {"parameters", v.extra.value("algorithm_parameters")},
                           {"protection",
                            QVariantMap{{"confirmed_features", confirmed},
                                        {"needs_review_features", review},
                                        {"policy", tr("已确认解释保留在旧版本；"
                                                      "新版本需人工复核后迁移")}}}};
    planned.insert(vid);
  }
  for (const auto &value : potential) {
    const auto vid = value.toMap().value("version_id").toString();
    if (vid.isEmpty() || planned.contains(vid))
      continue;
    const auto v = m_catalog->versionById(vid);
    if (v.id.isEmpty() ||
        v.extra.value("kind").toString() != QLatin1String("composed_facies"))
      continue;
    entries << QVariantMap{{"target_version", vid},
                           {"horizon", v.extra.value("horizon")},
                           {"mode", QStringLiteral("whole_rerun")},
                           {"inputs", v.extra.value("ordered_inputs")},
                           {"parameters", v.extra.value("algorithm_parameters")},
                           {"protection", QVariantMap{
                                              {"policy", tr("无法局部化：整体重算路径")}}}};
    planned.insert(vid);
  }
  plan.insert("entries", entries);
  plan.insert("status",
              entries.isEmpty() ? QStringLiteral("empty") : QStringLiteral("ok"));
  return plan;
}

QVariantMap MappingWorkbench::executeRecompute(const QVariantMap &plan,
                                               QString *error) {
  if (!m_catalog || !m_catalog->isOpen()) {
    fail(error, tr("请先打开工程再执行重算"));
    return {};
  }
  QVariantMap result{
      {"started_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
      {"results", QVariantList{}}};
  // 同 analyzeImpact：重算逐条解析持有归因图层裸指针，会话期禁用驱逐。
  const AttributionPin attributionPin(this);
  QVariantList results;
  bool sawTaskStart = false;
  for (const auto &value : plan.value("entries").toList()) {
    const auto entry = value.toMap();
    const auto vid = entry.value("target_version").toString();
    const auto horizon = entry.value("horizon").toString();
    QStringList layerIds;
    const auto params = entry.value("parameters").toMap();
    for (const auto &inputValue : entry.value("inputs").toList()) {
      const auto input = inputValue.toMap();
      // rerun_layer_id（当前版本的 product 声明）优先；旧计划/whole_rerun
      // 条目回退到原 layer_id。
      const auto layerId = input.contains(QStringLiteral("rerun_layer_id"))
                               ? input.value("rerun_layer_id").toString()
                               : input.value("layer_id").toString();
      if (!layerId.isEmpty())
        layerIds << layerId;
    }
    if (layerIds.isEmpty()) {
      results << QVariantMap{{"target_version", vid},
                             {"ok", false},
                             {"error", tr("计划条目缺少输入图层")}};
      continue;
    }
    QString startError;
    // 复用现有综合编图入口：同参数、当前输入版本 → 生成新版本，
    // 绝不覆盖历史版本；任务模式下异步，完成后走既有 productReady 链。
    // 任务模式下 compose runner 单飞：首个条目启动后其余条目如实报
    // 「请等待完成」，不伪装成已排队。
    if (m_taskSvc && sawTaskStart) {
      results << QVariantMap{{"target_version", vid},
                             {"ok", false},
                             {"async", true},
                             {"error", tr("任务模式下逐条重算：请等待上一条完成后再次执行")}};
      continue;
    }
    const bool started = startCompose(horizon, layerIds, params, &startError);
    if (started && m_taskSvc)
      sawTaskStart = true;
    results << QVariantMap{{"target_version", vid},
                           {"ok", started},
                           {"async", bool(m_taskSvc)},
                           {"error", started ? QString() : startError}};
  }
  result.insert("results", results);
  return result;
}

QVariantMap MappingWorkbench::compareVersions(const QString &oldVersionId,
                                              const QString &newVersionId,
                                              QString *error) {
  if (!m_catalog) {
    fail(error, tr("目录未绑定"));
    return {};
  }
  // before/after 两个栅格图层同时持有期间不得驱逐归因缓存（R2-01：F1 的
  // 覆盖缺口——第二个 attributionLayer 打开第 9 个不同版本时可能驱逐第一个）。
  const AttributionPin attributionPin(this);
  auto *before = qobject_cast<QgsRasterLayer *>(attributionLayer(oldVersionId));
  auto *after = qobject_cast<QgsRasterLayer *>(attributionLayer(newVersionId));
  if (!before || !after) {
    fail(error, tr("对比需要两个可读的栅格成果版本"));
    return {};
  }
  if (before->width() != after->width() || before->height() != after->height() ||
      before->extent() != after->extent()) {
    fail(error, tr("两版本网格口径不同（范围或分辨率不一致），无法像元级对比"));
    return {};
  }
  const int cols = before->width(), rows = before->height();
  if (qint64(cols) * rows > kImpactMaxGridCells) {
    fail(error, tr("网格超出对比上限"));
    return {};
  }
  std::unique_ptr<QgsRasterBlock> beforeBlock(
      before->dataProvider()->block(1, before->extent(), cols, rows));
  std::unique_ptr<QgsRasterBlock> afterBlock(
      after->dataProvider()->block(1, after->extent(), cols, rows));
  if (!beforeBlock || !beforeBlock->isValid() || !afterBlock || !afterBlock->isValid()) {
    fail(error, tr("无法读取成果栅格"));
    return {};
  }
  qint64 changed = 0, unchanged = 0, total = 0;
  QHash<QString, qint64> matrix; // "from>to" → cells
  const qint64 blockCells =
      qint64(beforeBlock->width()) * beforeBlock->height();
  for (int i = 0; i < blockCells; ++i) {
    const bool nodataBefore = beforeBlock->isNoData(i);
    const bool nodataAfter = afterBlock->isNoData(i);
    if (nodataBefore && nodataAfter)
      continue;
    ++total;
    const int codeBefore =
        nodataBefore ? -9999 : int(std::round(beforeBlock->value(i)));
    const int codeAfter = nodataAfter ? -9999 : int(std::round(afterBlock->value(i)));
    if (codeBefore == codeAfter) {
      ++unchanged;
      continue;
    }
    ++changed;
    matrix[QStringLiteral("%1>%2").arg(codeBefore).arg(codeAfter)]++;
  }
  QVariantList codeMatrix;
  for (auto it = matrix.cbegin(); it != matrix.cend(); ++it)
    codeMatrix << QVariantMap{{"transition", it.key()},
                              {"cells", QVariant::fromValue(it.value())}};
  QVariantMap out{{"method", QStringLiteral("cell_diff")},
                  {"old_version", oldVersionId},
                  {"new_version", newVersionId},
                  {"cells_changed", QVariant::fromValue(changed)},
                  {"cells_unchanged", QVariant::fromValue(unchanged)},
                  {"total_cells", QVariant::fromValue(total)},
                  {"code_matrix", codeMatrix}};
  // 相面统计：两版本各自相面子版本的图斑数。图斑身份在重算后重新生成，
  // 不做跨版本一一匹配——空间对应以像元转移矩阵为准。
  auto polygonCount = [this](const QString &versionId) -> int {
    for (const auto &cv : m_catalog->downstreamClosure(versionId)) {
      if (cv.extra.value("kind").toString() != QLatin1String("facies_polygons"))
        continue;
      auto *layer = qobject_cast<QgsVectorLayer *>(attributionLayer(cv.id));
      return layer ? int(layer->featureCount()) : -1;
    }
    return -1;
  };
  out.insert("polygon_counts",
             QVariantMap{{"before", polygonCount(oldVersionId)},
                         {"after", polygonCount(newVersionId)}});
  out.insert("note", tr("重算后图斑身份重新生成，不做跨版本图斑一一匹配；"
                        "空间对应以像元级转移矩阵为准"));
  return out;
}

QVariantList MappingWorkbench::evidenceReverseQuery(const QString &name,
                                                     const QString &argument,
                                                     QString *error) {
  QVariantList rows;
  if (!m_catalog || m_dir.isEmpty()) {
    fail(error, tr("请先打开工程再检索证据"));
    return rows;
  }
  // 反查本体不取归因图层；pin 是防御性保留（索引同步/行定位链后续若引入
  // 图层读取即自动受保护），与 compareVersions 的双图层 pin 同族。
  const AttributionPin attributionPin(this);
  if (!m_evidenceIndex)
    m_evidenceIndex = std::make_unique<paleo::evidenceindex::Index>();
  QString openError;
  if (!m_evidenceIndex->isOpen() && !m_evidenceIndex->open(m_dir, &openError)) {
    fail(error, tr("证据反向索引不可用：%1").arg(openError));
    return rows;
  }
  // 增量同步：按 (version_id, sha256) 跳过未变化版本——原始证据仍以成果
  // GPKG 属性为唯一事实源，这里只是可重建的检索结构。
  QString syncError;
  m_evidenceIndex->synchronize(m_catalog, m_dir, {}, &syncError);
  QVector<paleo::evidenceindex::Entry> entries;
  if (name == QLatin1String("pending_review"))
    entries = m_evidenceIndex->pendingReview(argument, 200);
  else if (name == QLatin1String("contradicted"))
    entries = m_evidenceIndex->contradicted(argument, 200);
  else if (name == QLatin1String("references_source"))
    entries = m_evidenceIndex->featuresReferencingSource(argument, 200);
  else if (name == QLatin1String("stale_sources"))
    entries = m_evidenceIndex->staleSources(argument, 200);
  else if (name == QLatin1String("search"))
    entries = m_evidenceIndex->search(argument, 200);
  else {
    fail(error, tr("未知的检索类型：%1").arg(name));
    return rows;
  }
  for (const auto &entry : entries) {
    rows << QVariantMap{{"layer_id", entry.layerId},
                       {"feature_uid", entry.featureUid},
                       {"version_id", entry.versionId},
                       {"horizon", entry.horizon},
                       {"evidence_id", entry.evidenceId},
                       {"evidence_type", entry.evidenceType},
                       {"role", entry.role},
                       {"text", entry.text},
                       {"review_state", entry.reviewState},
                       {"source_layer", entry.layerId},
                       {"source_version", entry.versionId},
                       {"source_feature_uid", entry.featureUid},
                       {"name", entry.text.isEmpty()
                                    ? tr("图斑 %1…").arg(entry.featureUid.left(12))
                                    : entry.text},
                       {"origin", QStringLiteral("reverse_index")}};
  }
  return rows;
}
