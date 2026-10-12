// 层：功能
// facies-ux 4：地震相 × 测井相预测一致性报告（编排：取样在此，统计在
// domain/faciesconsistency）。
#include "../domain/faciesconsistency.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../services/paleotaskservice.h"
#include "mappingworkbench.h"
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <cmath>
#include <qgscoordinatetransform.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerfeatureiterator.h>

namespace {
void fail(QString *error, const QString &text) {
  if (error)
    *error = text;
}
bool isSeismicKind(const QString &kind) {
  return kind == QLatin1String("seismic_prediction") ||
         kind == QLatin1String("facies_polygons") ||
         kind == QLatin1String("composed_facies") ||
         kind == QLatin1String("edited_facies");
}
} // namespace

QVariantList MappingWorkbench::consistencyCandidates(const QString &h) const {
  QVariantList out;
  if (!m_catalog || !m_catalog->isOpen())
    return out;
  for (const auto &row : products(h)) {
    const auto r = row.toMap();
    const auto id = r.value("id").toString();
    if (r.value("horizon").toString() != h || h.isEmpty())
      continue;
    QString family;
    if (isWellPrediction(id))
      family = QStringLiteral("well");
    else if (isSeismicKind(r.value("kind").toString()))
      family = QStringLiteral("seismic");
    if (family.isEmpty())
      continue;
    out << QVariantMap{{"id", id},
                       {"name", r.value("name")},
                       {"family", family},
                       {"draft", r.value("draft")},
                       {"created_at", r.value("parameters").toMap().value("created_at")}};
  }
  return out;
}

QVariantMap MappingWorkbench::consistencyImpl(const QString &layerId,
                                              QString *error,
                                              const QString &counterpartId,
                                              bool background) {
  const auto d = declaration(layerId);
  if (!ready(d.horizon, error))
    return {};
  const auto candidates = consistencyCandidates(d.horizon);
  QString selectedFamily;
  for (const auto &c : candidates)
    if (c.toMap().value("id") == layerId)
      selectedFamily = c.toMap().value("family").toString();
  if (selectedFamily.isEmpty()) {
    fail(error, tr("请选择测井相点或地震相（相栅格 / 相面）结果"));
    return {};
  }
  QString other = counterpartId;
  if (other.isEmpty()) {
    // 另一侧：工作副本优先（人工修订最新），否则按登记时间取最新。
    QString bestTime;
    bool bestDraft = false;
    for (const auto &c : candidates) {
      const auto m = c.toMap();
      if (m.value("family").toString() == selectedFamily)
        continue;
      const bool draft = m.value("draft").toBool();
      const auto time = m.value("created_at").toString();
      if (other.isEmpty() || (draft && !bestDraft) ||
          (draft == bestDraft && time >= bestTime)) {
        other = m.value("id").toString();
        bestTime = time;
        bestDraft = draft;
      }
    }
  }
  if (other.isEmpty()) {
    fail(error, selectedFamily == "well"
                    ? tr("本层位尚无地震相结果（相栅格或相面），无法比对")
                    : tr("本层位尚无测井相预测结果，无法比对"));
    return {};
  }
  const QString wellId = selectedFamily == "well" ? layerId : other;
  const QString seismicId = selectedFamily == "well" ? other : layerId;
  auto *wells = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(wellId, error));
  auto *seismic = m_layers->instantiate(seismicId, error);
  if (!wells || !seismic)
    return {};
  auto schema = versionForLayer(wellId).extra.value("facies").toList();
  if (schema.isEmpty())
    schema = facies(d.horizon);

  // 井点 → 地震相 CRS 变换（同为工程局部网格时恒等）。
  QgsCoordinateTransform toSeismic(wells->crs(), seismic->crs(),
                                   m_project && m_project->project()
                                       ? m_project->project()->transformContext()
                                       : QgsProject::instance()->transformContext());
  auto *rasterLayer = qobject_cast<QgsRasterLayer *>(seismic);
  auto *polygons = qobject_cast<QgsVectorLayer *>(seismic);
  if (polygons && polygons->fields().indexOf("facies_code") < 0) {
    fail(error, tr("地震相矢量缺少 facies_code 相编码字段"));
    return {};
  }
  const QVariantMap metadata{
      {"horizon", d.horizon},
      {"well_layer", wellId},
      {"well_title", declaration(wellId).title},
      {"well_version", versionForLayer(wellId).id},
      {"seismic_layer", seismicId},
      {"seismic_title", declaration(seismicId).title},
      {"seismic_version", versionForLayer(seismicId).id}};
  auto wellSource = std::make_shared<QgsVectorLayerFeatureSource>(wells);
  auto polygonSource =
      polygons ? std::make_shared<QgsVectorLayerFeatureSource>(polygons)
               : nullptr;
  const QString rasterUri = rasterLayer ? rasterLayer->source() : QString();
  const auto rasterCrs = seismic->crs();
  const qint64 total = wells->featureCount();
  auto compute = [wellSource, polygonSource, rasterUri, rasterCrs, toSeismic,
                  schema, metadata,
                  total](const std::function<bool()> &cancel,
                         const std::function<void(int)> &progress,
                         QString *error) mutable -> QVariantMap {
    std::unique_ptr<QgsRasterLayer> raster;
    if (!rasterUri.isEmpty()) {
      raster =
          std::make_unique<QgsRasterLayer>(rasterUri, "consistency", "gdal");
      if (!raster->isValid()) {
        fail(error, QObject::tr("无法读取地震相栅格"));
        return {};
      }
      raster->setCrs(rasterCrs);
    }
    QVector<paleo::facies::ConsistencySample> samples;
    QgsFeatureIterator it = wellSource->getFeatures();
    QgsFeature f;
    qint64 done = 0;
    while (it.nextFeature(f)) {
      if (cancel && cancel()) {
        fail(error, QObject::tr("已取消"));
        return {};
      }
      if (progress && (++done % 32 == 0 || done == total))
        progress(total > 0 ? int(95 * done / total) : 0);
      paleo::facies::ConsistencySample s;
      s.wellId = f.attribute("id").toString();
      s.wellName = wellSource->fields().indexOf("name") >= 0
                       ? f.attribute("name").toString()
                       : s.wellId;
      if (s.wellName.isEmpty())
        s.wellName = s.wellId;
      bool ok = false;
      const int code = f.attribute("facies_code").toInt(&ok);
      if (ok && !f.attribute("facies_code").isNull())
        s.wellCode = code;
      if (!f.hasGeometry()) {
        samples << s;
        continue;
      }
      QgsPointXY pt = f.geometry().centroid().asPoint();
      s.x = pt.x();
      s.y = pt.y();
      try {
        if (toSeismic.isValid() && !toSeismic.isShortCircuited())
          pt = toSeismic.transform(pt);
      } catch (...) {
        samples << s;
        continue;
      }
      if (raster && raster->dataProvider()) {
        bool valid = false;
        const double v = raster->dataProvider()->sample(pt, 1, &valid);
        if (valid && std::isfinite(v))
          s.seismicCode = static_cast<int>(std::lround(v));
      } else if (polygonSource) {
        QgsFeatureRequest req;
        req.setFilterRect(QgsRectangle(pt.x(), pt.y(), pt.x(), pt.y()));
        QgsFeatureIterator pit = polygonSource->getFeatures(req);
        QgsFeature pf;
        const QgsGeometry probe = QgsGeometry::fromPointXY(pt);
        while (pit.nextFeature(pf)) {
          if (cancel && cancel())
            return {};
          if (!pf.hasGeometry() || !pf.geometry().intersects(probe))
            continue;
          bool cok = false;
          const int c = pf.attribute("facies_code").toInt(&cok);
          if (cok && !pf.attribute("facies_code").isNull()) {
            s.seismicCode = c;
            break;
          }
        }
      }
      samples << s;
    }
    if (samples.isEmpty()) {
      fail(error, tr("测井相结果没有井点"));
      return {};
    }
    const auto report = paleo::facies::computeConsistency(samples, schema);
    QVariantMap out = report.toVariant(schema);
    for (auto it = metadata.cbegin(); it != metadata.cend(); ++it)
      out.insert(it.key(), it.value());
    out.insert("created_at",
               QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return out;
  };
  if (background && m_taskSvc) {
    auto report = std::make_shared<QVariantMap>();
    auto temporary = std::make_shared<QTemporaryDir>();
    if (!temporary->isValid()) {
      fail(error, tr("无法创建报告暂存目录"));
      return {};
    }
    const QString tempPath = temporary->filePath("report.json");
    const auto catalog = m_catalog;
    const QString projectDir = m_dir;
    const quint64 sequence = m_catalog->mutationSeq();
    auto *task = m_taskSvc->start(
        tr("%1 测井相与地震相一致性比较").arg(d.horizon),
        [compute, report, temporary, tempPath](PaleoTask *running) mutable {
          QString error;
          *report = compute(
              [running] { return running->cancelRequested(); },
              [running](int p) { running->reportStage("build", p); }, &error);
          if (!error.isEmpty() || running->cancelRequested())
            return error;
          if (report->isEmpty())
            return QObject::tr("一致性比较未返回结果");
          QSaveFile file(tempPath);
          const auto bytes = QJsonDocument::fromVariant(*report).toJson(
              QJsonDocument::Indented);
          if (!file.open(QIODevice::WriteOnly) ||
              file.write(bytes) != bytes.size() || !file.commit())
            return file.errorString();
          return QString();
        });
    connect(task, &PaleoTask::finished, this,
            [this, task, catalog, projectDir, sequence, report, temporary,
             tempPath] {
              if (task->state() != PaleoTask::State::Succeeded) {
                if (task->state() == PaleoTask::State::Failed)
                  emit errorOccurred(task->errorText());
                return;
              }
              if (catalog != m_catalog || m_dir != projectDir || !catalog ||
                  sequence != catalog->mutationSeq() ||
                  task->session() != m_taskSvc->session()) {
                task->discardPublication();
                return;
              }
              const QString directory = projectDir + "/artifacts/reports";
              const QString path = directory + "/facies-consistency-" +
                                   QUuid::createUuid().toString(QUuid::Id128) +
                                   ".json";
              if (!QDir().mkpath(directory) || !QFile::rename(tempPath, path)) {
                task->failPublication(tr("一致性报告发布失败"));
                emit errorOccurred(task->errorText());
                return;
              }
              report->insert("report_path", path);
              emit consistencyReady(*report);
            });
    return {{"queued", true}};
  }
  QVariantMap out = compute({}, {}, error);
  if (out.isEmpty())
    return {};
  const QString dir = m_dir + "/artifacts/reports";
  if (!QDir().mkpath(dir)) {
    fail(error, tr("无法创建报告目录：%1").arg(dir));
    return {};
  }
  const QString path = QStringLiteral("%1/facies-consistency-%2-%3.json")
                           .arg(dir, d.horizon,
                                QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzz"));
  QSaveFile file(path);
  const QByteArray bytes = QJsonDocument::fromVariant(out).toJson(QJsonDocument::Indented);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    fail(error, tr("一致性报告写入失败：%1").arg(file.errorString()));
    return {};
  }
  out.insert("report_path", path);
  return out;
}

QVariantMap MappingWorkbench::faciesConsistency(const QString &id,
                                                QString *error,
                                                const QString &other) {
  return consistencyImpl(id, error, other, false);
}
bool MappingWorkbench::startFaciesConsistency(const QString &id, QString *error,
                                              const QString &other) {
  const auto result = consistencyImpl(id, error, other, true);
  if (!result.isEmpty() && !result.value("queued").toBool())
    emit consistencyReady(result);
  return !result.isEmpty();
}
