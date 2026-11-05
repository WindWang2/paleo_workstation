// 层：功能
#include "analysisresultsync.h"
#include "catalog/datacatalog.h"
#include "services/subintervalfactors.h"
#include "services/analysisunitcontext.h"
#include "paleo_data/service.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QCryptographicHash>
#include <algorithm>

namespace {
const QString geometryStaleReason=QStringLiteral("分析层段输入已变化，保留历史成果待重新计算");
struct CatalogStaleProjection {
  DataCatalog *catalog;
  QSet<QString> versions;
  void include(const QString &versionId) {
    if(versions.contains(versionId))return;
    const auto version=catalog->versionById(versionId);
    if(version.id.isEmpty()||version.stage!="DERIVED")return;
    versions.insert(versionId);
    for(const auto &child:catalog->downstreamClosure(versionId))
      if(child.stage=="DERIVED")versions.insert(child.id);
  }
  QString publish(const std::function<bool()> &cancelled) {
    QVector<CatalogOp> ops;auto ids=versions.values();std::sort(ids.begin(),ids.end());
    for(const auto &id:ids) {
      if(cancelled&&cancelled())return QStringLiteral("分析成果同步已取消");
      if(catalog->versionById(id).extra.value("stale").toBool())continue;
      for(const auto &patch:{qMakePair(QStringLiteral("stale"),QVariant(true)),qMakePair(QStringLiteral("staleReason"),QVariant(geometryStaleReason))}) {
        CatalogOp op;op.kind=CatalogOp::Kind::UpdateVersionExtra;op.id=id;op.extraKey=patch.first;op.extraValue=patch.second;ops.append(op);
      }
    }
    QString error;
    if(!catalog->isStaging())return catalog->applyJournal(ops,&error)?QString():error;
    const auto checkpoint=catalog->checkpointStaging();
    for(const auto &op:ops) {
      if((cancelled&&cancelled())||!catalog->updateVersionExtra(op.id,op.extraKey,op.extraValue,&error)) {
        catalog->rollbackStaging(checkpoint);return error.isEmpty()?QStringLiteral("分析成果同步已取消"):error;
      }
    }
    return {};
  }
};
struct GeometryCache {
  paleo::data::DataService &service;
  QHash<QString,paleo::data::StratigraphicScheme> heads;
  QHash<QByteArray,paleo::data::StratigraphicScheme> revisions;
  QHash<QByteArray,bool> changes;
  const paleo::data::StratigraphicScheme *source(const paleo::strat::UnitSelection &unit,QString &error) {
    const auto key=QJsonDocument(QJsonArray{unit.schemeId,unit.revision}).toJson(QJsonDocument::Compact);
    auto found=revisions.constFind(key);
    if(found==revisions.cend()) {
      paleo::data::StratigraphicScheme scheme;std::string detail;
      if(!service.readStratigraphicScheme(unit.schemeId.toStdString(),unit.revision.toStdString(),scheme,detail)) {
        error=QString::fromStdString(detail);return nullptr;
      }
      revisions.insert(key,std::move(scheme));found=revisions.constFind(key);
    }
    return &found.value();
  }
  QString changed(const paleo::strat::UnitSelection &unit,const paleo::data::SubIntervalResultBinding &binding,bool &outdated) {
    outdated=false;
    if(!heads.contains(unit.schemeId))return QStringLiteral("分析成果所用的层段方案不存在");
    const auto &head=heads[unit.schemeId];if(head.revision.value==unit.revision.toStdString())return {};
    const auto key=QJsonDocument(QJsonArray{unit.schemeId,unit.revision,QString::fromStdString(binding.kind),
      QString::fromStdString(binding.subInterval.value),QString::fromStdString(binding.interval.value),binding.aggregate,QString::number(binding.wellId)}).toJson(QJsonDocument::Compact);
    auto found=changes.constFind(key);
    if(found==changes.cend()) {
      QString error;const auto *old=source(unit,error);if(!old)return error;
      const auto fingerprint=paleo::data::framework::resultGeometryFingerprint(*old,binding);
      changes.insert(key,fingerprint.empty()||fingerprint!=paleo::data::framework::resultGeometryFingerprint(head,binding));found=changes.constFind(key);
    }
    outdated=found.value();return {};
  }
};
QVariantMap identity(DataCatalog *catalog, const CatalogVersion &v, QSet<QString> &seen) {
  if (seen.contains(v.id)) return {};
  seen.insert(v.id);
  auto unit = v.extra.value("unitSelection").toMap();
  if (unit.isEmpty()) unit = v.extra.value("algorithm_parameters").toMap().value("unitSelection").toMap();
  if (!unit.isEmpty()) return unit;
  QVariantMap inherited;
  for (const auto &id : v.parentVersionIds) {
    auto branch = seen;
    const auto candidate = identity(catalog, catalog->versionById(id), branch);
    if (candidate.isEmpty()) continue;
    if (!inherited.isEmpty() && inherited != candidate) return {};
    inherited = candidate;
  }
  return inherited;
}
QString resultKind(const QString &type) {
  if (type == "prediction_raster" || type == "onnx_prediction" || type == "seismic_prediction") return "seismic_facies";
  if (type == "well_litho_intervals" || type == "well_facies_prediction" || type == "wells_prediction") return "well_facies";
  if (type == "facies_polygons" || type == "edited_facies" || type == "composed_facies") return "map";
  if (type == "facies_fusion_raster") return "composite";
  if (type.contains("factor") || type.startsWith("thickness_") || type=="contour_lines") return "single_factor";
  return {};
}
bool stale(DataCatalog *catalog, const CatalogVersion &v, QSet<QString> &seen,const QSet<QString> &projected,bool ignoreOwnGeometry=false) {
  if (v.id.isEmpty() || seen.contains(v.id)) return true;
  seen.insert(v.id);
  if(projected.contains(v.id)&&!ignoreOwnGeometry)return true;
  if(v.extra.value("stale").toBool()&&(!ignoreOwnGeometry||v.extra.value("staleReason").toString()!=geometryStaleReason))return true;
  for (const auto &id : v.parentVersionIds) {
    auto branch = seen;
    const auto parent = catalog->versionById(id);
    // An edit's own immutable predecessor is lineage, not a replaceable input.
    if ((parent.assetId != v.assetId && catalog->currentVersion(parent.assetId).id != id) ||
        stale(catalog, parent, branch,projected)) return true;
  }
  return false;
}
QString syncWellChildren(paleo::data::DataService &ds,DataCatalog *catalog,const CatalogVersion &version,
    const paleo::strat::UnitSelection &unit,bool inputsOutdated,QSet<QString> &registered,QSet<QString> &invalidated,
    GeometryCache &geometry,CatalogStaleProjection &projection){
  const auto wells=version.extra.value("supporting_wells").toList();if(wells.isEmpty())return {};
  QString failure;const auto *scheme=geometry.source(unit,failure);if(!scheme)return failure;std::string error;
  for(const auto &value:wells){
    const auto row=value.toMap();const auto entity=row.value("entity_id",row.value("id",row.value("well_id"))).toString();if(entity.isEmpty())continue;
    const auto native=catalog->entityById(entity).extra.value("areaModelWellId").toLongLong();
    const auto range=paleo::strat::analysisWellRange(*scheme,unit,entity,native);if(!range.resolved)continue;
    const auto id="catalog."+version.id+"/well/"+QString::number(range.wellId);
    paleo::data::SubIntervalResultBinding binding,stored;
    binding.id=id.toStdString();binding.kind="well_facies";binding.schemeId=unit.schemeId.toStdString();
    binding.schemeRevision.value=unit.revision.toStdString();binding.interval.value=unit.intervalId.toStdString();
    binding.subInterval.value=unit.subIntervalId.toStdString();binding.aggregate=unit.aggregate;binding.wellId=range.wellId;
    bool geometryOutdated=false;failure=geometry.changed(unit,binding,geometryOutdated);if(!failure.isEmpty())return failure;
    const bool outdated=inputsOutdated||geometryOutdated;
    if(outdated)projection.include(version.id);
    if(registered.contains(id)){
      if(outdated&&!invalidated.contains(id)){
        if(!ds.invalidateSubIntervalResult(id.toStdString(),error))return QString::fromStdString(error);invalidated.insert(id);
      }
      continue;
    }
    binding.assetRef=version.id.toStdString();binding.status=outdated?"stale":"current";
    binding.inputFingerprint=QCryptographicHash::hash((version.id+id+version.sha256).toUtf8(),QCryptographicHash::Sha256).toHex().toStdString();
    auto detail=row;detail.insert("versionId",version.id);detail.insert("wellEntityId",entity);
    detail.insert("topMd",range.top);detail.insert("bottomMd",range.bottom);
    binding.detailJson=QJsonDocument(QJsonObject::fromVariantMap(detail)).toJson(QJsonDocument::Compact).toStdString();
    if(!ds.bindSubIntervalResult(binding,stored,error))return QString::fromStdString(error);registered.insert(id);
  }
  return {};
}

paleo::data::SubIntervalResultBinding resultBinding(DataCatalog *catalog,const CatalogVersion &version,
    const paleo::strat::UnitSelection &unit,const QString &kind) {
  paleo::data::SubIntervalResultBinding binding;
  binding.id=("catalog."+version.id).toStdString();binding.kind=kind.toStdString();
  binding.schemeId=unit.schemeId.toStdString();binding.schemeRevision.value=unit.revision.toStdString();
  binding.interval.value=unit.intervalId.toStdString();binding.subInterval.value=unit.subIntervalId.toStdString();
  binding.aggregate=unit.aggregate;binding.assetRef=version.id.toStdString();
  const auto well=version.extra.value("wellEntityId").toString();
  if(version.extra.value("analysisWellId").toLongLong()>0)binding.wellId=version.extra.value("analysisWellId").toLongLong();
  else if(version.extra.value("modelWellId").toLongLong()>0)binding.wellId=version.extra.value("modelWellId").toLongLong();
  else if(!well.isEmpty()) {
    const auto native=version.extra.value("modelWellId",catalog->entityById(well).extra.value("areaModelWellId")).toLongLong();
    binding.wellId=native>0?native:paleo::strat::stableWellKey(well);
  }
  return binding;
}

}

paleo::strat::UnitSelection analysisResultUnitSelection(DataCatalog *catalog,const CatalogVersion &version) {
  if(!catalog)return {};
  QSet<QString> seen;return paleo::strat::fromMap(identity(catalog,version,seen));
}

QString syncAnalysisResults(DataCatalog *catalog, const QString &projectDir,
                            const std::function<bool()> &cancelled) {
  if (!catalog || !catalog->isOpen() || catalog->refusesWrites() || projectDir.isEmpty()) return {};
  // Legacy projects remain readable; do not create a second area model.
  if (!QFileInfo::exists(projectDir + "/artifacts/metadata/area-model/data.sqlite")) return {};
  paleo::data::DataService ds;
  QString error;
  if (!paleo::strat::openProjectDataService(projectDir, ds, &error)) return error;
  QSet<QString> registered;
  QSet<QString> invalidated;
  GeometryCache geometry{ds};CatalogStaleProjection projection{catalog};
  for (const auto &scheme : paleo::strat::listProjectSchemes(ds, &error)) {
    if (cancelled && cancelled()) return {};
    paleo::data::StratigraphicScheme head;
    if (!paleo::strat::loadProjectScheme(ds, scheme.id, scheme.headRevision, head, &error)) return error;
    geometry.heads.insert(scheme.id, head);
    std::vector<paleo::data::SubIntervalResultBinding> bindings;
    std::string detail;
    if (!ds.listSubIntervalResults(scheme.id.toStdString(), {}, bindings, detail))
      return QString::fromStdString(detail);
    for (const auto &b : bindings) {
      const auto id = QString::fromStdString(b.id);
      registered.insert(id);
      if (b.status == "stale" || b.status == "legacy_unresolved") invalidated.insert(id);
      if(b.status=="stale" && id.startsWith("catalog."))projection.include(QString::fromStdString(b.assetRef));
      if (!invalidated.contains(id) && id.startsWith("catalog.") && catalog->versionById(id.mid(8).section('/',0,0)).id.isEmpty()) {
        if (!ds.invalidateSubIntervalResult(b.id, detail)) return QString::fromStdString(detail);
        invalidated.insert(id); // #890：记账，后续同 id 不再重复 invalidate
      }
    }
  }
  if (!error.isEmpty()) return error;
  for (const auto &v : catalog->versions()) {
    if (cancelled && cancelled()) return {};
    const QString kind = resultKind(catalog->assetById(v.assetId).type);
    if (kind.isEmpty() || v.stage == "RAW") continue;
    const auto unit = analysisResultUnitSelection(catalog,v);
    if (!unit.isValid() || unit.schemeId.isEmpty() || unit.revision.isEmpty()) continue;
    auto b=resultBinding(catalog,v,unit,kind);
    const QString id=QString::fromStdString(b.id);
    QSet<QString> dependencies;
    bool geometryOutdated=false;
    error=geometry.changed(unit,b,geometryOutdated);if(!error.isEmpty())return error;
    const bool outdated=stale(catalog,v,dependencies,projection.versions)||geometryOutdated;
    if(kind=="well_facies"){
      // A batch's own geometry invalidation does not invalidate unaffected well
      // children. Re-evaluate their actual MD windows; upstream input changes
      // still invalidate every child that consumed that input version.
      QSet<QString> inputs;
      const bool inputsOutdated=stale(catalog,v,inputs,projection.versions,true);
      const auto children=syncWellChildren(ds,catalog,v,unit,inputsOutdated,registered,invalidated,geometry,projection);if(!children.isEmpty())return children;
    }
    if(outdated)projection.include(v.id);
    if (registered.contains(id)) {
      if (outdated && !invalidated.contains(id)) {
        std::string failure;
        if (!ds.invalidateSubIntervalResult(id.toStdString(), failure)) return QString::fromStdString(failure);
        invalidated.insert(id);
      }
      continue;
    }
    paleo::data::SubIntervalResultBinding stored;
    QVariantList inputs;
    for (const auto &parent : v.parentVersionIds)
      inputs.append(QVariantMap{{"id", parent}, {"sha256", catalog->versionById(parent).sha256}});
    const auto fingerprint = QJsonDocument(QJsonObject{{"unit", QJsonObject::fromVariantMap(paleo::strat::toMap(unit))},
        {"inputs", QJsonArray::fromVariantList(inputs)}, {"parameters", QJsonObject::fromVariantMap(v.extra)}}).toJson(QJsonDocument::Compact);
    b.inputFingerprint = QCryptographicHash::hash(fingerprint, QCryptographicHash::Sha256).toHex().toStdString();
    b.status = outdated ? "stale" : "current";
    QVariantMap detail = v.extra;
    detail.insert("assetId", v.assetId); detail.insert("versionId", v.id);
    detail.insert("parentVersionIds", v.parentVersionIds);
    b.detailJson = QJsonDocument(QJsonObject::fromVariantMap(detail)).toJson(QJsonDocument::Compact).toStdString();
    std::string failure;
    if (!ds.bindSubIntervalResult(b, stored, failure)) return QString::fromStdString(failure);
    registered.insert(id);
    if(outdated)invalidated.insert(id);
  }
  // A parent can appear after its descendants in catalog storage order. Finish
  // the SDK projection from the indexed downstream closure before publishing.
  for(const auto &version:projection.versions) {
    if(cancelled&&cancelled())return {};
    const auto id="catalog."+version;
    if(!registered.contains(id)||invalidated.contains(id))continue;
    std::string failure;
    if(!ds.invalidateSubIntervalResult(id.toStdString(),failure))return QString::fromStdString(failure);
    invalidated.insert(id);
  }
  return projection.publish(cancelled);
}
