// 层：数据
#include "documentsourceprojection.h"
#include "datacatalog.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <utility>

namespace paleo::document {
namespace {
bool safeModelPath(const QString &path) {
    if(path.isEmpty() || QDir::isAbsolutePath(path) || path.contains('\\'))return false;
    for(const auto &segment:path.split('/'))if(!DataCatalog::isSafePathSegment(segment))return false;
    return true;
}
SourceRecord recordFor(const CatalogVersion &version,const QString &documentId,const QString &project,QString *error) {
    SourceRecord record;record.assetId=version.assetId;record.versionId=version.id;
    record.originalPath=version.sourceUri;record.managed=version.managed;
    record.path=version.managed ? QDir(project).filePath(version.path) : version.path;
    record.fingerprint=SourceFingerprint::fromStored(version.sha256);
    record.ref.documentId=documentId;record.ref.fileRevisionId=record.fingerprint.valid()?record.fingerprint.stored():"unverified:"+version.id;
    const auto source=version.extra.value("areaModelSourceId").toLongLong();
    const auto native=version.extra.value("nativeModel").toMap();
    if(source>0) {
        record.ref.modelStoreId="area-model";record.ref.sourceId=source;
        record.modelDirectory=QDir(project).filePath("artifacts/metadata/area-model");
    } else if(native.value("sourceId").toLongLong()>0) {
        const auto database=native.value("path").toString();
        record.ref.modelStoreId=native.value("storeIdentity",database).toString();record.ref.sourceId=native.value("sourceId").toLongLong();
        if(!safeModelPath(database) || QFileInfo(database).fileName()!=QLatin1String("data.sqlite") || !safeModelPath(record.ref.modelStoreId)) {
            if(error)*error=QStringLiteral("原生来源模型路径或身份无效");
        } else record.modelDirectory=QFileInfo(QDir(project).filePath(database)).absolutePath();
    } else record.ref.modelStoreId="catalog:"+version.assetId;
    record.state.model=record.ref.sourceId>0?ModelAvailability::Registered:ModelAvailability::Unbound;
    if(record.ref.sourceId>0 && record.modelDirectory.isEmpty()) {
        record.state.model=ModelAvailability::Failed;record.state.code="model_path_invalid";
    }
    return record;
}
QString bindingId(const EntityAssetLink &link) {
    QJsonArray identity{link.entityType,link.entityId,link.assetId,link.role,link.ordinal};
    return "catalog-binding:"+QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
}
SourceProjection projectDocumentSources(const DataCatalog &catalog,const QString &documentId) {
    SourceProjection projection;QHash<QString,CatalogVersion> latest;QHash<QString,int> selectedVersions;
    const auto versions=catalog.versions();
    for(const auto &version:versions) {
        const auto container=version.extra.value("documentSourceBindings");
        if(version.extra.contains("documentSourceBindings") && container.metaType().id()!=QMetaType::QVariantMap)
            projection.error=QStringLiteral("来源关联集合必须为对象");
        const auto saved=container.toMap();
        for(auto it=saved.cbegin();it!=saved.cend();++it) {
            SourceBinding binding;QString error;
            if(!sourceBindingFromJson(QJsonObject::fromVariantMap(it.value().toMap()),binding,&error) ||
               binding.source.documentId!=documentId || binding.id!=it.key()) {
                projection.error=error.isEmpty()?QStringLiteral("来源关联身份损坏"):error;continue;
            }
            if(!projection.savedBindings.contains(binding.id) || binding.revision>=projection.savedBindings.value(binding.id).revision)
                projection.savedBindings.insert(binding.id,binding);
        }
        auto record=recordFor(version,documentId,catalog.projectDir(),&projection.error);
        record.kind=catalog.assetById(version.assetId).type;
        const auto key=record.ref.key();projection.versionKeys.insert(version.id,key);
        if(!projection.records.contains(key) || version.versionNumber>=selectedVersions.value(key)) {
            projection.records.insert(key,std::move(record));
            selectedVersions.insert(key,version.versionNumber);
        }
        if(!latest.contains(version.assetId) || version.versionNumber>=latest.value(version.assetId).versionNumber)
            latest.insert(version.assetId,version);
    }
    for(const auto &link:catalog.links()) {
        if(link.entityId.isEmpty() || !latest.contains(link.assetId)) continue;
        const auto source=projection.records.value(projection.versionKeys.value(latest.value(link.assetId).id));
        SourceBinding binding;binding.id=bindingId(link);binding.resourceKind=link.entityType;binding.resourceId=link.entityId;
        binding.source=source.ref;binding.role=link.role;binding.primary=link.isPrimary && !link.unresolved;
        binding.decision=link.unresolved?BindingDecision::Unresolved:BindingDecision::Confirmed;
        binding.evidence=QStringLiteral("catalog relation");projection.bindings.insert(binding.id,std::move(binding));
    }
    // The catalog link owns the file choice. A saved alias replaces its display
    // representative only after SelectPrimary, never merely on registration/confirmation.
    for(const auto &binding:std::as_const(projection.bindings)) {
        const auto key=binding.slotKey()+binding.source.key();
        if(!projection.fileRepresentatives.contains(key) || binding.primary)
            projection.fileRepresentatives.insert(key,binding.id);
    }
    QHash<QString,SourceBinding> selected;
    for(const auto &binding:std::as_const(projection.savedBindings)) {
        if(!binding.scope.isEmpty() || binding.locator.kind!=LocatorKind::File ||
           binding.decision!=BindingDecision::Confirmed || !binding.primary)continue;
        const auto key=binding.slotKey()+binding.source.key();
        if(!projection.fileRepresentatives.contains(key))continue;
        const auto previous=selected.constFind(key);
        if(previous==selected.cend() || binding.revision>previous->revision ||
           (binding.revision==previous->revision && binding.id<previous->id))selected.insert(key,binding);
    }
    for(auto it=selected.cbegin();it!=selected.cend();++it)projection.fileRepresentatives.insert(it.key(),it->id);
    return projection;
}
}
