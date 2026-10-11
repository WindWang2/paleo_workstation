// 层：功能
// facies-ux 4：地震相 × 测井相预测一致性报告（编排：取样在此，统计在
// domain/faciesconsistency）。
#include "mappingworkbench.h"
#include "../domain/faciesconsistency.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprojectservice.h"
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QSaveFile>
#include <qgscoordinatetransform.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsproject.h>
#include <qgsrasterdataprovider.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>
#include <cmath>

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

QVariantMap MappingWorkbench::faciesConsistency(const QString &layerId,
                                                QString *error,
                                                const QString &counterpartId) {
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
  auto *raster = qobject_cast<QgsRasterLayer *>(seismic);
  auto *polygons = qobject_cast<QgsVectorLayer *>(seismic);
  if (polygons && polygons->fields().indexOf("facies_code") < 0) {
    fail(error, tr("地震相矢量缺少 facies_code 相编码字段"));
    return {};
  }
  QVector<paleo::facies::ConsistencySample> samples;
  QgsFeatureIterator it = wells->getFeatures();
  QgsFeature f;
  while (it.nextFeature(f)) {
    paleo::facies::ConsistencySample s;
    s.wellId = f.attribute("id").toString();
    s.wellName = wells->fields().indexOf("name") >= 0 ? f.attribute("name").toString()
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
    } else if (polygons) {
      QgsFeatureRequest req;
      req.setFilterRect(QgsRectangle(pt.x(), pt.y(), pt.x(), pt.y()));
      QgsFeatureIterator pit = polygons->getFeatures(req);
      QgsFeature pf;
      const QgsGeometry probe = QgsGeometry::fromPointXY(pt);
      while (pit.nextFeature(pf)) {
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
  out.insert("horizon", d.horizon);
  out.insert("well_layer", wellId);
  out.insert("well_title", declaration(wellId).title);
  out.insert("well_version", versionForLayer(wellId).id);
  out.insert("seismic_layer", seismicId);
  out.insert("seismic_title", declaration(seismicId).title);
  out.insert("seismic_version", versionForLayer(seismicId).id);
  out.insert("created_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  // 报告随工程留档（原子写、校验写入结果）——证据链可回看。
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
