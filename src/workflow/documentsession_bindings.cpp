// 层：功能
#include "documentsession.h"
#include "catalog/datacatalog.h"
#include "metadata/documentsourcestore.h"
#include <QUuid>
#include <QJsonDocument>
#include <QSignalBlocker>

namespace paleo::document {
SourceCommandResult DocumentSession::editBindings(const SourceCommand &command) {
    SourceCommandResult result;result.operationId=command.operationId;result.revision=m_revision;
    const auto all=effectiveBindings();QVector<SourceBinding> changed;SourceBinding binding;
    if(command.kind==SourceCommand::Kind::RegisterBinding) {
        binding=command.binding;if(binding.id.isEmpty()) binding.id=QUuid::createUuid().toString(QUuid::WithoutBraces);
        if(all.contains(binding.id)) {result.code="binding_exists";result.error=QStringLiteral("来源关联标识已存在");return result;}
        binding.primary=false;binding.resolution.clear();
        binding.decision=binding.conflictingIdentities()?BindingDecision::Conflict:BindingDecision::Candidate;
    } else {
        if(!all.contains(command.bindingId)) {result.code="binding_missing";result.error=QStringLiteral("来源关联不存在");return result;}
        binding=all.value(command.bindingId);
        if(command.kind==SourceCommand::Kind::ConfirmBinding) {
            binding.resolution=command.note;binding.decision=BindingDecision::Confirmed;
        } else if(command.kind==SourceCommand::Kind::RemoveBinding) {
            binding.primary=false;binding.decision=BindingDecision::Rejected;binding.resolution=command.note;
        } else if(command.kind==SourceCommand::Kind::SelectPrimary) {
            if(binding.decision!=BindingDecision::Confirmed) {result.code="binding_unconfirmed";result.error=QStringLiteral("请先确认来源关联");return result;}
            binding.primary=true;
            for(auto other:all) if(other.id!=binding.id && other.slotKey()==binding.slotKey() && other.primary) {
                other.primary=false;other.revision=m_revision+1;changed.append(other);
            }
        }
    }
    if(binding.source.documentId!=m_documentId || !source(binding.source).ref.valid()) {
        result.code="source_missing";result.error=QStringLiteral("来源关联不属于当前文档或版本不存在");return result;
    }
    if(binding.resourceKind=="well" && m_catalog->entityById(binding.resourceId).entityType!="well") {
        result.code="resource_missing";result.error=QStringLiteral("关联的井实体不存在");return result;
    }
    binding.revision=m_revision+1;
    if(command.kind==SourceCommand::Kind::SelectPrimary && binding.scope.isEmpty() &&
       sourceForVersion(m_catalog->currentVersion(source(binding.source).assetId).id).ref!=binding.source) {
        result.code="source_revision_changed";result.error=QStringLiteral("当前文件版本已变化，请重新确认来源关联");return result;
    }
    if(!binding.valid(&result.error)) {result.code="binding_invalid";return result;}
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
        const bool bridge=binding.scope.isEmpty() && m_catalog->entityById(binding.resourceId).entityType==binding.resourceKind;
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
        return batch.flush(error);
    },&result.error);
    if(!saved) {
        result.code="binding_commit_failed";return result;
    }
    invalidate();QMetaObject::invokeMethod(m_catalog,"changed",Qt::QueuedConnection);
    SourceChangeSet change;change.epoch=m_epoch;change.revision=m_revision;change.operationId=command.operationId;
    for(const auto &value:changed) {change.bindingIds.append(value.id);change.sourceKeys.append(value.source.key());}
    result.ok=true;result.revision=m_revision;result.code="committed";
    emit sourcesChanged(change);emit commandFinished(result);return result;
}
}
