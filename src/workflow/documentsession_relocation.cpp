// 层：功能
#include "documentsession_internal.h"
#include "io/sourcefingerprint.h"
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
    const bool alreadyCatalogued=current.extra.value("documentSourceOperation").toString()==job.journalId;
    if(current.id!=job.parentVersion.id && !alreadyCatalogued) return fail(QStringLiteral("来源当前版本已变化，请重新准备"));
    if(!job.prepared && job.stamp!=sourceFileStamp(job.command.path)) return fail(QStringLiteral("新来源核验后已变化，请重新准备"));
    if(!DataCatalog::isSafePathSegment(QFileInfo(job.command.path).fileName())) return fail(QStringLiteral("新来源文件名无效"));
    QJsonObject intent{{"schema",1},{"source",sourceRefJson(job.record.ref)},{"parentVersionId",job.parentVersion.id},
        {"newPath",job.command.path},{"assetId",job.parentVersion.assetId},{"createdUtc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    if(m_operations.contains(job.journalId)) intent=m_operations.value(job.journalId);
    bool catalogChanged=false;
    const bool ok=queuedWrite([&](QString *why){
        if(!m_sourceStore->writeOperation(m_documentId,job.journalId,intent,why)) return false;
        m_operations.insert(job.journalId,intent);
        if(job.prepared) {
            paleo::data::DataService service;std::string detail;
            if(!service.openProject(job.record.modelDirectory.toUtf8().toStdString(),detail) || !service.commitSourceRelocation(*job.prepared,detail)) {
                if(why)*why=QString::fromUtf8(detail);
                QString cleanup;
                if(!job.recovering && m_sourceStore->finishOperation(m_documentId,job.journalId,&cleanup)) m_operations.remove(job.journalId);
                return false;
            }
        }
        if(alreadyCatalogued) job.newVersionId=current.id;
        else {
            CatalogVersion next=job.parentVersion;
            next.id=m_catalog->nextVersionId();next.versionNumber=current.versionNumber+1;
            next.path=QFileInfo(job.command.path).absoluteFilePath();next.sourceUri=next.path;next.fileName=QFileInfo(next.path).fileName();
            // Preserve immutable native snapshots and their scoped source identity.
            next.extra.insert("relocatedFrom",job.parentVersion.id);next.extra.insert("documentSourceOperation",job.journalId);
            const QSignalBlocker block(m_catalog);
            if(!m_catalog->addVersion(next,why)) return false; // Model commit is recoverable from the retained intent.
            catalogChanged=true;
            job.newVersionId=next.id;
        }
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
