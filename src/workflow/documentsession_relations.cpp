// 层：功能
#include "documentsession.h"
#include "catalog/datacatalog.h"
#include "services/jobrunner.h"
#include <paleo_data/service.h>
#include <paleo_data/object_descriptor.h>
#include <QUuid>

namespace paleo::document {
struct RelationJob {
    SourceCommand command;
    RelationResolveRequest request;
    SourceRecord upgradeSource;
    RelationResolution resolution;
    QVariantMap details;
    QString error;
    bool computed=false,upgraded=false;
};
namespace {
QString relationRole(RelationKind kind){return kind==RelationKind::TimeDepth?"time_depth":"stratigraphy";}
QVariantMap resolutionDetails(const RelationResolveRequest &request) {
    return {{"use",relationUseSpecJson(request.use).toVariantMap()},{"objectRef",modelObjectRefJson(request.object).toVariantMap()}};
}
bool upgradeRelationModel(RelationJob &work,const paleo::jobs::CancelFn &cancelled) {
    if(cancelled())return false;
    const auto &record=work.upgradeSource;paleo::data::DataService reader;std::string error;paleo::data::SourceIndexEntry source;
    if(!reader.openProjectReadOnly(record.modelDirectory.toUtf8().toStdString(),error) ||
       !reader.readSourceIndexEntry(record.ref.sourceId,source,error) || !source.active || source.sha256!=record.fingerprint.stored().toStdString()) {
        work.error=error.empty()?QStringLiteral("模型来源身份已变化或停用"):QString::fromStdString(error);return false;
    }
    reader.closeProject();
    if(cancelled())return false;
    work.upgraded=paleo::data::upgradeObjectRevisionSchema(record.modelDirectory.toUtf8().toStdString(),error,cancelled);
    if(!work.upgraded){work.error=QString::fromStdString(error);return false;}
    work.details.remove("metadataUpgradeMayHaveCommitted");work.details.insert("upgraded",true);work.computed=true;return true;
}
}

RelationChoiceView DocumentSession::relationView(const RelationUseSpec &use) const {
    RelationChoiceView result;result.use=use;
    if(!isOpen() || !use.valid() || use.context.documentId!=m_documentId) {
        result.code="relation_invalid";result.detail=QStringLiteral("关系用途不属于已打开文档");return result;
    }
    int selected=0;bool invalid=false;
    for(const auto &binding:bindings("well",use.wellEntityId,relationRole(use.relation),use.context.key())) {
        if(!hasRelationBinding(binding))continue; // Opaque legacy scopes never become typed choices.
        RelationUseSpec actual;ModelObjectRef object;QString why;
        if(!relationBinding(binding,actual,object,&why)){invalid=true;continue;}
        if(actual!=use)continue;
        result.choices.append(binding);
        if(binding.decision==BindingDecision::Confirmed && binding.primary) {
            ++selected;result.selectedBindingId=binding.id;result.selectedObject=object;
        }
    }
    result.selected=selected==1;
    if(!result.selected){result.selectedBindingId.clear();result.selectedObject={};}
    result.code=selected>1?"relation_selection_ambiguous":result.selected?"selection_unvalidated":invalid?"relation_binding_invalid":"relation_selection_missing";
    result.detail=result.selected?QStringLiteral("已保存关系选择；每次使用仍需校验对象修订，选择不代表单位或基准已确认"):
        selected>1?QStringLiteral("该用途存在多个主选择，请明确重新选择"):QStringLiteral("该用途没有唯一的已确认主选择");
    return result;
}

RelationResolveRequest DocumentSession::relationRequestFor(const RelationUseSpec &use,const ModelObjectRef &object,QString *error) const {
    if(error)error->clear();
    const auto fail=[&](const QString &why){if(error)*error=why;return RelationResolveRequest{};};
    if(!isOpen() || !use.valid() || !object.valid() || use.context.documentId!=m_documentId ||
       object.source.documentId!=m_documentId || use.context.modelStoreId!="area-model" || object.source.modelStoreId!=use.context.modelStoreId ||
       object.kind!=(use.relation==RelationKind::TimeDepth?ObjectKind::TimeDepth:ObjectKind::IntervalTrack))return fail(QStringLiteral("关系用途、模型或对象身份无效"));
    // Ordinary entityById projects model facts and can backfill a legacy match
    // by name. Typed requests require the explicit, stored catalog mapping.
    const auto well=m_catalog->storedEntityById(use.wellEntityId);
    bool mapped=false;const auto wellId=well.extra.value("areaModelWellId").toLongLong(&mapped);
    if(well.entityType!="well" || !mapped || wellId<=0 || wellId!=use.modelWellId)return fail(QStringLiteral("井缺少明确的模型编号映射，不允许按井名回退"));
    RelationResolveRequest request;request.use=use;request.object=object;request.catalogDocumentId=m_documentId;
    request.wellEntityId=well.id;request.mappedModelWellId=wellId;request.selected=source(object.source);
    if(request.selected.ref!=object.source || request.selected.modelDirectory.isEmpty())return fail(QStringLiteral("关系来源不在当前文档或模型目录不可用"));
    if(use.context.targetVolume) {
        request.targetVolume=source(use.context.targetVolume->source);
        if(request.targetVolume.ref!=use.context.targetVolume->source)return fail(QStringLiteral("目标地震体来源不在当前文档"));
    }
    return request;
}

RelationResolveRequest DocumentSession::relationRequest(const RelationUseSpec &use,QString *error) const {
    const auto view=relationView(use);
    if(!view.selected){if(error)*error=view.detail;return {};}
    auto request=relationRequestFor(use,view.selectedObject,error);
    if(request.object.valid()) {
        request.selectedBindingId=view.selectedBindingId;
        request.catalogRevision=m_catalog->catalogRevision();
    }
    return request;
}

SourceCommandResult DocumentSession::resolveRelation(const RelationUseSpec &use) {
    const auto view=relationView(use);
    if(!view.selected) {
        SourceCommandResult result;result.operationId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        result.code=view.code;result.error=view.detail;result.revision=m_revision;return result;
    }
    auto request=command(SourceCommand::Kind::ResolveRelation);request.binding=effectiveBindings().value(view.selectedBindingId);
    return execute(request);
}

SourceCommandResult DocumentSession::startRelationCommand(const SourceCommand &command) {
    SourceCommandResult result;result.operationId=command.operationId;result.revision=m_revision;
    if(!m_tasks || !m_relations || m_relations->busy()) {result.code="busy";result.error=QStringLiteral("关系操作正在进行或任务服务未配置");return result;}
    auto job=std::make_shared<RelationJob>();job->command=command;
    if(command.kind==SourceCommand::Kind::UpgradeModelMetadata) {
        job->upgradeSource=source(command.source);const auto &record=job->upgradeSource;
        if(record.ref!=command.source || record.ref.modelStoreId!="area-model" || record.modelDirectory.isEmpty() ||
           !record.fingerprint.valid() || record.ref.fileRevisionId!=record.fingerprint.stored()) {
            result.code="model_source_invalid";result.error=QStringLiteral("元数据升级仅适用于已登记的工区模型来源");return result;
        }
        // The model write and catalog/session lifetime are separate boundaries.
        // A close racing its final commit cannot promise that it was rolled back.
        job->details={{"source",sourceRefJson(record.ref).toVariantMap()},{"metadataUpgradeMayHaveCommitted",true}};
    } else {
        const auto binding=(command.kind==SourceCommand::Kind::RegisterBinding || command.kind==SourceCommand::Kind::ResolveRelation)?
            command.binding:effectiveBindings().value(command.bindingId);
        RelationUseSpec use;ModelObjectRef object;
        if(!relationBinding(binding,use,object,&result.error)){result.code="relation_binding_invalid";return result;}
        if(command.kind==SourceCommand::Kind::ResolveRelation) {
            job->request=relationRequest(use,&result.error);
            if(job->request.object!=object){result.code="relation_selection_changed";if(result.error.isEmpty())result.error=QStringLiteral("关系主选择已变化");return result;}
        } else job->request=relationRequestFor(use,object,&result.error);
        if(!result.error.isEmpty() || !job->request.object.valid()){result.code="relation_request_invalid";return result;}
        job->details=resolutionDetails(job->request);
    }
    paleo::jobs::JobRunner<RelationJob>::Callbacks callbacks;
    callbacks.compute=[](RelationJob &work,const auto &cancelled,const auto &) {
        if(work.command.kind==SourceCommand::Kind::UpgradeModelMetadata)return upgradeRelationModel(work,cancelled);
        if(cancelled())return false;
        work.resolution=resolveDocumentRelation(work.request);work.computed=true;work.error=work.resolution.error;
        return work.resolution.ok&&!cancelled();
    };
    callbacks.commit=[this](RelationJob &work,QString *error) {
        SourceCommandResult done;done.operationId=work.command.operationId;done.details=work.details;done.revision=m_revision;
        const bool write=work.command.kind!=SourceCommand::Kind::ResolveRelation;
        bool ok=check(work.command,write,done);
        if(ok && m_catalog->catalogRevision()!=work.command.expectedRevision) {
            ok=false;done.code="stale_command";done.error=QStringLiteral("目录修订已变化，关系结果未提交");
        }
        if(ok && (!work.computed || !work.error.isEmpty())) {
            ok=false;done.code=work.command.kind==SourceCommand::Kind::UpgradeModelMetadata?"model_metadata_upgrade_failed":
                work.resolution.code.isEmpty()?"relation_validation_failed":work.resolution.code;
            done.error=work.error.isEmpty()?QStringLiteral("关系验证未完成"):work.error;
        }
        m_relationOperation.clear();m_relationDetails.clear();
        if(ok && work.command.kind!=SourceCommand::Kind::ResolveRelation && work.command.kind!=SourceCommand::Kind::UpgradeModelMetadata) {
            // editBindings emits its successful completion. Failed edits do not.
            done=editBindings(work.command);if(!done.ok)emit commandFinished(done);
            if(error)*error=done.error;return done.ok;
        }
        done.ok=ok;
        if(ok)done.code=work.upgraded?"model_metadata_upgraded":"relation_resolved";
        if(error)*error=done.error;emit commandFinished(done);return ok;
    };
    callbacks.onDropped=[this](RelationJob &work,paleo::jobs::DropReason) {
        m_relationOperation.clear();m_relationDetails.clear();SourceCommandResult done;
        done.operationId=work.command.operationId;done.details=work.details;done.revision=m_revision;
        done.code="cancelled_or_stale";done.error=QStringLiteral("关系操作已取消或过期");emit commandFinished(done);
    };
    m_relationOperation=command.operationId;m_relationDetails=job->details;
    if(!m_relations->start(QStringLiteral("核验文档关系"),job,std::move(callbacks),{},true)) {
        m_relationOperation.clear();m_relationDetails.clear();result.code="start_failed";result.error=QStringLiteral("无法启动关系任务");return result;
    }
    result.pending=true;result.code="pending";result.details=job->details;return result;
}
}
