// 层：功能
#include "documentsession_internal.h"
#include "io/sourcefingerprint.h"
#include "services/documentsourceprobe.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace paleo::document {
SourceCommandResult DocumentSession::startInspection(const SourceCommand &command) {
    SourceCommandResult result;result.operationId=command.operationId;result.revision=m_revision;
    const auto record=source(command.source);
    if(!record.ref.valid()) {result.code="source_missing";result.error=QStringLiteral("来源版本不存在");return result;}
    if(!m_tasks || m_inspection->busy()) {result.code="busy";result.error=QStringLiteral("来源检查正在进行或任务服务未配置");return result;}
    auto job=std::make_shared<SourceJob>();job->command=command;job->record=record;
    paleo::jobs::JobRunner<SourceJob>::Callbacks callbacks;
    callbacks.compute=[](SourceJob &work,const auto &cancelled,const auto &){
        work.state=inspectDocumentSource(work.record,work.command.verify,cancelled);work.computed=true;return true;
    };
    callbacks.commit=[this](SourceJob &work,QString *){
        if(!work.computed) {
            SourceCommandResult done;done.operationId=work.command.operationId;done.code="inspection_failed";
            done.error=work.error.isEmpty()?QStringLiteral("来源检查失败"):work.error;emit commandFinished(done);return false;
        }
        if(work.command.expectedEpoch!=m_epoch || source(work.record.ref).versionId!=work.record.versionId ||
           source(work.record.ref).path!=work.record.path) {
            SourceCommandResult done;done.operationId=work.command.operationId;done.code="stale_command";
            done.error=QStringLiteral("来源检查结果已过期");emit commandFinished(done);return false;
        }
        if(m_observations.size()>=256) m_observations.clear();
        m_observations.insert(work.record.ref.key(),work.state);
        SourceCommandResult done;done.operationId=work.command.operationId;done.ok=true;done.code="observed";done.state=work.state;done.revision=m_revision;
        emit sourceObserved(work.record.ref,work.state);emit commandFinished(done);return true;
    };
    callbacks.onDropped=[this](SourceJob &work,paleo::jobs::DropReason){
        SourceCommandResult done;done.operationId=work.command.operationId;done.code="cancelled_or_stale";done.error=QStringLiteral("来源检查已取消或过期");emit commandFinished(done);
    };
    if(!m_inspection->start(QStringLiteral("检查资料来源"),job,std::move(callbacks),{},true)) {result.code="start_failed";return result;}
    result.pending=true;result.code="pending";return result;
}
SourceCommandResult DocumentSession::startRelocation(const SourceCommand &requested) {
    auto command=requested;SourceCommandResult result;result.operationId=command.operationId;result.revision=m_revision;
    if(command.path.trimmed().isEmpty()) {result.code="relocation_invalid";result.error=QStringLiteral("新来源路径不能为空");return result;}
    command.path=QFileInfo(command.path).absoluteFilePath();
    QString parentVersion,journalId=command.operationId;
    for(auto it=m_operations.cbegin();it!=m_operations.cend();++it) {
        SourceRef ref;
        if(sourceRefFromJson(it->value("source").toObject(),ref) && ref==command.source) {
            if(it->value("newPath").toString()!=command.path) {result.code="recovery_required";result.error=QStringLiteral("此来源有未完成的重定位，请先恢复原操作");return result;}
            journalId=it.key();break;
        }
    }
    if(m_operations.contains(journalId)) {
        const auto intent=m_operations.value(journalId);SourceRef ref;
        if(!sourceRefFromJson(intent["source"].toObject(),ref) || ref!=command.source || intent["newPath"].toString()!=command.path) {
            result.code="operation_conflict";result.error=QStringLiteral("恢复操作与原输入不一致");return result;
        }
        parentVersion=intent["parentVersionId"].toString();
    }
    const auto record=source(command.source);
    if(!record.ref.valid() || record.managed || !record.fingerprint.valid() ||
       (record.fingerprint.kind==FingerprintKind::SegyQuickV1 && record.ref.modelStoreId!="area-model")) {
        result.code="relocation_invalid";result.error=QStringLiteral("重定位需要有身份指纹的外链来源和新文件路径");return result;
    }
    if(!m_tasks || m_relocation->busy()) {result.code="busy";result.error=QStringLiteral("来源重定位正在进行或任务服务未配置");return result;}
    auto job=std::make_shared<SourceJob>();job->command=command;job->record=record;job->projectDirectory=m_projectDir;
    job->journalId=journalId;job->recovering=m_operations.contains(journalId);
    job->parentVersion=m_catalog->versionById(parentVersion.isEmpty()?record.versionId:parentVersion);
    if(job->parentVersion.id.isEmpty()) {result.code="parent_missing";result.error=QStringLiteral("重定位父版本不存在");return result;}
    paleo::jobs::JobRunner<SourceJob>::Callbacks callbacks;
    callbacks.compute=[](SourceJob &work,const auto &cancelled,const auto &progress){
        progress(0,QStringLiteral("核验新来源"));
        if(work.record.ref.modelStoreId=="area-model") {
            paleo::data::DataService service;std::string error;paleo::data::SourceIndexEntry entry;
            if(!service.openProjectReadOnly(work.record.modelDirectory.toUtf8().toStdString(),error) ||
               !service.readSourceIndexEntry(work.record.ref.sourceId,entry,error) || entry.sha256!=work.record.fingerprint.stored().toStdString() ||
               !service.prepareSourceRelocation(work.record.ref.sourceId,work.command.path.toUtf8().toStdString(),work.prepared,error,cancelled)) {
                work.error=error.empty()?QStringLiteral("来源模型身份不符"):QString::fromUtf8(error);return false;
            }
            work.verifiedBytes=work.prepared->verifiedBytes();
            const auto modelPath=QDir::cleanPath(QString::fromUtf8(work.prepared->originalLocation()));
            if(modelPath!=QDir::cleanPath(work.record.path) && !(work.recovering && modelPath==QDir::cleanPath(work.command.path))) {
                work.error=QStringLiteral("来源模型位置与目录索引不一致，请刷新或恢复未完成操作");return false;
            }
        } else if(!verifySourceFingerprint(work.command.path,work.record.fingerprint,work.stamp,work.verifiedBytes,&work.error,cancelled)) return false;
        work.computed=true;progress(100,QStringLiteral("准备提交来源位置"));return true;
    };
    callbacks.commit=[this](SourceJob &work,QString *error){
        SourceCommandResult done;done.operationId=work.command.operationId;
        bool ok=check(work.command,true,done);
        if(!ok) work.error=done.error;
        if(ok && !work.computed) {ok=false;if(work.error.isEmpty())work.error=QStringLiteral("来源核验未完成");}
        if(ok) ok=commitRelocation(work,&work.error);
        done.ok=ok;done.error=work.error;done.code=ok?"committed":m_operations.contains(work.journalId)?"recovery_required":"relocation_failed";
        done.versionId=work.newVersionId;done.revision=m_revision;done.state.bytesRead=work.verifiedBytes;
        if(error)*error=work.error;
        emit relocationFinished(work.parentVersion.id,work.newVersionId,work.error);emit commandFinished(done);return ok;
    };
    callbacks.onDropped=[this](SourceJob &work,paleo::jobs::DropReason){
        SourceCommandResult done;done.operationId=work.command.operationId;done.code="cancelled_or_stale";done.error=QStringLiteral("重定位已取消或工程已切换");
        emit relocationFinished(work.parentVersion.id,{},done.error);emit commandFinished(done);
    };
    if(!m_relocation->start(QStringLiteral("重新定位资料来源"),job,std::move(callbacks))) {result.code="start_failed";return result;}
    result.pending=true;result.code="pending";return result;
}
}
