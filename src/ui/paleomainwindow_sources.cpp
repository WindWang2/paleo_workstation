// 层：视图
#include "paleomainwindow.h"
#include "datapreview/datapreviewtabs.h"
#include "workflow/documentsession.h"
#include "workflow/sectionworkbench.h"
#include "workflow/wellsectionworkflow.h"
#include <QUuid>

void PaleoMainWindow::attachDocumentSession(paleo::document::DocumentSession *session) {
    if(!m_previewTabs || !session) return;
    m_documentSession=session;
    m_previewTabs->setDocumentSession(session);
    if(m_sectionWorkbench)m_sectionWorkbench->setDocumentSession(session);
    if(m_wellSectionWf)m_wellSectionWf->setDocumentSession(session);
    const QPointer<paleo::document::DocumentSession> document(session);
    m_previewTabs->setSourceRelocationHandler([this,document](const QString &version,const QString &path,DataPreviewTabs::RelocationReply done){
        if(!document || !document->isOpen()) {done({},tr("来源文档未打开"));return;}
        auto command=document->command(paleo::document::SourceCommand::Kind::Relocate);
        command.source=document->sourceForVersion(version).ref;command.path=path;
        command.operationId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QPointer<QObject> reply(new QObject(this));
        connect(document,&paleo::document::DocumentSession::commandFinished,reply,
            [reply,done,id=command.operationId](const auto &result){
                if(result.operationId!=id) return;
                done(result.ok?result.versionId:QString(),result.error);delete reply;
            });
        connect(document,&paleo::document::DocumentSession::sessionChanged,reply,
            [reply,done](quint64){done({},tr("工程会话已变化"));delete reply;});
        const auto result=document->execute(command);
        if(!result.pending && reply) {done(result.ok?result.versionId:QString(),result.error);delete reply;}
    });
}
