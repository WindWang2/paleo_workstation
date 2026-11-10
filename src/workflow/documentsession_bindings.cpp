// 层：功能
#include "documentsession.h"
#include "catalog/datacatalog.h"
#include "metadata/documentsourcestore.h"
#include "services/timedepthdependencies.h"
#include <QUuid>
#include <QJsonDocument>
#include <QSignalBlocker>
#include <algorithm>
#include <utility>

namespace paleo::document {
namespace {
std::optional<WellCurveTrackBinding> curveTrackFromBinding(const SourceBinding &binding) {
    if(binding.resourceKind!="well" || binding.role!=QLatin1String(kWellCurveTrackRole) ||
       binding.scope!=binding.id || binding.locator.kind!=LocatorKind::RecordGroup)return std::nullopt;
    const auto &fields=binding.locator.fields;
    WellCurveTrackBinding track{binding.id,binding.resourceId,fields.value("title").toString(),
        binding.source,fields.value("modelWellId").toLongLong(),fields.value("logSetId").toLongLong(),
        fields.value("curveId").toLongLong(),binding.revision};
    if(track.id.isEmpty() || track.title.trimmed().isEmpty() || track.title.size()>160 ||
       track.modelWellId<=0 || track.logSetId<=0 || track.curveId<=0 ||
       fields.value("key").toString()!=QString::number(track.logSetId))return std::nullopt;
    return track;
}
}

QVector<WellCurveTrackBinding> DocumentSession::wellCurveTracks(const QString &wellEntityId) const {
    QVector<WellCurveTrackBinding> tracks;
    const auto &all=effectiveBindings();
    SourceBinding resource;
    resource.resourceKind="well";resource.resourceId=wellEntityId;
    for(const auto &id:m_resourceBindingIds.values(resource.resourceKey())) {
        const auto binding=all.constFind(id);
        if(binding==all.cend() || !binding->primary || binding->decision!=BindingDecision::Confirmed)continue;
        if(const auto track=curveTrackFromBinding(*binding))tracks.append(*track);
    }
    std::sort(tracks.begin(),tracks.end(),[](const auto &a,const auto &b) {
        return a.revision==b.revision?a.id<b.id:a.revision<b.revision;
    });
    return tracks;
}

SourceCommandResult DocumentSession::createWellCurveTrack(WellCurveTrackBinding track,quint64 expectedEpoch,quint64 expectedRevision) {
    SourceCommand request;
    request.kind=SourceCommand::Kind::CreateWellCurveTrack;
    request.expectedEpoch=expectedEpoch;request.expectedRevision=expectedRevision;
    auto &binding=request.binding;
    binding.id=track.id.isEmpty()?QUuid::createUuid().toString(QUuid::WithoutBraces):track.id;
    binding.resourceKind="well";binding.resourceId=track.wellEntityId;
    binding.role=QLatin1String(kWellCurveTrackRole);binding.scope=binding.id;binding.source=track.source;
    binding.locator={LocatorKind::RecordGroup,{{"key",QString::number(track.logSetId)},
        {"logSetId",QString::number(track.logSetId)},{"curveId",QString::number(track.curveId)},
        {"modelWellId",QString::number(track.modelWellId)},{"title",track.title.trimmed()}}};
    return execute(std::move(request));
}

SourceCommandResult DocumentSession::removeWellCurveTrack(const QString &wellEntityId,const QString &trackId,
                                                         quint64 expectedEpoch,quint64 expectedRevision) {
    SourceCommand request;
    request.kind=SourceCommand::Kind::RemoveBinding;request.bindingId=trackId;
    request.expectedEpoch=expectedEpoch;request.expectedRevision=expectedRevision;
    request.operationId=QUuid::createUuid().toString(QUuid::WithoutBraces);
    SourceCommandResult result;result.operationId=request.operationId;result.revision=m_revision;
    if(!check(request,true,result))return result;
    const auto binding=effectiveBindings().value(trackId);
    if(!curveTrackFromBinding(binding) || binding.resourceId!=wellEntityId || binding.decision==BindingDecision::Rejected) {
        result.code="track_missing";result.error=QStringLiteral("该井没有指定的曲线道");return result;
    }
    if(recoveryRequired()) {
        result.code="recovery_required";result.error=QStringLiteral("请先恢复未完成的来源操作");return result;
    }
    return editBindings(request);
}

bool DocumentSession::checkWellCurveTrack(const SourceBinding &binding,SourceCommandResult &result) const {
    if(recoveryRequired()) {
        result.code="recovery_required";result.error=QStringLiteral("请先恢复未完成的来源操作");return false;
    }
    const auto track=curveTrackFromBinding(binding);
    const auto record=source(binding.source);
    const auto well=m_catalog->storedEntityById(binding.resourceId);
    const auto version=m_catalog->currentVersion(record.assetId);
    if(!track || well.extra.value("areaModelWellId").toLongLong()!=track->modelWellId ||
       record.ref.modelStoreId!="area-model" || record.fingerprint.kind!=FingerprintKind::Sha256 ||
       record.state.model==ModelAvailability::Failed || record.modelDirectory.isEmpty() ||
       m_catalog->assetById(record.assetId).format.compare("las",Qt::CaseInsensitive)!=0 ||
       sourceForVersion(version.id).ref!=binding.source) {
        result.code="track_source_invalid";result.error=QStringLiteral("所选 LAS 来源、文件修订或模型井编号已失效");return false;
    }
    return true;
}

SourceCommandResult DocumentSession::editBindings(const SourceCommand &command) {
    SourceCommandResult result;result.operationId=command.operationId;result.revision=m_revision;
    const auto all=effectiveBindings();QVector<SourceBinding> changed;SourceBinding binding;
    const bool choose=command.kind==SourceCommand::Kind::ChooseRelation;
    const bool createTrack=command.kind==SourceCommand::Kind::CreateWellCurveTrack;
    const bool select=choose || command.kind==SourceCommand::Kind::SelectPrimary;
    if(choose || createTrack || command.kind==SourceCommand::Kind::RegisterBinding) {
        binding=command.binding;if(binding.id.isEmpty()) binding.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        if(all.contains(binding.id)) {result.code="binding_exists";result.error=QStringLiteral("来源关联标识已存在");return result;}
        binding.primary=createTrack;
        binding.resolution=choose?command.note:QString();
        binding.decision=(choose || createTrack)?BindingDecision::Confirmed:
            binding.conflictingIdentities()?BindingDecision::Conflict:BindingDecision::Candidate;
    } else {
        if(!all.contains(command.bindingId)) {result.code="binding_missing";result.error=QStringLiteral("来源关联不存在");return result;}
        binding=all.value(command.bindingId);
        if(command.kind==SourceCommand::Kind::ConfirmBinding) {
            binding.resolution=command.note;binding.decision=BindingDecision::Confirmed;
        } else if(command.kind==SourceCommand::Kind::RemoveBinding) {
            binding.primary=false;binding.decision=BindingDecision::Rejected;binding.resolution=command.note;
        }
    }
    if(select) {
        if(binding.decision!=BindingDecision::Confirmed) {result.code="binding_unconfirmed";result.error=QStringLiteral("请先确认来源关联");return result;}
        binding.primary=true;
        const auto slot=binding.slotKey();
        for(auto other:all) if(other.id!=binding.id && other.slotKey()==slot && other.primary) {
            other.primary=false;other.revision=m_revision+1;changed.append(other);
        }
    }
    if(binding.source.documentId!=m_documentId || !source(binding.source).ref.valid()) {
        result.code="source_missing";result.error=QStringLiteral("来源关联不属于当前文档或版本不存在");return result;
    }
    if(binding.resourceKind=="well" && m_catalog->storedEntityById(binding.resourceId).entityType!="well") {
        result.code="resource_missing";result.error=QStringLiteral("关联的井实体不存在");return result;
    }
    if(createTrack && !checkWellCurveTrack(binding,result))return result;
    binding.revision=m_revision+1;
    if(command.kind==SourceCommand::Kind::SelectPrimary && binding.scope.isEmpty() &&
       sourceForVersion(m_catalog->currentVersion(source(binding.source).assetId).id).ref!=binding.source) {
        result.code="source_revision_changed";result.error=QStringLiteral("当前文件版本已变化，请重新确认来源关联");return result;
    }
    if(!binding.valid(&result.error)) {result.code="binding_invalid";return result;}
    RelationUseSpec use;ModelObjectRef object;
    const bool typed=relationBinding(binding,use,object);
    if(choose && !typed) {
        result.code="relation_binding_invalid";result.error=QStringLiteral("必须明确选择有效的类型化关系");return result;
    }
    const bool relationSelectionChanged=typed && (select ||
        (command.kind==SourceCommand::Kind::RemoveBinding && all.value(binding.id).primary));
    bool activeSelectionChanged=false;
    if(relationSelectionChanged && m_interpretation) {
        auto active=*m_interpretation;
        active.purpose=use.context.purpose;
        activeSelectionChanged=active==use.context;
    }
    changed.append(binding);
    QHash<QString,QVariantMap> payloads;
    for(const auto &value:changed) {
        const auto version=source(value.source).versionId;
        auto payload=payloads.contains(version)?payloads.value(version):m_catalog->versionById(version).extra.value("documentSourceBindings").toMap();
        payload.insert(value.id,sourceBindingJson(value).toVariantMap());
        if(QJsonDocument(QJsonObject::fromVariantMap(payload)).toJson(QJsonDocument::Compact).size()>1024*1024) {
            result.code="binding_budget";result.error=QStringLiteral("单来源关联元数据超过1 MiB预算");return result;
        }
        payloads.insert(version,std::move(payload));
    }
    const bool saved=queuedWrite([&](QString *error){
        if(!m_sourceStore->ensureIdentity(m_documentId,error))return false;
        const QSignalBlocker block(m_catalog);DataCatalog::BatchSave batch(m_catalog);
        const bool bridge=binding.scope.isEmpty() && m_catalog->storedEntityById(binding.resourceId).entityType==binding.resourceKind;
        if(bridge && command.kind==SourceCommand::Kind::SelectPrimary) {
            const auto asset=source(binding.source).assetId;int index=-1;const auto links=m_catalog->links();
            for(int i=0;i<links.size();++i)if(links[i].entityType==binding.resourceKind && links[i].entityId==binding.resourceId && links[i].assetId==asset && links[i].role==binding.role){index=i;break;}
            if(index<0){EntityAssetLink link;link.entityType=binding.resourceKind;link.entityId=binding.resourceId;link.assetId=asset;link.role=binding.role;link.isPrimary=false;
                if(!m_catalog->addLink(link,error)){batch.abort();return false;}index=m_catalog->links().size()-1;}
            if(!m_catalog->setLinkPrimary(index,error)){batch.abort();return false;}
        }
        const bool ownsLegacy=m_projection.bindings.contains(binding.id) ||
            m_projection.fileRepresentatives.value(binding.slotKey()+binding.source.key())==binding.id;
        if(bridge && command.kind==SourceCommand::Kind::RemoveBinding && binding.locator.kind==LocatorKind::File && ownsLegacy) {
            const auto asset=source(binding.source).assetId;const auto links=m_catalog->links();
            for(int i=0;i<links.size();++i)
                if(links[i].entityType==binding.resourceKind && links[i].entityId==binding.resourceId && links[i].assetId==asset && links[i].role==binding.role)
                    if(!m_catalog->setLinkUnresolved(i,error)){batch.abort();return false;}
        }
        for(auto it=payloads.cbegin();it!=payloads.cend();++it)
            if(!m_catalog->updateVersionExtra(it.key(),"documentSourceBindings",it.value(),error)){batch.abort();return false;}
        if(relationSelectionChanged && use.relation==RelationKind::TimeDepth &&
           !invalidateTimeDepthSelection(*m_catalog,use,binding.primary?&object:nullptr,error)) {
            batch.abort();return false;
        }
        if(choose && use.relation==RelationKind::TimeDepth && !command.timeDepthSemantics.isEmpty() &&
           !refreshTimeDepthObjectDependencies(*m_catalog,object.id,error)) {
            batch.abort();return false;
        }
        return batch.flush(error);
    },&result.error);
    if(!saved) {
        result.code="binding_commit_failed";return result;
    }
    invalidate();QMetaObject::invokeMethod(m_catalog,"changed",Qt::QueuedConnection);
    SourceChangeSet change;change.epoch=m_epoch;change.revision=m_revision;change.operationId=command.operationId;
    for(const auto &value:changed) {change.bindingIds.append(value.id);change.sourceKeys.append(value.source.key());}
    result.ok=true;result.revision=m_revision;result.code="committed";
    if(binding.role==QLatin1String(kWellCurveTrackRole))result.details.insert("trackId",binding.id);
    emit sourcesChanged(change);
    if(activeSelectionChanged)emit interpretationChanged(binding.resourceId);
    if(!choose)emit commandFinished(result);
    return result;
}
}
