// 层：功能
#include "documentsession.h"
#include "catalog/datacatalog.h"
#include "io/areamodel.h"
#include "metadata/documentsourcestore.h"
#include "services/jobrunner.h"
#include "services/timedepthdependencies.h"
#include <paleo_data/service.h>
#include <paleo_data/object_descriptor.h>
#include <QUuid>
#include <algorithm>

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
QVariantMap objectOption(const DocumentSession &document,paleo::data::DataService &service,
                         paleo::data::DataObjectKind kind,qlonglong id) {
    paleo::data::DataObjectDescriptor item;paleo::data::SourceIndexEntry source;std::string error;
    if(!service.describeDataObject(kind,id,item,error) || !item.revisionKnown ||
       !service.readSourceIndexEntry(item.sourceId,source,error) || !source.active)return {};
    const auto type=kind==paleo::data::DataObjectKind::TimeDepth?ObjectKind::TimeDepth:
        kind==paleo::data::DataObjectKind::IntervalTrack?ObjectKind::IntervalTrack:ObjectKind::SeismicVolume;
    ModelObjectRef ref{{document.documentId(),QStringLiteral("area-model"),item.sourceId,QString::fromStdString(source.sha256)},
        type,item.id,QString::fromStdString(item.revision)};
    if(!ref.valid() || document.source(ref.source).ref!=ref.source)return {};
    return {{"label",QStringLiteral("%1 · #%2 · %3").arg(QString::fromStdString(item.name)).arg(item.id)
                         .arg(QString::fromStdString(source.path))},
            {"object",modelObjectRefJson(ref).toVariantMap()}};
}
bool confirmExactTimeDepth(RelationJob &job) {
    const auto &semantics=job.command.timeDepthSemantics;
    if(semantics.isEmpty())return true;
    paleo::data::DataService service;paleo::data::DataObjectDescriptor updated;std::string error;
    const auto text=[&](const char *key){return semantics.value(QLatin1String(key)).toString().trimmed().toStdString();};
    auto &request=job.request;
    if(!service.openProject(request.selected.modelDirectory.toUtf8().toStdString(),error) ||
       !service.confirmTimeDepthSemanticsAtRevision(request.object.id,request.object.revision.toStdString(),
           text("mdReference"),text("unit"),text("type"),text("reference"),
           semantics.value("negate").toBool(),updated,error)) {
        job.error=QString::fromStdString(error);return false;
    }
    job.details.insert("modelSemanticsCommitted",true);
    request.object.revision=QString::fromStdString(updated.revision);
    if(!setRelationBinding(job.command.binding,request.use,request.object,&job.error))return false;
    job.details.insert("objectRef",modelObjectRefJson(request.object).toVariantMap());
    return true;
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

InterpretationSelection DocumentSession::interpretationSelection() const {
    InterpretationSelection result;
    result.context=m_interpretation;
    result.error=m_interpretationError;
    if(!result.context)return result;
    result.error=loadError();
    if(!result.error.isEmpty() || !result.context->targetVolume)return result;
    result.volume=source(result.context->targetVolume->source);
    if(result.volume.ref!=result.context->targetVolume->source)
        result.error=QStringLiteral("所选目标地震体来源已失效，请重新选择");
    return result;
}
std::shared_ptr<const InterpretationReadPlan> DocumentSession::captureInterpretationPlan() const {
    const auto selection=interpretationSelection();
    if(!selection.context && selection.error.isEmpty())return {};
    auto plan=std::make_shared<InterpretationReadPlan>();plan->selection=selection;
    for(const auto &well:m_catalog->storedEntities("well"))
        plan->timeDepth.insert(well.id,interpretationRelation(well.id,RelationKind::TimeDepth));
    return plan;
}

bool DocumentSession::selectInterpretation(const InterpretationContext &context,QString *error) {
    if(error)error->clear();
    if(readOnly() || recoveryRequired() || !context.valid() || context.documentId!=m_documentId ||
       context.modelStoreId!="area-model" || context.purpose!=RelationPurpose::WellTie) {
        if(error)*error=QStringLiteral("活动解释上下文无效，或文档当前不可写");
        return false;
    }
    InterpretationSelection selection;
    selection.context=context;
    if(context.targetVolume)selection.volume=source(context.targetVolume->source);
    const auto detail=validateInterpretationSelection(paleo::area_model::directory(m_projectDir),selection);
    if(!detail.isEmpty()){if(error)*error=detail;return false;}
    if(m_interpretation==context)return true;
    if(!queuedWrite([&](QString *why){return m_sourceStore->saveInterpretation(context,why);},error))return false;
    m_interpretation=context;m_interpretationError.clear();
    emit interpretationChanged();
    return true;
}
bool DocumentSession::clearInterpretation(QString *error) {
    if(error)error->clear();
    if(readOnly() || recoveryRequired()) {
        if(error)*error=QStringLiteral("解释文档当前不可写");return false;
    }
    if(!m_interpretation && m_interpretationError.isEmpty())return true;
    if(!queuedWrite([&](QString *why){return m_sourceStore->clearInterpretation(m_documentId,why);},error))return false;
    m_interpretation.reset();m_interpretationError.clear();emit interpretationChanged();return true;
}

QVariantMap DocumentSession::interpretationOptions(const QStringList &wellIds,QString *error) const {
    if(error)error->clear();
    if(!isOpen()){if(error)*error=QStringLiteral("解释文档未打开");return {};}
    paleo::data::DataService service;std::string detail;
    const auto fail=[&]{if(error)*error=QString::fromStdString(detail);return QVariantMap{};};
    if(!service.openProjectReadOnly(paleo::area_model::directory(m_projectDir).toUtf8().toStdString(),detail))return fail();
    QVariantList wells,schemes,volumes;
    for(const auto &well:m_catalog->storedEntities("well"))
        if(well.extra.value("areaModelWellId").toLongLong()>0 && (wellIds.isEmpty() || wellIds.contains(well.id)))
            wells.append(QVariantMap{{"id",well.id},{"label",well.name}});
    std::vector<paleo::data::StratigraphicSchemeSummary> summaries;
    if(!service.listStratigraphicSchemes(summaries,detail))return fail();
    for(const auto &scheme:summaries) {
        std::vector<paleo::data::StratigraphicSchemeRevisionSummary> revisions;
        if(!service.listStratigraphicSchemeRevisions(scheme.id,revisions,detail))return fail();
        for(const auto &revision:revisions)
            schemes.append(QVariantMap{{"id",QString::fromStdString(scheme.id)},
                {"revision",QString::fromStdString(revision.revision.value)},
                {"label",QStringLiteral("%1 · %2").arg(QString::fromStdString(scheme.name),QString::fromStdString(revision.revision.value))}});
    }
    std::vector<paleo::data::DataEntry> entries;
    if(!service.listData("volumes",0,entries,detail))return fail();
    for(const auto &entry:entries) {
        const auto option=objectOption(*this,service,paleo::data::DataObjectKind::SeismicVolume,entry.id);
        if(!option.isEmpty())volumes.append(option);
    }
    return {{"wells",wells},{"schemes",schemes},{"volumes",volumes}};
}

QVariantList DocumentSession::interpretationRelationOptions(const QString &wellId,QString *error) const {
    if(error)error->clear();
    if(!isOpen() || wellId.isEmpty())return {};
    const auto well=m_catalog->storedEntityById(wellId);
    const auto id=well.extra.value("areaModelWellId").toLongLong();
    if(well.entityType!="well" || id<=0)return {};
    paleo::data::DataService service;std::string detail;
    const auto fail=[&]{if(error)*error=QString::fromStdString(detail);return QVariantList{};};
    if(!service.openProjectReadOnly(paleo::area_model::directory(m_projectDir).toUtf8().toStdString(),detail))return fail();
    QVariantList result;
    for(const auto kind:{paleo::data::DataObjectKind::TimeDepth,paleo::data::DataObjectKind::IntervalTrack}) {
        std::vector<paleo::data::DataEntry> entries;
        if(!service.listData(kind==paleo::data::DataObjectKind::TimeDepth?"time_depth":"intervals",id,entries,detail))return fail();
        for(const auto &entry:entries) {
            auto option=objectOption(*this,service,kind,entry.id);
            if(!option.isEmpty()) {
                option.insert("timeDepth",kind==paleo::data::DataObjectKind::TimeDepth);
                result.append(option);
            }
        }
    }
    return result;
}

SourceCommand DocumentSession::chooseRelationCommand(const InterpretationContext &context,const QString &wellId,
        const ModelObjectRef &object,const QVariantMap &semantics,QString *error) const {
    auto result=command(SourceCommand::Kind::ChooseRelation);
    if(error)error->clear();
    if(!isOpen() || (object.kind!=ObjectKind::TimeDepth && object.kind!=ObjectKind::IntervalTrack) ||
       (!semantics.isEmpty() && object.kind!=ObjectKind::TimeDepth)) {
        if(error)*error=QStringLiteral("所选井关系或物理口径无效");
        return result;
    }
    const auto kind=object.kind==ObjectKind::TimeDepth?RelationKind::TimeDepth:RelationKind::StratigraphicTrack;
    auto useContext=context;
    useContext.purpose=kind==RelationKind::TimeDepth?RelationPurpose::WellTie:RelationPurpose::WellColumn;
    const auto id=m_catalog->storedEntityById(wellId).extra.value("areaModelWellId").toLongLong();
    const RelationUseSpec use{useContext,wellId,id,kind};
    result.note=QStringLiteral("用户已明确选择此井的关系及其用途");
    result.timeDepthSemantics=semantics;
    setRelationBinding(result.binding,use,object,error);
    return result;
}

RelationInput DocumentSession::interpretationRelation(const QString &wellId,RelationKind kind,
                                                       RelationPurpose purpose) const {
    RelationInput result;
    const auto selection=interpretationSelection();
    if(!selection.error.isEmpty())return {RelationInput::State::Invalid,{},selection.error};
    if(!selection.context)return result;
    auto context=*selection.context;
    context.purpose=purpose;
    const auto candidates=bindings("well",wellId,relationRole(kind),context.key(),true);
    if(std::none_of(candidates.cbegin(),candidates.cend(),hasRelationBinding)) {
        if(kind==RelationKind::TimeDepth)
            return {RelationInput::State::Invalid,{},QStringLiteral("当前解释上下文尚未为该井选择时深关系")};
        return result;
    }
    const auto well=m_catalog->storedEntityById(wellId);
    const RelationUseSpec use{context,wellId,well.extra.value("areaModelWellId").toLongLong(),kind};
    result.request=relationRequest(use,&result.error);
    result.state=result.request.object.valid()?RelationInput::State::Selected:RelationInput::State::Invalid;
    return result;
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
        const auto binding=(command.kind==SourceCommand::Kind::RegisterBinding || command.kind==SourceCommand::Kind::ResolveRelation ||
                            command.kind==SourceCommand::Kind::ChooseRelation)?
            command.binding:effectiveBindings().value(command.bindingId);
        RelationUseSpec use;ModelObjectRef object;
        if(!relationBinding(binding,use,object,&result.error)){result.code="relation_binding_invalid";return result;}
        if(command.kind==SourceCommand::Kind::ResolveRelation) {
            job->request=relationRequest(use,&result.error);
            if(job->request.object!=object){result.code="relation_selection_changed";if(result.error.isEmpty())result.error=QStringLiteral("关系主选择已变化");return result;}
        } else job->request=relationRequestFor(use,object,&result.error);
        if(!result.error.isEmpty() || !job->request.object.valid()){result.code="relation_request_invalid";return result;}
        job->details=resolutionDetails(job->request);
        if(command.kind==SourceCommand::Kind::ChooseRelation && !command.timeDepthSemantics.isEmpty()) {
            if(object.kind!=ObjectKind::TimeDepth || command.timeDepthSemantics.value("negate").metaType().id()!=QMetaType::Bool) {
                result.code="relation_semantics_invalid";result.error=QStringLiteral("时深物理口径不属于所选对象");return result;
            }
            job->details.insert("modelSemanticsMayHaveCommitted",true);
        }
    }
    paleo::jobs::JobRunner<RelationJob>::Callbacks callbacks;
    const auto refreshCommittedSemantics=[this](const RelationJob &work,SourceCommandResult &done) {
        if(!work.details.value("modelSemanticsCommitted").toBool() || work.command.expectedEpoch!=m_epoch || readOnly())return;
        QString detail;
        if(!queuedWrite([&](QString *why){return refreshTimeDepthObjectDependencies(*m_catalog,work.request.object.id,why);},&detail))
            done.error+=QStringLiteral(" 成果依赖状态将在重新打开工程时恢复。")+detail;
        done.revision=m_revision;
    };
    callbacks.compute=[](RelationJob &work,const auto &cancelled,const auto &) {
        if(work.command.kind==SourceCommand::Kind::UpgradeModelMetadata)return upgradeRelationModel(work,cancelled);
        if(cancelled())return false;
        work.resolution=resolveDocumentRelation(work.request);work.computed=true;work.error=work.resolution.error;
        if(work.resolution.ok && !cancelled() && work.command.kind==SourceCommand::Kind::ChooseRelation &&
           !confirmExactTimeDepth(work))return false;
        return work.resolution.ok&&!cancelled();
    };
    callbacks.commit=[this,refreshCommittedSemantics](RelationJob &work,QString *error) {
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
            done=editBindings(work.command);
            if(work.command.kind==SourceCommand::Kind::ChooseRelation) {
                done.details=work.details;
                if(done.ok) {
                    auto context=work.request.use.context;
                    context.purpose=RelationPurpose::WellTie;
                    done.ok=selectInterpretation(context,&done.error);
                    if(!done.ok)done.error.prepend(QStringLiteral("井关系已保存，活动解释未切换。"));
                } else if(work.details.value("modelSemanticsCommitted").toBool()) {
                    done.error.prepend(QStringLiteral("时深口径已保存，关系选择未提交。"));
                    refreshCommittedSemantics(work,done);
                }
                emit commandFinished(done);
            } else if(!done.ok) {
                emit commandFinished(done);
            }
            if(error)*error=done.error;return done.ok;
        }
        done.ok=ok;
        if(!ok && work.details.value("modelSemanticsCommitted").toBool()) {
            done.error.prepend(QStringLiteral("时深口径已保存，关系选择未提交。"));
            refreshCommittedSemantics(work,done);
        }
        if(ok)done.code=work.upgraded?"model_metadata_upgraded":"relation_resolved";
        if(error)*error=done.error;emit commandFinished(done);return ok;
    };
    callbacks.onDropped=[this,refreshCommittedSemantics](RelationJob &work,paleo::jobs::DropReason) {
        m_relationOperation.clear();m_relationDetails.clear();SourceCommandResult done;
        done.operationId=work.command.operationId;done.details=work.details;done.revision=m_revision;
        done.code="cancelled_or_stale";done.error=QStringLiteral("关系操作已取消或过期");
        if(work.details.value("modelSemanticsCommitted").toBool()) {
            done.error.prepend(QStringLiteral("时深口径已保存，关系选择未提交。"));
            refreshCommittedSemantics(work,done);
        }
        emit commandFinished(done);
    };
    m_relationOperation=command.operationId;m_relationDetails=job->details;
    if(!m_relations->start(QStringLiteral("核验文档关系"),job,std::move(callbacks),{},true)) {
        m_relationOperation.clear();m_relationDetails.clear();result.code="start_failed";result.error=QStringLiteral("无法启动关系任务");return result;
    }
    result.pending=true;result.code="pending";result.details=job->details;return result;
}
}
