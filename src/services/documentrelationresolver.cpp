// 层：数据
#include "documentrelationresolver.h"
#include <paleo_data/service.h>
#include <paleo_data/object_descriptor.h>
#include <QDir>

namespace paleo::document {
namespace {
namespace data=paleo::data;
data::DataObjectKind sdkKind(ObjectKind kind) {
    switch(kind) {case ObjectKind::TimeDepth:return data::DataObjectKind::TimeDepth;
    case ObjectKind::IntervalTrack:return data::DataObjectKind::IntervalTrack;
    case ObjectKind::SeismicVolume:return data::DataObjectKind::SeismicVolume;}
    return static_cast<data::DataObjectKind>(-1);
}
bool checkSource(data::DataService &service,const SourceRecord &record,const ModelObjectRef &object,QString &code,QString &error) {
    if(record.ref!=object.source || !record.fingerprint.valid() || record.fingerprint.stored()!=object.source.fileRevisionId) {
        code="source_revision_changed";error=QStringLiteral("对象来源不属于当前文档版本");return false;
    }
    data::SourceIndexEntry source;std::string detail;
    if(!service.readSourceIndexEntry(object.source.sourceId,source,detail)) {code="source_missing";error=QString::fromStdString(detail);return false;}
    if(!source.active || source.sha256!=record.fingerprint.stored().toStdString()) {
        code="source_revision_changed";error=QStringLiteral("来源已替换或不再有效");return false;
    }
    return true;
}
bool checkObject(data::DataService &service,const ModelObjectRef &object,qlonglong well,QString &code,QString &error) {
    data::DataObjectDescriptor entry;std::string detail;
    if(!service.describeDataObject(sdkKind(object.kind),object.id,entry,detail)) {code="object_missing";error=QString::fromStdString(detail);return false;}
    if(entry.sourceId!=object.source.sourceId || entry.wellId!=well) {code="object_owner_mismatch";error=QStringLiteral("关系对象不属于指定来源或井");return false;}
    if(!entry.revisionKnown) {code="object_revision_upgrade_required";error=QStringLiteral("模型缺少对象修订元数据，请明确升级后再确认用途");return false;}
    if(QString::fromStdString(entry.revision)!=object.revision) {code="object_revision_changed";error=QStringLiteral("对象已修改，请重新选择并确认");return false;}
    return true;
}
bool checkScheme(data::DataService &service,const InterpretationContext &context,QString &error) {
    if(context.schemeId.isEmpty())return true;
    std::string detail;
    if(context.intervalId.isEmpty()) {
        std::vector<data::StratigraphicSchemeRevisionSummary> revisions;
        if(!service.listStratigraphicSchemeRevisions(context.schemeId.toStdString(),revisions,detail)) {
            error=QString::fromStdString(detail);return false;
        }
        for(const auto &revision:revisions)if(revision.revision.value==context.schemeRevision.toStdString())return true;
        error=QStringLiteral("所选方案修订已失效");return false;
    }
    data::StratigraphicScheme scheme;
    if(!service.readStratigraphicScheme(context.schemeId.toStdString(),context.schemeRevision.toStdString(),scheme,detail)) {error=QString::fromStdString(detail);return false;}
    if(QString::fromStdString(scheme.id)!=context.schemeId || QString::fromStdString(scheme.revision.value)!=context.schemeRevision) {error=QStringLiteral("方案固定修订不匹配");return false;}
    const auto *interval=data::framework::findInterval(scheme,{context.intervalId.toStdString()});
    if(!interval || interval->status!="valid") {error=QStringLiteral("父层间单元不存在或无效");return false;}
    if(context.aggregate)return true;
    const auto *sub=data::framework::findSubInterval(scheme,{context.subIntervalId.toStdString()});
    if(!sub || sub->interval!=interval->id || sub->status!="valid") {error=QStringLiteral("子层段不存在、无效或不属于指定父单元");return false;}
    return true;
}
}
QString validateInterpretationSelection(const QString &modelDirectory,const InterpretationSelection &selection,QString *timeReference) {
    if(timeReference)timeReference->clear();
    if(!selection.error.isEmpty())return selection.error;
    if(!selection.context)return {};
    const auto &context=*selection.context;
    if(context.schemeId.isEmpty()&&!context.targetVolume)return {};
    data::DataService service;std::string detail;
    if(!service.openProjectReadOnly(modelDirectory.toUtf8().toStdString(),detail))return QString::fromStdString(detail);
    QString code,error;
    if(!checkScheme(service,context,error))return error;
    if(context.targetVolume) {
        if(QDir::cleanPath(selection.volume.modelDirectory)!=QDir::cleanPath(modelDirectory))
            return QStringLiteral("目标地震体不属于当前工区模型");
        if(!checkSource(service,selection.volume,*context.targetVolume,code,error) ||
           !checkObject(service,*context.targetVolume,0,code,error))return error;
        if(timeReference) {
            data::SeismicVolume volume;
            if(!service.readVolume(context.targetVolume->id,volume,detail) ||
               !checkObject(service,*context.targetVolume,0,code,error))
                return error.isEmpty()?QString::fromStdString(detail):error;
            if(volume.sampling.timeType!="TWT" || volume.sampling.timeReference.empty())
                return QStringLiteral("所选地震体尚未确认双程时间基准");
            *timeReference=QString::fromStdString(volume.sampling.timeReference);
        }
    }
    return {};
}
RelationResolution resolveDocumentRelation(const RelationResolveRequest &request) {
    RelationResolution result;result.object=request.object;
    const auto fail=[&](const QString &code,const QString &error){result.code=code;result.error=error;return result;};
    const auto &use=request.use;const auto &object=request.object;
    if(!use.valid() || !object.valid() || use.context.documentId!=request.catalogDocumentId || object.source.documentId!=request.catalogDocumentId ||
       use.context.modelStoreId!=object.source.modelStoreId || object.kind!=(use.relation==RelationKind::TimeDepth?ObjectKind::TimeDepth:ObjectKind::IntervalTrack))
        return fail("relation_invalid",QStringLiteral("关系用途、文档或对象类别无效"));
    if(object.source.modelStoreId!="area-model" || request.wellEntityId!=use.wellEntityId || request.mappedModelWellId<=0 || request.mappedModelWellId!=use.modelWellId)
        return fail("model_well_mapping_missing",QStringLiteral("井缺少与所选模型一致的明确编号映射，不允许按井名匹配"));
    if(request.selected.modelDirectory.isEmpty())return fail("model_missing",QStringLiteral("来源模型目录不可用"));
    data::DataService service;std::string detail;
    if(!service.openProjectReadOnly(request.selected.modelDirectory.toUtf8().toStdString(),detail))return fail("model_unavailable",QString::fromStdString(detail));
    if(!checkSource(service,request.selected,object,result.code,result.error) ||
       !checkObject(service,object,use.modelWellId,result.code,result.error))return result;
    if(!checkScheme(service,use.context,result.error)){result.code="context_revision_invalid";return result;}
    if(use.context.targetVolume) {
        const auto &volume=*use.context.targetVolume;
        if(QDir::cleanPath(request.targetVolume.modelDirectory)!=QDir::cleanPath(request.selected.modelDirectory))return fail("target_model_mismatch",QStringLiteral("目标地震体不属于当前工区模型"));
        if(!checkSource(service,request.targetVolume,volume,result.code,result.error) || !checkObject(service,volume,0,result.code,result.error))return result;
    }
    result.ok=true;result.code="relation_resolved";return result;
}
}
