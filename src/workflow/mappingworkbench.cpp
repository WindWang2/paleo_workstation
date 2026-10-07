// 层：功能
#include "mappingworkbench.h"
#include "../domain/faciescatalog.h"
#include "../domain/mappinghorizons.h"
#include "../domain/singlefactorrequest.h"
#include "../io/constraintstore.h"
#include "../qgis/facieshierarchyrenderer.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/layervocabulary.h"
#include "../qgis/mappingartifactwriter.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgisstyleservice.h"
#include "constraintimport.h"
#include "derivedassets.h"
#include "workflows.h"
#include "services/seismichorizoncluster.h"
#include <QFutureWatcher>
#include <QtConcurrent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <cmath>
#include <gdal.h>
#include <limits>
#include <qgscoordinatereferencesystem.h>
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
// 无大地基准的工程/局部直角米制 CRS（LOCAL_CS / ENGCRS，均映射到
// Qgis::CrsType::Engineering）与本工程局部网格等价；带基准的投影或
// 地理 CRS 仍拒绝。约束线与测区边界导入共用同一坐标门。
bool isLocalGridCrs(const QgsCoordinateReferenceSystem &c) {
  return c.isValid() && !c.isGeographic() &&
         c.mapUnits() == Qgis::DistanceUnit::Meters &&
         c.type() == Qgis::CrsType::Engineering;
}
bool acceptsLocalGrid(const QgsCoordinateReferenceSystem &c) {
  const auto local = QgsCoordinateReferenceSystem::fromWkt(
      DataCatalog::localGridCrsWkt());
  return c == local || isLocalGridCrs(c);
}
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
  // 方向51：不再自装测试替身（原先这里 setPredictionService(new <替身>(this))，
  // 产品里的远端预测就这样悄悄跑着假数据）。
  // 远端预测由装配根显式注入（app/aiwiring.cpp）；本地层位窗聚类是用户
  // 显式选择的 Mock 模式，产物记录真实反射特征来源与 Mock 类别解释。
  connect(m_layers, &QgisLayerService::layerInstantiated, this,
          &MappingWorkbench::styleLayer);
  connect(m_layers, &QgisLayerService::layerDeclared, this,
          &MappingWorkbench::changed);
  // 不在 constraintAdded 上自动快照：每画一条线出一个「约束过程·vN」版本
  // 会刷屏图层树。快照只在使用点做——generateFactor/importConstraints
  // 前置 snapshotConstraints（谱系语义：被计算的约束状态）；实时显示走
  // constraints.<horizon> 声明图层（直读 store，reload 即新）。
}
MappingWorkbench::~MappingWorkbench() {
  if (m_mockCancelled)
    m_mockCancelled->store(true);
}
void MappingWorkbench::bindCatalog(DataCatalog *catalog, const QString &dir) {
  cancelPrediction();
  if (m_catalog)
    disconnect(m_catalog, nullptr, this, nullptr);
  for (const auto &name : m_constraints->dynamicPropertyNames())
    if (name.startsWith("paleo.constraint.snapshot."))
      m_constraints->setProperty(name.constData(), QVariant());
  m_catalog = catalog;
  m_catalogSyncQueued = false;
  m_dir = dir;
  m_logVersion.clear();
  m_logCache = {};
  if (catalog)
    connect(catalog, &DataCatalog::changed, this, [this] {
      emit changed();
      if (m_catalogSyncQueued)
        return;
      m_catalogSyncQueued = true;
      const QPointer<DataCatalog> bound = m_catalog;
      QMetaObject::invokeMethod(
          this,
          [this, bound] {
            if (m_catalog != bound)
              return;
            m_catalogSyncQueued = false;
            synchronizeCatalogLayers();
            emit changed();
          },
          Qt::QueuedConnection);
    });
  synchronizeCatalogLayers();
  emit catalogBound();
  emit changed();
}
void MappingWorkbench::synchronizeCatalogLayers() {
  if (!m_catalog || !m_catalog->isOpen())
    return;
  QVector<LayerDeclaration> declarations;
  QString error;
  if (!m_layers->tryDeclared(&declarations, &error)) {
    emit errorOccurred(error);
    return;
  }
  QHash<QString, LayerDeclaration> known;
  for (const auto &d : declarations)
    known.insert(d.layerId, d);
  const auto declareMissing = [this, &known](const LayerDeclaration &d) {
    const auto old = known.value(d.layerId);
    if (old.source == d.source && old.horizon == d.horizon &&
        old.type == d.type && old.title == d.title && old.group == d.group)
      return;
    QString error;
    if (m_layers->declare(d, &error))
      known.insert(d.layerId, d);
    else
      emit errorOccurred(error);
  };
  // Catalog also receives products from the single-factor advanced tools.
  // Every saved version gets its own immutable layer, including products
  // whose original manifest_layer_id or layer_id is a moving current alias.
  for (const auto &a : m_catalog->assets())
    for (const auto &v : m_catalog->versionsForAsset(a.id)) {
      const auto e = v.extra;
      if (!e.value("mapping_product").toBool() ||
          (e.value("layer_type") != "vector" &&
           e.value("layer_type") != "raster"))
        continue;
      LayerDeclaration d;
      d.layerId = QStringLiteral("product.%1").arg(v.id);
      d.horizon = e.value("horizon").toString();
      d.type = e.value("layer_type").toString();
      d.source = DataCatalog::resolvedVersionPath(m_dir, v) +
                 e.value("source_suffix").toString();
      d.group = e.value("group").toString();
      d.title = e.value("title", a.displayName).toString() +
                tr(" · v%1").arg(v.versionNumber);
      declareMissing(d);
      if (e.value("kind") == "constraint_snapshot" &&
          m_catalog->currentVersion(a.id).id == v.id)
        m_constraints->setProperty(
            ("paleo.constraint.snapshot." + d.horizon).toUtf8().constData(),
            filePath(d.source));
      const auto draftId = e.value("draft_id").toString();
      const auto working = e.value("working_path").toString();
      if (!draftId.isEmpty() && !working.isEmpty()) {
        CatalogVersion pathCheck;
        pathCheck.managed = true;
        pathCheck.path = working;
        const auto wp = DataCatalog::resolvedVersionPath(m_dir, pathCheck);
        if (!wp.isEmpty() && QFileInfo::exists(wp)) {
          d.layerId = draftId;
          d.source = wp + QStringLiteral("|layername=features");
          d.title = e.value("title").toString() + tr(" · 工作副本");
          declareMissing(d);
        }
      }
    }
}
QString MappingWorkbench::layerForVersion(const QString &versionId,
                                          QString *error) {
  const auto v =
      m_catalog ? m_catalog->versionById(versionId) : CatalogVersion();
  if (v.id.isEmpty() || !v.extra.value("mapping_product").toBool() ||
      (v.extra.value("layer_type") != "vector" &&
       v.extra.value("layer_type") != "raster")) {
    fail(error, tr("所选版本不是可联动的编图或单因素图件"));
    return {};
  }
  const auto id = QStringLiteral("product.%1").arg(v.id);
  synchronizeCatalogLayers();
  if (declaration(id).layerId.isEmpty()) {
    fail(error, tr("所选图件版本尚未登记到图层目录"));
    return {};
  }
  return id;
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
void MappingWorkbench::setPredictionStatusHint(const QString &hint) {
  if (m_predictionHint == hint)
    return;
  m_predictionHint = hint;
  emit predictionStatusChanged();
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
  return FaciesCatalog::defaults();
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
    if (!f.value("texture").toString().isEmpty() &&
        FaciesCatalog::resourcePath(f.value("texture").toString()).isEmpty()) {
      fail(error, tr("纹理不在本项目纹理库中"));
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
      QString logVersion;
      for (const auto &link : m_catalog->linksForEntity(well.id))
        if (!link.unresolved &&
            QStringList{"well_log", "well_head", "tops", "time_depth"}.contains(
                link.role)) {
          const auto v = m_catalog->currentVersion(link.assetId);
          if (!v.id.isEmpty() &&
              QFileInfo::exists(DataCatalog::resolvedVersionPath(m_dir, v))) {
            parents << v.id;
            hasLog = hasLog || link.role == "well_log";
            if (link.role == "well_log" &&
                (logVersion.isEmpty() || link.isPrimary))
              logVersion = v.id;
          }
        }
      if (hasLog && well.hasSurface && std::isfinite(well.surfaceX) &&
          std::isfinite(well.surfaceY))
        out << QVariantMap{
            {"id", well.id},      {"name", well.name},
            {"x", well.surfaceX}, {"y", well.surfaceY},
            {"parents", parents}, {"log_version_id", logVersion}};
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
  if (id.startsWith("product.")) {
    const auto exact = m_catalog->versionById(id.mid(8));
    if (!exact.id.isEmpty())
      return exact;
  }
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
    if (e.value("mapping_product").toBool() &&
        d.layerId != QStringLiteral("product.%1").arg(v.id) &&
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
                        {"asset_id", v.assetId},
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
      : kind.contains("constraint") ? PaleoLayerVocabulary::kConstraintsGroup
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
  if (!m_remote && !mock) {
    // 远端预测需要装配服务；本地层位窗聚类独立运行。
    fail(error, tr("远端预测未配置，走本地引擎"));
    return false;
  }
  if (ids.isEmpty() || (seismicInput && ids.size() != 1)) {
    fail(error, tr("地震预测选择一个地震体；测井预测至少选择一口井"));
    return false;
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
  for (const auto &v : choices) {
    const auto row = v.toMap();
    if (!ids.contains(row.value("id").toString()))
      continue;
    seen.insert(row.value("id").toString());
    request.sourceVersionIds << row.value("parents").toStringList();
    if (kind == "wells") {
      auto well = row;
      const auto log = predictionLog(row.value("log_version_id").toString());
      double top = std::numeric_limits<double>::infinity(), bottom = -top;
      if (log.ok && !log.curves.isEmpty())
        for (double d : log.curves.first().values)
          if (std::isfinite(d)) {
            top = std::min(top, d);
            bottom = std::max(bottom, d);
          }
      const bool measured = std::isfinite(top) && bottom > top;
      well.insert("depth_top", measured ? top : 0.0);
      well.insert("depth_bottom", measured ? bottom : 120.0);
      well.insert("depth_mock", !measured);
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
        [seismicPath, horizonPath, cacheDir, codes, cancelled, source, inputSnapshot](QPromise<SeismicClusterResult> &promise) {
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
  emit predictionBusyChanged(true);
  m_remote->start(request);
  return true;
}
void MappingWorkbench::cancelPrediction() {
  if (m_mockCancelled)
    m_mockCancelled->store(true);
  else if (m_remote)
    m_remote->cancel();
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
  QSet<int> codes;
  for (const auto &f : request.facies)
    codes.insert(f.toMap().value("code").toInt());
  bool valid = result.request.horizon == request.horizon &&
               result.request.kind == request.kind;
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
      valid = codes.contains(p.value("facies_code").toInt()) &&
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
      double last = request.wells[i].toMap().value("depth_top").toDouble();
      valid = !intervals.isEmpty() && intervals.size() <= 10000;
      for (const auto &v : intervals) {
        const auto interval = v.toMap();
        bool topOk = false, bottomOk = false;
        double top = interval.value("top").toDouble(&topOk),
               bottom = interval.value("bottom").toDouble(&bottomOk);
        valid = valid && topOk && bottomOk && std::isfinite(top) &&
                std::isfinite(bottom) && bottom > top &&
                std::abs(top - last) < 1e-6 &&
                codes.contains(interval.value("code").toInt());
        last = bottom;
      }
      valid =
          valid &&
          std::abs(last -
                   request.wells[i].toMap().value("depth_bottom").toDouble()) <
              1e-6;
      for (const auto &key : {"log_version_id", "horizon", "depth_mock"})
        point.insert(key, key == QStringLiteral("horizon")
                              ? QVariant(request.horizon)
                              : request.wells[i].toMap().value(key));
      point.insert("predicted_intervals", point.value("facies_intervals"));
      const auto attrs =
          FaciesCatalog::attributes(request.facies, point.value("facies_code"));
      for (auto a = attrs.cbegin(); a != attrs.cend(); ++a)
        point.insert(a.key(), a.value());
      points[i] = point;
    }
  QString error;
  if (!valid)
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
    if (ok) {
      auto parameters = m_predictionParameters;
      const QVariantMap common =
             {{"mock", result.mock},
              {"method", result.method},
              {"window_half_ms", result.method == QLatin1String("mock-horizon-window-kmeans-v1") ? 12.0 : 0.0},
              {"request_id", request.id},
              {"facies", request.facies},
              {"wells", request.wells},
              {"columns", request.columns},
              {"rows", request.rows},
              {"extent", QVariantList{request.extent.left(), request.extent.top(),
                                      request.extent.width(), request.extent.height()}}};
      for (auto it = common.cbegin(); it != common.cend(); ++it)
        parameters.insert(it.key(), it.value());
      record(path, request.horizon, request.kind + "_prediction",
             tr("%1 %2预测%3")
                 .arg(request.horizon,
                      request.kind == "seismic" ? tr("地震相") : tr("测井相"),
                      result.mock ? tr("（Mock）") : QString()),
             request.kind == "seismic" ? "raster" : "vector",
             request.sourceVersionIds,
             parameters,
             &error);
    }
  }
  m_request = {};
  m_predictionParameters.clear();
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
  const auto sourceKind = sourceVersion.extra.value("kind").toString();
  const auto valueSource = sourceVersion.extra.value("value_source").toString();
  if (id.startsWith("factor.") ||
      sourceKind == QLatin1String("single_factor_raster") ||
      paleo::singlefactor::rejectsQuantitativeUse(sourceKind, valueSource)) {
    fail(error, paleo::singlefactor::rejectsQuantitativeUse(sourceKind, valueSource)
                    ? tr("解释性制图工作场不能参与分相或转面")
                    : tr("连续单因素须先按阈值分相，再转为相面"));
    return {};
  }
  QDir().mkpath(m_dir + "/artifacts/staging");
  const auto path = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  // 相栅格转面默认开平滑+聚合：逐像元聚类的椒盐噪点若直接多边形化会碎成
  // 噪声图斑（用户要求——「一定要做平滑和聚合，不然太细了」）。
  auto output = m_processing->run("paleo:paleo_facies_polygonize",
                                  {{"INPUT", QVariant::fromValue(layer)},
                                   {"OUTPUT", path},
                                   {"SMOOTH", 1},
                                   {"MIN_CELLS", 4},
                                   {"MIN_AREA", 0.0},
                                   {"SIMPLIFY", 0.0}},
                                  error);
  if (output.isEmpty())
    return {};
  {
    QgsVectorLayer outputLayer(path + "|layername=facies_polygons", "facies",
                               "ogr");
    if (!MappingArtifactWriter::syncFaciesAttributes(
            &outputLayer,
            sourceVersion.extra.value("facies", facies(d.horizon)).toList(),
            error))
      return {};
  }
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
  {
    QgsVectorLayer copy(path + "|layername=features", "copy", "ogr");
    if (!MappingArtifactWriter::syncFaciesAttributes(
            &copy, source.extra.value("facies", facies(d.horizon)).toList(),
            error))
      return {};
  }
  const QVariantMap extra{
      {"draft_id", draft},
      {"working_path", relative},
      {"facies", source.extra.value("facies", facies(d.horizon))},
      {"reference_layers", references},
      {"display_mode", displayMode(id)},
      {"hierarchy_model", "shared-polygon-partition-v1"},
      {"mock", source.extra.value("mock")},
      {"method", "manual-edit-copy"}};
  const auto editTitle = layer->fields().indexOf("facies_intervals") >= 0
                             ? tr("%1 测井相修订").arg(d.horizon)
                             : tr("%1 相图修订").arg(d.horizon);
  const auto snapshot =
      record(path, d.horizon, "edited_facies", editTitle, "vector", parents,
             extra, error, "|layername=features", draft);
  if (snapshot.isEmpty())
    return {};
  d.layerId = draft;
  d.source = path + "|layername=features";
  d.title = editTitle + tr(" · 工作副本");
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
  if (!FaciesHierarchyRenderer::validateTopology(layer, error))
    return false;
  const auto previous = versionForLayer(id);
  if (previous.id.isEmpty()) {
    fail(error, tr("找不到编辑副本的来源版本"));
    return false;
  }
  auto features = layer->getFeatures();
  QgsFeature feature;
  while (features.nextFeature(feature)) {
    bool ok = false;
    const double code = feature.attribute("facies_code").toDouble(&ok);
    if ((!feature.attribute("facies_code").isNull() &&
         (!ok || !std::isfinite(code) || code < 1 || code > 32767 ||
          code != std::round(code))) ||
        feature.geometry().isEmpty() || !feature.geometry().isGeosValid()) {
      fail(error, tr("工作副本含空几何、无效几何或非法相编码。请修正属性 / "
                     "几何后再保存图件版本；历史版本未改变。"));
      return false;
    }
  }
  if (!MappingArtifactWriter::syncFaciesAttributes(
          layer, previous.extra.value("facies").toList(), error))
    return false;
  auto extra = previous.extra;
  extra.insert("method", "manual-edit-save");
  extra.insert("display_mode", displayMode(id));
  extra.insert("hierarchy_model", "shared-polygon-partition-v1");
  QStringList evidenceParents{previous.id};
  auto evidenceFeatures = layer->getFeatures();
  QgsFeature evidenceFeature;
  while (evidenceFeatures.nextFeature(evidenceFeature)) {
    if (layer->fields().indexOf("facies_evidence") < 0)
      break;
    for (const auto &entry :
         QJsonDocument::fromJson(
             evidenceFeature.attribute("facies_evidence").toString().toUtf8())
             .toVariant()
             .toList()) {
      const auto source = entry.toMap().value("source_version").toString();
      if (!source.isEmpty() && !evidenceParents.contains(source))
        evidenceParents << source;
    }
  }
  const auto snapshotPath = m_dir + "/artifacts/staging/" + uid() + ".gpkg";
  if (!MappingArtifactWriter::vectorSnapshot(layer, snapshotPath, error))
    return false;
  const auto output =
      record(snapshotPath, d.horizon, "edited_facies",
             previous.extra.value("title").toString(), "vector",
             evidenceParents, extra, error, "|layername=features", id);
  if (!output.isEmpty())
    styleLayer(id);
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
                                         const QString &role, QString *error) {
  if (!ready(h, error))
    return false;
  // 与既有导入同一坐标门：约束几何必须落在本工程局部米制网格。
  {
    QgsVectorLayer probe(path, QStringLiteral("constraints"),
                         QStringLiteral("ogr"));
    if (probe.isValid() && !acceptsLocalGrid(probe.crs())) {
      fail(error, tr("约束线必须采用本工程的局部米制坐标；请先完成配准"));
      return false;
    }
  }
  const auto records =
      paleo::readConstraintImportFeatures(path, role, nullptr, error);
  if (records.isEmpty())
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
  QStringList created;
  bool ok = true;
  for (const auto &record : records) {
    QString cid;
    if (!m_constraints->addConstraint(h, record.wkt, record.type, -1, error,
                                      &cid, record.params)) {
      ok = false;
      break;
    }
    created << cid;
  }
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

// 测区范围/成图边界导入（WS-B/WS-C）：面图层快照进 artifacts/layers/，
// 声明为 horizon 无关的 00_Data 共享层（图层树布局器置顶）。layerId 按
// 文件名稳定——同名重导是原位更新（declare upsert 换源重挂），不同名
// 边界并存供切换。
bool MappingWorkbench::importBoundaryLayer(const QString &path,
                                           QString *error) {
  if (!m_catalog || !m_catalog->isOpen() || m_dir.isEmpty()) {
    fail(error, tr("请先打开可写工程"));
    return false;
  }
  QgsVectorLayer source(path, QStringLiteral("boundary"), QStringLiteral("ogr"));
  if (!source.isValid()) {
    fail(error, tr("边界文件无法读取：%1").arg(path));
    return false;
  }
  if (source.geometryType() != Qgis::GeometryType::Polygon) {
    fail(error, tr("测区边界必须是面图层（多边形）"));
    return false;
  }
  if (!acceptsLocalGrid(source.crs())) {
    fail(error, tr("测区边界必须采用本工程的局部米制坐标；请先完成配准"));
    return false;
  }
  QDir().mkpath(m_dir + "/artifacts/layers");
  const QString copy = QDir(m_dir).filePath(
      QStringLiteral("artifacts/layers/boundary-%1.gpkg").arg(uid().left(8)));
  if (!MappingArtifactWriter::vectorSnapshot(&source, copy, error))
    return false;

  QString stem = QFileInfo(path).completeBaseName();
  stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")),
               QStringLiteral("_"));
  LayerDeclaration decl;
  decl.layerId = QStringLiteral("boundary.%1").arg(
      stem.isEmpty() ? uid().left(8) : stem);
  decl.horizon.clear();          // 共享层——不随层位切换释放
  decl.type = QStringLiteral("vector");
  decl.source = copy;
  decl.group = QStringLiteral("00_Data");
  decl.title = QFileInfo(path).completeBaseName();
  if (decl.title.isEmpty())
    decl.title = tr("测区边界");
  if (!m_layers->declare(decl, error))
    return false;
  auto *layer = qobject_cast<QgsVectorLayer *>(
      m_layers->instantiate(decl.layerId, error));
  if (!layer)
    return false;
  QgisStyleService::applyBoundaryLayerStyle(layer);
  layer->triggerRepaint();
  emit changed();
  if (m_project && !m_project->projectPath().isEmpty() &&
      !m_project->writeProject())
    emit errorOccurred(tr("边界已导入，但工程视图保存失败：%1")
                           .arg(m_project->lastErrors().join("；")));
  return true;
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
  // structural_idw 按格网分辨率成图，没有 cellSize 契约——跳过像元数闸门
  //（其域规模由边界多边形 + GRID_RESOLUTION 控制，引擎自限）。
  const bool structural =
      options.value("method").toString() == QLatin1String("structural_idw");
  if (!structural) {
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
  }
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
  const auto contourExtra = versionForLayer(id).extra;
  const auto contourKind = contourExtra.value("kind").toString();
  const auto contourSource = contourExtra.value("value_source").toString();
  if (paleo::singlefactor::rejectsQuantitativeUse(contourKind, contourSource)) {
    fail(error, tr("解释性制图成果不能当作分析场提取等值线"));
    return false;
  }
  if (contourKind != QLatin1String("single_factor_raster") &&
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
int MappingWorkbench::labelMode(const QString &id) const {
  return m_project && m_project->project()
             ? m_project->project()->readNumEntry("paleo/faciesLabels", id, 3)
             : 3;
}
bool MappingWorkbench::setLabelMode(const QString &id, int mode,
                                    QString *error) {
  auto *layer = qobject_cast<QgsVectorLayer *>(m_layers->instantiate(id));
  if (!layer || layer->fields().indexOf("facies_code") < 0 || mode < 0 ||
      mode > 3) {
    fail(error, tr("请选择井相点图或矢量相面；栅格请先转为相面。"));
    return false;
  }
  MappingArtifactWriter::applyFaciesLabels(layer, mode);
  if (m_project && m_project->project())
    m_project->project()->writeEntry("paleo/faciesLabels", id, mode);
  emit changed();
  return true;
}
void MappingWorkbench::styleLayer(const QString &id) {
  // 约束线图层（constraints.<层位>）按 type 语义分类渲染——方向线/打断线
  // /软边界等在图面可辨（版本目录不入册，mapping_product 分支管不着）。
  if (id.startsWith(QLatin1String("constraints."))) {
    if (auto *v = qobject_cast<QgsVectorLayer *>(m_layers->layer(id)))
      QgisStyleService::applyConstraintLayerStyle(v);
    return;
  }
  const auto v = versionForLayer(id);
  if (!v.extra.value("mapping_product").toBool())
    return;
  auto *layer = m_layers->layer(id);
  if (!layer)
    return;
  layer->setCustomProperty("paleoAssetId", v.assetId);
  layer->setCustomProperty("paleoVersionId", v.id);
  const auto styledKind = v.extra.value("kind").toString();
  const auto styledSource = v.extra.value("value_source").toString();
  const bool analysisRaster =
      paleo::singlefactor::isAnalysisFactorRaster(styledKind, styledSource);
  const bool cartographic =
      paleo::singlefactor::rejectsQuantitativeUse(styledKind, styledSource);
  if (auto *vector = qobject_cast<QgsVectorLayer *>(layer)) {
    vector->setReadOnly(!id.startsWith("draft."));
    if (!vector->property("faciesSelectionAttached").toBool()) {
      vector->setProperty("faciesSelectionAttached", true);
      connect(vector, &QgsVectorLayer::selectionChanged, this,
              [this, id] { emit displayChanged(id); });
    }
    if (id.startsWith("draft.") &&
        !vector->property("faciesSyncAttached").toBool()) {
      vector->setProperty("faciesSyncAttached", true);
      vector->setCustomProperty("paleo/requireFaciesTopology",
                                vector->geometryType() ==
                                    Qgis::GeometryType::Polygon);
      connect(vector, &QgsVectorLayer::editCommandEnded, this, [this, id] {
        styleLayer(id);
        emit displayChanged(id);
      });
      connect(vector, &QgsVectorLayer::afterRollBack, this, [this, id] {
        styleLayer(id);
        emit displayChanged(id);
      });
      FaciesHierarchyRenderer::watchUndo(vector, this, [this, id] {
        if (m_layers->layer(id)) {
          styleLayer(id);
          emit displayChanged(id);
        }
      });
      connect(vector, &QgsVectorLayer::beforeCommitChanges, this,
              [this, vector, schema = v.extra.value("facies").toList()] {
                QString error;
                const bool ok =
                    FaciesHierarchyRenderer::validateTopology(vector, &error) &&
                    MappingArtifactWriter::syncFaciesAttributes(vector, schema,
                                                                &error);
                vector->setAllowCommit(ok);
                if (!ok)
                  emit errorOccurred(error);
              });
    }
  }
  if (analysisRaster || cartographic)
    FactorStyleWriter::applyTo(qobject_cast<QgsRasterLayer *>(layer),
                               v.extra.value("factor_id").toString());
  if (styledKind.startsWith(QLatin1String("constraint")) || analysisRaster ||
      cartographic || styledKind == QLatin1String("contour_lines")) {
    // 等值线（分析与制图绕行两族）：细灰线 + ELEV 标注——制图绕行仍
    // 挂 ELEV，同线型同标注规约。
    if ((styledKind == QLatin1String("contour_lines") ||
         styledKind == QLatin1String("single_factor_cartographic_contour")))
      if (auto *vector = qobject_cast<QgsVectorLayer *>(layer))
        QgisStyleService::applyContourLayerStyle(vector);
    return;
  }
  if (!layer->customProperty("paleo/faciesDisplayMode").isValid())
    layer->setCustomProperty("paleo/faciesDisplayMode",
                             v.extra.value("display_mode", "auto"));
  FaciesHierarchyRenderer::apply(layer, v.extra.value("facies").toList(),
                                 resolvedLevel(id));
  if (auto *vector = qobject_cast<QgsVectorLayer *>(layer)) {
    MappingArtifactWriter::applyFaciesLabels(vector, labelMode(id));
    vector->setReadOnly(!id.startsWith("draft."));
  }
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
    const auto inputKind = v.extra.value("kind").toString();
    const auto inputSource = v.extra.value("value_source").toString();
    if (paleo::singlefactor::rejectsQuantitativeUse(inputKind, inputSource)) {
      fail(error, tr("解释性制图工作场不能参与连续融合、分相或厚度统计"));
      return {};
    }
    if (id.startsWith("factor.") || v.extra.contains("factor_id") ||
        paleo::singlefactor::isAnalysisFactorRaster(inputKind, inputSource))
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
