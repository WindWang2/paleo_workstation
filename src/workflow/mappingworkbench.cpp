// 层：功能
#include "mappingworkbench.h"
#include "../domain/mappinghorizons.h"
#include "../io/constraintstore.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgisprojectservice.h"
#include "derivedassets.h"
#include "workflows.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <cmath>
#include <gdal.h>
#include <limits>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

namespace {
void fail(QString *error, const QString &text) {
  if (error)
    *error = text;
}
QString uid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString filePath(const QString &source) { return source.section('|', 0, 0); }
bool writeJson(const QString &path, const QVariant &value, QString *error) {
  QSaveFile f(path);
  const auto bytes = QJsonDocument::fromVariant(value).toJson();
  if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() ||
      !f.commit()) {
    fail(error, f.errorString());
    return false;
  }
  return true;
}
} // namespace
MappingWorkbench::MappingWorkbench(QgisLayerService *layers,
                                   QgisProcessingService *processing,
                                   QgisProjectService *project,
                                   ConstraintWorkflow *constraints,
                                   QObject *parent)
    : QObject(parent), m_layers(layers), m_processing(processing),
      m_project(project), m_constraints(constraints) {
  setPredictionService(new MockRemotePredictionService(this));
  connect(m_layers, &QgisLayerService::layerInstantiated, this,
          &MappingWorkbench::styleLayer);
  connect(m_layers, &QgisLayerService::layerDeclared, this,
          &MappingWorkbench::changed);
  connect(m_constraints, &ConstraintWorkflow::constraintAdded, this,
          [this](const QString &addedId) {
            if (m_importing || !m_catalog || !m_catalog->isOpen())
              return;
            // A drawn geometry is already committed by ConstraintWorkflow.
            // Capture the horizon from its newest record, not the UI's possibly
            // changed selection.
            const auto rows = m_constraints->loadConstraints();
            if (rows.isEmpty())
              return;
            QString h;
            for (const auto &row : rows)
              if (row.value("id").toString() == addedId)
                h = row.value("horizon").toString();
            if (h.isEmpty())
              return;
            QString error;
            snapshotConstraints(h, {}, &error);
            if (!error.isEmpty())
              emit errorOccurred(error);
          });
}
void MappingWorkbench::bindCatalog(DataCatalog *catalog, const QString &dir) {
  cancelPrediction();
  if (m_catalog)
    disconnect(m_catalog, nullptr, this, nullptr);
  for (const auto &name : m_constraints->dynamicPropertyNames())
    if (name.startsWith("paleo.constraint.snapshot."))
      m_constraints->setProperty(name.constData(), QVariant());
  m_catalog = catalog;
  m_dir = dir;
  if (catalog)
    connect(catalog, &DataCatalog::changed, this, &MappingWorkbench::changed);
  // Catalog is the durable recovery source if .qgz was not saved after a
  // result.
  if (catalog && catalog->isOpen())
    for (const auto &a : catalog->assets())
      for (const auto &v : catalog->versionsForAsset(a.id)) {
        const auto e = v.extra;
        const auto id = e.value("layer_id").toString();
        if (id.isEmpty() || !e.value("mapping_product").toBool())
          continue;
        LayerDeclaration d;
        d.layerId = id;
        d.horizon = e.value("horizon").toString();
        d.type = e.value("layer_type").toString();
        d.source = DataCatalog::resolvedVersionPath(dir, v) +
                   e.value("source_suffix").toString();
        d.group = e.value("group").toString();
        d.title =
            e.value("title").toString() + tr(" · v%1").arg(v.versionNumber);
        m_layers->declare(d);
        if (e.value("kind") == "constraint_snapshot" &&
            catalog->currentVersion(a.id).id == v.id)
          m_constraints->setProperty(
              ("paleo.constraint.snapshot." + d.horizon).toUtf8().constData(),
              filePath(d.source));
        const auto draftId = e.value("draft_id").toString();
        const auto working = e.value("working_path").toString();
        if (!draftId.isEmpty() && !working.isEmpty()) {
          CatalogVersion pathCheck;
          pathCheck.managed = true;
          pathCheck.path = working;
          const auto wp = DataCatalog::resolvedVersionPath(dir, pathCheck);
          if (!wp.isEmpty() && QFileInfo::exists(wp)) {
            d.layerId = draftId;
            d.source = wp + QStringLiteral("|layername=features");
            d.title = e.value("title").toString() + tr(" · 工作副本");
            m_layers->declare(d);
          }
        }
      }
  emit changed();
}
void MappingWorkbench::setPredictionService(RemotePredictionService *service) {
  cancelPrediction();
  if (m_remote)
    disconnect(m_remote, nullptr, this, nullptr);
  m_remote = service;
  if (!service)
    return;
  connect(service, &RemotePredictionService::completed, this,
          &MappingWorkbench::finishPrediction);
  connect(service, &RemotePredictionService::progress, this,
          [this](const QString &id, int p) {
            if (id == m_request.id)
              emit predictionProgress(p);
          });
  connect(service, &RemotePredictionService::failed, this,
          [this](const QString &id, const QString &message) {
            if (id != m_request.id)
              return;
            m_request = {};
            emit predictionBusyChanged(false);
            emit errorOccurred(message);
          });
}
bool MappingWorkbench::ready(const QString &h, QString *error) const {
  if (!m_catalog || !m_catalog->isOpen() || m_dir.isEmpty()) {
    fail(error, tr("请先打开可写工程"));
    return false;
  }
  if (h.isEmpty() || !DataCatalog::isSafePathSegment(h) ||
      !isMappingHorizon(h)) {
    fail(error, tr("请先选择有效编图层位"));
    return false;
  }
  return true;
}
QString MappingWorkbench::schemaVersion(const QString &h) const {
  if (m_catalog)
    for (const auto &a : m_catalog->assets())
      if (a.type == QLatin1String("facies_schema") &&
          a.displayName == QStringLiteral("facies-schema-%1").arg(h))
        return m_catalog->currentVersion(a.id).id;
  return {};
}
QVariantList MappingWorkbench::facies(const QString &h) const {
  const auto id = schemaVersion(h);
  if (!id.isEmpty())
    return m_catalog->versionById(id).extra.value("facies").toList();
  return {QVariantMap{{"code", 1},
                      {"name", tr("%1 Mock 相1").arg(h)},
                      {"color", "#E6C875"}},
          QVariantMap{{"code", 2},
                      {"name", tr("%1 Mock 相2").arg(h)},
                      {"color", "#81B99A"}},
          QVariantMap{{"code", 3},
                      {"name", tr("%1 Mock 相3").arg(h)},
                      {"color", "#97B4CE"}}};
}
bool MappingWorkbench::saveFacies(const QString &h, const QVariantList &items,
                                  QString *error) {
  if (!ready(h, error))
    return false;
  if (items.isEmpty() || items.size() > 32) {
    fail(error, tr("每个层位需要 1–32 个相类别"));
    return false;
  }
  QSet<int> codes;
  for (const auto &v : items) {
    const auto f = v.toMap();
    bool ok = false;
    int code = f.value("code").toInt(&ok);
    if (!ok || code < 1 || code > 32767 || codes.contains(code) ||
        f.value("name").toString().trimmed().isEmpty() ||
        !QColor(f.value("color").toString()).isValid()) {
      fail(error, tr("相编码须为不重复的正整数，名称与颜色不能为空"));
      return false;
    }
    codes.insert(code);
  }
  DerivedAssetRegistrar r(m_catalog, m_dir);
  auto st = r.stage("facies_schema", QStringLiteral("facies-schema-%1").arg(h),
                    "facies.json", error);
  if (!st.isValid() || !writeJson(st.absolutePath, items, error))
    return false;
  QStringList parents;
  if (!schemaVersion(h).isEmpty())
    parents << schemaVersion(h);
  QVariantList normalized;
  for (const auto &v : items) {
    auto f = v.toMap();
    f.insert("code", f.value("code").toInt());
    normalized << f;
  }
  const bool ok = r.commit(st, parents, "facies-schema",
                           {{"horizon", h}, {"facies", normalized}}, error);
  if (ok)
    emit faciesChanged(h);
  return ok;
}
QVariantList MappingWorkbench::inputs(const QString &kind) const {
  QVariantList out;
  if (!m_catalog || !m_catalog->isOpen())
    return out;
  if (kind == QLatin1String("wells")) {
    for (const auto &well : m_catalog->entities("well")) {
      QStringList parents;
      bool hasLog = false;
      for (const auto &link : m_catalog->linksForEntity(well.id))
        if (!link.unresolved &&
            QStringList{"well_log", "well_head", "tops", "time_depth"}.contains(
                link.role)) {
          const auto v = m_catalog->currentVersion(link.assetId);
          if (!v.id.isEmpty() &&
              QFileInfo::exists(DataCatalog::resolvedVersionPath(m_dir, v))) {
            parents << v.id;
            hasLog = hasLog || link.role == "well_log";
          }
        }
      if (hasLog && well.hasSurface && std::isfinite(well.surfaceX) &&
          std::isfinite(well.surfaceY))
        out << QVariantMap{{"id", well.id},
                           {"name", well.name},
                           {"x", well.surfaceX},
                           {"y", well.surfaceY},
                           {"parents", parents}};
    }
  } else
    for (const auto &a : m_catalog->assets()) {
      bool volume = a.type == QLatin1String("seismic") ||
                    a.type == QLatin1String("seismic_volume");
      for (const auto &l : m_catalog->linksForAsset(a.id))
        volume = volume ||
                 (!l.unresolved && l.role == QLatin1String("seismic_volume"));
      const auto v = m_catalog->currentVersion(a.id);
      if (volume && !v.id.isEmpty() &&
          QFileInfo::exists(DataCatalog::resolvedVersionPath(m_dir, v)))
        out << QVariantMap{{"id", a.id},
                           {"name", a.displayName},
                           {"parents", QStringList{v.id}}};
    }
  return out;
}
LayerDeclaration MappingWorkbench::declaration(const QString &id) const {
  for (const auto &d : m_layers->declared())
    if (d.layerId == id)
      return d;
  return {};
}
CatalogVersion MappingWorkbench::versionForLayer(const QString &id) const {
  CatalogVersion found;
  if (!m_catalog)
    return found;
  const auto d = declaration(id);
  const auto path = filePath(d.source);
  for (const auto &a : m_catalog->assets())
    for (const auto &v : m_catalog->versionsForAsset(a.id))
      if (v.extra.value("layer_id").toString() == id ||
          (!id.isEmpty() && v.extra.value("draft_id").toString() == id) ||
          (!path.isEmpty() &&
           DataCatalog::resolvedVersionPath(m_dir, v) == path))
        if (found.id.isEmpty() || v.versionNumber > found.versionNumber)
          found = v;
  return found;
}
QVariantList MappingWorkbench::products(const QString &h) const {
  QVariantList rows;
  for (const auto &d : m_layers->declared()) {
    if (!d.horizon.isEmpty() && d.horizon != h)
      continue;
    const auto v = versionForLayer(d.layerId);
    const auto e = v.extra;
    // Current aliases remain in the layer tree; the result list shows one row
    // per immutable version plus each writable draft.
    if (!e.value("layer_id").toString().isEmpty() &&
        e.value("layer_id").toString() != d.layerId &&
        !d.layerId.startsWith("draft."))
      continue;
    QStringList parentNames, childNames;
    if (m_catalog) {
      for (const auto &parentId : v.parentVersionIds) {
        const auto p = m_catalog->versionById(parentId);
        const auto asset = m_catalog->assetById(p.assetId);
        parentNames
            << tr("%1 · v%2")
                   .arg(p.extra.value("title", asset.displayName).toString())
                   .arg(p.versionNumber);
      }
      if (!v.id.isEmpty())
        for (const auto &asset : m_catalog->assets())
          for (const auto &child : m_catalog->versionsForAsset(asset.id))
            if (child.parentVersionIds.contains(v.id))
              childNames << tr("%1 · v%2")
                                .arg(child.extra
                                         .value("title", asset.displayName)
                                         .toString())
                                .arg(child.versionNumber);
    }
    rows << QVariantMap{{"id", d.layerId},
                        {"name", d.title.isEmpty() ? d.layerId : d.title},
                        {"horizon", d.horizon},
                        {"type", d.type},
                        {"version", v.versionNumber},
                        {"version_id", v.id},
                        {"parents", v.parentVersionIds},
                        {"parent_names", parentNames},
                        {"children", childNames},
                        {"kind", e.value("kind")},
                        {"mock", e.value("mock")},
                        {"path", d.source},
                        {"method", v.sourceUri},
                        {"parameters", e},
                        {"draft", d.layerId.startsWith("draft.")}};
  }
  return rows;
}
QString MappingWorkbench::record(const QString &path, const QString &h,
                                 const QString &kind, const QString &title,
                                 const QString &type,
                                 const QStringList &parents, QVariantMap extra,
                                 QString *error, const QString &suffix,
                                 const QString &assetKey) {
  DerivedAssetRegistrar r(m_catalog, m_dir);
  auto st = r.stage(
      kind,
      assetKey.isEmpty() ? QStringLiteral("%1-%2").arg(h, kind) : assetKey,
      QStringLiteral("result.%1").arg(QFileInfo(path).suffix()), error);
  if (!st.isValid())
    return {};
  const auto id = QStringLiteral("product.%1").arg(st.versionId);
  const auto group =
      kind.contains("prediction")   ? QStringLiteral("02_Prediction")
      : kind.contains("constraint") ? QStringLiteral("03_Constraints")
                                    : QStringLiteral("05_PaleoMap");
  extra.insert("mapping_product", true);
  extra.insert("layer_id", id);
  extra.insert("horizon", h);
  extra.insert("kind", kind);
  extra.insert("title", title);
  extra.insert("layer_type", type);
  extra.insert("source_suffix", suffix);
  extra.insert("group", group);
  extra.insert("created_at",
               QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
  for (const auto &parent : parents)
    if (m_catalog->versionById(parent).extra.value("mock").toBool())
      extra.insert("mock", true);
  if (!extra.contains("facies"))
    extra.insert("facies", facies(h));
  if (!r.commitExternal(st, path, parents,
                        extra.value("method", kind).toString(), extra, error))
    return {};
  LayerDeclaration d;
  d.layerId = id;
  d.horizon = h;
  d.type = type;
  d.source = st.absolutePath + suffix;
  d.group = group;
  d.title = title + tr(" · v%1").arg(st.versionNumber);
  if (!m_layers->declare(d, error))
    return {};
  if (QDir::cleanPath(path).startsWith(
          QDir(m_dir).filePath("artifacts/staging/"))) {
    QFile::remove(path);
    QFile::remove(path + "-wal");
    QFile::remove(path + "-shm");
    QFile::remove(path + ".aux.xml");
  }
  emit changed();
  emit productReady(h, id);
  if (m_project && !m_project->projectPath().isEmpty() &&
      !m_project->writeProject())
    emit errorOccurred(tr("图件已登记，但工程视图保存失败：%1")
                           .arg(m_project->lastErrors().join("；")));
  return id;
}
bool MappingWorkbench::predict(const QString &h, const QString &kind,
                               const QStringList &ids, QString *error) {
  if (!ready(h, error))
    return false;
  if (kind != "wells" && kind != "seismic") {
    fail(error, tr("未知预测类型"));
    return false;
  }
  if (busy() || !m_remote) {
    fail(error, tr("已有预测运行中，或未绑定预测服务"));
    return false;
  }
  if (ids.isEmpty() || (kind == "seismic" && ids.size() != 1)) {
    fail(error, tr("地震预测选择一个地震体；测井预测至少选择一口井"));
    return false;
  }
  if (schemaVersion(h).isEmpty() && !saveFacies(h, facies(h), error))
    return false;
  RemotePredictionRequest request;
  request.id = uid();
  request.horizon = h;
  request.kind = kind;
  request.facies = facies(h);
  const auto choices = inputs(kind);
  QSet<QString> seen;
  for (const auto &v : choices) {
    const auto row = v.toMap();
    if (!ids.contains(row.value("id").toString()))
      continue;
    seen.insert(row.value("id").toString());
    request.sourceVersionIds << row.value("parents").toStringList();
    if (kind == "wells")
      request.wells << row;
  }
  if (seen.size() != QSet<QString>(ids.cbegin(), ids.cend()).size()) {
    fail(error, tr("输入缺少可读源文件、测井曲线或有效井坐标，请检查数据管理"));
    return false;
  }
  request.sourceVersionIds << schemaVersion(h);
  request.sourceVersionIds.removeDuplicates();
  if (kind == "seismic") {
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
  m_request = request;
  emit predictionBusyChanged(true);
  m_remote->start(request);
  return true;
}
void MappingWorkbench::cancelPrediction() {
  if (m_remote)
    m_remote->cancel();
  bool was = busy();
  m_request = {};
  if (was)
    emit predictionBusyChanged(false);
}
void MappingWorkbench::finishPrediction(const RemotePredictionResult &result) {
  if (result.request.id != m_request.id || !m_catalog || !m_catalog->isOpen())
    return;
  const auto request = m_request;
  // Validate the response against the frozen request before publishing any
  // artifact.
  QSet<int> codes;
  for (const auto &f : request.facies)
    codes.insert(f.toMap().value("code").toInt());
  bool valid = result.request.horizon == request.horizon &&
               result.request.kind == request.kind;
  if (request.kind == "seismic") {
    valid = valid && result.cells.size() == request.rows * request.columns;
    for (int code : result.cells)
      valid = valid && codes.contains(code);
  } else {
    valid = valid && result.points.size() == request.wells.size();
    for (int i = 0; i < result.points.size() && valid; ++i) {
      auto p = result.points[i].toMap();
      auto w = request.wells[i].toMap();
      valid = codes.contains(p.value("facies_code").toInt()) &&
              p.value("id") == w.value("id") && p.value("x") == w.value("x") &&
              p.value("y") == w.value("y");
    }
  }
  QString error;
  if (!valid)
    error = tr("预测服务返回的网格、井集合或相编码与请求不一致，未发布");
  const auto stageDir = QDir(m_dir).filePath("artifacts/staging/" + request.id);
  QDir().mkpath(stageDir);
  const auto path = QDir(stageDir).filePath(
      request.kind == "seismic" ? "prediction.tif" : "prediction.geojson");
  if (error.isEmpty()) {
    const bool ok =
        request.kind == "seismic"
            ? MappingArtifactWriter::raster(path, result.cells, request.columns,
                                            request.rows, request.extent,
                                            &error)
            : MappingArtifactWriter::points(path, result.points, &error);
    if (ok)
      record(path, request.horizon, request.kind + "_prediction",
             tr("%1 %2预测%3")
                 .arg(request.horizon,
                      request.kind == "seismic" ? tr("地震相") : tr("测井相"),
                      result.mock ? tr("（Mock）") : QString()),
             request.kind == "seismic" ? "raster" : "vector",
             request.sourceVersionIds,
             {{"mock", result.mock},
              {"method", result.method},
              {"request_id", request.id},
              {"facies", request.facies},
              {"wells", request.wells},
              {"columns", request.columns},
              {"rows", request.rows},
              {"extent",
               QVariantList{request.extent.left(), request.extent.top(),
                            request.extent.width(), request.extent.height()}}},
             &error);
  }
  m_request = {};
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
QString MappingWorkbench::polygonize(const QString &id, QString *error) {
  const auto d = declaration(id);
  if (!ready(d.horizon, error))
    return {};
  if (d.type != "raster") {
    fail(error, tr("栅格转面需要选择相栅格"));
    return {};
  }
  auto *layer = m_layers->instantiate(id, error);
  if (!layer)
    return {};
  const auto parent = ensureInputVersion(id, error);
  if (parent.isEmpty())
    return {};
  const auto sourceVersion = m_catalog->versionById(parent);
  if (id.startsWith("factor.") ||
      sourceVersion.extra.value("kind").toString() == "single_factor_raster") {
    fail(error, tr("连续单因素须先按阈值分相，再转为相面"));
    return {};
  }
  QDir().mkpath(m_dir + "/artifacts/staging");
  const auto path = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  auto output = m_processing->run("paleo:paleo_facies_polygonize",
                                  {{"INPUT", QVariant::fromValue(layer)},
                                   {"OUTPUT", path},
                                   {"MIN_AREA", 0.0},
                                   {"SIMPLIFY", 0.0}},
                                  error);
  if (output.isEmpty())
    return {};
  return record(
      path, d.horizon, "facies_polygons", tr("%1 相面").arg(d.title), "vector",
      {parent},
      {{"facies", sourceVersion.extra.value("facies", facies(d.horizon))},
       {"method", "paleo:paleo_facies_polygonize"},
       {"mock", sourceVersion.extra.value("mock")}},
      error, "|layername=facies_polygons");
}
QString MappingWorkbench::copyForEditing(const QString &id,
                                         const QStringList &references,
                                         QString *error) {
  auto d = declaration(id);
  if (!ready(d.horizon, error))
    return {};
  if (d.type != "vector") {
    fail(error, tr("请先将相栅格转为矢量面，再创建编辑副本"));
    return {};
  }
  auto *layer =
      qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!layer)
    return {};
  if (layer->fields().indexOf("facies_code") < 0) {
    fail(error,
         tr("编辑底图需要 facies_code 相编码字段；请选择相面或预测相点"));
    return {};
  }
  QStringList parents;
  auto parent = ensureInputVersion(id, error);
  if (parent.isEmpty())
    return {};
  parents << parent;
  for (const auto &ref : references) {
    auto version = ensureInputVersion(ref, error);
    if (version.isEmpty())
      return {};
    parents << version;
  }
  const auto draft = QStringLiteral("draft.%1").arg(uid());
  const auto relative =
      QStringLiteral("artifacts/working/%1/features.gpkg").arg(draft);
  const auto path = QDir(m_dir).filePath(relative);
  QDir().mkpath(QFileInfo(path).absolutePath());
  if (!MappingArtifactWriter::vectorSnapshot(layer, path, error))
    return {};
  const auto source = m_catalog->versionById(parent);
  const QVariantMap extra{
      {"draft_id", draft},
      {"working_path", relative},
      {"facies", source.extra.value("facies", facies(d.horizon))},
      {"reference_layers", references},
      {"mock", source.extra.value("mock")},
      {"method", "manual-edit-copy"}};
  const auto snapshot =
      record(path, d.horizon, "edited_facies", tr("%1 人工编图").arg(d.horizon),
             "vector", parents, extra, error, "|layername=features", draft);
  if (snapshot.isEmpty())
    return {};
  d.layerId = draft;
  d.source = path + "|layername=features";
  d.title = tr("%1 人工编图 · 工作副本").arg(d.horizon);
  d.group = "05_PaleoMap";
  if (!m_layers->declare(d, error))
    return {};
  emit productReady(d.horizon, draft);
  return draft;
}
bool MappingWorkbench::saveEditingVersion(const QString &id, QString *error) {
  if (!id.startsWith("draft.")) {
    fail(error, tr("只有人工编辑副本可以保存编辑版本"));
    return false;
  }
  const auto d = declaration(id);
  if (!ready(d.horizon, error))
    return false;
  auto *layer =
      qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id, error));
  if (!layer)
    return false;
  if (layer->isEditable()) {
    fail(error, tr("请先点击“保存编辑”提交当前编辑，再保存图件版本"));
    return false;
  }
  const auto previous = versionForLayer(id);
  if (previous.id.isEmpty()) {
    fail(error, tr("找不到编辑副本的来源版本"));
    return false;
  }
  QSet<int> codes;
  for (const auto &f : previous.extra.value("facies").toList())
    codes.insert(f.toMap().value("code").toInt());
  auto features = layer->getFeatures();
  QgsFeature feature;
  while (features.nextFeature(feature)) {
    bool ok = false;
    const double code = feature.attribute("facies_code").toDouble(&ok);
    if (!ok || !std::isfinite(code) || code < 1 || code > 32767 ||
        code != std::round(code) || !codes.contains(int(code)) ||
        feature.geometry().isEmpty() || !feature.geometry().isGeosValid()) {
      fail(error, tr("工作副本含空几何、无效几何或未定义相编码。请修正属性 / "
                     "几何后再保存图件版本；历史版本未改变。"));
      return false;
    }
  }
  auto extra = previous.extra;
  extra.insert("method", "manual-edit-save");
  const auto snapshotPath = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  if (!MappingArtifactWriter::vectorSnapshot(layer, snapshotPath, error))
    return false;
  const auto output =
      record(snapshotPath, d.horizon, "edited_facies",
             previous.extra.value("title").toString(), "vector", {previous.id},
             extra, error, "|layername=features", id);
  return !output.isEmpty();
}
QString MappingWorkbench::snapshotConstraints(const QString &h,
                                              const QStringList &parents,
                                              QString *error) {
  if (!ready(h, error))
    return {};
  auto *store = m_constraints->constraintStore();
  if (!store || m_constraints->loadConstraints(h).isEmpty())
    return {};
  QString escaped = h;
  escaped.replace("'", "''");
  QgsVectorLayer layer(store->gpkgPath() +
                           "|layername=constraints|subset=horizon='" + escaped +
                           "'",
                       "constraints", "ogr");
  layer.setCrs(
      QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
  QDir().mkpath(m_dir + "/artifacts/staging");
  const auto path = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  if (!MappingArtifactWriter::vectorSnapshot(&layer, path, error))
    return {};
  QStringList upstream = parents;
  const auto old =
      m_constraints
          ->property(("paleo.constraint.snapshot." + h).toUtf8().constData())
          .toString();
  if (!old.isEmpty())
    upstream
        << DerivedAssetRegistrar(m_catalog, m_dir).parentVersionIdsFor({old});
  const auto id =
      record(path, h, "constraint_snapshot", tr("%1 约束过程").arg(h), "vector",
             upstream, {{"method", "constraint-snapshot"}}, error,
             "|layername=features");
  if (!id.isEmpty())
    m_constraints->setProperty(
        ("paleo.constraint.snapshot." + h).toUtf8().constData(),
        filePath(declaration(id).source));
  return id;
}
bool MappingWorkbench::importConstraints(const QString &h, const QString &path,
                                         QString *error) {
  if (!ready(h, error))
    return false;
  const auto geometries =
      MappingArtifactWriter::constraintGeometries(path, error);
  if (geometries.isEmpty())
    return false;
  // Copy the entire source into a managed GPKG (including shapefile sidecars).
  QgsVectorLayer source(path, "constraints", "ogr");
  QDir().mkpath(m_dir + "/artifacts/staging");
  const auto input = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  if (!MappingArtifactWriter::vectorSnapshot(&source, input, error))
    return false;
  const auto inputId =
      record(input, h, "constraint_source", tr("%1 导入约束").arg(h), "vector",
             {}, {{"source_file", path}, {"method", "constraint-import"}},
             error, "|layername=features");
  if (inputId.isEmpty())
    return false;
  m_importing = true;
  QStringList created;
  bool ok = true;
  for (const auto &wkt : geometries) {
    QString cid;
    if (!m_constraints->addConstraint(h, wkt.toString(), "line", -1, error,
                                      &cid)) {
      ok = false;
      break;
    }
    created << cid;
  }
  m_importing = false;
  if (!ok) {
    if (auto *store = m_constraints->constraintStore())
      for (const auto &cid : created)
        store->remove(cid);
    m_constraints->loadConstraints(h);
    return false;
  }
  return !snapshotConstraints(h, {versionForLayer(inputId).id}, error)
              .isEmpty();
}
bool MappingWorkbench::generateFactor(const QString &h, const QString &factor,
                                      const QVariantMap &params,
                                      QString *error) {
  if (!ready(h, error))
    return false;
  if (!m_constraints->loadConstraints(h).isEmpty() &&
      snapshotConstraints(h, {}, error).isEmpty())
    return false;
  const auto points = params.value("pointsLayerId").toString();
  auto options = params;
  if (!points.isEmpty()) {
    auto parent = ensureInputVersion(points, error);
    if (parent.isEmpty())
      return false;
    options.insert("parentVersionIds", QStringList{parent});
    const auto snapshot =
        m_catalog->versionById(parent).extra.value("layer_id").toString();
    if (!snapshot.isEmpty())
      options.insert("pointsLayerId", snapshot);
  }
  if (auto *samples = m_layers->instantiate(
          options.value("pointsLayerId").toString(), error)) {
    const auto extent = samples->extent();
    const double cell = options.value("cellSize", 100).toDouble();
    if (!std::isfinite(cell) || cell <= 0 ||
        std::ceil(std::max(extent.width() * 1.2, cell * 2) / cell) *
                std::ceil(std::max(extent.height() * 1.2, cell * 2) / cell) >
            4 * 1024 * 1024) {
      fail(error,
           tr("单因素网格超过 400 万像元，请增大网格间距或缩小输入范围"));
      return false;
    }
  } else
    return false;
  if (!m_constraints->generateFactor(h, factor, options, error))
    return false;
  const auto id = QStringLiteral("factor.%1.%2").arg(h, factor);
  const auto v = versionForLayer(id);
  auto d = declaration(id);
  d.layerId = v.extra.value("layer_id").toString();
  d.title += tr(" · v%1").arg(v.versionNumber);
  if (!d.layerId.isEmpty())
    m_layers->declare(d, error);
  emit changed();
  emit productReady(h, d.layerId.isEmpty() ? id : d.layerId);
  if (m_project && !m_project->projectPath().isEmpty())
    m_project->writeProject();
  return true;
}
bool MappingWorkbench::generateContours(const QString &h, const QString &id,
                                        double interval, QString *error) {
  if (!ready(h, error) || declaration(id).horizon != h) {
    fail(error, tr("等值线输入必须属于当前层位"));
    return false;
  }
  if (versionForLayer(id).extra.value("kind") != "single_factor_raster" &&
      !id.startsWith("factor.")) {
    fail(error, tr("请选择连续单因素栅格，类别相图不能生成等值线"));
    return false;
  }
  if (!m_constraints->generateContours(h, id, interval, error))
    return false;
  const auto prefix = QStringLiteral("factor.%1.").arg(h);
  const auto key = id.startsWith(prefix) ? id.mid(prefix.size()) : id;
  const auto output = QStringLiteral("contours.%1.%2").arg(h, key);
  const auto v = versionForLayer(output);
  auto d = declaration(output);
  d.layerId = v.extra.value("layer_id").toString();
  d.title += tr(" · v%1").arg(v.versionNumber);
  if (!d.layerId.isEmpty())
    m_layers->declare(d, error);
  emit changed();
  emit productReady(h, d.layerId.isEmpty() ? output : d.layerId);
  if (m_project && !m_project->projectPath().isEmpty())
    m_project->writeProject();
  return true;
}
void MappingWorkbench::styleLayer(const QString &id) {
  const auto v = versionForLayer(id);
  if (!v.extra.value("mapping_product").toBool())
    return;
  auto *layer = m_layers->layer(id);
  if (!layer)
    return;
  if (auto *vector = qobject_cast<QgsVectorLayer *>(layer))
    vector->setReadOnly(!id.startsWith("draft."));
  if (v.extra.value("kind") == "single_factor_raster")
    FactorStyleWriter::applyTo(qobject_cast<QgsRasterLayer *>(layer),
                               v.extra.value("factor_id").toString());
  if (v.extra.value("kind").toString().startsWith("constraint") ||
      v.extra.value("kind") == "single_factor_raster" ||
      v.extra.value("kind") == "contour_lines")
    return;
  MappingArtifactWriter::applyFaciesStyle(layer,
                                          v.extra.value("facies").toList());
  if (auto *vector = qobject_cast<QgsVectorLayer *>(layer))
    vector->setReadOnly(!id.startsWith("draft."));
}

QString MappingWorkbench::compose(const QString &h, const QStringList &ids,
                                  const QVariantMap &params, QString *error) {
  if (!ready(h, error))
    return {};
  if (ids.isEmpty()) {
    fail(error, tr("请选择编图输入，列表从上到下为优先级"));
    return {};
  }
  QList<QgsMapLayer *> layers;
  QStringList parents;
  QSet<QString> continuous;
  for (const auto &id : ids) {
    const auto d = declaration(id);
    if (d.horizon != h) {
      fail(error, tr("自动编图输入必须属于当前层位；其他层位可在参考窗口查看"));
      return {};
    }
    auto *layer = m_layers->instantiate(id, error);
    if (!layer)
      return {};
    auto version = ensureInputVersion(id, error);
    if (version.isEmpty())
      return {};
    parents << version;
    layers << layer;
    const auto v = m_catalog->versionById(version);
    if (id.startsWith("factor.") || v.extra.contains("factor_id") ||
        v.extra.value("kind").toString() == "single_factor_raster")
      continuous.insert(layer->id());
    else if (v.extra.contains("facies") &&
             v.extra.value("facies").toList() != facies(h)) {
      fail(error, tr("输入相分类与当前层位定义不同，请使用对应版本的相分类，或"
                     "仅作为参考"));
      return {};
    }
  }
  QList<double> thresholds;
  for (const auto &v : params.value("thresholds").toList()) {
    bool ok;
    double n = v.toDouble(&ok);
    if (!ok) {
      fail(error, tr("分相阈值必须为数字"));
      return {};
    }
    thresholds << n;
  }
  QDir().mkpath(m_dir + "/artifacts/staging");
  const auto path = m_dir + "/artifacts/staging/" + uid() + ".tif";
  if (!MappingArtifactWriter::composeRaster(path, layers, facies(h), thresholds,
                                            continuous, error))
    return {};
  if (schemaVersion(h).isEmpty() && !saveFacies(h, facies(h), error))
    return {};
  parents << schemaVersion(h);
  const auto id = record(
      path, h, "composed_facies", tr("%1 综合相图").arg(h), "raster", parents,
      {{"method",
        "priority-first-valid; categorical-nearest; threshold-classification"},
       {"input_layers", ids},
       {"thresholds", params.value("thresholds")},
       {"facies", facies(h)}},
      error);
  if (id.isEmpty())
    return {};
  return polygonize(id, error);
}
