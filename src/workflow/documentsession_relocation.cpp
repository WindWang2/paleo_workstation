// 层：功能
#include "documentsession_internal.h"
#include "io/sourcefingerprint.h"
#include "io/nativesourcesnapshot.h"
#include "metadata/documentsourcestore.h"
#include <paleo_data/service.h>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSignalBlocker>

namespace paleo::document {
bool DocumentSession::commitRelocation(SourceJob &job,QString *error) {
    const auto fail=[&](const QString &why){if(error)*error=why;return false;};
    if(job.command.expectedEpoch!=m_epoch || readOnly()) return fail(QStringLiteral("工程已关闭、切换或变为只读"));
    const auto current=m_catalog->currentVersion(job.parentVersion.assetId);
    CatalogVersion completed;
    for(const auto &version:m_catalog->versionsForAsset(job.parentVersion.assetId)) {
        if(version.extra.value("documentSourceOperation").toString()!=job.journalId)continue;
        const auto intent=m_operations.value(job.journalId);const auto input=version.extra.value("documentSourceRequest").toMap();
        SourceRef requested,recorded;
        if(!job.recovering || intent.value("schema").toInt()!=1 ||
           !sourceRefFromJson(intent.value("source").toObject(),requested) || requested!=job.command.source ||
           intent.value("assetId").toString()!=job.parentVersion.assetId || intent.value("parentVersionId").toString()!=job.parentVersion.id ||
           intent.value("newPath").toString()!=job.command.path || version.extra.value("relocatedFrom").toString()!=job.parentVersion.id ||
           !sourceRefFromJson(QJsonObject::fromVariantMap(input.value("source").toMap()),recorded) || recorded!=requested ||
           input.value("newPath").toString()!=job.command.path || QDir::cleanPath(version.path)!=QDir::cleanPath(job.command.path) ||
           version.sha256!=job.parentVersion.sha256 || version.stage!=job.parentVersion.stage || version.managed!=job.parentVersion.managed ||
           version.parentVersionIds!=job.parentVersion.parentVersionIds || sourceForVersion(version.id).ref!=requested ||
           version.extra.value("nativeModel").toMap()!=intent.value("nativeModel").toObject().toVariantMap())
            return fail(QStringLiteral("来源操作标识与已有提交输入不一致"));
        completed=version;break;
    }
    if(!completed.id.isEmpty()) {
        job.newVersionId=completed.id;
        return queuedWrite([&](QString *why){
            if(!m_sourceStore->finishOperation(m_documentId,job.journalId,why))return false;
            m_operations.remove(job.journalId);return true;
        },error);
    }
    if(current.id!=job.expectedCurrentVersion) return fail(QStringLiteral("来源当前版本已变化，请重新准备"));
    if(!job.prepared && job.stamp!=sourceFileStamp(job.command.path)) return fail(QStringLiteral("新来源核验后已变化，请重新准备"));
    if(!DataCatalog::isSafePathSegment(QFileInfo(job.command.path).fileName())) return fail(QStringLiteral("新来源文件名无效"));
    QJsonObject intent{{"schema",1},{"source",sourceRefJson(job.record.ref)},{"parentVersionId",job.parentVersion.id},
        {"newPath",job.command.path},{"assetId",job.parentVersion.assetId},{"expectedCurrentVersion",job.expectedCurrentVersion},
        {"createdUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    if(job.nativeSnapshot)intent.insert("nativeModel",QJsonObject::fromVariantMap(job.nativeSnapshot->binding()));
    if(m_operations.contains(job.journalId)) intent=m_operations.value(job.journalId);
    bool catalogChanged=false;
    const bool ok=queuedWrite([&](QString *why){
        // Only a generated immutable artifact is published here; the original DB is untouched.
        // A journal failure removes this instance's unreferenced artifact automatically.
        if(job.nativeSnapshot && !job.nativeSnapshot->publish(why))return false;
        if(!m_sourceStore->writeOperation(m_documentId,job.journalId,intent,why)) return false;
        m_operations.insert(job.journalId,intent);
        if(job.nativeSnapshot)job.nativeSnapshot->retain();
        if(job.prepared) {
            paleo::data::DataService service;std::string detail;
            if(!service.openProjectForSourceRelocation(*job.prepared,detail) || !service.commitSourceRelocation(*job.prepared,detail)) {
                if(why)*why=QString::fromUtf8(detail);
                QString cleanup;
                if(!job.recovering && m_sourceStore->finishOperation(m_documentId,job.journalId,&cleanup)) m_operations.remove(job.journalId);
                return false;
            }
        }
        CatalogVersion next=job.parentVersion;
        next.id=m_catalog->nextVersionId();next.versionNumber=current.versionNumber+1;
        next.path=QFileInfo(job.command.path).absoluteFilePath();next.sourceUri=next.path;next.fileName=QFileInfo(next.path).fileName();
        if(job.nativeSnapshot)next.extra.insert("nativeModel",job.nativeSnapshot->binding());
        next.extra.insert("relocatedFrom",job.parentVersion.id);next.extra.insert("documentSourceOperation",job.journalId);
        next.extra.insert("documentSourceRequest",QVariantMap{{"source",sourceRefJson(job.command.source).toVariantMap()},{"newPath",job.command.path}});
        {
            const QSignalBlocker block(m_catalog);
            if(!m_catalog->addVersion(next,why)) return false; // Model commit is recoverable from the retained intent.
        }
        catalogChanged=true;job.newVersionId=next.id;
        if(!m_sourceStore->finishOperation(m_documentId,job.journalId,why)) return false;
        m_operations.remove(job.journalId);
        return true;
    },error);
    // No observer may re-enter the project write mutex from a catalog signal.
    if(catalogChanged) QMetaObject::invokeMethod(m_catalog,"changed",Qt::QueuedConnection);
    if(ok || catalogChanged) invalidate();
    if(ok) {
        SourceChangeSet change;change.epoch=m_epoch;change.revision=m_revision;change.operationId=job.command.operationId;
        change.sourceKeys={job.record.ref.key(),sourceForVersion(job.newVersionId).ref.key()};emit sourcesChanged(change);
    }
    return ok;
}
}
