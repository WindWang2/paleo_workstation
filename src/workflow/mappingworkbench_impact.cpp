// 层：功能
// G05 证据变化空间影响分析：直接依赖 / 潜在影响 / 可确定空间影响三级分明。
// 可确定影响用冻结输入版本的像元级决策重放 diff（composeattribution::replayGrid）
// 证明；证明不了的部分如实归 potential。分析快照固定 before/after 版本，
// 报告 JSON 落 artifacts/reports/（与一致性报告同目录约定）。
#include "mappingworkbench.h"
#include "../domain/interpretationevidence.h"
#include "../domain/singlefactorrequest.h"
#include "../qgis/composeattribution.h"
#include "../services/jobrunner.h"
#include "../services/paleotaskservice.h"
#include "../io/atomicwrite.h"
#include <QDebug>
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
#include <qgsexpression.h>
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

// commit 期输入版本核对：live 目录里的版本与快照完全一致才发布。版本本应
// 不可变，但 updateVersionExtra 可就地改 extra（revision 同步前进）；逐字段
// 比对把「分析期间输入被动过」显式拦下，不靠 revision 单一信号。
bool versionMatches(const DataCatalog *cat, const CatalogVersion &snapshot) {
  const auto live = cat->versionById(snapshot.id);
  return live.id == snapshot.id && live.assetId == snapshot.assetId &&
         live.versionNumber == snapshot.versionNumber &&
         live.stage == snapshot.stage && live.managed == snapshot.managed &&
         live.path == snapshot.path && live.fileName == snapshot.fileName &&
         live.sha256 == snapshot.sha256 &&
         live.parentVersionIds == snapshot.parentVersionIds &&
         live.extra == snapshot.extra;
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
  ImpactJob job;
  if (!prepareImpactJob(change, QString(), job, error))
    return {};
  // 同步路径（测试壳/无任务服务）：图层经 attributionLayer 的 GUI 线程 LRU
  // 打开——与历史行为一致（缓存跨操作保活）。
  const VersionLayerOpener ownerOpener = [this](const CatalogVersion &v) {
    return attributionLayer(v.id);
  };
  if (!computeImpactWith(job, {}, {}, ownerOpener)) {
    fail(error, job.error);
    return {};
  }
  writeImpactReport(job);
  return job.result;
}

bool MappingWorkbench::prepareImpactJob(const QVariantMap &change,
                                        const QString &layerId,
                                        ImpactJob &job, QString *error) {
  job = ImpactJob{};
  job.dir = m_dir;
  job.change = change;
  job.layerId = layerId;
  job.catalog = m_catalog;
  if (!m_catalog || !m_catalog->isOpen() || m_dir.isEmpty()) {
    fail(error, tr("请先打开工程再分析影响"));
    return false;
  }
  job.revision = m_catalog->catalogRevision();
  const QString kind = change.value("kind").toString();
  const QString before = change.value("before_version_id").toString();
  const QString after = change.value("after_version_id").toString();
  job.kind = kind;
  job.before = before;
  job.after = after;
  job.beforeSnapshot = m_catalog->versionById(before);
  job.afterSnapshot = m_catalog->versionById(after);
  if (kind.isEmpty()) {
    fail(error, tr("影响分析需要变化类型"));
    return false;
  }
  if (kind == QLatin1String("input_version") &&
      (before.isEmpty() || after.isEmpty())) {
    fail(error, tr("输入版本变化需要 before/after 版本号"));
    return false;
  }
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
    return false;
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
  QVariantList direct;
  for (const auto &vid : downstream) {
    const auto v = m_catalog->versionById(vid);
    if (v.id.isEmpty())
      continue;
    direct << QVariantMap{{"version_id", vid},
                          {"kind", v.extra.value("kind")},
                          {"title", v.extra.value("title")}};
    // 下游成果快照：元数据 + determined 候选的冻结输入/替换输入版本 +
    // 图斑归属子层。compute 不再回读目录（跨线程读被拒且属 UB）。
    ImpactJob::Product product;
    product.versionId = vid;
    product.extra = v.extra;
    if (v.extra.value("kind").toString() == QLatin1String("composed_facies")) {
      if (kind == QLatin1String("input_version") &&
          !v.parentVersionIds.contains(before))
        product.potentialReason = tr("间接下游：无法局部证明空间影响");
      else if (kind != QLatin1String("input_version"))
        product.potentialReason = tr("上游单因素需重算后才能确定空间影响");
      else {
        product.candidate = true;
        product.facies = v.extra.value("facies").toList();
        for (const auto &t : v.extra.value("thresholds").toList())
          product.thresholds << t.toDouble();
        product.grid = paleo::attribution::frozenGridFromExtra(v.extra);
        // base = 冻结原版本；sub = before→after 替换后的重算口径。两张表
        // 必须分开：同一替换表会让两侧重放完全相同，diff 恒为零。
        const QHash<QString, QString> identity;
        const QHash<QString, QString> sub{{before, after}};
        const auto fill = [this](const QVariantList &ordered,
                                 const QHash<QString, QString> &substitute,
                                 QList<ImpactJob::Input> *inputs) {
          for (const auto &entryValue : ordered) {
            const auto entry = entryValue.toMap();
            ImpactJob::Input input;
            input.layerId = entry.value("layer_id").toString();
            input.versionId = entry.value("version_id").toString();
            input.priority = entry.value("priority").toInt();
            input.effectiveId =
                substitute.value(input.versionId, input.versionId);
            input.effective = m_catalog->versionById(input.effectiveId);
            *inputs << input;
          }
        };
        const auto ordered = v.extra.value("ordered_inputs").toList();
        fill(ordered, identity, &product.baseInputs);
        fill(ordered, sub, &product.subInputs);
        product.ordered = !ordered.isEmpty();
        if (!product.ordered)
          product.frozenUnresolved << QVariantMap{
              {"reason", tr("成果缺少 ordered_inputs 冻结记录，无法重放")}};
        // 变化像元 → 相面图斑归属（有界）：下游闭包内全部相面与人工编辑
        // 成果（解释/复核记录保存在 edited_facies 后代版本，X7）。
        for (const auto &cv : m_catalog->downstreamClosure(vid)) {
          const auto childKind = cv.extra.value("kind").toString();
          if (childKind == QLatin1String("facies_polygons") ||
              childKind == QLatin1String("edited_facies"))
            product.children << cv;
        }
      }
    }
    job.products << product;
  }
  out.insert("direct_products", direct);
  // 元数据级输出（骨架/直接依赖/截断/无消费提示）就此定稿；determined 与
  // compute 期分类的 potential/unresolved 由 computeImpactWith 收口。
  job.result = out;
  return true;
}

bool MappingWorkbench::computeImpactWith(
    ImpactJob &job, const std::function<bool()> &cancel,
    const std::function<void(double, const QString &)> &progress,
    const VersionLayerOpener &opener) {
  QVariantMap out = job.result;
  QVariantList potential = out.value("potential_products").toList();
  QVariantList determined;
  QVariantList unresolvedOut = out.value("unresolved").toList();
  // 快照输入规格 → 重放输入（口径 = resolveFrozenCompose：目录删除 →
  // 路径存在性 → 打开，missing 分类与原因逐字一致；图层经 opener 拿，
  // sync 路径是 GUI 线程 LRU，worker 路径是本作业独立打开的层）。
  const auto resolveInputs = [&job, &opener](
                                 const QList<ImpactJob::Input> &specs,
                                 QVariantList *unresolved) {
    QList<paleo::attribution::ResolvedInput> inputs;
    for (const auto &spec : specs) {
      paleo::attribution::ResolvedInput input;
      input.layerId = spec.layerId;
      input.versionId = spec.versionId;
      input.priority = spec.priority;
      const auto &v = spec.effective;
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
        input.catalogMissing = true;
        input.missingReason =
            tr("输入版本 %1 已从目录删除").arg(spec.effectiveId);
      } else {
        const auto path = DataCatalog::resolvedVersionPath(job.dir, v) +
                          v.extra.value("source_suffix").toString();
        const auto file = path.section('|', 0, 0);
        // 声明口径的栅格位：文件缺失时也已知（网格重建不依赖能否打开）。
        input.raster =
            v.extra.value("layer_type").toString() ==
                QLatin1String("raster") ||
            file.endsWith(QLatin1String(".tif"), Qt::CaseInsensitive) ||
            file.endsWith(QLatin1String(".tiff"), Qt::CaseInsensitive);
        if (path.isEmpty() || !QFileInfo::exists(file)) {
          input.missing = true;
          input.missingReason =
              tr("冻结输入文件缺失（版本 %1）").arg(spec.effectiveId);
        } else {
          input.hold = opener(v);
          input.layer = input.hold.get();
          if (!input.layer) {
            input.missing = true;
            input.missingReason = tr("冻结输入无法按保存时状态打开（版本 %1）")
                                      .arg(spec.effectiveId);
          }
        }
      }
      if (input.missing)
        *unresolved << QVariantMap{{"layer_id", input.layerId},
                                   {"version_id", input.versionId},
                                   {"reason", input.missingReason}};
      inputs << input;
    }
    return inputs;
  };
  int done = 0;
  const int total = job.products.size();
  for (const auto &product : job.products) {
    if (cancel && cancel()) {
      job.error = tr("已取消");
      return false;
    }
    if (!product.potentialReason.isEmpty()) {
      potential << QVariantMap{{"version_id", product.versionId},
                               {"reason", product.potentialReason}};
      continue;
    }
    if (!product.candidate)
      continue;
    if (progress)
      progress(total > 0 ? 100.0 * done / qMax(1, total) : 0.0,
               QStringLiteral("scan"));
    QVariantList baseUnresolved = product.frozenUnresolved;
    QVariantList subUnresolved = product.frozenUnresolved;
    const auto baseInputs = resolveInputs(product.baseInputs, &baseUnresolved);
    const auto subInputs = resolveInputs(product.subInputs, &subUnresolved);
    if (!baseUnresolved.isEmpty() || !subUnresolved.isEmpty()) {
      unresolvedOut << QVariantMap{
          {"version_id", product.versionId},
          {"reason", baseUnresolved + subUnresolved}};
      potential << QVariantMap{{"version_id", product.versionId},
                               {"reason", tr("冻结输入不完整，退化为潜在影响")}};
      ++done;
      continue;
    }
    const auto beforeGrid = paleo::attribution::replayGrid(
        baseInputs, product.facies, product.thresholds, kImpactMaxGridCells,
        product.grid);
    // 变更后版本按新输入重建网格（composeRaster 同规则：新定网格栅格或新的
    // 合并范围）。沿用旧冻结网格会让「范围变了」的重算被错认成同口径 diff。
    const auto afterGrid = paleo::attribution::replayGrid(
        subInputs, product.facies, product.thresholds, kImpactMaxGridCells,
        paleo::attribution::FrozenGrid{});
    const auto diff = diffGrids(beforeGrid, afterGrid);
    if (!diff.valid) {
      unresolvedOut << QVariantMap{
          {"version_id", product.versionId},
          {"reason", tr("网格口径不同或超出像元上限，无法像元级对比")}};
      potential << QVariantMap{{"version_id", product.versionId},
                               {"reason", tr("整体可能受影响（无法局部化）")}};
      ++done;
      continue;
    }
    QVariantMap entry{{"product_version", product.versionId},
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
      // 变化像元 → 相面图斑归属（有界）。口径 = 下游闭包内全部相面与人工
      // 编辑成果：解释/复核记录保存在 edited_facies 后代版本（转面子版本的
      // 下游），只看直接 facies_polygons 子版本会让确认数恒为 0（X7）。
      QVariantList affected;
      constexpr int kMaxAffectedFeatures = 5000; // 每成果行数上界
      bool affectedTruncated = false;
      for (const auto &cv : product.children) {
        if (cancel && cancel()) {
          job.error = tr("已取消");
          return false;
        }
        const auto child = cv.id;
        const auto childKind = cv.extra.value("kind").toString();
        const auto childHold = opener(cv);
        auto *layer = qobject_cast<QgsVectorLayer *>(childHold.get());
        if (!layer) {
          unresolvedOut << QVariantMap{{"version_id", child},
                                       {"reason", tr("相面版本无法打开，图斑归属不可用")}};
          continue;
        }
        struct FeatureInfo {
          QString uid, reviewStatus, interpretationId;
        };
        QgsSpatialIndex index;
        QHash<QgsFeatureId, FeatureInfo> infos;
        {
          QgsFeature f;
          auto it = layer->getFeatures();
          while (it.nextFeature(f)) {
            if (cancel && cancel()) {
              job.error = tr("已取消");
              return false;
            }
            if (f.geometry().isEmpty())
              continue;
            QgsFeature indexed(f.id());
            indexed.setGeometry(f.geometry());
            index.addFeature(indexed);
            FeatureInfo info;
            const int field = f.fields().indexOf("paleo_feature_uid");
            info.uid = field < 0 ? QString() : f.attribute(field).toString();
            // 建索引时一次性读取解释（本图层每要素一次；N1 纪律）。
            QVariantMap interpretation;
            QString parseError;
            if (paleo::evidence::decodeInterpretation(
                    f.attribute("paleo_interpretation").toString(),
                    &interpretation, &parseError)) {
              info.reviewStatus = interpretation.value("status").toString();
              info.interpretationId = interpretation.value("id").toString();
            }
            infos.insert(f.id(), info);
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
            const auto info = infos.value(id);
            if (info.uid.isEmpty() || seen.contains(info.uid))
              continue;
            seen.insert(info.uid);
            affected << QVariantMap{
                {"feature_uid", info.uid},
                {"layer_id", cv.extra.value("layer_id")},
                {"product_version", child},
                {"kind", childKind},
                {"review_status", info.reviewStatus.isEmpty()
                                      ? QStringLiteral("none")
                                      : info.reviewStatus},
                {"interpretation_id", info.interpretationId}};
            if (affected.size() >= kMaxAffectedFeatures) {
              affectedTruncated = true;
              break;
            }
          }
          if (affectedTruncated)
            break;
        }
        if (affectedTruncated)
          break;
      }
      entry.insert("affected_features", affected);
      if (affectedTruncated)
        entry.insert("affected_features_truncated", true);
      if (diff.samplingStride > 1)
        entry.insert("affected_features_note",
                     tr("受影响图斑为 %1 步长采样下界，非全量清单")
                         .arg(diff.samplingStride));
    }
    determined << entry;
    ++done;
  }
  out.insert("potential_products", potential);
  out.insert("determined", determined);
  out.insert("unresolved", unresolvedOut);
  out.insert("status", determined.isEmpty() && potential.isEmpty()
                           ? QStringLiteral("no_impact")
                           : QStringLiteral("ok"));
  job.result = out;
  return true;
}

void MappingWorkbench::writeImpactReport(ImpactJob &job) const {
  // 历史快照：固定版本对的完整报告落盘（不建立在当前活动图层上）。
  if (QDir().mkpath(job.dir + "/artifacts/reports")) {
    QJsonDocument doc(QJsonObject::fromVariantMap(job.result));
    const auto path = job.dir + "/artifacts/reports/impact-" +
                      QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) +
                      ".json";
    // 原子整写并校验：写失败（磁盘满等）不得对外宣称 report_path 可用。
    QString writeErr;
    if (paleo::writeJsonAtomic(path, doc, &writeErr))
      job.result.insert("report_path", path);
    else
      qWarning() << "impact report write failed:" << writeErr;
  }
}

bool MappingWorkbench::commitImpact(ImpactJob &job, QString *error) {
  if (!job.error.isEmpty()) {
    fail(error, job.error);
    emit impactAnalysisReady({}, job.error);
    return false;
  }
  if (!m_catalog || m_catalog != job.catalog || m_dir != job.dir) {
    fail(error, tr("工程已切换，影响分析结果已丢弃"));
    emit impactAnalysisReady({}, *error);
    return false;
  }
  if (m_catalog->catalogRevision() != job.revision) {
    fail(error, tr("工程目录已变化（修订 %1 → %2），影响分析结果已过期，请重新分析")
                  .arg(job.revision)
                  .arg(m_catalog->catalogRevision()));
    emit impactAnalysisReady({}, *error);
    return false;
  }
  if (!versionMatches(m_catalog, job.beforeSnapshot) ||
      !versionMatches(m_catalog, job.afterSnapshot)) {
    fail(error, tr("输入版本已变化，影响分析结果已过期，请重新分析"));
    emit impactAnalysisReady({}, *error);
    return false;
  }
  writeImpactReport(job);
  emit impactAnalysisReady(job.result, QString());
  return true;
}

bool MappingWorkbench::startImpactAnalysis(const QString &layerId,
                                           const QString &horizon,
                                           QString *error) {
  if (!m_catalog || !m_catalog->isOpen() || m_dir.isEmpty()) {
    fail(error, tr("请先打开工程再分析影响"));
    return false;
  }
  if (!m_taskSvc) {
    fail(error, tr("后台影响分析需要任务服务"));
    return false;
  }
  // X13 版本对解析（同 analyzeLayerVersionImpact 的口径，GUI 线程完成）。
  const auto after = versionForLayer(layerId);
  if (after.id.isEmpty()) {
    fail(error, tr("所选图件尚未登记到数据管理"));
    return false;
  }
  QString before;
  int beforeNumber = 0;
  for (const auto &v : m_catalog->versionsForAsset(after.assetId)) {
    if (v.versionNumber < after.versionNumber && v.versionNumber > beforeNumber) {
      before = v.id;
      beforeNumber = v.versionNumber;
    }
  }
  if (before.isEmpty()) {
    fail(error, tr("该图件只有单一版本，没有可分析的版本变化"));
    return false;
  }
  auto job = std::make_shared<ImpactJob>();
  if (!prepareImpactJob({{QStringLiteral("kind"),
                          QStringLiteral("input_version")},
                         {QStringLiteral("before_version_id"), before},
                         {QStringLiteral("after_version_id"), after.id},
                         {QStringLiteral("horizon"), horizon}},
                        layerId, *job, error))
    return false;
  if (!m_impactRunner)
    m_impactRunner = std::make_unique<paleo::jobs::JobRunner<ImpactJob>>(this);
  m_impactRunner->setTaskService(m_taskSvc);
  if (m_impactRunner->busy()) {
    fail(error, tr("已有影响分析任务在进行"));
    return false;
  }
  paleo::jobs::JobRunner<ImpactJob>::Callbacks cb;
  cb.compute = [](ImpactJob &j, const std::function<bool()> &cancel,
                  const std::function<void(double, const QString &)> &progress) {
    // worker 图层面：版本 id → 本作业局部缓存（compute 结束随缓存析构），
    // 不触碰 attributionLayer 的 GUI 线程 LRU 与 live catalog。
    QHash<QString, std::shared_ptr<QgsMapLayer>> local;
    const VersionLayerOpener workerOpener = [&local, &j](const CatalogVersion &v) {
      if (const auto hit = local.value(v.id))
        return hit;
      const auto layer = openVersionLayer(j.dir, v);
      if (layer)
        local.insert(v.id, layer);
      return layer;
    };
    return computeImpactWith(j, cancel, progress, workerOpener);
  };
  cb.commit = [this](ImpactJob &j, QString *err) { return commitImpact(j, err); };
  cb.onDropped = [this](ImpactJob &, paleo::jobs::DropReason reason) {
    // 取消/过期丢弃：不发布任何结果（半成品不登记成报告）。
    if (reason == paleo::jobs::DropReason::PrepareFailed)
      return; // 启动失败由 start 的同步 error 上报
    emit impactAnalysisReady(
        {}, reason == paleo::jobs::DropReason::Cancelled
                          ? tr("影响分析已取消")
                          : tr("影响分析已过期（工程已切换），结果未发布"));
  };
  return m_impactRunner->start(tr("影响分析（重放冻结输入）"), job, cb,
                               job->layerId) != nullptr;
}

bool MappingWorkbench::impactAnalysisBusy() const {
  return m_impactRunner && m_impactRunner->busy();
}

void MappingWorkbench::cancelImpactAnalysis() {
  if (m_impactRunner)
    m_impactRunner->requestCancel();
}


QVariantMap MappingWorkbench::recomputePlan(const QVariantMap &impactResult,
                                            QString *error) {
  if (!m_catalog || !m_catalog->isOpen()) {
    fail(error, tr("请先打开工程再生成重算计划"));
    return {};
  }
  const auto afterVersion =
      impactResult.value("source_after_version").toString();
  // X5：计划绑定生成它的影响报告快照（变化类型、前后版本、资产、目录修订
  // 号）。执行时按绑定判定报告是否已过期，不静默按更新后的输入重算。
  QVariantMap plan{
      {"created_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
      {"impact_report", impactResult.value("report_path")},
      {"binding",
       QVariantMap{{"kind", impactResult.value("kind")},
                   {"source_before_version",
                    impactResult.value("source_before_version")},
                   {"source_after_version", afterVersion},
                   {"source_asset_id",
                    m_catalog->versionById(afterVersion).assetId},
                   {"report_path", impactResult.value("report_path")},
                   {"analyzed_at", impactResult.value("analyzed_at")},
                   {"catalog_revision", m_catalog->catalogRevision()}}},
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
  // X6：确定与潜在条目同一口径重建 inputs（冻结 → 当前版本对照）。旧实现
  // 对 potential 直接复制 ordered_inputs（冻结的 product 版本 id），执行
  // 时拾取的是旧输入——产出与旧版相同的「新版本」，伪装完成重算。
  const auto buildInputs = [&currentOf](const CatalogVersion &v,
                                        int *inputsChanged) {
    QVariantList inputs;
    *inputsChanged = 0;
    for (const auto &inputValue : v.extra.value("ordered_inputs").toList()) {
      const auto input = inputValue.toMap();
      const auto frozen = input.value("version_id").toString();
      const auto current = currentOf(frozen);
      if (current != frozen)
        ++(*inputsChanged);
      inputs << QVariantMap{
          {"layer_id", input.value("layer_id")},
          {"frozen_version", frozen},
          {"current_version", current},
          // 重算入口：ordered_inputs 里的 layer_id 可能是冻结当时的 product
          // id（永远指向旧版本）；重算必须用当前版本的 product 声明 id。
          {"rerun_layer_id",
           current.isEmpty() ? input.value("layer_id")
                             : QStringLiteral("product.%1").arg(current)}};
    }
    return inputs;
  };
  // 保护口径：受影响/全部图斑上人工解释的确认与待复核计数。确认记录保留
  // 在旧版本；新版本不自动迁移。review_required 有界 500。
  const auto protectionFor = [&](const QString &vid, const QVariantList &rows,
                                 bool wholeProduct) {
    int confirmed = 0, review = 0;
    QVariantList reviewRequired;
    bool truncated = false;
    const auto count = [&](const QString &status, const QString &uid,
                           const QString &product, const QString &kind) {
      const bool needsReview = status == QLatin1String("NeedsReview") ||
                               status == QLatin1String("Disputed") ||
                               status == QLatin1String("Proposed");
      if (status == QLatin1String("Confirmed"))
        ++confirmed;
      if (needsReview)
        ++review;
      // review_required 收所有带解释结论的图斑（含 Confirmed）——确认解释
      // 不自动迁移，新版本上这些图斑都需要人工复核后才算数。
      if (!status.isEmpty() && status != QLatin1String("none") &&
          reviewRequired.size() < 500)
        reviewRequired << QVariantMap{{"feature_uid", uid},
                                      {"product_version", product},
                                      {"kind", kind},
                                      {"review_status", status}};
    };
    if (!wholeProduct) {
      // analyzeImpact 已在受影响行上携带 review_status（含 edited_facies
      // 后代），这里直接计数，不再回读图层。
      for (const auto &value : rows) {
        const auto row = value.toMap();
        count(row.value("review_status").toString(),
              row.value("feature_uid").toString(),
              row.value("product_version").toString(),
              row.value("kind").toString());
      }
    } else {
      // 无法局部化：扫全部下游相面/人工编辑成果的全部要素（有界）。
      int scanned = 0;
      for (const auto &cv : m_catalog->downstreamClosure(vid)) {
        const auto childKind = cv.extra.value("kind").toString();
        if (childKind != QLatin1String("facies_polygons") &&
            childKind != QLatin1String("edited_facies"))
          continue;
        const auto hold = attributionLayer(cv.id);
        auto *layer = qobject_cast<QgsVectorLayer *>(hold.get());
        if (!layer)
          continue;
        QgsFeatureRequest request;
        request.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
        QgsFeature f;
        auto it = layer->getFeatures(request);
        while (it.nextFeature(f)) {
          if (++scanned > 5000) {
            truncated = true;
            break;
          }
          QVariantMap interpretation;
          QString parseError;
          QString status;
          if (paleo::evidence::decodeInterpretation(
                  f.attribute("paleo_interpretation").toString(),
                  &interpretation, &parseError))
            status = interpretation.value("status").toString();
          const int uidField = f.fields().indexOf("paleo_feature_uid");
          count(status,
                uidField < 0 ? QString() : f.attribute(uidField).toString(),
                cv.id, childKind);
        }
        if (truncated)
          break;
      }
    }
    QVariantMap protection{
        {"confirmed_features", confirmed},
        {"needs_review_features", review},
        {"review_required", reviewRequired},
        {"scope", wholeProduct ? QStringLiteral("whole_product")
                               : QStringLiteral("affected")},
        {"policy", tr("已确认解释保留在旧版本；新版本需人工复核后迁移")}};
    if (truncated)
      protection.insert("scan_truncated", true);
    return protection;
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
    int inputsChanged = 0;
    const auto inputs = buildInputs(v, &inputsChanged);
    const auto changedCells = entry.value("changed_cells").toLongLong();
    QString mode, reason;
    if (changedCells == 0) {
      mode = QStringLiteral("no_change");
      reason = tr("像元级重放证明结果不变，无需重算");
    } else if (inputsChanged == 0) {
      mode = QStringLiteral("blocked_upstream");
      reason = tr("输入尚无新版本：需先重算上游（如单因素）后再重算本成果");
    } else {
      mode = QStringLiteral("local_rerun");
      // 口径如实：局部证明只用于定位影响范围，执行仍是整幅网格同口径重算。
      reason = tr("已证明局部变化 %1 像元；为保证同口径仍按整幅网格重算")
                   .arg(changedCells);
    }
    QVariantMap out{
        {"target_version", vid},
        {"horizon", v.extra.value("horizon")},
        {"mode", mode},
        {"reason", reason},
        {"inputs", inputs},
        {"inputs_changed", inputsChanged},
        {"parameters", v.extra.value("algorithm_parameters")},
        {"protection",
         protectionFor(vid, entry.value("affected_features").toList(), false)}};
    if (mode == QLatin1String("local_rerun"))
      out.insert("execution_scope", QStringLiteral("full_grid"));
    entries << out;
    planned.insert(vid);
  }
  for (const auto &value : potential) {
    const auto row = value.toMap();
    const auto vid = row.value("version_id").toString();
    if (vid.isEmpty() || planned.contains(vid))
      continue;
    const auto v = m_catalog->versionById(vid);
    if (v.id.isEmpty() ||
        v.extra.value("kind").toString() != QLatin1String("composed_facies"))
      continue;
    int inputsChanged = 0;
    const auto inputs = buildInputs(v, &inputsChanged);
    QString mode, reason;
    if (inputsChanged == 0) {
      mode = QStringLiteral("blocked_upstream");
      reason = tr("输入尚无新版本：需先重算上游（如单因素）后再重算本成果");
    } else {
      mode = QStringLiteral("whole_rerun");
      reason = row.value("reason").toString().isEmpty()
                   ? tr("无法局部化：按整幅网格整体重算")
                   : row.value("reason").toString();
    }
    entries << QVariantMap{
        {"target_version", vid},
        {"horizon", v.extra.value("horizon")},
        {"mode", mode},
        {"reason", reason},
        {"inputs", inputs},
        {"inputs_changed", inputsChanged},
        {"parameters", v.extra.value("algorithm_parameters")},
        {"protection", protectionFor(vid, {}, true)}};
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
  const auto binding = plan.value("binding").toMap();
  if (binding.isEmpty()) {
    fail(error, tr("计划缺少影响报告绑定，请重新分析"));
    return {};
  }
  // X5：input_version 报告绑定「分析时刻的 after = 输入资产当前版本」；
  // 输入又前进则报告过期，整个计划拒绝执行（未启动任何编图）。
  if (binding.value("kind").toString() == QLatin1String("input_version")) {
    const auto assetId = binding.value("source_asset_id").toString();
    const auto after = binding.value("source_after_version").toString();
    const auto current = m_catalog->currentVersion(assetId).id;
    if (current != after) {
      fail(error, tr("影响报告已过期：输入已更新到 %1，请重新分析影响")
                      .arg(current.isEmpty() ? tr("（输入资产已删除）")
                                             : current));
      return {};
    }
  }
  QVariantMap result{
      {"started_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
      {"results", QVariantList{}}};
  QVariantList results;
  bool sawTaskStart = false;
  for (const auto &value : plan.value("entries").toList()) {
    const auto entry = value.toMap();
    const auto vid = entry.value("target_version").toString();
    const auto horizon = entry.value("horizon").toString();
    const auto mode = entry.value("mode").toString();
    const auto reason = entry.value("reason").toString();
    QVariantMap row{{"target_version", vid}, {"mode", mode}};
    const auto target = m_catalog->versionById(vid);
    if (target.id.isEmpty()) {
      row.insert("ok", false);
      row.insert("error", tr("目标成果版本已不在目录"));
      results << row;
      continue;
    }
    // 输入当前版本仍须与计划一致——计划外上游又变了等于分析过期。
    QString staleError;
    for (const auto &inputValue : entry.value("inputs").toList()) {
      const auto input = inputValue.toMap();
      const auto fv =
          m_catalog->versionById(input.value("frozen_version").toString());
      const QString actual =
          fv.id.isEmpty() ? QString()
                          : m_catalog->currentVersion(fv.assetId).id;
      if (actual != input.value("current_version").toString()) {
        staleError =
            tr("计划已过期：输入 %1 的当前版本已变化，请重新分析影响")
                .arg(input.value("layer_id").toString());
        break;
      }
    }
    if (!staleError.isEmpty()) {
      row.insert("ok", false);
      row.insert("stale", true);
      row.insert("error", staleError);
      results << row;
      continue;
    }
    // 无需执行的条目如实跳过——not ok、不启动编图、不产生新版本。
    if (mode == QLatin1String("no_change") ||
        mode == QLatin1String("blocked_upstream")) {
      row.insert("ok", false);
      row.insert("skipped", true);
      row.insert("reason", reason);
      results << row;
      continue;
    }
    // 幂等续跑：已有同一来源的重算成果（同 after 版本 + 同计划输入版本）
    // → 如实报已完成。任务模式下「再按一次继续」不再伪造第二次执行。
    QStringList plannedInputs;
    for (const auto &inputValue : entry.value("inputs").toList())
      plannedInputs << inputValue.toMap().value("current_version").toString();
    // PW-145：输入顺序决定综合优先级（ordered_inputs）——按序比较，不排序；
    // 计划参数也必须与成果存档的算法参数一致，否则不是「同一计划」。
    const QVariantMap plannedParams = entry.value("parameters").toMap();
    QString alreadyDone;
    for (const auto &cv : m_catalog->versions()) {
      if (cv.extra.value("kind").toString() != QLatin1String("composed_facies"))
        continue;
      const auto provenance = cv.extra.value("recompute_provenance").toMap();
      if (provenance.value("recompute_of").toString() != vid ||
          provenance.value("source_after_version").toString() !=
              binding.value("source_after_version").toString())
        continue;
      QStringList actual;
      for (const auto &inputValue : cv.extra.value("ordered_inputs").toList())
        actual << inputValue.toMap().value("version_id").toString();
      if (actual != plannedInputs)
        continue;
      const QVariantMap stored = cv.extra.value("algorithm_parameters").toMap();
      bool sameParams = true;
      for (auto it = plannedParams.cbegin(); it != plannedParams.cend(); ++it) {
        if (it.key() == QLatin1String("recompute"))
          continue;
        // ordered_inputs 是计划携带的冻结旧版本快照，成果存档里是重算当次的
        // 新输入——输入身份已由上方 actual/plannedInputs 按序列表比对，参数
        // 快照不重复当输入比对（否则幂等命中永不成立）。
        if (it.key() == QLatin1String("ordered_inputs"))
          continue;
        if (stored.value(it.key()) != it.value()) {
          sameParams = false;
          break;
        }
      }
      if (sameParams)
        alreadyDone = cv.id;
    }
    if (!alreadyDone.isEmpty()) {
      row.insert("ok", true);
      row.insert("already_done", true);
      row.insert("new_version", alreadyDone);
      row.insert("reason", tr("该计划已执行完成，成果版本已存在"));
      results << row;
      continue;
    }
    QStringList layerIds;
    auto params = entry.value("parameters").toMap();
    // 重算血缘写进参数 → commitCompose 落成 recompute_provenance 并存档
    //（X13 后可从成果反查「它替代了哪一版」）。
    params.insert("recompute",
                  QVariantMap{{"recompute_of", vid},
                              {"impact_report",
                               binding.value("report_path",
                                             plan.value("impact_report"))},
                              {"source_before_version",
                               binding.value("source_before_version")},
                              {"source_after_version",
                               binding.value("source_after_version")},
                              {"plan_created_at", plan.value("created_at")}});
    for (const auto &inputValue : entry.value("inputs").toList()) {
      const auto input = inputValue.toMap();
      // rerun_layer_id（当前版本的 product 声明）优先；旧计划条目回退到原
      // layer_id。
      const auto layerId = input.contains(QStringLiteral("rerun_layer_id"))
                               ? input.value("rerun_layer_id").toString()
                               : input.value("layer_id").toString();
      if (!layerId.isEmpty())
        layerIds << layerId;
    }
    if (layerIds.isEmpty()) {
      row.insert("ok", false);
      row.insert("error", tr("计划条目缺少输入图层"));
      results << row;
      continue;
    }
    QString startError;
    // 复用现有综合编图入口：同参数、当前输入版本 → 生成新版本，
    // 绝不覆盖历史版本；任务模式下异步，完成后走既有 productReady 链。
    // 任务模式下 compose runner 单飞：首个条目启动后其余条目如实报
    // 「请等待完成」，不伪装成已排队。
    if (m_taskSvc && sawTaskStart) {
      row.insert("ok", false);
      row.insert("async", true);
      row.insert("error", tr("任务模式下逐条重算：请等待上一条完成后再次执行"));
      results << row;
      continue;
    }
    const bool started = startCompose(horizon, layerIds, params, &startError);
    if (started && m_taskSvc)
      sawTaskStart = true;
    row.insert("ok", started);
    row.insert("async", bool(m_taskSvc));
    row.insert(started ? "reason" : "error",
               started ? reason : startError);
    results << row;
  }
  result.insert("results", results);
  return result;
}

QVariantMap MappingWorkbench::compareVersions(const QString &oldVersionId,
                                              const QString &newVersionId,
                                              QString *error) {
  CompareJob job;
  if (!prepareVersionCompare(oldVersionId, newVersionId, QString(), job, error))
    return {};
  // 同步路径：图层经 attributionLayer LRU 打开（与历史行为一致）。
  const VersionLayerOpener ownerOpener = [this](const CatalogVersion &v) {
    return attributionLayer(v.id);
  };
  if (!computeVersionCompareWith(job, {}, {}, ownerOpener)) {
    fail(error, job.error);
    return {};
  }
  return job.result;
}

bool MappingWorkbench::prepareVersionCompare(const QString &oldVersionId,
                                             const QString &newVersionId,
                                             const QString &layerId,
                                             CompareJob &job, QString *error) {
  job = CompareJob{};
  if (!m_catalog) {
    fail(error, tr("目录未绑定"));
    return false;
  }
  job.dir = m_dir;
  job.layerId = layerId;
  job.catalog = m_catalog;
  job.revision = m_catalog->catalogRevision();
  job.oldVersionId = oldVersionId;
  job.newVersionId = newVersionId;
  job.oldVersion = m_catalog->versionById(oldVersionId);
  job.newVersion = m_catalog->versionById(newVersionId);
  if (job.oldVersion.id.isEmpty() || job.newVersion.id.isEmpty()) {
    fail(error, tr("对比需要两个可读的栅格成果版本"));
    return false;
  }
  // 相面计数子层快照（worker 不回读目录）。
  const auto polygonChildren = [this](const QString &vid) {
    QVector<CatalogVersion> out;
    for (const auto &cv : m_catalog->downstreamClosure(vid))
      if (cv.extra.value("kind").toString() == QLatin1String("facies_polygons"))
        out << cv;
    return out;
  };
  job.oldPolygons = polygonChildren(oldVersionId);
  job.newPolygons = polygonChildren(newVersionId);
  return true;
}

bool MappingWorkbench::computeVersionCompareWith(
    CompareJob &job, const std::function<bool()> &cancel,
    const std::function<void(double, const QString &)> &progress,
    const VersionLayerOpener &opener) {
  const auto beforeHold = opener(job.oldVersion);
  const auto afterHold = opener(job.newVersion);
  auto *before = qobject_cast<QgsRasterLayer *>(beforeHold.get());
  auto *after = qobject_cast<QgsRasterLayer *>(afterHold.get());
  if (!before || !after) {
    job.error = tr("对比需要两个可读的栅格成果版本");
    return false;
  }
  // 坐标系不同则逐下标比较的像元不在同一空间口径——如实拒绝而不是错位
  // 对比（与网格口径防御同理；两侧图层同源打开时 CRS 表示一致）。
  if (before->crs() != after->crs()) {
    job.error = tr("两版本坐标系不同，无法像元级对比");
    return false;
  }
  if (before->width() != after->width() || before->height() != after->height() ||
      before->extent() != after->extent()) {
    job.error = tr("两版本网格口径不同（范围或分辨率不一致），无法像元级对比");
    return false;
  }
  const int cols = before->width(), rows = before->height();
  if (qint64(cols) * rows > kImpactMaxGridCells) {
    job.error = tr("网格超出对比上限");
    return false;
  }
  std::unique_ptr<QgsRasterBlock> beforeBlock(
      before->dataProvider()->block(1, before->extent(), cols, rows));
  std::unique_ptr<QgsRasterBlock> afterBlock(
      after->dataProvider()->block(1, after->extent(), cols, rows));
  if (!beforeBlock || !beforeBlock->isValid() || !afterBlock || !afterBlock->isValid()) {
    job.error = tr("无法读取成果栅格");
    return false;
  }
  qint64 changed = 0, unchanged = 0, total = 0;
  QHash<QString, qint64> matrix; // "from>to" → cells
  const qint64 blockCells =
      qint64(beforeBlock->width()) * beforeBlock->height();
  for (qint64 i = 0; i < blockCells; ++i) {
    if (cancel && ((i & 0xFFFF) == 0) && cancel()) {
      job.error = tr("已取消");
      return false;
    }
    if ((i & 0x3FFFF) == 0 && progress)
      progress(blockCells > 0 ? 100.0 * i / blockCells : 0.0,
               QStringLiteral("scan"));
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
                  {"old_version", job.oldVersionId},
                  {"new_version", job.newVersionId},
                  {"cells_changed", QVariant::fromValue(changed)},
                  {"cells_unchanged", QVariant::fromValue(unchanged)},
                  {"total_cells", QVariant::fromValue(total)},
                  {"code_matrix", codeMatrix}};
  // 相面统计：两版本各自相面子版本的图斑数。图斑身份在重算后重新生成，
  // 不做跨版本一一匹配——空间对应以像元转移矩阵为准。
  const auto polygonCount = [&opener](const QVector<CatalogVersion> &children) {
    for (const auto &cv : children) {
      const auto hold = opener(cv);
      auto *layer = qobject_cast<QgsVectorLayer *>(hold.get());
      return layer ? int(layer->featureCount()) : -1;
    }
    return -1;
  };
  out.insert("polygon_counts",
             QVariantMap{{"before", polygonCount(job.oldPolygons)},
                         {"after", polygonCount(job.newPolygons)}});
  out.insert("note", tr("重算后图斑身份重新生成，不做跨版本图斑一一匹配；"
                        "空间对应以像元级转移矩阵为准"));
  job.result = out;
  return true;
}

bool MappingWorkbench::commitVersionCompare(CompareJob &job, QString *error) {
  if (!job.error.isEmpty()) {
    fail(error, job.error);
    emit versionCompareReady({}, job.error);
    return false;
  }
  if (!m_catalog || m_catalog != job.catalog || m_dir != job.dir) {
    fail(error, tr("工程已切换，版本对比结果已丢弃"));
    emit versionCompareReady({}, *error);
    return false;
  }
  if (m_catalog->catalogRevision() != job.revision ||
      !versionMatches(m_catalog, job.oldVersion) ||
      !versionMatches(m_catalog, job.newVersion)) {
    fail(error, tr("工程目录或对比版本已变化，对比结果已过期，请重新对比"));
    emit versionCompareReady({}, *error);
    return false;
  }
  QVariantMap out = job.result;
  // 对比报告落盘：与影响报告同一目录约定。
  if (QDir().mkpath(m_dir + "/artifacts/reports")) {
    const auto path = m_dir + "/artifacts/reports/compare-" +
                      QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) +
                      ".json";
    QString writeErr;
    if (paleo::writeJsonAtomic(path, QJsonDocument(QJsonObject::fromVariantMap(out)), &writeErr))
      out.insert("report_path", path);
    else
      qWarning() << "compare report write failed:" << writeErr;
  }
  job.result = out;
  emit versionCompareReady(out, QString());
  return true;
}

bool MappingWorkbench::startVersionCompare(const QString &layerId,
                                            QString *error) {
  if (!m_catalog || !m_catalog->isOpen()) {
    fail(error, tr("请先打开工程再对比"));
    return false;
  }
  if (!m_taskSvc) {
    fail(error, tr("后台版本对比需要任务服务"));
    return false;
  }
  // 血统解析（compareWithRecomputeSource 的口径，GUI 线程完成）。
  const auto composedId = composedAncestorVersion(layerId);
  if (composedId.isEmpty()) {
    fail(error, tr("该图件没有综合编图计算血统"));
    return false;
  }
  const auto composed = m_catalog->versionById(composedId);
  const auto provenance = composed.extra.value("recompute_provenance").toMap();
  const auto oldId = provenance.value("recompute_of").toString();
  if (oldId.isEmpty()) {
    fail(error, tr("该成果不是受控重算产物，无可对比的旧版本"));
    return false;
  }
  auto job = std::make_shared<CompareJob>();
  if (!prepareVersionCompare(oldId, composedId, layerId, *job, error))
    return false;
  if (!m_compareRunner)
    m_compareRunner = std::make_unique<paleo::jobs::JobRunner<CompareJob>>(this);
  m_compareRunner->setTaskService(m_taskSvc);
  if (m_compareRunner->busy()) {
    fail(error, tr("已有版本对比任务在进行"));
    return false;
  }
  paleo::jobs::JobRunner<CompareJob>::Callbacks cb;
  cb.compute = [](CompareJob &j, const std::function<bool()> &cancel,
                  const std::function<void(double, const QString &)> &progress) {
    QHash<QString, std::shared_ptr<QgsMapLayer>> local;
    const VersionLayerOpener workerOpener = [&local, &j](const CatalogVersion &v) {
      if (const auto hit = local.value(v.id))
        return hit;
      const auto layer = openVersionLayer(j.dir, v);
      if (layer)
        local.insert(v.id, layer);
      return layer;
    };
    return computeVersionCompareWith(j, cancel, progress, workerOpener);
  };
  cb.commit = [this](CompareJob &j, QString *err) { return commitVersionCompare(j, err); };
  cb.onDropped = [this](CompareJob &, paleo::jobs::DropReason reason) {
    if (reason == paleo::jobs::DropReason::PrepareFailed)
      return;
    emit versionCompareReady(
        {}, reason == paleo::jobs::DropReason::Cancelled
                          ? tr("版本对比已取消")
                          : tr("版本对比已过期（工程已切换），结果未发布"));
  };
  return m_compareRunner->start(tr("成果版本对比（像元级 diff）"), job, cb,
                                job->layerId) != nullptr;
}

bool MappingWorkbench::versionCompareBusy() const {
  return m_compareRunner && m_compareRunner->busy();
}

void MappingWorkbench::cancelVersionCompare() {
  if (m_compareRunner)
    m_compareRunner->requestCancel();
}

QVariantMap MappingWorkbench::analyzeLayerVersionImpact(
    const QString &layerId, const QString &horizon, QString *error) {
  if (!m_catalog || !m_catalog->isOpen()) {
    fail(error, tr("请先打开工程再分析影响"));
    return {};
  }
  // 所选图件 = 变化后的版本；同资产的上一版 = 变化前（X13：从视图移入）。
  const auto after = versionForLayer(layerId);
  if (after.id.isEmpty()) {
    fail(error, tr("所选图件尚未登记到数据管理"));
    return {};
  }
  QString before;
  int beforeNumber = 0;
  for (const auto &v : m_catalog->versionsForAsset(after.assetId)) {
    if (v.versionNumber < after.versionNumber &&
        v.versionNumber > beforeNumber) {
      before = v.id;
      beforeNumber = v.versionNumber;
    }
  }
  if (before.isEmpty()) {
    fail(error, tr("该图件只有单一版本，没有可分析的版本变化"));
    return {};
  }
  return analyzeImpact(
      {{"kind", QStringLiteral("input_version")},
       {"before_version_id", before},
       {"after_version_id", after.id},
       {"horizon", horizon}},
      error);
}

QVariantMap MappingWorkbench::compareWithRecomputeSource(const QString &layerId,
                                                         QString *error) {
  if (!m_catalog || !m_catalog->isOpen()) {
    fail(error, tr("请先打开工程再对比"));
    return {};
  }
  const auto composedId = composedAncestorVersion(layerId);
  if (composedId.isEmpty()) {
    fail(error, tr("该图件没有综合编图计算血统"));
    return {};
  }
  const auto composed = m_catalog->versionById(composedId);
  const auto provenance = composed.extra.value("recompute_provenance").toMap();
  const auto oldId = provenance.value("recompute_of").toString();
  if (oldId.isEmpty()) {
    fail(error, tr("该成果不是受控重算产物，无可对比的旧版本"));
    return {};
  }
  auto out = compareVersions(oldId, composedId, error);
  if (out.isEmpty())
    return {};
  // 对比报告落盘：与影响报告同一目录约定。
  if (QDir().mkpath(m_dir + "/artifacts/reports")) {
    const auto path = m_dir + "/artifacts/reports/compare-" +
                      QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) +
                      ".json";
    QString writeErr;
    if (paleo::writeJsonAtomic(path, QJsonDocument(QJsonObject::fromVariantMap(out)), &writeErr))
      out.insert("report_path", path);
    else
      qWarning() << "compare report write failed:" << writeErr;
  }
  return out;
}

// X9：索引同步作业三段式。worker 线程用本地 Index——QSQLITE 连接建/用/销
// 全在 worker 内（DataCatalog 不跨线程，版本快照由发起线程取好）。无任务
// 服务时同步路径与 JobRunner 回调共用同一实现。
bool MappingWorkbench::computeIndexSync(
    IndexSyncJob &job, const std::function<bool()> &cancel,
    const std::function<void(double, const QString &)> &) {
  paleo::evidenceindex::Index index;
  if (!index.open(job.dir, &job.error))
    return false;
  job.changed = index.synchronize(job.versions, job.dir, cancel, &job.error);
  if (job.changed >= 0)
    job.stats = index.stats();
  index.close(); // 连接归还必须在 worker 线程内完成
  return job.changed >= 0;
}

bool MappingWorkbench::commitIndexSync(IndexSyncJob &job, QString *error) {
  if (m_catalog != job.catalog || m_dir != job.dir) {
    fail(error, tr("工程已切换，索引同步结果已丢弃"));
    return false;
  }
  if (!job.error.isEmpty()) {
    // 同步失败如实落错误态；attempted==revision 时查询走 error 分支不再
    // 自动重启，目录修订变化后恢复自动重试。
    m_evidenceIndexAttemptedRevision = job.revision;
    m_evidenceIndexSyncError = job.error;
    emit evidenceIndexSynced({{QStringLiteral("index_state"),
                               QStringLiteral("error")},
                              {QStringLiteral("index_error"), job.error},
                              {QStringLiteral("stats"), job.stats}});
    return true; // 同步失败是登记事实，不算 commit 段失败
  }
  m_evidenceIndexSyncedRevision = job.revision;
  m_evidenceIndexAttemptedRevision = -1;
  m_evidenceIndexSyncError.clear();
  m_evidenceIndexStats = job.stats;
  emit evidenceIndexSynced(
      {{QStringLiteral("index_state"),
        m_catalog->catalogRevision() == job.revision
            ? QStringLiteral("fresh")
            : QStringLiteral("stale")},
       {QStringLiteral("stats"), job.stats},
       {QStringLiteral("catalog_revision"), job.revision}});
  return true;
}

bool MappingWorkbench::startEvidenceIndexSync(QString *error) {
  if (!m_catalog || m_dir.isEmpty()) {
    fail(error, tr("请先打开工程"));
    return false;
  }
  IndexSyncJob snapshot;
  snapshot.dir = m_dir;
  snapshot.versions = m_catalog->versions();
  snapshot.revision = m_catalog->catalogRevision();
  snapshot.catalog = m_catalog;
  if (!m_taskSvc) {
    // 无任务服务（测试壳/shell）：同一代码路径在当前线程同步执行。
    if (!computeIndexSync(snapshot, {}, {}))
      return fail(error, snapshot.error), false;
    QString commitError;
    if (!commitIndexSync(snapshot, &commitError))
      return fail(error, commitError), false;
    return true;
  }
  if (!m_indexSyncRunner)
    m_indexSyncRunner =
        std::make_unique<paleo::jobs::JobRunner<IndexSyncJob>>(this);
  m_indexSyncRunner->setTaskService(m_taskSvc);
  if (m_indexSyncRunner->busy()) {
    fail(error, tr("证据索引同步正在进行"));
    return false;
  }
  auto job = std::make_shared<IndexSyncJob>(snapshot);
  paleo::jobs::JobRunner<IndexSyncJob>::Callbacks cb;
  cb.compute = &computeIndexSync;
  cb.commit = [this](IndexSyncJob &j, QString *err) {
    return commitIndexSync(j, err);
  };
  cb.onDropped = [this](IndexSyncJob &j, paleo::jobs::DropReason reason) {
    // 取消/过期丢弃：索引保持已入库状态可用，下轮查询按 stale 重启；
    // 只有调度层失败（prepare）登记错误态——同步本身没跑起来。
    if (reason == paleo::jobs::DropReason::PrepareFailed) {
      m_evidenceIndexAttemptedRevision = j.revision;
      m_evidenceIndexSyncError = paleo::jobs::dropReasonText(reason);
    }
  };
  return m_indexSyncRunner->start(tr("证据索引同步"), job, cb) != nullptr;
}

QVariantList MappingWorkbench::evidenceReverseQuery(const QString &name,
                                                     const QString &argument,
                                                     QString *error,
                                                     QVariantMap *meta) {
  QVariantList rows;
  QVariantMap metaOut;
  const auto emitMeta = [&metaOut, meta]() {
    if (meta) *meta = metaOut;
  };
  if (!m_catalog || m_dir.isEmpty()) {
    fail(error, tr("请先打开工程再检索证据"));
    emitMeta();
    return rows;
  }
  if (!m_evidenceIndex)
    m_evidenceIndex = std::make_unique<paleo::evidenceindex::Index>();
  QString openError;
  if (!m_evidenceIndex->isOpen() && !m_evidenceIndex->open(m_dir, &openError)) {
    fail(error, tr("证据反向索引不可用：%1").arg(openError));
    emitMeta();
    return rows;
  }
  // 新鲜度状态机（X9）：fresh=已同步修订对账一致；syncing=后台同步在跑；
  // stale=落后（发起后台同步，本次仍打现有索引）；error=上次同步失败。
  // 有任务服务时查询永不在调用线程同步；无任务服务时
  // startEvidenceIndexSync 走同步内联路径，调用返回即 fresh。
  const int revision = m_catalog->catalogRevision();
  const bool syncRunning = m_indexSyncRunner && m_indexSyncRunner->busy();
  QString indexState, indexError;
  if (syncRunning) {
    indexState = QStringLiteral("syncing");
  } else if (m_evidenceIndexSyncedRevision == revision) {
    indexState = QStringLiteral("fresh");
  } else if (m_evidenceIndexAttemptedRevision == revision &&
             !m_evidenceIndexSyncError.isEmpty()) {
    indexState = QStringLiteral("error");
    indexError = m_evidenceIndexSyncError;
  } else {
    QString syncError;
    if (startEvidenceIndexSync(&syncError)) {
      indexState = (m_indexSyncRunner && m_indexSyncRunner->busy())
                       ? QStringLiteral("stale")   // 后台在跑，本次打旧索引
                       : QStringLiteral("fresh");  // 同步内联已完成
    } else if (!syncError.isEmpty()) {
      indexState = QStringLiteral("error");
      indexError = syncError;
    } else {
      indexState = QStringLiteral("stale");
    }
  }
  metaOut.insert(QStringLiteral("index_state"), indexState);
  if (!indexError.isEmpty())
    metaOut.insert(QStringLiteral("index_error"), indexError);

  QVector<paleo::evidenceindex::Entry> entries;
  QStringList catalogReasons; // 目录核验补查行逐条对应的 hit_reason
  if (name == QLatin1String("pending_review")) {
    entries = m_evidenceIndex->pendingReview(argument, 200);
    metaOut.insert(QStringLiteral("total"),
                   m_evidenceIndex->countFor(QStringLiteral("pendingReview"),
                                             argument));
  } else if (name == QLatin1String("contradicted")) {
    entries = m_evidenceIndex->contradicted(argument, 200);
    metaOut.insert(QStringLiteral("total"),
                   m_evidenceIndex->countFor(QStringLiteral("contradicted"),
                                             argument));
  } else if (name == QLatin1String("references_source")) {
    entries = m_evidenceIndex->featuresReferencingSource(argument, 200);
    metaOut.insert(QStringLiteral("total"),
                   m_evidenceIndex->countFor(
                       QStringLiteral("featuresReferencingSource"), argument));
  } else if (name == QLatin1String("stale_sources")) {
    entries = m_evidenceIndex->staleSources(argument, 200);
    metaOut.insert(QStringLiteral("total"),
                   m_evidenceIndex->countFor(QStringLiteral("staleSources"),
                                             argument));
    // 目录核验：被引用的来源版本已从目录删除（或文件已失）时，SQL 谓词
    // 看不到——逐来源版本按目录事实补查，命中行如实标注 hit_reason。
    const QStringList sources =
        m_evidenceIndex->distinctSourceVersions(argument);
    QSet<QString> seen;
    for (const auto &e : entries)
      seen.insert(e.versionId + QLatin1Char('|') + e.featureUid +
                  QLatin1Char('|') + e.evidenceId);
    for (const auto &src : sources) {
      const CatalogVersion sv = m_catalog->versionById(src);
      QString reason;
      if (sv.id.isEmpty()) {
        reason = tr("来源版本已不在目录（检索时核验）");
      } else {
        const QString file = (DataCatalog::resolvedVersionPath(m_dir, sv) +
            sv.extra.value(QStringLiteral("source_suffix")).toString())
                .section(QLatin1Char('|'), 0, 0);
        if (file.isEmpty() || !QFileInfo::exists(file))
          reason = tr("来源版本文件已缺失（检索时核验）");
      }
      if (reason.isEmpty())
        continue;
      const auto extra = m_evidenceIndex->featuresReferencingSource(src, 500);
      for (const auto &e : extra) {
        if (e.unlinked)
          continue; // 断链历史行不进「过期来源」结论
        const QString key = e.versionId + QLatin1Char('|') + e.featureUid +
                            QLatin1Char('|') + e.evidenceId;
        if (seen.contains(key))
          continue;
        seen.insert(key);
        entries.append(e);
        catalogReasons.append(reason);
      }
    }
  } else if (name == QLatin1String("search")) {
    entries = m_evidenceIndex->search(argument, 200);
  } else {
    fail(error, tr("未知的检索类型：%1").arg(name));
    emitMeta();
    return rows;
  }
  const int sqlCount = entries.size() - catalogReasons.size();
  int index = 0;
  for (const auto &entry : entries) {
    QString hitReason;
    if (index >= sqlCount) {
      hitReason = catalogReasons.at(index - sqlCount);
    } else if (name == QLatin1String("pending_review")) {
      hitReason = entry.identityReview && entry.reviewState.isEmpty()
                      ? tr("待复核：图斑复核标记")
                      : tr("待复核：解释状态 %1").arg(entry.reviewState);
    } else if (name == QLatin1String("contradicted")) {
      hitReason = tr("反驳关联（%1）").arg(entry.role);
    } else if (name == QLatin1String("references_source")) {
      hitReason = tr("引用来源版本 %1").arg(entry.sourceVersion);
    } else if (name == QLatin1String("stale_sources")) {
      hitReason = entry.sourceVersion.isEmpty()
                      ? tr("来源未钉定版本")
                      : tr("来源或来源图元缺失（索引标记）");
    } else {
      hitReason = tr("文本命中");
    }
    ++index;
    rows << QVariantMap{{"layer_id", entry.layerId},
                       {"feature_uid", entry.featureUid},
                       {"version_id", entry.versionId},
                       {"asset_id", entry.assetId},
                       {"horizon", entry.horizon},
                       {"evidence_id", entry.evidenceId},
                       {"evidence_type", entry.evidenceType},
                       {"role", entry.role},
                       {"text", entry.text},
                       {"review_state", entry.reviewState},
                       {"source_layer", entry.layerId},
                       {"source_version", entry.sourceVersion},
                       {"source_feature_uid", entry.sourceFeatureUid},
                       {"unlinked", entry.unlinked},
                       {"relation_state", entry.unlinked
                                              ? QStringLiteral("history")
                                              : QStringLiteral("active")},
                       {"is_current_version",
                        m_catalog->currentVersion(entry.assetId).id ==
                            entry.versionId},
                       {"hit_reason", hitReason},
                       {"name", entry.text.isEmpty()
                                    ? tr("图斑 %1…").arg(entry.featureUid.left(12))
                                    : entry.text},
                       {"origin", QStringLiteral("reverse_index")}};
  }
  metaOut.insert(QStringLiteral("shown"), rows.size());
  emitMeta();
  return rows;
}
