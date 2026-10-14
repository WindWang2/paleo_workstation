// 层：功能
// 预测管线：任务行三件套 + predict/cancelPrediction/finishPrediction/ensureInputVersion。
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
QVariantList wellAttributeUpdates(const QVariantList &points, const QVariantList &schema,
                                  const QString &horizon, bool predicted, const QString &method = {}, const QString &jobId = {}) {
  QVariantList updates;
  for (const auto &v : points) {
    const auto point = v.toMap();
    if (point.value("depth_mock").toBool()) continue; // 合成深度只供旧结果展示，不能维护真实井段。
    const auto intervals = QJsonDocument::fromJson(point.value("facies_intervals").toString().toUtf8()).toVariant().toList();
    for (const auto &value : intervals) {
      const auto interval = value.toMap();
      const auto code = interval.value("code");
      const auto name = FaciesCatalog::find(schema, code).value("name").toString();
      const auto hierarchy = FaciesCatalog::attributes(schema, code);
      QVariantMap row{{"well_id", point.value("id")}, {"horizon", horizon},
        {"top_md", interval.value("top")}, {"base_md", interval.value("bottom")},
        {"facies_code", code}, {"facies", hierarchy.value("facies_name")},
        {"sub_facies", hierarchy.value("subfacies")},
        {"micro_facies", hierarchy.value("microfacies")}, {"facies_pattern", QStringLiteral("")}};
      if (predicted) { row.insert("predicted_facies", name); row.insert("model", point.value("prediction_model", method)); row.insert("job_id", point.value("prediction_job_id", jobId)); }
      if (predicted && point.contains("prediction_status")) {
        row.insert("prediction_status", point.value("prediction_status"));
        row.insert("prediction_reason", point.value("prediction_reason"));
      }
      updates << row;
    }
  }
  return updates;
}
} // namespace

void MappingWorkbench::setTaskService(PaleoTaskService *tasks) {
  m_taskSvc = tasks;
  if (m_wellRemote)
    m_wellRemote->setTaskService(tasks);
}
void MappingWorkbench::beginPredictionTask(const QString &title) {
  endPredictionTask();
  if (!m_taskSvc)
    return;
  auto *task = m_taskSvc->beginExternal(title);
  m_task = task;
  task->setNavigationContext({{"catalog_path", m_catalog ? m_catalog->catalogPath() : QString()},
      {"graph_focus", m_request.sourceVersionIds.isEmpty() ? QString() : QStringLiteral("version:") + m_request.sourceVersionIds.first()},
      {"request_id", m_request.id}, {"horizon", m_request.horizon}});
  connect(task, &PaleoTask::cancelAsked, this, [this, task] {
    if (m_task == task)
      cancelPrediction();
  });
}
void MappingWorkbench::endPredictionTask(const QString &error) {
  if (PaleoTask *task = m_task) {
    m_task = nullptr;
    if (error.isEmpty())
      task->reportBytes(100, 100);
    task->finishExternal(error);
  }
}
void MappingWorkbench::cancelPredictionTask() {
  if (PaleoTask *task = m_task) {
    m_task = nullptr; // 先摘：requestCancel 发的 cancelAsked 不再回调取消
    if (!task->cancelRequested())
      task->requestCancel();
    task->finishExternal();
  }
}

bool MappingWorkbench::predict(const QString &h, const QString &kind,
                               const QStringList &ids, QString *error,
                               const QString &horizonFile) {
  if (!ready(h, error))
    return false;
  const bool mock = kind == QLatin1String("seismic_mock");
  const bool seismicInput = kind == QLatin1String("seismic") || mock;
  if (kind != "wells" && !seismicInput) {
    fail(error, tr("未知预测类型"));
    return false;
  }
  if (busy()) {
    fail(error, tr("已有预测运行中"));
    return false;
  }
  if (!m_remote && !(kind == "wells" && m_wellRemote) && !mock) {
    // 远端预测需要装配服务；本地层位窗聚类独立运行。
    fail(error, tr("远端预测未配置，走本地引擎"));
    return false;
  }
  if (ids.isEmpty() || (seismicInput && ids.size() != 1)) {
    fail(error, tr("地震预测选择一个地震体；测井预测至少选择一口井"));
    return false;
  }
  if (kind == QLatin1String("wells")) {
    for (const auto &id : {WellAttributeStore::intervalLayerId(), WellAttributeStore::formationLayerId()}) {
      auto *attributes = qobject_cast<QgsVectorLayer *>(m_layers->layer(id));
      if (attributes && attributes->isEditable()) { fail(error, tr("测井属性表正在编辑，请先保存或放弃编辑")); return false; }
    }
  }
  if (schemaVersion(h).isEmpty() && !saveFacies(h, facies(h), error))
    return false;
  RemotePredictionRequest request;
  request.id = uid();
  request.horizon = h;
  request.kind = seismicInput ? QStringLiteral("seismic") : kind;
  request.facies = facies(h);
  const auto choices = inputs(kind);
  QSet<QString> seen;
  // 测井曲线的深度范围放到后台读（整份 LAS 解析，多井时主线程会卡住）；
  // 这里只在 catalog 所属线程解析出文件路径。
  QStringList logPaths;
  for (const auto &v : choices) {
    const auto row = v.toMap();
    if (!ids.contains(row.value("id").toString()))
      continue;
    seen.insert(row.value("id").toString());
    request.sourceVersionIds << row.value("parents").toStringList();
    if (kind == "wells") {
      auto well = row;
      const auto logVersion = row.value("log_version_id").toString();
      logPaths << (logVersion.isEmpty()
                       ? QString()
                       : DataCatalog::resolvedVersionPath(
                             m_dir, m_catalog->versionById(logVersion)));
      well.insert("vector_intervals", WellAttributeStore::rows(m_dir, m_layers, false, row.value("id").toString()));
      well.insert("vector_factors", WellAttributeStore::rows(m_dir, m_layers, true, row.value("id").toString(), h));
      well.insert("vector_formations", WellAttributeStore::rows(m_dir, m_layers, WellAttributeStore::Table::Formations, row.value("id").toString()));
      request.wells << well;
    }
  }
  if (seen.size() != QSet<QString>(ids.cbegin(), ids.cend()).size()) {
    fail(error, tr("输入缺少可读源文件、测井曲线或有效井坐标，请检查数据管理"));
    return false;
  }
  request.sourceVersionIds << schemaVersion(h);
  request.sourceVersionIds.removeDuplicates();
  if (seismicInput && !mock) {
    for (const auto &l : m_catalog->linksForAsset(ids.first()))
      if (!l.unresolved) {
        const auto entity = m_catalog->entityById(l.entityId);
        for (const auto &p : entity.corners) {
          QRectF pt(p.first, p.second, 0.001, 0.001);
          request.extent =
              request.extent.isNull() ? pt : request.extent.united(pt);
        }
      }
    if (request.extent.width() < 1 || request.extent.height() < 1) {
      for (const auto &d : m_layers->declared())
        if (d.horizon == h && d.type == "raster") {
          auto *r =
              qobject_cast<QgsRasterLayer *>(m_layers->instantiate(d.layerId));
          if (r) {
            auto e = r->extent();
            request.extent =
                QRectF(e.xMinimum(), e.yMinimum(), e.width(), e.height());
            break;
          }
        }
    }
    if (request.extent.width() < 1 || request.extent.height() < 1) {
      fail(error,
           tr("地震体缺少工区角点，且当前层位没有可用网格，请先完成空间配准"));
      return false;
    }
  }
  if (mock) {
    // 原始层位的道号/TWT 可直接提取；时间栅格只作为已有工程的兼容输入。
    QString horizonPath = horizonFile, horizonVersion;
    auto source = SeismicHorizonSource::Scatter;
    const QString selectedName = QFileInfo(horizonFile).completeBaseName().toUpper();
    if (!horizonFile.isEmpty() && isMappingHorizon(selectedName) && selectedName != h) {
      fail(error, tr("所选文件属于 %1，当前编图层位是 %2，请选择对应层位的文件").arg(selectedName, h));
      return false;
    }
    if (horizonPath.isEmpty()) {
      QMap<QString, QString> originals;
      CatalogVersion snapshot;
      for (const auto &asset : m_catalog->assets()) {
        if (asset.type != QLatin1String("horizon")) continue;
        bool linked = false;
        for (const auto &link : m_catalog->linksForAsset(asset.id))
          if (!link.unresolved && link.role == QLatin1String("horizon") &&
              m_catalog->entityById(link.entityId).name.compare(h, Qt::CaseInsensitive) == 0)
            linked = true;
        CatalogVersion raw;
        for (const auto &version : m_catalog->versionsForAsset(asset.id)) {
          const QString path = DataCatalog::resolvedVersionPath(m_dir, version);
          if (!QFileInfo(path).isFile()) continue;
          if (version.extra.value("horizon_input_snapshot").toBool() &&
              version.extra.value("horizon").toString() == h) {
            if (snapshot.id.isEmpty() || version.versionNumber > snapshot.versionNumber)
              snapshot = version;
          } else if (version.stage == QLatin1String("RAW") &&
                     (linked || QFileInfo(version.fileName).completeBaseName().compare(h, Qt::CaseInsensitive) == 0) &&
                     (raw.id.isEmpty() || version.versionNumber > raw.versionNumber)) {
            raw = version;
          }
        }
        if (!raw.id.isEmpty())
          originals.insert(DataCatalog::resolvedVersionPath(m_dir, raw), raw.id);
      }
      if (originals.size() > 1) {
        fail(error, tr("%1 有多个原始层位文件，请在「层位文件」选择本次使用的文件").arg(h));
        return false;
      }
      if (!originals.isEmpty()) {
        horizonPath = originals.firstKey(); horizonVersion = originals.first();
      } else if (!snapshot.id.isEmpty()) {
        horizonPath = DataCatalog::resolvedVersionPath(m_dir, snapshot);
        horizonVersion = snapshot.id;
      } else {
        source = SeismicHorizonSource::TimeRaster;
        for (const auto &d : m_layers->declared())
          if (d.horizon == h && d.type == QLatin1String("raster") &&
              d.layerId.startsWith(QLatin1String("horizon.")) &&
              QFileInfo(filePath(d.source)).isFile()) {
            if (horizonPath.isEmpty() || d.layerId == QStringLiteral("horizon.") + h) {
              horizonPath = filePath(d.source);
              horizonVersion = versionForLayer(d.layerId).id;
            }
          }
      }
    }
    if (!QFileInfo(horizonPath).isFile() || !QFileInfo(horizonPath).isReadable()) {
      fail(error, tr("未找到 %1 的可读层位数据，请在「层位文件」直接选择 DAT，或在数据管理中导入层位资料").arg(h));
      return false;
    }
    if (!horizonVersion.isEmpty() && source == SeismicHorizonSource::TimeRaster)
      request.sourceVersionIds << horizonVersion;
    request.sourceVersionIds.removeDuplicates();
    QVector<int> codes;
    for (const auto &f : request.facies)
      codes << f.toMap().value("code").toInt();
    const auto seismicVersion = m_catalog->currentVersion(ids.first());
    const QString seismicPath = DataCatalog::resolvedVersionPath(m_dir, seismicVersion);
    const QString cacheDir = QDir(m_dir).filePath("artifacts/cache/segy-index");
    std::shared_ptr<QTemporaryDir> inputSnapshot;
    if (source == SeismicHorizonSource::Scatter) {
      const QString staging = QDir(m_dir).filePath("artifacts/staging");
      QDir().mkpath(staging);
      inputSnapshot = std::make_shared<QTemporaryDir>(staging + "/horizon-input-XXXXXX");
      if (!inputSnapshot->isValid()) {
        fail(error, tr("无法创建层位输入快照目录"));
        return false;
      }
    }
    m_request = request;
    m_predictionParameters = {
      {"horizon_source_format", source == SeismicHorizonSource::Scatter ? "smi_xyz_inline_crossline" : "time_raster"},
      {"horizon_source_uri", QFileInfo(horizonPath).absoluteFilePath()}};
    beginPredictionTask(tr("智能预测 · %1 · 地震层位窗聚类（Mock）").arg(h));
    if (m_task)
      m_task->reportDetail(tr("后台读取地震道与层位窗"));
    m_mockCancelled = std::make_shared<std::atomic_bool>(false);
    auto cancelled = m_mockCancelled;
    auto *watcher = new QFutureWatcher<SeismicClusterResult>(this);
    connect(watcher, &QFutureWatcherBase::progressValueChanged, this,
            [this, request](int value) {
      if (m_request.id == request.id)
        emit predictionProgress(value);
    });
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, request, cancelled, inputSnapshot, horizonPath, horizonVersion] {
      const auto clustered = watcher->result();
      watcher->deleteLater();
      if (m_request.id != request.id || cancelled->load())
        return;
      if (!clustered.error.isEmpty()) {
        m_request = {};
        m_predictionParameters.clear();
        endPredictionTask(clustered.error);
        emit predictionBusyChanged(false);
        emit errorOccurred(clustered.error);
        return;
      }
      if (inputSnapshot) {
        // 保存本次实际使用的原始字节，形成可追溯输入；不生成时间栅格。
        DerivedAssetRegistrar registrar(m_catalog, m_dir);
        QString error;
        const auto st = registrar.stage("horizon", request.horizon + "-prediction-input",
                                        request.horizon + ".dat", &error);
        const QStringList parents = horizonVersion.isEmpty() ? QStringList() : QStringList{horizonVersion};
        if (!st.isValid() || !registrar.commitExternal(st, inputSnapshot->filePath("horizon.dat"),
            parents, horizonPath, {{"horizon", request.horizon}, {"horizon_input_snapshot", true},
                                  {"z_units", "ms"}}, &error)) {
          m_request = {};
          m_predictionParameters.clear();
          endPredictionTask(error);
          emit predictionBusyChanged(false);
          emit errorOccurred(error);
          return;
        }
        m_request.sourceVersionIds << st.versionId;
      }
      m_predictionParameters.insert("valid_cells", clustered.validCells);
      m_request.extent = clustered.extent;
      m_request.rows = clustered.rows;
      m_request.columns = clustered.columns;
      RemotePredictionResult result;
      result.request = m_request;
      result.cells = clustered.cells;
      result.mock = true;
      result.method = QStringLiteral("mock-horizon-window-kmeans-v1");
      finishPrediction(result);
    });
    emit predictionBusyChanged(true);
    watcher->setFuture(QtConcurrent::run(
        paleo::tasks::pool(m_taskSvc),
        [seismicPath, horizonPath, cacheDir, codes, cancelled, source,
         inputSnapshot](QPromise<SeismicClusterResult> &promise) {
          promise.setProgressRange(0, 100);
          QString inputPath = horizonPath;
          if (inputSnapshot) {
            inputPath = inputSnapshot->filePath("horizon.dat");
            const QFileInfo before(horizonPath);
            const qint64 sourceSize = before.size();
            const auto modified = before.lastModified();
            if (!QFile::copy(horizonPath, inputPath) ||
                sourceSize != QFileInfo(inputPath).size() ||
                sourceSize != QFileInfo(horizonPath).size() ||
                modified != QFileInfo(horizonPath).lastModified()) {
              SeismicClusterResult failed;
              failed.error = tr("层位文件复制失败或复制期间被修改，请重试");
              promise.addResult(failed);
              return;
            }
          }
          promise.addResult(clusterSeismicHorizon(seismicPath, inputPath, cacheDir, codes,
              [&promise](int value) { promise.setProgressValue(value); },
              [cancelled] { return cancelled->load(); }, source));
        }));
    return true;
  }
  m_mockCancelled.reset();
  m_predictionParameters.clear();
  m_request = request;
  beginPredictionTask(request.kind == "wells"
                          ? tr("智能预测 · %1 · 测井 %2 口井（每井一个预测相）")
                                .arg(h).arg(request.wells.size())
                          : tr("智能预测 · %1 · 地震相（预测服务）").arg(h));
  emit predictionBusyChanged(true);
  emit predictionDetailChanged(tr("准备预测输入"));
  if (request.kind == "wells" && m_wellRemote) {
    m_wellRemote->start(request);
    return true;
  }
  if (request.kind != "wells") {
    if (m_task)
      m_task->reportDetail(tr("已提交预测服务，等待返回"));
    m_remote->start(request);
    return true;
  }
  // 测井：后台读各井曲线深度范围 → 回主线程补进请求 → 提交预测服务。
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  m_prepareCancelled = cancelled;
  if (m_task)
    m_task->reportDetail(tr("后台读取 %1 口井的测井曲线").arg(logPaths.size()));
  auto *watcher = new QFutureWatcher<QVector<QPair<double, double>>>(this);
  connect(watcher, &QFutureWatcherBase::progressValueChanged, this,
          [this, id = request.id, total = logPaths.size()](int done) {
    if (m_task && m_request.id == id)
      m_task->reportDetail(tr("读取测井曲线 %1/%2").arg(done).arg(total));
  });
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, id = request.id, cancelled] {
    const auto ranges = watcher->result();
    watcher->deleteLater();
    if (m_request.id != id || cancelled->load())
      return;
    m_prepareCancelled.reset();
    for (qsizetype i = 0; i < m_request.wells.size() && i < ranges.size(); ++i) {
      auto well = m_request.wells[i].toMap();
      const bool measured = std::isfinite(ranges[i].first) &&
                            ranges[i].second > ranges[i].first;
      well.insert("depth_top", measured ? ranges[i].first : 0.0);
      well.insert("depth_bottom", measured ? ranges[i].second : 120.0);
      well.insert("depth_mock", !measured);
      m_request.wells[i] = well;
    }
    if (m_task)
      m_task->reportDetail(
          tr("已提交 %1 口井，等待预测服务返回").arg(m_request.wells.size()));
    m_remote->start(m_request);
  });
  watcher->setFuture(QtConcurrent::run(
      paleo::tasks::pool(m_taskSvc),
      [logPaths, cancelled](QPromise<QVector<QPair<double, double>>> &promise) {
        promise.setProgressRange(0, int(logPaths.size()));
        QVector<QPair<double, double>> ranges;
        for (const auto &path : logPaths) {
          if (cancelled->load())
            break;
          double top = std::numeric_limits<double>::infinity(), bottom = -top;
          QStringList names;
          QList<LasCurve> curves;
          if (!path.isEmpty() &&
              LasParser::parse(path, names, curves, nullptr) &&
              !curves.isEmpty())
            for (double d : curves.first().values)
              if (std::isfinite(d)) {
                top = std::min(top, d);
                bottom = std::max(bottom, d);
              }
          ranges.append({top, bottom});
          promise.setProgressValue(int(ranges.size()));
        }
        promise.addResult(ranges);
      }));
  return true;
}
void MappingWorkbench::cancelPrediction() {
  cancelPredictionTask();
  if (m_mockCancelled)
    m_mockCancelled->store(true);
  else if (m_prepareCancelled)
    m_prepareCancelled->store(true); // 还在读曲线，预测服务尚未提交
  else if (m_request.kind == "wells" && m_wellRemote)
    m_wellRemote->cancel();
  else if (m_remote)
    m_remote->cancel();
  m_prepareCancelled.reset();
  bool was = busy();
  m_request = {};
  m_predictionParameters.clear();
  if (was)
    emit predictionBusyChanged(false);
}
void MappingWorkbench::finishPrediction(const RemotePredictionResult &result) {
  if (result.request.id != m_request.id || !m_catalog || !m_catalog->isOpen())
    return;
  const auto request = m_request;
  // Validate the response against the frozen request before publishing any
  // artifact.
  const auto schema = result.facies.isEmpty() ? request.facies : result.facies;
  QSet<int> codes;
  bool schemaValid = !schema.isEmpty() && schema.size() <= 32;
  for (const auto &f : schema) {
    const auto row = f.toMap();
    bool ok = false;
    const int code = row.value("code").toInt(&ok);
    schemaValid &= ok && code > 0 && code <= 32767 && !codes.contains(code) &&
        !row.value("name").toString().trimmed().isEmpty() &&
        QColor(row.value("color").toString()).isValid();
    codes.insert(code);
  }
  for (int i = 0; i < request.facies.size(); ++i)
    schemaValid &= i < schema.size() && schema.value(i) == request.facies[i];
  // 只有真实测井适配器可返回明确的逐井跳过记录；仍严格核对井 ID 和坐标。
  const auto isSkipped = [&result](const QVariantMap &point) {
    return result.partialWellCoverage && !result.mock &&
        result.method == QLatin1String("remote-well-facies-api-v2") &&
        point.value("prediction_status").toString() == QStringLiteral("未预测") &&
        !point.value("prediction_reason").toString().trimmed().isEmpty() &&
        point.contains("facies_code") && point.value("facies_code").isNull();
  };
  bool valid = schemaValid && result.request.horizon == request.horizon &&
               result.request.kind == request.kind;
  if (result.currentFormationOnly)
    valid = valid && request.kind == "wells" && result.partialWellCoverage &&
        !result.mock && result.method == QLatin1String("remote-well-facies-api-v2");
  if (request.kind == "seismic") {
    valid = valid && result.cells.size() == request.rows * request.columns;
    for (int code : result.cells)
      valid = valid && (codes.contains(code) ||
          (result.mock && result.method == QLatin1String("mock-horizon-window-kmeans-v1") && code == -9999));
  } else {
    valid = valid && result.points.size() == request.wells.size();
    for (int i = 0; i < result.points.size() && valid; ++i) {
      auto p = result.points[i].toMap();
      auto w = request.wells[i].toMap();
      valid = (codes.contains(p.value("facies_code").toInt()) || isSkipped(p)) &&
              p.value("id") == w.value("id") && p.value("x") == w.value("x") &&
              p.value("y") == w.value("y");
    }
  }
  QVariantList points = result.points;
  if (request.kind == "wells")
    for (int i = 0; i < points.size() && valid; ++i) {
      auto point = points[i].toMap();
      const auto intervals =
          QJsonDocument::fromJson(
              point.value("facies_intervals").toString().toUtf8())
              .toVariant()
              .toList();
      if (isSkipped(point)) {
        const auto json = QJsonDocument::fromJson(point.value("facies_intervals").toString().toUtf8());
        valid = json.isArray() && json.array().isEmpty();
        point.insert("horizon", request.horizon);
        point.insert("depth_mock", false);
        point.insert("predicted_intervals", QStringLiteral("[]"));
        for (const auto &key : {"facies_name", "subfacies", "microfacies", "dominant_facies", "predicted_dominant"})
          point.insert(key, QVariant());
        point.insert("facies_label", tr("未预测"));
        point.insert("facies_scope", QStringLiteral("formation"));
        point.insert("formation", request.horizon);
        point.insert("coverage", 0.0);
        points[i] = point;
        continue;
      }
      const double rangeTop = result.partialWellCoverage
          ? point.value("prediction_depth_top").toDouble()
          : request.wells[i].toMap().value("depth_top").toDouble();
      const double rangeBase = result.partialWellCoverage
          ? point.value("prediction_depth_bottom").toDouble()
          : request.wells[i].toMap().value("depth_bottom").toDouble();
      if (result.currentFormationOnly) {
        const auto frozen = QJsonDocument::fromJson(point.value("formation_intervals").toString().toUtf8()).toVariant().toList();
        const auto formation = frozen.value(0).toMap();
        valid = frozen.size() == 1 &&
            formation.value("name").toString().compare(request.horizon, Qt::CaseInsensitive) == 0 &&
            formation.value("top").toDouble() == rangeTop &&
            formation.value("bottom").toDouble() == rangeBase;
      }
      double last = rangeTop;
      valid = valid && !intervals.isEmpty() && intervals.size() <= 10000;
      for (const auto &v : intervals) {
        const auto interval = v.toMap();
        bool topOk = false, bottomOk = false;
        double top = interval.value("top").toDouble(&topOk),
               bottom = interval.value("bottom").toDouble(&bottomOk);
        valid = valid && topOk && bottomOk && std::isfinite(top) &&
                std::isfinite(bottom) && bottom > top &&
                (result.partialWellCoverage ? top >= last - 1e-6 && bottom <= rangeBase + 1e-6
                                            : std::abs(top - last) < 1e-6) &&
                codes.contains(interval.value("code").toInt());
        last = bottom;
      }
      valid =
          valid &&
          std::isfinite(rangeTop) && std::isfinite(rangeBase) && rangeBase > rangeTop &&
          (result.partialWellCoverage || std::abs(last - rangeBase) < 1e-6);
      for (const auto &key : {"log_version_id", "horizon", "depth_mock"})
        point.insert(key, key == QStringLiteral("horizon")
                              ? QVariant(request.horizon)
                              : key == QStringLiteral("depth_mock") && result.partialWellCoverage
                                    ? QVariant(false) : request.wells[i].toMap().value(key));
      point.insert("predicted_intervals", point.value("facies_intervals"));
      const auto attrs =
          FaciesCatalog::attributes(schema, point.value("facies_code"));
      for (auto a = attrs.cbegin(); a != attrs.cend(); ++a)
        point.insert(a.key(), a.value());
      points[i] = point;
    }
  // 智能预测的井点相 = 该井在本层位地层（tops：本层位顶 → 下一层位顶）内
  // 厚度最大的相；统计后仅发布每井每地层的一条预测相。不能预测的井保留 NULL。
  QVariantList formationRows, skippedIntervalRows;
  int skippedWells = 0;
  if (valid && request.kind == "wells") {
    emit predictionProgress(96);
    emit predictionDetailChanged(tr("统计当前地层的预测相"));
    if (m_task)
      m_task->reportDetail(tr("按地层统计每井的相并写入图件"));
    formationRows = wellFormationUpdates(points, schema, true,
                                         result.method, request.id);
    for (auto &value : points) {
      auto point = value.toMap();
      const auto intervals =
          QJsonDocument::fromJson(point.value("facies_intervals").toString().toUtf8())
              .toVariant().toList();
      if (isSkipped(point)) {
        ++skippedWells;
        QVariantMap failedFormation;
        const auto frozen = QJsonDocument::fromJson(point.value("formation_intervals").toString().toUtf8()).toVariant().toList();
        for (const auto &v : frozen) {
          const auto f = v.toMap();
          if (f.value("name").toString().compare(request.horizon, Qt::CaseInsensitive) == 0)
            failedFormation = {{"well_id", point.value("id")}, {"formation", request.horizon},
                {"unit_type", f.value("unit_type")}, {"top_md", f.value("top")}, {"base_md", f.value("bottom")}};
        }
        // 缺少分层的井也清除工区中该层位的旧预测，不能沿用上次成功值。
        if (failedFormation.isEmpty()) {
          const auto old = WellAttributeStore::rows(m_dir, m_layers, WellAttributeStore::Table::Formations,
              point.value("id").toString(), request.horizon);
          if (!old.isEmpty()) failedFormation = old.first().toMap();
        }
        double top = 0, base = 0;
        FaciesCatalog::formationRange(failedFormation.value("top_md"), failedFormation.value("base_md"), &top, &base);
        if (std::isfinite(top) && std::isfinite(base) && base > top) {
          for (const auto &key : {"dominant_facies", "predicted_dominant", "facies_code", "dominant_ratio", "dominant_thickness_md", "facies_composition", "tie"})
            failedFormation.insert(key, QVariant());
          failedFormation.insert("thickness_md", base - top);
          failedFormation.insert("coverage", 0.0);
          failedFormation.insert("source", QStringLiteral("wells_prediction"));
          failedFormation.insert("model", QVariant());
          failedFormation.insert("job_id", request.id);
          failedFormation.insert("prediction_status", point.value("prediction_status"));
          failedFormation.insert("prediction_reason", point.value("prediction_reason"));
          formationRows << failedFormation;
          point.insert("formation_top", top);
          point.insert("formation_base", base);
        }
        auto oldIntervals = WellAttributeStore::rows(m_dir, m_layers, false,
            point.value("id").toString(), request.horizon);
        if (oldIntervals.isEmpty() && std::isfinite(top) && std::isfinite(base) && base > top)
          oldIntervals << QVariantMap{{"top_md", top}, {"base_md", base}};
        for (const auto &v : oldIntervals) {
          const auto old = v.toMap();
          skippedIntervalRows << QVariantMap{{"well_id", point.value("id")}, {"horizon", request.horizon},
              {"top_md", old.value("top_md")}, {"base_md", old.value("base_md")},
              {"predicted_facies", QVariant()}, {"model", QVariant()}, {"job_id", request.id},
              {"prediction_status", point.value("prediction_status")}, {"prediction_reason", point.value("prediction_reason")}};
        }
        value = point;
        continue;
      }
      QVariantMap formation;
      for (const auto &row : formationRows)
        if (row.toMap().value("well_id") == point.value("id") &&
            row.toMap().value("formation").toString().compare(
                request.horizon, Qt::CaseInsensitive) == 0)
          formation = row.toMap();
      double top = 0, base = 0;
      FaciesCatalog::formationRange(formation.value("top_md"),
                                    formation.value("base_md"), &top, &base);
      const int code = formation.isEmpty()
                           ? 0
                           : result.partialWellCoverage
                                 ? formation.value("facies_code").toInt()
                                 : FaciesCatalog::dominantCode(intervals, top, base);
      if (code) {
        point.insert("facies_code", code);
        point.insert("facies_scope", QStringLiteral("formation"));
        point.insert("formation", formation.value("formation"));
        point.insert("formation_top", top);
        point.insert("formation_base", base);
        point.insert("facies_ratio", formation.value("dominant_ratio"));
        if (result.partialWellCoverage)
          for (const auto &key : {"dominant_facies", "predicted_dominant", "dominant_thickness_md",
                                "dominant_ratio", "coverage", "tie", "facies_composition"})
            point.insert(key, formation.value(key));
        const auto attrs = FaciesCatalog::attributes(schema, code);
        for (auto a = attrs.cbegin(); a != attrs.cend(); ++a)
          point.insert(a.key(), a.value());
      } else {
        if (result.partialWellCoverage) valid = false;
        point.insert("facies_scope", QStringLiteral("well"));
      }
      value = point;
    }
  }
  if (valid && request.kind == "wells") {
    for (auto &point : points)
      point = FaciesCatalog::formationPrediction(point.toMap(), schema, request.horizon);
    for (auto &value : formationRows) {
      auto row = value.toMap();
      row.insert("facies_composition", QVariant());
      value = row;
    }
  }
  QString error;
  // 全井被跳过：产出的只会是一张「未预测」空相图——如实报错、不发布
  // 无信息产品（部分覆盖照常发布，未预测井随行标注跳过原因）。
  if (valid && request.kind == "wells" && !points.isEmpty() &&
      skippedWells == points.size()) {
    QStringList reasons;
    for (const auto &value : points) {
      const auto r = value.toMap().value("prediction_reason").toString();
      if (!r.isEmpty() && !reasons.contains(r))
        reasons << r;
    }
    error = tr("所选井均无法预测%1")
                .arg(reasons.isEmpty() ? QString()
                                       : tr("：%1").arg(reasons.join(tr("；"))));
    valid = false;
  }
  if (!valid && error.isEmpty())
    error = tr("预测服务返回的网格、井集合或相编码与请求不一致，未发布");
  const auto stageDir = QDir(m_dir).filePath("artifacts/staging/" + request.id);
  QDir().mkpath(stageDir);
  const auto path = QDir(stageDir).filePath(
      request.kind == "seismic" ? "prediction.tif" : "prediction.geojson");
  if (error.isEmpty()) {
    const bool ok = request.kind == "seismic"
                        ? MappingArtifactWriter::raster(
                              path, result.cells, request.columns, request.rows,
                              request.extent, &error)
                        : MappingArtifactWriter::points(path, points, &error);
    bool maintained = ok;
    if (ok && request.kind == "wells")
    {
      emit predictionProgress(97);
      emit predictionDetailChanged(tr("更新工区测井及地层预测信息"));
      // 井段批与地层批必须同成败——分开调时第二批失败会留半更新域
      //（#415），由事务化包装按批前快照恢复井段。
      maintained = WellAttributeStore::mergeIntervalsAndFormations(
          m_catalog, m_dir, m_layers,
          wellAttributeUpdates(points, schema, request.horizon, true, result.method, request.id) + skippedIntervalRows,
          formationRows, &error);
    }
    // 首次预测刚落盘的属性/地层表立刻进图层列表（无需重开工程或切页）。
    if (ok && request.kind == "wells")
      ensureWellBaseLayers();
    if (maintained) {
      emit predictionProgress(99);
      emit predictionDetailChanged(tr("登记并显示预测图件"));
      auto parameters = m_predictionParameters;
      if (result.currentFormationOnly) {
        parameters.insert("result_scope", QStringLiteral("current_formation"));
        parameters.insert("result_kind", QStringLiteral("formation_prediction"));
      }
      if (request.kind == "wells") {
        parameters.insert("predicted_well_count", points.size() - skippedWells);
        parameters.insert("skipped_well_count", skippedWells);
        // facies-ux 5：支撑井与模型名进证据链（逐井：预测相 / 跳过原因）。
        QVariantList supporting, skipped;
        QHash<QString, QVariantMap> requestedWells;
        for (const auto &entry : request.wells) requestedWells.insert(entry.toMap().value("id").toString(), entry.toMap());
        QStringList models;
        for (const auto &value : points) {
          const auto point = value.toMap();
          const auto model = point.value("prediction_model").toString();
          if (!model.isEmpty())
            for (const auto &name : model.split(QStringLiteral("；"), Qt::SkipEmptyParts))
              if (!models.contains(name))
                models << name;
          const auto requested = requestedWells.value(point.value("id").toString());
          const bool used = !point.value("facies_code").isNull() &&
              point.value("prediction_status").toString() != QStringLiteral("未预测");
          QVariantMap well{{"id", point.value("id")}, {"entity_id", point.value("id")},
                           {"name", point.value("name", requested.value("name"))},
                           {"horizon", request.horizon}, {"facies_code", point.value("facies_code")},
                           {"log_version_id", requested.value("log_version_id")},
                           {"source_version_ids", requested.value("parents")},
                           {"source_record_id", point.value("id")}, {"participating", used}};
          if (!used) {
            well.insert("reason", point.value("prediction_reason")); skipped << well;
          } else supporting << well;
        }
        parameters.insert("supporting_wells", supporting);
        parameters.insert("skipped_wells", skipped);
        if (!models.isEmpty())
          parameters.insert("model_names", models);
      }
      const QVariantMap common =
             {{"mock", result.mock},
              {"method", result.method},
              {"window_half_ms", result.method == QLatin1String("mock-horizon-window-kmeans-v1") ? 12.0 : 0.0},
              {"request_id", request.id},
              {"facies", schema},
              {"wells", request.wells},
              {"columns", request.columns},
              {"rows", request.rows},
              {"extent", QVariantList{request.extent.left(), request.extent.top(),
                                      request.extent.width(), request.extent.height()}}};
      for (auto it = common.cbegin(); it != common.cend(); ++it)
        parameters.insert(it.key(), it.value());
      auto sourceVersions = request.sourceVersionIds;
      if (request.kind == "wells" && result.partialWellCoverage)
        for (const auto &source : result.request.sourceVersionIds)
          if (!m_catalog->versionById(source).id.isEmpty()) sourceVersions << source;
      sourceVersions.removeDuplicates();
      const auto outputLayer = record(path, request.horizon, request.kind + "_prediction",
             tr("%1 %2预测%3")
                 .arg(request.horizon,
                      request.kind == "seismic" ? tr("地震相")
                          : tr("测井相"),
                      result.mock ? tr("（Mock）") : QString()),
             request.kind == "seismic" ? "raster" : "vector",
             sourceVersions,
             parameters,
             &error);
      const auto outputVersion = versionForLayer(outputLayer);
      if (m_task && !outputVersion.id.isEmpty())
        m_task->setNavigationContext({{"catalog_path", m_catalog->catalogPath()},
            {"version_id", outputVersion.id}, {"request_id", request.id}, {"horizon", request.horizon}});
    }
  }
  m_request = {};
  m_predictionParameters.clear();
  if (error.isEmpty()) {
    emit predictionDetailChanged(request.kind == "wells"
        ? tr("预测完成：成功 %1 口，未预测 %2 口").arg(points.size() - skippedWells).arg(skippedWells)
        : tr("预测完成"));
    emit predictionProgress(100);
  }
  endPredictionTask(error);
  emit predictionBusyChanged(false);
  if (!error.isEmpty())
    emit errorOccurred(error);
}
QString MappingWorkbench::ensureInputVersion(const QString &id,
                                             QString *error) {
  const auto existing = versionForLayer(id);
  if (!existing.id.isEmpty() && !id.startsWith("draft."))
    return existing.id;
  const auto d = declaration(id);
  if (d.layerId.isEmpty()) {
    fail(error, tr("未找到输入图件"));
    return {};
  }
  auto *layer = m_layers->instantiate(id, error);
  if (!layer)
    return {};
  QString path = filePath(d.source), suffix;
  if (auto *vl = qobject_cast<QgsVectorLayer *>(layer)) {
    QDir().mkpath(m_dir + "/artifacts/staging");
    path = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
    if (!MappingArtifactWriter::vectorSnapshot(vl, path, error))
      return {};
    suffix = "|layername=features";
  }
  QStringList parents;
  if (!existing.id.isEmpty())
    parents << existing.id;
  auto snapshot =
      record(path, d.horizon, "input_snapshot",
             tr("输入快照 %1").arg(d.title.isEmpty() ? id : d.title), d.type,
             parents, {}, error, suffix, QStringLiteral("input-%1").arg(id));
  return versionForLayer(snapshot).id;
}
