// 层：功能
// 融合合成管线：prepareCompose/computeCompose/commitCompose/compose/startCompose/publishFactorAlias/~ComposeJob。
// 从 mappingworkbench.cpp 析出——行为守恒平移（goal/core-deepening A-R2）：
// 函数体逐字未动，仅按子域归位到独立 TU。
#include "mappingworkbench.h"
#include "../algorithms/faciesmapping/formationdominance.h"
#include "../catalog/purgelease.h"
#include "../domain/faciescatalog.h"
#include "../domain/mappinghorizons.h"
#include "../domain/singlefactorrequest.h"
#include "../io/constraintstore.h"
#include "../io/lasparser.h"
#include "../qgis/facieshierarchyrenderer.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/layervocabulary.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgisstyleservice.h"
#include "../qgis/wellattributestore.h"
#include "../services/futuretask.h"
#include "../services/jobrunner.h"
#include "../services/paleotaskservice.h"
#include "assetops.h"
#include "constraintimport.h"
#include "derivedassets.h"
#include "evidencerecord.h"
#include "services/projectdata.h"
#include "services/seismichorizoncluster.h"
#include "wellpredictionservice.h"
#include "workflows.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <gdal.h>
#include <limits>
#include <qgscoordinatereferencesystem.h>
#include <qgseditorwidgetsetup.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsunittypes.h>
#include <qgsvectorlayer.h>

namespace {
void fail(QString *error, const QString &text) {
  if (error)
    *error = text;
}
QString uid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString filePath(const QString &source) { return source.section('|', 0, 0); }
} // namespace

bool MappingWorkbench::prepareCompose(const QString &h, const QStringList &ids,
                                      const QVariantMap &params,
                                      ComposeJob &job, QString *error) {
  if (!ready(h, error))
    return false;
  if (QSet<QString>(ids.cbegin(), ids.cend()).size() != ids.size()) {
    fail(error, tr("编图输入不能重复，请检查优先级列表")); return false;
  }
  if (ids.isEmpty()) {
    fail(error, tr("请选择编图输入，列表从上到下为优先级"));
    return false;
  }
  job.horizon = h;
  job.ids = ids;
  job.params = params;
  job.dir = m_dir;
  job.catalog = m_catalog;
  job.facies = facies(h);
  QVariantList sources, orderedInputs;
  QStringList parents;
  QSet<QString> continuous;
  for (const auto &id : ids) {
    const auto d = declaration(id);
    if (d.horizon != h) {
      fail(error, tr("自动编图输入必须属于当前层位；其他层位可在参考窗口查看"));
      return false;
    }
    auto *layer = m_layers->instantiate(id, error);
    if (!layer)
      return false;
    auto version = ensureInputVersion(id, error);
    if (version.isEmpty())
      return false;
    parents << version;
    orderedInputs << QVariantMap{{"layer_id", id}, {"version_id", version},
                                {"priority", orderedInputs.size() + 1}};

    const auto v = m_catalog->versionById(version);
    const auto inputKind = v.extra.value("kind").toString();
    const auto inputSource = v.extra.value("value_source").toString();
    if (paleo::singlefactor::rejectsQuantitativeUse(inputKind, inputSource)) {
      fail(error, tr("解释性制图工作场不能参与连续融合、分相或厚度统计"));
      return false;
    }
    if (id.startsWith("factor.") || v.extra.contains("factor_id") ||
        paleo::singlefactor::isAnalysisFactorRaster(inputKind, inputSource))
      continuous.insert(id);
    else if (v.extra.contains("facies") &&
             v.extra.value("facies").toList() != facies(h)) {
      fail(error, tr("输入相分类与当前层位定义不同，请使用对应版本的相分类，或"
                     "仅作为参考"));
      return false;
    }
    const auto frozenId = v.extra.value("layer_id").toString();
    const auto frozen = frozenId.isEmpty() ? d : declaration(frozenId);
    sources << QVariantMap{{"source", frozen.source},
                           {"provider", layer->providerType()},
                           {"type", frozen.type},
                           {"crs", layer->crs().toWkt()},
                           {"continuous", continuous.contains(id)}};
  }
  QList<double> thresholds;
  for (const auto &v : params.value("thresholds").toList()) {
    bool ok;
    double n = v.toDouble(&ok);
    if (!ok) {
      fail(error, tr("分相阈值必须为数字"));
      return false;
    }
    thresholds << n;
  }
  if (schemaVersion(h).isEmpty() && !saveFacies(h, job.facies, error))
    return false;
  parents << schemaVersion(h);
  if (!QDir().mkpath(m_dir + "/artifacts/staging")) {
    fail(error, tr("无法创建暂存目录"));
    return false;
  }
  job.outputPath = m_dir + "/artifacts/staging/" + uid() + ".tif";
  job.params.insert("ordered_inputs", orderedInputs);
  job.params.insert("horizon", h);
  job.params.insert("crs", DataCatalog::localGridCrsWkt());
  job.sources = sources;
  job.parents = parents;
  job.thresholds = thresholds;
  return true;
}

bool MappingWorkbench::computeCompose(
    ComposeJob &job, const std::function<bool()> &cancel,
    const std::function<void(double)> &progress) {
  std::vector<std::unique_ptr<QgsMapLayer>> owned;
  QList<QgsMapLayer *> layers;
  QSet<QString> continuous;
  for (const auto &entry : job.sources) {
    if (cancel && cancel())
      return false;
    const auto source = entry.toMap();
    std::unique_ptr<QgsMapLayer> layer;
    if (source.value("type").toString() == "raster")
      layer = std::make_unique<QgsRasterLayer>(
          source.value("source").toString(), "compose", "gdal");
    else
      layer = std::make_unique<QgsVectorLayer>(
          source.value("source").toString(), "compose",
          source.value("provider", "ogr").toString());
    layer->setCrs(
        QgsCoordinateReferenceSystem::fromWkt(source.value("crs").toString()));
    if (source.value("continuous").toBool())
      continuous.insert(layer->id());
    layers << layer.get();
    owned.push_back(std::move(layer));
  }
  if (!MappingArtifactWriter::composeRaster(job.outputPath, layers, job.facies,
                                            job.thresholds, continuous,
                                            &job.error, cancel, progress,
                                            &job.receipt))
    return false;
  job.fingerprint =
      DerivedFileFingerprint::read(job.outputPath, cancel, &job.error);
  return !job.fingerprint.sha256.isEmpty();
}

bool MappingWorkbench::commitCompose(ComposeJob &job, QString *error) {
  if (!job.error.isEmpty()) {
    fail(error, job.error);
    return false;
  }
  if (m_catalog != job.catalog || m_dir != job.dir) {
    fail(error, tr("工程已切换，综合编图结果已丢弃"));
    return false;
  }
  for (const auto &parent : job.parents)
    if (m_catalog->versionById(parent).id.isEmpty()) {
      fail(error, tr("输入已删除，综合编图结果已丢弃"));
      return false;
    }
  // 每输入实际胜出像元回执：计算事实（consumed），非地质支持关系。
  QVariantList receiptRows;
  const auto ordered = job.params.value("ordered_inputs").toList();
  for (int i = 0; i < job.ids.size() && i < job.receipt.winningCells.size(); ++i) {
    const auto entry = i < ordered.size() ? ordered[i].toMap() : QVariantMap();
    receiptRows << QVariantMap{
        {"layer_id", job.ids[i]},
        {"version_id", entry.value("version_id")},
        {"priority", entry.value("priority", i + 1)},
        {"won_cells", QVariant::fromValue(job.receipt.winningCells[i])}};
  }
  QVariantMap composeReceipt{{"inputs", receiptRows},
                             {"decided_cells", QVariant::fromValue(job.receipt.decidedCells)},
                             {"total_cells", QVariant::fromValue(job.receipt.totalCells)}};
  // X2：冻结编图网格——点输入 compose 无栅格定网格，重放必须按保存时
  // 网格逐像元复现；缺此记录的历史成果才退回按规则重建。
  const QVariantMap composeGrid{
      {"cols", job.receipt.cols},
      {"rows", job.receipt.rows},
      {"extent", QVariantList{job.receipt.xMin, job.receipt.yMin,
                              job.receipt.xMax, job.receipt.yMax}}};
  QVariantMap extra{
      {"method",
       "priority-first-valid; categorical-nearest; threshold-classification"},
      {"input_layers", job.ids},
      {"computational_parent_versions", job.parents},
      {"ordered_inputs", job.params.value("ordered_inputs")},
      {"compose_receipts", composeReceipt},
      {"compose_grid", composeGrid},
      {"crs", job.params.value("crs")},
      {"thresholds", job.params.value("thresholds")},
      {"facies", job.facies}};
  // 受控重算血缘（X13）：计划携带的 recompute 段落到版本 extra——成果从此
  // 能反查「它替代了哪一版、依据哪份影响报告」；不作为算法参数存档。
  auto storedParams = job.params;
  if (storedParams.contains("recompute")) {
    extra.insert("recompute_provenance", storedParams.value("recompute"));
    storedParams.remove("recompute");
  }
  extra.insert("algorithm_parameters", storedParams);
  job.productId = record(
      job.outputPath, job.horizon, "composed_facies",
      tr("%1 综合相图").arg(job.horizon), "raster", job.parents, extra,
      error, {}, {}, &job.fingerprint);
  return !job.productId.isEmpty();
}

QString MappingWorkbench::compose(const QString &h, const QStringList &ids,
                                  const QVariantMap &params, QString *error) {
  ComposeJob job;
  if (!prepareCompose(h, ids, params, job, error))
    return {};
  if (!computeCompose(job, {}, {}) || !commitCompose(job, error)) {
    QFile::remove(job.outputPath);
    if (!job.error.isEmpty())
      fail(error, job.error);
    return {};
  }
  return polygonize(job.productId, error);
}

bool MappingWorkbench::startCompose(const QString &h, const QStringList &ids,
                                    const QVariantMap &params, QString *error) {
  if (!m_taskSvc)
    return !compose(h, ids, params, error).isEmpty();
  using Runner = paleo::jobs::JobRunner<ComposeJob>;
  if (!m_composeRunner)
    m_composeRunner = std::make_unique<Runner>(this);
  m_composeRunner->setTaskService(m_taskSvc);
  if (m_composeRunner->busy()) {
    fail(error, tr("已有综合编图任务，请等待或在任务中心取消"));
    return false;
  }
  auto job = std::make_shared<ComposeJob>();
  if (!prepareCompose(h, ids, params, *job, error))
    return false;
  Runner::Callbacks cb;
  cb.compute = [](ComposeJob &j, const paleo::jobs::CancelFn &cancel,
                  const paleo::jobs::ProgressFn &report) {
    return computeCompose(j, cancel,
                          [report](double p) { report(p, "build"); });
  };
  cb.commit = [this](ComposeJob &j, QString *err) {
    if (!commitCompose(j, err)) {
      emit errorOccurred(err ? *err : j.error);
      return false;
    }
    QString polyError;
    if (!startPolygonize(j.productId, &polyError))
      emit errorOccurred(polyError);
    return true;
  };
  cb.cleanup = [](ComposeJob &j) { QFile::remove(j.outputPath); };
  return m_composeRunner->start(tr("%1 综合编图").arg(h), job, cb) != nullptr;
}

void MappingWorkbench::publishFactorAlias(const QString &h, const QString &id) {
  const auto v = versionForLayer(id);
  auto d = declaration(id);
  const auto alias = v.extra.value("layer_id").toString();
  if (!alias.isEmpty()) {
    d.layerId = alias;
    d.title += tr(" · v%1").arg(v.versionNumber);
  }
  QString error;
  if (!d.layerId.isEmpty() && !m_layers->declare(d, &error)) {
    emit errorOccurred(error);
    return;
  }
  emit changed();
  emit productReady(h, d.layerId.isEmpty() ? id : d.layerId);
}

MappingWorkbench::ComposeJob::~ComposeJob() {
  if (!outputPath.isEmpty())
    QFile::remove(outputPath);
}
